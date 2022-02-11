//
// Created by 邓沛堆 on 2020-05-28.
//

#include "type.h"
#include <GLES2/gl2.h>
#include <jni.h>
#include <string>
#include <sstream>
#include <thread>
#include "BacktraceDefine.h"
#include "Backtrace.h"
#include <sys/prctl.h>
#include "BufferQueue.h"
#include <EGL/egl.h>
#include <pthread.h>
#include "StackMeta.h"
#include <android/log.h>

#ifndef OPENGL_API_HOOK_MY_FUNCTIONS_H
#define OPENGL_API_HOOK_MY_FUNCTIONS_H

using namespace std;
using namespace matrix;

#define MEMHOOK_BACKTRACE_MAX_FRAMES MAX_FRAME_SHORT
#define RENDER_THREAD_NAME "RenderThread"

static System_GlNormal_TYPE system_glGenTextures = NULL;
static System_GlNormal_TYPE system_glDeleteTextures = NULL;
static System_GlNormal_TYPE system_glGenBuffers = NULL;
static System_GlNormal_TYPE system_glDeleteBuffers = NULL;
static System_GlNormal_TYPE system_glGenFramebuffers = NULL;
static System_GlNormal_TYPE system_glDeleteFramebuffers = NULL;
static System_GlNormal_TYPE system_glGenRenderbuffers = NULL;
static System_GlNormal_TYPE system_glDeleteRenderbuffers = NULL;
static System_GlGetError_TYPE system_glGetError = NULL;
static System_GlTexImage2D system_glTexImage2D = NULL;
static System_GlTexImage3D system_glTexImage3D = NULL;
static System_GlBind_TYPE system_glBindTexture = NULL;
static System_GlBind_TYPE system_glBindBuffer = NULL;
static System_GlBind_TYPE system_glBindFramebuffer = NULL;
static System_GlBind_TYPE system_glBindRenderbuffer = NULL;
static System_GlBufferData system_glBufferData = NULL;
static System_GlRenderbufferStorage system_glRenderbufferStorage = NULL;

static JavaVM *m_java_vm;

static jclass class_OpenGLHook;
static jmethodID method_onGlGenTextures;
static jmethodID method_onGlDeleteTextures;
static jmethodID method_onGlGenBuffers;
static jmethodID method_onGlDeleteBuffers;
static jmethodID method_onGlGenFramebuffers;
static jmethodID method_onGlDeleteFramebuffers;
static jmethodID method_onGlGenRenderbuffers;
static jmethodID method_onGlDeleteRenderbuffers;
static jmethodID method_onGetError;
static jmethodID method_onGlBindTexture;
static jmethodID method_onGlBindBuffer;
static jmethodID method_onGlBindFramebuffer;
static jmethodID method_onGlBindRenderbuffer;
static jmethodID method_onGlTexImage2D;
static jmethodID method_onGlTexImage3D;
static jmethodID method_onGlBufferData;
static jmethodID method_onGlRenderbufferStorage;
static jmethodID method_getThrowable;

const size_t BUF_SIZE = 1024;

static pthread_once_t g_onceInitTls = PTHREAD_ONCE_INIT;
static pthread_key_t g_tlsJavaEnv;
static pthread_key_t g_thread_name_key;
static pthread_key_t g_thread_id_key;
static bool is_stacktrace_enabled = true;
static bool is_javastack_enabled = true;

static matrix::BufferManagement *messages_containers;

void enable_stacktrace(bool enable) {
    is_stacktrace_enabled = enable;
}

void enable_javastack(bool enable) {
    is_javastack_enabled = enable;
}

void thread_id_to_string(thread::id thread_id, char *&result) {
    stringstream stream;
    stream << thread_id;
    result = new char[stream.str().size() + 1];
    strcpy(result, stream.str().c_str());
}

inline void get_thread_name(char *&thread_name) {
    char *local_name = static_cast<char *>(pthread_getspecific(g_thread_name_key));
    if (local_name == nullptr) {
        thread_name = static_cast<char *>(malloc(BUF_SIZE));
        prctl(PR_GET_NAME, (char *) (thread_name));
        pthread_setspecific(g_thread_name_key, thread_name);
    } else {
        thread_name = local_name;
    }
}

bool is_render_thread() {
    bool result = false;
    char *thread_name;
    get_thread_name(thread_name);
    if (strcmp(RENDER_THREAD_NAME, thread_name) == 0) {
        result = true;
    }
    return result;
}

JNIEnv *GET_ENV() {
    JNIEnv *env;
    int ret = m_java_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (ret != JNI_OK) {
        pthread_once(&g_onceInitTls, []() {
            pthread_key_create(&g_tlsJavaEnv, [](void *d) {
                if (d && m_java_vm)
                    m_java_vm->DetachCurrentThread();
            });
        });

        char *thread_name = static_cast<char *>(malloc(BUF_SIZE));
        get_thread_name(thread_name);

        JavaVMAttachArgs args{
                .version = JNI_VERSION_1_6,
                .name = thread_name,
                .group = nullptr
        };

        if (m_java_vm->AttachCurrentThread(&env, &args) == JNI_OK) {
            pthread_setspecific(g_tlsJavaEnv, reinterpret_cast<const void *>(1));
        } else {
            env = nullptr;
        }

        if (thread_name != nullptr) {
            free(thread_name);
        }
    }
    return env;
}

bool is_need_get_java_stack() {
    JNIEnv *env;
    int ret = m_java_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    return ret == JNI_OK;
}

inline void get_thread_id_string(char *&result) {
    char *local_id = static_cast<char *>(pthread_getspecific(g_thread_id_key));
    if (local_id == nullptr) {
        thread::id thread_id = this_thread::get_id();
        thread_id_to_string(thread_id, result);
        pthread_setspecific(g_thread_id_key, result);
    } else {
        result = local_id;
    }
}

wechat_backtrace::Backtrace *get_native_backtrace() {
    wechat_backtrace::Backtrace *backtracePrt = nullptr;
    if (is_stacktrace_enabled) {
        wechat_backtrace::Backtrace backtrace_zero = BACKTRACE_INITIALIZER(
                MEMHOOK_BACKTRACE_MAX_FRAMES);

        backtracePrt = new wechat_backtrace::Backtrace;
        backtracePrt->max_frames = backtrace_zero.max_frames;
        backtracePrt->frame_size = backtrace_zero.frame_size;
        backtracePrt->frames = backtrace_zero.frames;

        wechat_backtrace::unwind_adapter(backtracePrt->frames.get(), backtracePrt->max_frames,
                                         backtracePrt->frame_size);
    }
    return backtracePrt;
}

int get_java_throwable() {
    int throwable = -1;
    if (is_javastack_enabled && is_need_get_java_stack()) {
        JNIEnv *env = GET_ENV();
        throwable = env->CallStaticIntMethod(class_OpenGLHook, method_getThrowable);
    }
    return throwable;
}

void gen_jni_callback(int alloc_count, GLuint *copy_resource, int throwable, char *thread_id,
                      wechat_backtrace::Backtrace *backtracePtr, EGLContext egl_context,
                      jmethodID jmethodId) {
    JNIEnv *env = GET_ENV();

    int *result = new int[alloc_count];
    for (int i = 0; i < alloc_count; i++) {
        result[i] = *(copy_resource + i);
    }
    jintArray newArr = env->NewIntArray(alloc_count);

    env->SetIntArrayRegion(newArr, 0, alloc_count, result);

    jstring j_thread_id = env->NewStringUTF(thread_id);

    wechat_backtrace::Backtrace *backtrace = deduplicate_backtrace(backtracePtr);

    env->CallStaticVoidMethod(
            class_OpenGLHook,
            jmethodId,
            newArr,
            j_thread_id,
            (jint) throwable,
            (jlong) backtrace,
            (jlong) egl_context);

    delete[] result;
    env->DeleteLocalRef(newArr);
    env->DeleteLocalRef(j_thread_id);

    if (copy_resource != nullptr) {
        free(copy_resource);
    }
}

void delete_jni_callback(int delete_count, GLuint *copy_resource, char *thread_id,
                         EGLContext egl_context, jmethodID jmethodId) {
    JNIEnv *env = GET_ENV();

    int *result = new int[delete_count];
    for (int i = 0; i < delete_count; i++) {
        result[i] = *(copy_resource + i);
    }
    jintArray newArr = env->NewIntArray(delete_count);
    env->SetIntArrayRegion(newArr, 0, delete_count, result);

    jstring j_thread_id = env->NewStringUTF(thread_id);

    env->CallStaticVoidMethod(class_OpenGLHook,
                              jmethodId,
                              newArr,
                              j_thread_id,
                              (jlong) egl_context);

    delete[] result;
    env->DeleteLocalRef(newArr);
    env->DeleteLocalRef(j_thread_id);

    if (copy_resource != nullptr) {
        free(copy_resource);
    }
}

GL_APICALL void GL_APIENTRY my_glGenTextures(GLsizei n, GLuint *textures) {
    if (NULL != system_glGenTextures) {
        system_glGenTextures(n, textures);

        if (is_render_thread()) {
            return;
        }

        GLuint *copy_textures = new GLuint[n];
        memcpy(copy_textures, textures, n * sizeof(GLuint));

        wechat_backtrace::Backtrace *backtracePrt = get_native_backtrace();

        int throwable = get_java_throwable();

        char *thread_id = nullptr;
        get_thread_id_string(thread_id);

        EGLContext egl_context = eglGetCurrentContext();

        messages_containers->
                enqueue_message((uintptr_t) egl_context,
                                [n, copy_textures, throwable, thread_id, backtracePrt, egl_context]() {

                                    gen_jni_callback(n, copy_textures, throwable, thread_id,
                                                     backtracePrt, egl_context,
                                                     method_onGlGenTextures);

                                });
    }
}

GL_APICALL void GL_APIENTRY my_glDeleteTextures(GLsizei n, GLuint *textures) {
    if (NULL != system_glDeleteTextures) {
        system_glDeleteTextures(n, textures);

        if (is_render_thread()) {
            return;
        }

        GLuint *copy_textures = new GLuint[n];
        memcpy(copy_textures, textures, n * sizeof(GLuint));

        char *thread_id = nullptr;
        get_thread_id_string(thread_id);

        EGLContext egl_context = eglGetCurrentContext();

        messages_containers
                ->enqueue_message((uintptr_t) egl_context,
                                  [n, copy_textures, thread_id, egl_context] {

                                      delete_jni_callback(n, copy_textures, thread_id, egl_context,
                                                          method_onGlDeleteTextures);

                                  });
    }
}

GL_APICALL void GL_APIENTRY my_glGenBuffers(GLsizei n, GLuint *buffers) {
    if (NULL != system_glGenBuffers) {
        system_glGenBuffers(n, buffers);

        if (is_render_thread()) {
            return;
        }

        GLuint *copy_buffers = new GLuint[n];
        memcpy(copy_buffers, buffers, n * sizeof(GLuint));

        wechat_backtrace::Backtrace *backtracePrt = get_native_backtrace();

        int throwable = get_java_throwable();

        char *thread_id = nullptr;
        get_thread_id_string(thread_id);

        EGLContext egl_context = eglGetCurrentContext();

        messages_containers
                ->enqueue_message((uintptr_t) egl_context,
                                  [n, copy_buffers, throwable, thread_id, backtracePrt, egl_context]() {

                                      gen_jni_callback(n, copy_buffers, throwable, thread_id,
                                                       backtracePrt, egl_context,
                                                       method_onGlGenBuffers);

                                  });

    }
}

GL_APICALL void GL_APIENTRY my_glDeleteBuffers(GLsizei n, GLuint *buffers) {
    if (NULL != system_glDeleteBuffers) {
        system_glDeleteBuffers(n, buffers);

        if (is_render_thread()) {
            return;
        }

        GLuint *copy_buffers = new GLuint[n];
        memcpy(copy_buffers, buffers, n * sizeof(GLuint));

        char *thread_id = nullptr;
        get_thread_id_string(thread_id);

        EGLContext egl_context = eglGetCurrentContext();

        messages_containers
                ->enqueue_message((uintptr_t) egl_context,
                                  [n, copy_buffers, thread_id, egl_context]() {

                                      delete_jni_callback(n, copy_buffers, thread_id, egl_context,
                                                          method_onGlDeleteBuffers);

                                  });
    }
}

GL_APICALL void GL_APIENTRY my_glGenFramebuffers(GLsizei n, GLuint *buffers) {
    if (NULL != system_glGenFramebuffers) {
        system_glGenFramebuffers(n, buffers);

        if (is_render_thread()) {
            return;
        }
        GLuint *copy_buffers = new GLuint[n];
        memcpy(copy_buffers, buffers, n * sizeof(GLuint));

        wechat_backtrace::Backtrace *backtracePrt = get_native_backtrace();

        int throwable = get_java_throwable();

        char *thread_id = nullptr;
        get_thread_id_string(thread_id);

        EGLContext egl_context = eglGetCurrentContext();

        messages_containers
                ->enqueue_message((uintptr_t) egl_context,
                                  [n, copy_buffers, throwable, thread_id, backtracePrt, egl_context]() {

                                      gen_jni_callback(n, copy_buffers, throwable, thread_id,
                                                       backtracePrt, egl_context,
                                                       method_onGlGenFramebuffers);

                                  });

    }
}

GL_APICALL void GL_APIENTRY my_glDeleteFramebuffers(GLsizei n, GLuint *buffers) {
    if (NULL != system_glDeleteFramebuffers) {
        system_glDeleteFramebuffers(n, buffers);

        if (is_render_thread()) {
            return;
        }

        GLuint *copy_buffers = new GLuint[n];
        memcpy(copy_buffers, buffers, n * sizeof(GLuint));

        char *thread_id = nullptr;
        get_thread_id_string(thread_id);

        EGLContext egl_context = eglGetCurrentContext();

        messages_containers
                ->enqueue_message((uintptr_t) egl_context,
                                  [n, copy_buffers, thread_id, egl_context]() {
                                      delete_jni_callback(n, copy_buffers, thread_id, egl_context,
                                                          method_onGlDeleteFramebuffers);
                                  });

    }
}

GL_APICALL void GL_APIENTRY my_glGenRenderbuffers(GLsizei n, GLuint *buffers) {
    if (NULL != system_glGenRenderbuffers) {
        system_glGenRenderbuffers(n, buffers);

        GLuint *copy_buffers = new GLuint[n];
        memcpy(copy_buffers, buffers, n * sizeof(GLuint));

        if (is_render_thread()) {
            return;
        }

        wechat_backtrace::Backtrace *backtracePrt = get_native_backtrace();

        int throwable = get_java_throwable();

        char *thread_id = nullptr;
        get_thread_id_string(thread_id);

        EGLContext egl_context = eglGetCurrentContext();

        messages_containers
                ->enqueue_message((uintptr_t) egl_context,
                                  [n, copy_buffers, throwable, thread_id, backtracePrt, egl_context]() {

                                      gen_jni_callback(n, copy_buffers, throwable, thread_id,
                                                       backtracePrt, egl_context,
                                                       method_onGlGenRenderbuffers);

                                  });
    }
}

GL_APICALL void GL_APIENTRY my_glDeleteRenderbuffers(GLsizei n, GLuint *buffers) {
    if (NULL != system_glDeleteRenderbuffers) {
        system_glDeleteRenderbuffers(n, buffers);

        if (is_render_thread()) {
            return;
        }

        GLuint *copy_buffers = new GLuint[n];
        memcpy(copy_buffers, buffers, n * sizeof(GLuint));

        char *thread_id = nullptr;
        get_thread_id_string(thread_id);

        EGLContext egl_context = eglGetCurrentContext();

        messages_containers
                ->enqueue_message((uintptr_t) egl_context,
                                  [n, copy_buffers, thread_id, egl_context]() {

                                      delete_jni_callback(n, copy_buffers, thread_id, egl_context,
                                                          method_onGlDeleteRenderbuffers);

                                  });
    }
}

GL_APICALL int GL_APIENTRY my_glGetError() {
    if (NULL != system_glGetError) {
        int result = system_glGetError();
        JNIEnv *env = GET_ENV();
        jint jresult = result;

        env->CallStaticVoidMethod(class_OpenGLHook, method_onGetError, jresult);

        return result;
    }

    return 0;
}

GL_APICALL void GL_APIENTRY
my_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                GLint border, GLenum format, GLenum type, const void *pixels) {
    if (NULL != system_glTexImage2D) {
        system_glTexImage2D(target, level, internalformat, width, height, border, format, type,
                            pixels);

        if (is_render_thread()) {
            return;
        }

        wechat_backtrace::Backtrace *backtracePrt = get_native_backtrace();

        int throwable = get_java_throwable();

        EGLContext egl_context = eglGetCurrentContext();

        messages_containers
                ->enqueue_message((uintptr_t) egl_context,
                                  [target, level, internalformat, width, height, border, format, type, throwable, backtracePrt, egl_context]() {
                                      JNIEnv *env = GET_ENV();

                                      int pixel = Utils::getSizeOfPerPixel(internalformat, format,
                                                                           type);
                                      long size = width * height * pixel;

                                      wechat_backtrace::Backtrace *backtrace = deduplicate_backtrace(backtracePrt);

                                      env->CallStaticVoidMethod(class_OpenGLHook,
                                                                method_onGlTexImage2D,
                                                                target,
                                                                level,
                                                                internalformat, width,
                                                                height, border, format,
                                                                type,
                                                                (jlong) size,
                                                                (jint) throwable,
                                                                (jlong) backtrace,
                                                                (jlong) egl_context);
                                  });

    }
}

GL_APICALL void GL_APIENTRY
my_glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                GLsizei depth, GLint border, GLenum format, GLenum type, const void *pixels) {
    if (NULL != system_glTexImage3D) {
        system_glTexImage3D(target, level, internalformat, width, height, depth, border, format,
                            type, pixels);

        if (is_render_thread()) {
            return;
        }

        wechat_backtrace::Backtrace *backtracePrt = get_native_backtrace();

        int throwable = get_java_throwable();

        EGLContext egl_context = eglGetCurrentContext();

        messages_containers
                ->enqueue_message((uintptr_t) egl_context,
                                  [target, level, internalformat, width, height, depth, border, format, type, backtracePrt, throwable, egl_context]() {

                                      JNIEnv *env = GET_ENV();

                                      int pixel = Utils::getSizeOfPerPixel(internalformat,
                                                                           format,
                                                                           type);

                                      long size = width * height * depth * pixel;

                                      wechat_backtrace::Backtrace *backtrace = deduplicate_backtrace(backtracePrt);

                                      env->CallStaticVoidMethod(class_OpenGLHook,
                                                                method_onGlTexImage3D,
                                                                target,
                                                                level,
                                                                internalformat, width, height,
                                                                depth, border,
                                                                format,
                                                                type,
                                                                (jlong) size,
                                                                (jint) throwable,
                                                                (jlong) backtrace,
                                                                (jlong) egl_context);

                                  });
    }
}


GL_APICALL void GL_APIENTRY my_glBindTexture(GLenum target, GLuint resourceId) {
    if (NULL != system_glBindTexture) {
        system_glBindTexture(target, resourceId);

        if (is_render_thread()) {
            return;
        }

        EGLContext egl_context = eglGetCurrentContext();

        messages_containers
                ->enqueue_message((uintptr_t) egl_context, [target, resourceId, egl_context]() {
                    JNIEnv *env = GET_ENV();
                    env->CallStaticVoidMethod(class_OpenGLHook, method_onGlBindTexture, target,
                                              (jint) resourceId,
                                              (jlong) egl_context);

                });

    }
}

GL_APICALL void GL_APIENTRY my_glBindBuffer(GLenum target, GLuint resourceId) {
    if (NULL != system_glBindTexture) {
        system_glBindBuffer(target, resourceId);

        if (is_render_thread()) {
            return;
        }

        EGLContext egl_context = eglGetCurrentContext();

        messages_containers
                ->enqueue_message((uintptr_t) egl_context, [target, resourceId, egl_context]() {
                    JNIEnv *env = GET_ENV();

                    env->CallStaticVoidMethod(class_OpenGLHook, method_onGlBindBuffer, target,
                                              (jint) resourceId,
                                              (jlong) egl_context);

                });
    }
}

GL_APICALL void GL_APIENTRY my_glBindFramebuffer(GLenum target, GLuint resourceId) {
    if (NULL != system_glBindTexture) {
        system_glBindFramebuffer(target, resourceId);

        if (is_render_thread()) {
            return;
        }

        EGLContext egl_context = eglGetCurrentContext();

        messages_containers
                ->enqueue_message((uintptr_t) egl_context, [target, resourceId, egl_context]() {
                    JNIEnv *env = GET_ENV();
                    env->CallStaticVoidMethod(class_OpenGLHook, method_onGlBindFramebuffer,
                                              target,
                                              resourceId, (jlong) egl_context);
                });
    }
}

GL_APICALL void GL_APIENTRY my_glBindRenderbuffer(GLenum target, GLuint resourceId) {
    if (NULL != system_glBindTexture) {
        system_glBindRenderbuffer(target, resourceId);

        if (is_render_thread()) {
            return;
        }
        EGLContext egl_context = eglGetCurrentContext();

        messages_containers
                ->enqueue_message((uintptr_t) egl_context, [target, resourceId, egl_context]() {
                    JNIEnv *env = GET_ENV();
                    env->CallStaticVoidMethod(class_OpenGLHook, method_onGlBindRenderbuffer,
                                              target,
                                              (jint) resourceId, (jlong) egl_context);
                });
    }
}

GL_APICALL void GL_APIENTRY
my_glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage) {
    if (NULL != system_glBindTexture) {
        system_glBufferData(target, size, data, usage);

        if (is_render_thread()) {
            return;
        }

        wechat_backtrace::Backtrace *backtracePrt = get_native_backtrace();

        int throwable = get_java_throwable();

        EGLContext egl_context = eglGetCurrentContext();

        messages_containers
                ->enqueue_message((uintptr_t) egl_context,
                                  [target, size, usage, throwable, backtracePrt, egl_context]() {

                                      JNIEnv *env = GET_ENV();

                                      wechat_backtrace::Backtrace *backtrace = deduplicate_backtrace(backtracePrt);

                                      env->CallStaticVoidMethod(class_OpenGLHook,
                                                                method_onGlBufferData,
                                                                target,
                                                                usage, (jlong) size,
                                                                (jint) throwable,
                                                                (jlong) backtrace,
                                                                (jlong) egl_context);

                                  });
    }
}

GL_APICALL void GL_APIENTRY
my_glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {
    if (NULL != system_glBindTexture) {
        system_glRenderbufferStorage(target, internalformat, width, height);

        if (is_render_thread()) {
            return;
        }

        wechat_backtrace::Backtrace *backtracePrt = get_native_backtrace();

        int throwable = get_java_throwable();

        EGLContext egl_context = eglGetCurrentContext();

        messages_containers
                ->enqueue_message((uintptr_t) egl_context,
                                  [target, internalformat, width, height, backtracePrt, throwable, egl_context]() {

                                      JNIEnv *env = GET_ENV();

                                      long size = Utils::getRenderbufferSizeByFormula(
                                              internalformat, width, height);

                                      wechat_backtrace::Backtrace *backtrace = deduplicate_backtrace(backtracePrt);

                                      env->CallStaticVoidMethod(class_OpenGLHook,
                                                                method_onGlRenderbufferStorage,
                                                                target,
                                                                width, height,
                                                                internalformat,
                                                                (jlong) size,
                                                                (jint) throwable,
                                                                (jlong) backtrace,
                                                                (jlong) egl_context);
                                  });

    }
}


#endif //OPENGL_API_HOOK_MY_FUNCTIONS_H