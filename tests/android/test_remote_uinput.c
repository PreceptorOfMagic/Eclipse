/* A TV remote for the device smoke tests (tests/android/device_smoke.sh), packaged in the self-test APK only.
 *
 * It creates a uinput device with a remote's keys -- the d-pad, its centre and Back -- which Android classifies
 * as SOURCE_KEYBOARD | SOURCE_DPAD, as it does a SHIELD or TV remote (Generic.kl: KEY_SELECT is DPAD_CENTER).
 * That is the device scripts/android/patches/sdl2-2.32.10-android-tv-remote.patch is about: it must be a
 * keyboard to SDL, never a joystick. The emulator's own key injection (adb emu event send) reached no input
 * device on emulator 37.2 (2026-10-15), so the smoke test brings its own remote.
 *
 *   libeclipse_test_remote.so <seconds>
 *
 * Run as adb's shell user (group uhid opens /dev/uinput). On SIGUSR1 it presses Up, then Back; it removes the
 * device after <seconds> or on SIGTERM.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t press_requested = 0;
static volatile sig_atomic_t stop_requested = 0;

static void on_usr1(int sig) {
    (void) sig;
    press_requested = 1;
}

static void on_term(int sig) {
    (void) sig;
    stop_requested = 1;
}

static int emit(int fd, int type, int code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = (unsigned short) type;
    ev.code = (unsigned short) code;
    ev.value = value;
    return write(fd, &ev, sizeof(ev)) == (ssize_t) sizeof(ev) ? 0 : -1;
}

static int press(int fd, int code) {
    if (emit(fd, EV_KEY, code, 1) != 0 || emit(fd, EV_SYN, SYN_REPORT, 0) != 0) {
        return -1;
    }
    usleep(80000);
    if (emit(fd, EV_KEY, code, 0) != 0 || emit(fd, EV_SYN, SYN_REPORT, 0) != 0) {
        return -1;
    }
    usleep(300000);
    return 0;
}

int main(int argc, char *argv[]) {
    const int seconds = argc > 1 ? atoi(argv[1]) : 60;
    const int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "test remote: /dev/uinput: %s\n", strerror(errno));
        return 1;
    }
    static const int keys[] = {KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_SELECT, KEY_BACK};
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) != 0) {
        fprintf(stderr, "test remote: UI_SET_EVBIT: %s\n", strerror(errno));
        return 1;
    }
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (ioctl(fd, UI_SET_KEYBIT, keys[i]) != 0) {
            fprintf(stderr, "test remote: UI_SET_KEYBIT %d: %s\n", keys[i], strerror(errno));
            return 1;
        }
    }
    /* The legacy set-up (struct uinput_user_dev), which every kernel a supported device runs accepts. */
    struct uinput_user_dev dev;
    memset(&dev, 0, sizeof(dev));
    snprintf(dev.name, sizeof(dev.name), "Eclipse test remote");
    dev.id.bustype = BUS_VIRTUAL;
    dev.id.vendor = 0x1209;
    dev.id.product = 0xec1e;
    dev.id.version = 1;
    if (write(fd, &dev, sizeof(dev)) != (ssize_t) sizeof(dev) || ioctl(fd, UI_DEV_CREATE) != 0) {
        fprintf(stderr, "test remote: create: %s\n", strerror(errno));
        return 1;
    }
    signal(SIGUSR1, on_usr1);
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGHUP, on_term);
    printf("test remote: created\n");
    fflush(stdout);

    const time_t end = time(NULL) + seconds;
    int status = 0;
    while (!stop_requested && time(NULL) < end) {
        if (press_requested) {
            press_requested = 0;
            if (press(fd, KEY_UP) != 0 || press(fd, KEY_BACK) != 0) {
                fprintf(stderr, "test remote: write: %s\n", strerror(errno));
                status = 1;
                break;
            }
            printf("test remote: pressed Up, Back\n");
            fflush(stdout);
        }
        usleep(50000);
    }
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return status;
}
