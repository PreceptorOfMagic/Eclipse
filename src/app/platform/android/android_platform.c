#include "android_platform.h"

#include "util/path.h"
#include "logging.h"

#include <SDL.h>
#include <SDL_system.h>
#include <jni.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Set by android_platform_init(true): the co-op peer is an exec'd process with no Java VM. SDL's JNI helpers
 * dereference the VM's JNIEnv without checking it, so nothing in this process may reach them. */
static bool peer_process = false;

static bool absolute_env(const char *name) {
    const char *value = getenv(name);
    return value != NULL && value[0] == '/';
}

static int ensure_dir(const char *dir) {
    if (mkdir(dir, 0700) == 0 || errno == EEXIST) {
        return 0;
    }
    return -1;
}

bool android_platform_init(bool peer) {
    peer_process = peer;
    static const char *const required[] = {
            ANDROID_ENV_FILES_DIR, ANDROID_ENV_CACHE_DIR, ANDROID_ENV_ASSETS_DIR, ANDROID_ENV_NATIVE_DIR,
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        if (!absolute_env(required[i])) {
            /* Only EclipseActivity starts this program; anything else has no storage to run in. */
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Eclipse: %s is not set by the activity", required[i]);
            return false;
        }
    }
    if (ensure_dir(getenv(ANDROID_ENV_FILES_DIR)) != 0 || ensure_dir(getenv(ANDROID_ENV_CACHE_DIR)) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Eclipse: app storage is not writable: %s", strerror(errno));
        return false;
    }
    /* SS4S reads its module list from the file this names (patched modules_ini.c): the list is generated
     * with the build and packaged as an asset, because the package manager extracts only lib*.so files into
     * the native library directory where the desktop builds keep it. */
    if (getenv("SS4S_MODULES_INI") == NULL) {
        char *ini = path_join(getenv(ANDROID_ENV_ASSETS_DIR), "ss4s_modules.ini");
        if (ini != NULL) {
            setenv("SS4S_MODULES_INI", ini, 1);
            free(ini);
        }
    }
    if (peer) {
        /* Tells the MediaCodec module (a separate library) not to look for Java. */
        setenv("ECLIPSE_ANDROID_PEER", "1", 1);
        /* A network/decode worker with no Java VM: SDL's Android audio backends enumerate devices through
         * JNI as soon as the audio subsystem starts. The peer never plays sound itself -- its PCM goes to the
         * primary over the co-op audio socket -- so give SDL the dummy driver. */
        setenv("SDL_AUDIODRIVER", "dummy", 1);
    } else {
        /* Back reaches the app as SDL_SCANCODE_AC_BACK instead of finishing the activity; the UI and the
         * stream decide what it means (platform/android/keyboard_android.c). */
        SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
        /* The device's own accelerometer (phones, some boxes) must not appear as a game controller. */
        SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
    }
    return true;
}

int android_sdk_int(void) {
    const char *value = getenv(ANDROID_ENV_SDK_INT);
    return value != NULL ? atoi(value) : 0;
}

const char *android_device_name(void) {
    const char *value = getenv(ANDROID_ENV_DEVICE);
    return value != NULL && value[0] != '\0' ? value : "Android device";
}

/* The activity's class, from the live activity object: FindClass on a thread SDL attached to the VM uses the
 * system class loader, which cannot see the app's classes. */
static jclass activity_class(JNIEnv *env) {
    jobject activity = (jobject) SDL_AndroidGetActivity();
    if (activity == NULL) {
        return NULL;
    }
    jclass cls = (*env)->GetObjectClass(env, activity);
    (*env)->DeleteLocalRef(env, activity);
    return cls;
}

static JNIEnv *activity_env(void) {
    if (peer_process) {
        return NULL;
    }
    return (JNIEnv *) SDL_AndroidGetJNIEnv();
}

static void clear_exception(JNIEnv *env, const char *what) {
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        commons_log_warn("Android", "%s threw a Java exception", what);
    }
}

bool android_display_mode(int *width, int *height, int *refresh_x100, int *max_refresh_x100, bool *hdr10) {
    JNIEnv *env = activity_env();
    if (env == NULL) {
        return false;
    }
    jclass cls = activity_class(env);
    if (cls == NULL) {
        return false;
    }
    bool ok = false;
    jmethodID method = (*env)->GetStaticMethodID(env, cls, "getDisplayMode", "()[I");
    if (method != NULL) {
        jintArray result = (jintArray) (*env)->CallStaticObjectMethod(env, cls, method);
        clear_exception(env, "getDisplayMode");
        /* {width, height, current refresh x100, highest refresh x100 at that size, HDR10 0/1} */
        if (result != NULL && (*env)->GetArrayLength(env, result) >= 5) {
            jint values[5];
            (*env)->GetIntArrayRegion(env, result, 0, 5, values);
            if (values[0] > 0 && values[1] > 0) {
                if (width != NULL) *width = values[0];
                if (height != NULL) *height = values[1];
                if (refresh_x100 != NULL) *refresh_x100 = values[2];
                if (max_refresh_x100 != NULL) *max_refresh_x100 = values[3];
                if (hdr10 != NULL) *hdr10 = values[4] != 0;
                ok = true;
            }
        }
        if (result != NULL) {
            (*env)->DeleteLocalRef(env, result);
        }
    } else {
        clear_exception(env, "getDisplayMode lookup");
    }
    (*env)->DeleteLocalRef(env, cls);
    return ok;
}

static void call_static_void_int(const char *name, int value) {
    JNIEnv *env = activity_env();
    if (env == NULL) {
        return;
    }
    jclass cls = activity_class(env);
    if (cls == NULL) {
        return;
    }
    jmethodID method = (*env)->GetStaticMethodID(env, cls, name, "(I)V");
    if (method != NULL) {
        (*env)->CallStaticVoidMethod(env, cls, method, (jint) value);
    }
    clear_exception(env, name);
    (*env)->DeleteLocalRef(env, cls);
}

void android_display_request_refresh(int refresh_x100) {
    call_static_void_int("requestRefreshRate", refresh_x100);
}

/* EclipseActivity.nativeTvKey(int key, boolean pressed): a remote key SDL2 leaves unmapped (see
 * android_platform.h). Called on the UI thread; SDL_PushEvent is thread safe. */
JNIEXPORT void JNICALL
Java_io_github_preceptorofmagic_eclipse_EclipseActivity_nativeTvKey(JNIEnv *env, jclass cls, jint key,
                                                                    jboolean pressed) {
    (void) env;
    (void) cls;
    const int scancode = ANDROID_SCANCODE_TV_BASE + key;
    if (scancode <= ANDROID_SCANCODE_TV_BASE || scancode > ANDROID_SCANCODE_TV_LAST) {
        return;
    }
    SDL_Event event;
    SDL_zero(event);
    event.type = pressed ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.timestamp = SDL_GetTicks();
    event.key.state = pressed ? SDL_PRESSED : SDL_RELEASED;
    event.key.keysym.scancode = (SDL_Scancode) scancode;
    event.key.keysym.sym = SDLK_UNKNOWN;
    SDL_Window *window = SDL_GetKeyboardFocus();
    event.key.windowID = window != NULL ? SDL_GetWindowID(window) : 0;
    SDL_PushEvent(&event);
}
