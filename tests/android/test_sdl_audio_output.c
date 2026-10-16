/*
 * The SS4S SDL audio module on Android (third_party/ss4s/modules/sdl/sdl.c, __ANDROID__ parts): a stored output
 * name is turned into the output's current number through the activity, the module waits for an output that is
 * reconnecting and never falls back to the default, and a chosen output selects SDL's AAudio driver.
 * The activity (JNI) and SDL's device open are faked; everything else is the module as built.
 */
#define SDL_OpenAudioDevice FakeOpenAudioDevice
#define SDL_PauseAudioDevice FakePauseAudioDevice
#define SDL_CloseAudioDevice FakeCloseAudioDevice
#define SDL_InitSubSystem FakeInitSubSystem
#define SDL_Delay FakeDelay
#define SDL_AndroidGetJNIEnv FakeGetJNIEnv
#define SDL_AndroidGetActivity FakeGetActivity

#include MODULE_SOURCE

#include <stdarg.h>
#include <stdio.h>

/* --- the activity ------------------------------------------------------------------------------------------- */
static int lookups, bluetooth_absent_for, local_refs;
static char last_string[256];
static int fake_activity, fake_class, fake_method;

static jclass JNICALL GetObjectClassFake(JNIEnv *env, jobject obj) {
    (void) env;
    (void) obj;
    local_refs++;
    return (jclass) &fake_class;
}
static void JNICALL DeleteLocalRefFake(JNIEnv *env, jobject obj) {
    (void) env;
    (void) obj;
    local_refs--;
}
static jmethodID JNICALL GetStaticMethodIDFake(JNIEnv *env, jclass cls, const char *name, const char *sig) {
    (void) env;
    (void) cls;
    return strcmp(name, "getAudioOutputId") == 0 && strcmp(sig, "(Ljava/lang/String;)I") == 0
           ? (jmethodID) &fake_method : NULL;
}
static jstring JNICALL NewStringUTFFake(JNIEnv *env, const char *utf) {
    (void) env;
    snprintf(last_string, sizeof(last_string), "%s", utf);
    local_refs++;
    return (jstring) last_string;
}
static jint JNICALL CallStaticIntMethodFake(JNIEnv *env, jclass cls, jmethodID method, ...) {
    (void) env;
    (void) cls;
    (void) method;
    va_list ap;
    va_start(ap, method);
    const char *name = (const char *) va_arg(ap, jstring);
    va_end(ap);
    lookups++;
    if (strcmp(name, "HDMI: LG TV") == 0) return 7;
    if (strcmp(name, "Bluetooth: WH-1000XM5") == 0) return bluetooth_absent_for-- > 0 ? 0 : 12;
    return 0;
}
static jboolean JNICALL ExceptionCheckFake(JNIEnv *env) {
    (void) env;
    return JNI_FALSE;
}
static void JNICALL ExceptionClearFake(JNIEnv *env) {
    (void) env;
}

static struct JNINativeInterface_ fake_functions;
static const struct JNINativeInterface_ *fake_env = &fake_functions;

void *FakeGetJNIEnv(void) {
    return (void *) &fake_env;
}
void *FakeGetActivity(void) {
    local_refs++;
    return &fake_activity;
}

/* --- SDL ---------------------------------------------------------------------------------------------------- */
static char opened_name[64];
static int opens, delays;
static const char *hint_at_init;

SDL_AudioDeviceID FakeOpenAudioDevice(const char *device, int iscapture, const SDL_AudioSpec *desired,
                                      SDL_AudioSpec *obtained, int allowed_changes) {
    (void) iscapture;
    (void) allowed_changes;
    opens++;
    snprintf(opened_name, sizeof(opened_name), "%s", device != NULL ? device : "(default)");
    *obtained = *desired;
    return 2;
}
void FakePauseAudioDevice(SDL_AudioDeviceID dev, int pause_on) {
    (void) dev;
    (void) pause_on;
}
void FakeCloseAudioDevice(SDL_AudioDeviceID dev) {
    (void) dev;
}
int FakeInitSubSystem(Uint32 flags) {
    (void) flags;
    hint_at_init = SDL_GetHint(SDL_HINT_AUDIODRIVER);
    return 0;
}
void FakeDelay(Uint32 ms) {
    (void) ms;
    delays++;
}

static void QuietLog(SS4S_LogLevel level, const char *tag, const char *fmt, ...) {
    (void) level;
    (void) tag;
    (void) fmt;
}

/* --- the tests ---------------------------------------------------------------------------------------------- */
static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static SS4S_LibraryContext context = {.Log = QuietLog};

static SS4S_AudioOpenResult open_output(const char *device) {
    context.Audio.device = device;
    lookups = opens = delays = 0;
    opened_name[0] = '\0';
    SS4S_AudioInfo info = {.codec = SS4S_AUDIO_PCM_S16LE, .sampleRate = 48000, .numOfChannels = 2,
            .samplesPerFrame = 240};
    SS4S_AudioInstance *instance = NULL;
    const SS4S_AudioOpenResult result = OpenInternal(&info, &instance, NULL, NULL, NULL);
    if (instance != NULL) {
        Close(instance);
    }
    return result;
}

int main(void) {
    fake_functions.GetObjectClass = GetObjectClassFake;
    fake_functions.DeleteLocalRef = DeleteLocalRefFake;
    fake_functions.GetStaticMethodID = GetStaticMethodIDFake;
    fake_functions.NewStringUTF = NewStringUTFFake;
    fake_functions.CallStaticIntMethod = CallStaticIntMethodFake;
    fake_functions.ExceptionCheck = ExceptionCheckFake;
    fake_functions.ExceptionClear = ExceptionClearFake;
    LibContext = &context;
    unsetenv("SDL_AUDIODRIVER");
    unsetenv("ECLIPSE_ANDROID_PEER");

    /* A chosen output starts SDL on AAudio (OpenSL ES plays only to the default); the default keeps SDL's own
     * choice; an SDL_AUDIODRIVER already set (the co-op peer's dummy) wins. */
    context.Audio.device = "HDMI: LG TV";
    Init(0, NULL);
    CHECK(hint_at_init != NULL && strcmp(hint_at_init, "AAudio,openslES") == 0);
    SDL_ClearHints();
    context.Audio.device = "default";
    Init(0, NULL);
    CHECK(hint_at_init == NULL);
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    context.Audio.device = "HDMI: LG TV";
    Init(0, NULL);
    CHECK(hint_at_init != NULL && strcmp(hint_at_init, "dummy") == 0);
    unsetenv("SDL_AUDIODRIVER");
    SDL_ClearHints();

    /* The stored name opens the output's current number. */
    CHECK(open_output("HDMI: LG TV") == SS4S_AUDIO_OPEN_OK);
    CHECK(strcmp(opened_name, "7") == 0 && opens == 1 && delays == 0);
    CHECK(strcmp(last_string, "HDMI: LG TV") == 0);

    /* Bluetooth reconnecting as the stream starts: waited for, then opened. */
    bluetooth_absent_for = 4;
    CHECK(open_output("Bluetooth: WH-1000XM5") == SS4S_AUDIO_OPEN_OK);
    CHECK(strcmp(opened_name, "12") == 0 && opens == 1 && delays == 3);

    /* Not connected at all: about 5 s of looking, then an error -- never the default instead. */
    CHECK(open_output("USB: gone") == SS4S_AUDIO_OPEN_ERROR);
    CHECK(opens == 0 && delays == 20);

    /* The default output never asks the activity. */
    CHECK(open_output("default") == SS4S_AUDIO_OPEN_OK);
    CHECK(strcmp(opened_name, "(default)") == 0 && lookups == 0);
    CHECK(open_output(NULL) == SS4S_AUDIO_OPEN_OK && strcmp(opened_name, "(default)") == 0);

    /* No Java VM to ask (the co-op peer): the default at once, no waiting. */
    setenv("ECLIPSE_ANDROID_PEER", "1", 1);
    CHECK(open_output("HDMI: LG TV") == SS4S_AUDIO_OPEN_OK);
    CHECK(strcmp(opened_name, "(default)") == 0 && lookups == 0 && delays == 0);
    unsetenv("ECLIPSE_ANDROID_PEER");

    CHECK(local_refs == 0);   /* every local reference released */
    if (failures == 0) {
        printf("sdl audio output (Android): OK\n");
    }
    return failures == 0 ? 0 : 1;
}
