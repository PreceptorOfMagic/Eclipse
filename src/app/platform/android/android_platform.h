/*
 * Android TV platform layer (NVIDIA Shield and other Android 9+ TV devices).
 *
 * THE SHAPE. Eclipse runs inside SDL2's Android activity (deploy/android, EclipseActivity extends
 * SDLActivity): Java owns the window, the video surface under it and a few system services, and calls
 * SDL_main() -- this program's main() -- on SDL's own thread. Everything else is the same C program as on the
 * TV and the desktops. Java publishes what the C side needs before SDL_main runs, as environment variables
 * (SDLActivity.nativeSetenv), because the co-op peer is a separate exec'd process with no JVM: it inherits
 * the environment and nothing else.
 */
#pragma once

#include <stdbool.h>

/* Environment published by EclipseActivity (deploy/android) before SDL_main. */
#define ANDROID_ENV_FILES_DIR "ECLIPSE_ANDROID_FILES_DIR"     /* Context.getFilesDir(): settings, pairing */
#define ANDROID_ENV_CACHE_DIR "ECLIPSE_ANDROID_CACHE_DIR"     /* Context.getCacheDir(): cover art, runtime */
#define ANDROID_ENV_ASSETS_DIR "ECLIPSE_ANDROID_ASSETS_DIR"   /* APK assets, unpacked once per version */
#define ANDROID_ENV_NATIVE_DIR "ECLIPSE_ANDROID_NATIVE_DIR"   /* ApplicationInfo.nativeLibraryDir */
#define ANDROID_ENV_SDK_INT "ECLIPSE_ANDROID_SDK_INT"         /* Build.VERSION.SDK_INT */
#define ANDROID_ENV_DEVICE "ECLIPSE_ANDROID_DEVICE"           /* Build.MANUFACTURER + " " + Build.MODEL */
#define ANDROID_ENV_GUIDE_IS_HOME "ECLIPSE_ANDROID_GUIDE_IS_HOME" /* "1": the system turns a pad's Guide into Home */

/* The co-op peer executable, packaged as lib/<abi>/libeclipse_peer.so (see CMakeLists.txt). */
#define ANDROID_PEER_EXECUTABLE "libeclipse_peer.so"

/*
 * TV REMOTE KEYS SDL2 DOES NOT MAP. SDL's Android keymap leaves the colour keys, the channel keys and the
 * info/guide/settings keys as SDL_SCANCODE_UNKNOWN, so EclipseActivity.dispatchKeyEvent() forwards them through
 * nativeTvKey() and they arrive as ordinary SDL_KEYDOWN/SDL_KEYUP events carrying these scancodes. They sit
 * far above SDL_NUM_SCANCODES so they can never collide with a real key, and they are pushed with
 * SDL_PushEvent, so SDL's own keyboard state array never sees them. The numbering after the base matches
 * EclipseActivity.TV_KEY_*.
 */
#define ANDROID_SCANCODE_TV_BASE 0x10000
#define ANDROID_SCANCODE_RED (ANDROID_SCANCODE_TV_BASE + 1)
#define ANDROID_SCANCODE_GREEN (ANDROID_SCANCODE_TV_BASE + 2)
#define ANDROID_SCANCODE_YELLOW (ANDROID_SCANCODE_TV_BASE + 3)
#define ANDROID_SCANCODE_BLUE (ANDROID_SCANCODE_TV_BASE + 4)
#define ANDROID_SCANCODE_CH_UP (ANDROID_SCANCODE_TV_BASE + 5)
#define ANDROID_SCANCODE_CH_DOWN (ANDROID_SCANCODE_TV_BASE + 6)
/* Info and guide: on webOS the EXIT key's job in a stream (open the overlay). */
#define ANDROID_SCANCODE_OVERLAY (ANDROID_SCANCODE_TV_BASE + 7)
/* A REMOTE's Menu key (a keyboard's stays SDL_SCANCODE_MENU, the host's Apps key): the UI's menu, and in a
 * stream the overlay. */
#define ANDROID_SCANCODE_MENU (ANDROID_SCANCODE_TV_BASE + 8)
#define ANDROID_SCANCODE_TV_LAST ANDROID_SCANCODE_MENU

/* Back held this long in a stream opens the overlay instead of sending Escape (as the remote OK key's hold
 * becomes a right click, session_keyboard.c). Android TV remotes have no EXIT key and many lack MENU. */
#define ANDROID_BACK_OVERLAY_HOLD_MS 600

/**
 * Called first in main(), before app_init: checks the environment EclipseActivity published, makes the
 * private directories, and points SS4S at its module list. A peer process does the same with the inherited
 * environment. Returns false if the app's storage is unusable.
 */
bool android_platform_init(bool peer);

/** Android API level of the device (Build.VERSION.SDK_INT), 0 if unknown. */
int android_sdk_int(void);

/** "NVIDIA SHIELD Android TV" and the like, for logs and the support report. */
const char *android_device_name(void);

/** True where the system takes a controller's Guide button for itself before any app sees it: the SHIELD TV,
 *  whose com.nvidia.shieldtech.hooks turns it into Home (EclipseActivity.guideIsHome). */
bool android_guide_is_home(void);

/**
 * The physical display: resolution of the current mode (not the 1080p UI surface Android TV gives an app),
 * the current refresh rate and the highest one any mode of that resolution offers (hundredths of a Hz; the
 * activity switches modes for a stream, see android_display_request_refresh), and whether the display
 * accepts HDR10. Any pointer may be NULL. False if unknown, and always in the co-op peer.
 */
bool android_display_mode(int *width, int *height, int *refresh_x100, int *max_refresh_x100, bool *hdr10);

/**
 * Ask Android for the display mode that best matches a stream at @p refresh_x100 (0 restores the system's
 * choice). The webOS build switches the panel through SDL_webOS; on Android the activity's
 * preferredDisplayModeId (and Surface.setFrameRate on Android 11+) does it.
 */
void android_display_request_refresh(int refresh_x100);

/** Show the TV's home screen, as the Home key does (the controller's View + Start, session_gamepad.c). */
void android_go_home(void);

/** Which of @p keycodes the controller with this vendor and product (or this name, when it reports neither) can
 *  produce on this Android version, from its key layout (EclipseActivity.controllerKeys). false in the co-op peer
 *  or when no such controller is connected. */
bool android_controller_keys(int vendor, int product, const char *name, const int *keycodes, int count, bool *has);

/**
 * The audio outputs a stream can play to, by the names the audio setting stores ("HDMI: LG TV", see
 * deploy/android/.../AudioOutputs.java): at most @p max, each malloc'd for the caller. 0 in the co-op peer.
 * The SS4S SDL module turns the stored name into the output's current number when a stream opens its sound.
 */
int android_audio_output_names(char **names, int max);

/** The current number of the output called @p name (SDL's name for it is that number); 0 if not connected. */
int android_audio_output_id(const char *name);

/** The "id|name" lines of EclipseActivity.getAudioOutputs() -> names, as above (audio_outputs_android.c). */
int android_audio_outputs_parse(const char *text, char **names, int max);

/**
 * The display's refresh rates (hundredths of a Hz) at its current resolution, at most @p max. The primary asks
 * the activity and leaves the list in the environment for the co-op peer, which has no Java VM to ask.
 */
int android_display_refresh_rates(int *rates_x100, int max);

/**
 * The clientRefreshRateX100 a stream at @p fps should ask its host for, given the display's @p rates_x100:
 * @p requested_x100 when the user chose an exact rate; the NTSC rate (60 -> 5994) when the display has no mode
 * that shows @p fps evenly but has one for the NTSC rate, as the webOS build asks for its panel's; else 0 (the
 * whole frame rate). display_android.c.
 */
int android_stream_refresh_x100(int fps, int requested_x100, const int *rates_x100, int count);

/** "5994,6000" -> rates, at most @p max (the peer's copy of the display's rates). display_android.c. */
int android_parse_rates(const char *text, int *rates_x100, int max);

/** EclipseActivity.describeInputDevices(): "name|sources (hex)|SDL joystick 0/1" per line, malloc'd; NULL in the
 *  co-op peer. A TV remote (a keyboard with a d-pad) must not be a joystick to SDL. */
char *android_input_devices(void);

#if defined(ECLIPSE_ANDROID_SELFTEST)
/* TEST BUILDS ONLY: the emulator smoke tests (selftest_android.c). */
void android_selftest_parse(int argc, char *argv[]);
void android_selftest_start(void);
#endif
