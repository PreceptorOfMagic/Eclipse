/*
 * steam_triton.c -- the 2026 Steam Controller ("Triton") and its puck as SDL2 game controllers.
 * See steam_triton.h for why this exists and how the threads are split.
 *
 * PORTED FROM SDL3: src/joystick/hidapi/SDL_hidapi_steam_triton.c and the Triton parts of
 * src/joystick/hidapi/steam/controller_structs.h / controller_constants.h (libsdl-org/SDL main,
 * d6fbbbe, 2026-11-02). This is an ALTERED version of that software: the report layouts, the
 * button bits, the lizard-mode feature report, the rumble output report and the slot rules are
 * SDL's; the threading, the SDL2 virtual-joystick output and the button mapping onto SDL2's
 * game-controller buttons are ours. SDL's notice, which this file must keep:
 *
 *   Simple DirectMedia Layer
 *   Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>
 *
 *   This software is provided 'as-is', without any express or implied
 *   warranty.  In no event will the authors be held liable for any damages
 *   arising from the use of this software.
 *
 *   Permission is granted to anyone to use this software for any purpose,
 *   including commercial applications, and to alter it and redistribute it
 *   freely, subject to the following restrictions:
 *
 *   1. The origin of this software must not be misrepresented; you must not
 *      claim that you wrote the original software. If you use this software
 *      in a product, an acknowledgment in the product documentation would be
 *      appreciated but is not required.
 *   2. Altered source versions must be plainly marked as such, and must not be
 *      misrepresented as being the original software.
 *   3. This notice may not be removed or altered from any source distribution.
 */
#include "steam_triton.h"

#include <SDL.h>

#ifdef STEAM_TRITON_PROBE
#include "steam_triton_probe_shim.h"   /* scripts/padtest/tritonprobe: the driver alone, no app */
#else
#include "app.h"
#include "logging.h"
#endif

/* SDL2's HID API arrived in 2.0.18 and virtual-joystick descriptors in 2.24. The TV reaches the
 * Steam Controller through the Pi, and Android has its own input path: neither builds this. */
#if SDL_VERSION_ATLEAST(2, 24, 0) && !defined(TARGET_WEBOS) && !defined(__ANDROID__)
#define STEAM_TRITON_ENABLED 1
#endif

#ifndef STEAM_TRITON_ENABLED

void steam_triton_init(void) {}
void steam_triton_pump(void) {}
void steam_triton_deinit(void) {}
bool steam_triton_device_path(SDL_JoystickID id, char *out, size_t out_len) {
    (void) id; (void) out; (void) out_len;
    return false;
}

#else

#include <stdint.h>
#include <string.h>

#define VALVE_VID 0x28DE
#define TRITON_PID_WIRED 0x1302     /* SDL3 controller_list.h: "Valve Steam Triton Controller" */
#define TRITON_PID_BLE 0x1303       /* "... (BLE)" */
#define TRITON_PID_PROTEUS 0x1304   /* "Valve Steam Proteus Dongle" -- the puck seen on SKYNET 2026-11-03 */
#define TRITON_PID_NEREID 0x1305    /* "Valve Steam Nereid Dongle" */

#define TRITON_MAX_SLOTS 8
#define TRITON_ENUM_INTERVAL_MS 2000
#define TRITON_READ_TIMEOUT_MS 10
#define TRITON_LIZARD_INTERVAL_MS 3000   /* SDL3: lizard mode is re-disabled every 3 s */
#define TRITON_RUMBLE_RESEND_MS 40       /* SDL3: the controller's own rumble safety timeout is ~50 ms */

/* ---- report layouts, verbatim from SDL3 steam/controller_structs.h (packed) ---- */
#pragma pack(push, 1)
typedef struct {
    unsigned char type;
    unsigned char length;
} TritonFeatureHeader;
typedef struct {
    unsigned char settingNum;
    unsigned short settingValue;
} TritonControllerSetting;
typedef struct {
    uint8_t seq_num;
    uint32_t buttons;
    short sTriggerLeft;
    short sTriggerRight;
    short sLeftStickX;
    short sLeftStickY;
    short sRightStickX;
    short sRightStickY;
    /* touchpads and IMU follow; not used (SDL2 virtual joysticks carry neither) */
} TritonStatePrefix;
typedef struct {
    unsigned char state;
} TritonWirelessStatus;
typedef struct {
    uint8_t report_id;
    uint8_t type;
    uint16_t intensity;
    uint16_t left_speed;
    int8_t left_gain;
    uint16_t right_speed;
    int8_t right_gain;
} TritonRumbleReport;
#pragma pack(pop)

#define HID_FEATURE_REPORT_BYTES 64
#define ID_SET_SETTINGS_VALUES 0x87          /* controller_constants.h */
#define SETTING_LIZARD_MODE 9                /* ControllerSettings: 10th entry, "never change the order" */
#define LIZARD_MODE_OFF 0
#define ID_OUT_REPORT_HAPTIC_RUMBLE 0x80
#define ID_TRITON_CONTROLLER_STATE 0x42
#define ID_TRITON_CONTROLLER_STATE_BLE 0x45
#define ID_TRITON_WIRELESS_STATUS_X 0x46
#define ID_TRITON_CONTROLLER_STATE_TIMESTAMP 0x47
#define ID_TRITON_WIRELESS_STATUS 0x79
#define TRITON_WIRELESS_DISCONNECT 1
#define TRITON_WIRELESS_CONNECT 2
/* The two state layouts SDL3 parses (TritonMTUNoQuat_t 0x42/0x45, TritonMTUNoQuat32TS_t 0x47) share
 * this prefix; SDL3 itself reads both through the first layout's generic-state routine. Minimum
 * lengths are SDL3's sizeof() checks, report id included: 1 + sizeof(TritonMTUNoQuat_t) = 1 + 45, and
 * 1 + sizeof(TritonMTUNoQuat32TS_t) = 1 + 45 (2-byte trackpad timestamp in, 2 bytes of IMU timestamp out). */
#define TRITON_STATE_MIN_BYTES 46
#define TRITON_STATE_TS_MIN_BYTES 46

/* SDL3 TritonButtons */
enum {
    TB_A = 0x00000001, TB_B = 0x00000002, TB_X = 0x00000004, TB_Y = 0x00000008,
    TB_QAM = 0x00000010, TB_R3 = 0x00000020, TB_VIEW = 0x00000040, TB_R4 = 0x00000080,
    TB_R5 = 0x00000100, TB_R = 0x00000200, TB_DPAD_DOWN = 0x00000400, TB_DPAD_RIGHT = 0x00000800,
    TB_DPAD_LEFT = 0x00001000, TB_DPAD_UP = 0x00002000, TB_MENU = 0x00004000, TB_L3 = 0x00008000,
    TB_STEAM = 0x00010000, TB_L4 = 0x00020000, TB_L5 = 0x00040000, TB_L = 0x00080000,
};

/* Bit -> SDL2 game-controller button. Back/Start follow SDL3's Triton driver (BACK <- MENU bit,
 * START <- VIEW bit), the opposite of SDL2's Steam Deck driver -- MEASURED right on the real
 * controller 2026-11-03: the physical View button gives `back`, Menu gives `start`. Paddles follow SDL2's Steam Deck driver
 * (R4 P1, L4 P2, R5 P3, L5 P4); QAM is MISC1 as on the Deck. Touchpad clicks have no slot:
 * SDL2's virtual mapping has no touchpad button. */
static const struct {
    uint32_t bit;
    SDL_GameControllerButton button;
} button_map[] = {
    {TB_A, SDL_CONTROLLER_BUTTON_A},
    {TB_B, SDL_CONTROLLER_BUTTON_B},
    {TB_X, SDL_CONTROLLER_BUTTON_X},
    {TB_Y, SDL_CONTROLLER_BUTTON_Y},
    {TB_MENU, SDL_CONTROLLER_BUTTON_BACK},
    {TB_STEAM, SDL_CONTROLLER_BUTTON_GUIDE},
    {TB_VIEW, SDL_CONTROLLER_BUTTON_START},
    {TB_L3, SDL_CONTROLLER_BUTTON_LEFTSTICK},
    {TB_R3, SDL_CONTROLLER_BUTTON_RIGHTSTICK},
    {TB_L, SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
    {TB_R, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
    {TB_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_UP},
    {TB_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_DOWN},
    {TB_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_LEFT},
    {TB_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
    {TB_QAM, SDL_CONTROLLER_BUTTON_MISC1},
    {TB_R4, SDL_CONTROLLER_BUTTON_PADDLE1},
    {TB_L4, SDL_CONTROLLER_BUTTON_PADDLE2},
    {TB_R5, SDL_CONTROLLER_BUTTON_PADDLE3},
    {TB_L5, SDL_CONTROLLER_BUTTON_PADDLE4},
};
/* SDL2's virtual mapping numbers buttons in SDL_GameControllerButton order over the set bits of
 * button_mask; with A..PADDLE4 all set (0..19), joystick button i IS SDL_GameControllerButton i. */
#define TRITON_NUM_BUTTONS (SDL_CONTROLLER_BUTTON_PADDLE4 + 1)
#define TRITON_NUM_AXES SDL_CONTROLLER_AXIS_MAX

typedef struct triton_slot {
    bool used;
    char *path;
    uint16_t product_id;
    int interface_number;
    SDL_hid_device *dev;
    SDL_Thread *thread;
    SDL_atomic_t stop;
    SDL_atomic_t connected;   /* reader: wireless status / state reports */
    SDL_atomic_t failed;      /* reader: read error -> watcher/pump retire the slot */
    SDL_atomic_t rumble;      /* (low << 16) | high, written by the Rumble callback */
    SDL_mutex *joy_lock;      /* guards `joystick` between the pump (attach/detach) and the reader */
    SDL_Joystick *joystick;   /* our own handle on the slot's virtual joystick, NULL = not attached */
    uint32_t last_buttons;
} triton_slot_t;

static triton_slot_t slots[TRITON_MAX_SLOTS];
static SDL_mutex *slots_lock;   /* slot creation/retirement (watcher) vs iteration (pump) */
static SDL_Thread *watcher;
static SDL_atomic_t watcher_stop;
static bool started;

static bool is_triton(uint16_t pid) {
    return pid == TRITON_PID_WIRED || pid == TRITON_PID_BLE || pid == TRITON_PID_PROTEUS || pid == TRITON_PID_NEREID;
}

static bool is_puck(uint16_t pid) {
    return pid == TRITON_PID_PROTEUS || pid == TRITON_PID_NEREID;
}

static bool feature_enabled(void) {
    return app_configuration == NULL || app_configuration->steam_controller;
}

static void disable_lizard_mode(SDL_hid_device *dev) {
    Uint8 buffer[HID_FEATURE_REPORT_BYTES + 1];
    memset(buffer, 0, sizeof(buffer));
    /* SDL3 sends { report 1, header, one setting } in a HID_FEATURE_REPORT_BYTES buffer whose first
     * byte is the report number. */
    buffer[0] = 1;
    TritonFeatureHeader *hdr = (TritonFeatureHeader *) (buffer + 1);
    hdr->type = ID_SET_SETTINGS_VALUES;
    hdr->length = sizeof(TritonControllerSetting);
    TritonControllerSetting *setting = (TritonControllerSetting *) (buffer + 1 + sizeof(*hdr));
    setting->settingNum = SETTING_LIZARD_MODE;
    setting->settingValue = LIZARD_MODE_OFF;
    SDL_hid_send_feature_report(dev, buffer, HID_FEATURE_REPORT_BYTES);
}

static void send_rumble(SDL_hid_device *dev, uint16_t low, uint16_t high) {
    TritonRumbleReport r;
    memset(&r, 0, sizeof(r));
    r.report_id = ID_OUT_REPORT_HAPTIC_RUMBLE;
    r.left_speed = low;
    r.right_speed = high;
    SDL_hid_write(dev, (const unsigned char *) &r, sizeof(r));
}

static Sint16 clamp16(int v) {
    return (Sint16) (v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

static void apply_state(triton_slot_t *slot, const TritonStatePrefix *s) {
    SDL_LockMutex(slot->joy_lock);
    SDL_Joystick *joy = slot->joystick;
    if (joy != NULL) {
        if (s->buttons != slot->last_buttons) {
            for (size_t i = 0; i < SDL_arraysize(button_map); i++) {
                SDL_JoystickSetVirtualButton(joy, button_map[i].button,
                                             (s->buttons & button_map[i].bit) ? SDL_PRESSED : SDL_RELEASED);
            }
            slot->last_buttons = s->buttons;
        }
        /* Same scaling as SDL3: triggers as trigger*2 - 32768, Y axes inverted. */
        SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_TRIGGERLEFT, clamp16((int) s->sTriggerLeft * 2 - 32768));
        SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, clamp16((int) s->sTriggerRight * 2 - 32768));
        SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_LEFTX, s->sLeftStickX);
        SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_LEFTY, clamp16(-(int) s->sLeftStickY));
        SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_RIGHTX, s->sRightStickX);
        SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_RIGHTY, clamp16(-(int) s->sRightStickY));
    }
    SDL_UnlockMutex(slot->joy_lock);
}

static int SDLCALL reader_main(void *data) {
    triton_slot_t *slot = data;
    Uint32 last_lizard = 0, last_rumble_sent = 0;
    int last_rumble = 0;
    uint8_t buf[64];
    if (!is_puck(slot->product_id)) {
        SDL_AtomicSet(&slot->connected, 1);   /* SDL3: a wired/BLE controller is connected when opened */
    }
    while (!SDL_AtomicGet(&slot->stop)) {
        const Uint32 now = SDL_GetTicks();
        if (SDL_AtomicGet(&slot->connected)) {
            if (last_lizard == 0 || now - last_lizard >= TRITON_LIZARD_INTERVAL_MS) {
                disable_lizard_mode(slot->dev);
                last_lizard = now ? now : 1;
            }
            const int rumble = SDL_AtomicGet(&slot->rumble);
            if (rumble != last_rumble || (rumble != 0 && now - last_rumble_sent >= TRITON_RUMBLE_RESEND_MS)) {
                send_rumble(slot->dev, (uint16_t) ((unsigned) rumble >> 16), (uint16_t) (rumble & 0xFFFF));
                last_rumble = rumble;
                last_rumble_sent = now;
            }
        }
        const int r = SDL_hid_read_timeout(slot->dev, buf, sizeof(buf), TRITON_READ_TIMEOUT_MS);
        if (r == 0) {
            continue;
        }
        if (r < 0) {
            commons_log_warn("SteamTriton", "slot if%d: read failed, closing it", slot->interface_number);
            SDL_AtomicSet(&slot->connected, 0);
            SDL_AtomicSet(&slot->failed, 1);
            break;
        }
        switch (buf[0]) {
            case ID_TRITON_CONTROLLER_STATE:
            case ID_TRITON_CONTROLLER_STATE_BLE:
                SDL_AtomicSet(&slot->connected, 1);
                if (r >= TRITON_STATE_MIN_BYTES) {
                    apply_state(slot, (const TritonStatePrefix *) &buf[1]);
                }
                break;
            case ID_TRITON_CONTROLLER_STATE_TIMESTAMP:
                SDL_AtomicSet(&slot->connected, 1);
                if (r >= TRITON_STATE_TS_MIN_BYTES) {
                    apply_state(slot, (const TritonStatePrefix *) &buf[1]);
                }
                break;
            case ID_TRITON_WIRELESS_STATUS_X:
            case ID_TRITON_WIRELESS_STATUS:
                if (r >= 2) {
                    const TritonWirelessStatus *ws = (const TritonWirelessStatus *) &buf[1];
                    if (ws->state == TRITON_WIRELESS_CONNECT) {
                        SDL_AtomicSet(&slot->connected, 1);
                    } else if (ws->state == TRITON_WIRELESS_DISCONNECT) {
                        SDL_AtomicSet(&slot->connected, 0);
                    }
                }
                break;
            default:
                break;
        }
    }
    return 0;
}

static int SDLCALL virtual_rumble(void *userdata, Uint16 low, Uint16 high) {
    triton_slot_t *slot = userdata;
    SDL_AtomicSet(&slot->rumble, (int) (((Uint32) low << 16) | high));
    return 0;
}

static void detach(triton_slot_t *slot) {
    SDL_LockMutex(slot->joy_lock);
    SDL_Joystick *joy = slot->joystick;
    slot->joystick = NULL;
    SDL_UnlockMutex(slot->joy_lock);
    if (joy == NULL) {
        return;
    }
    const SDL_JoystickID id = SDL_JoystickInstanceID(joy);
    SDL_JoystickClose(joy);
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_JoystickGetDeviceInstanceID(i) == id) {
            SDL_JoystickDetachVirtual(i);
            break;
        }
    }
    commons_log_info("SteamTriton", "slot if%d: controller gone, virtual controller detached", slot->interface_number);
}

static void attach(triton_slot_t *slot) {
    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = TRITON_NUM_AXES;
    desc.nbuttons = TRITON_NUM_BUTTONS;
    desc.vendor_id = VALVE_VID;
    desc.product_id = slot->product_id;
    desc.button_mask = (1u << TRITON_NUM_BUTTONS) - 1;
    desc.axis_mask = (1u << TRITON_NUM_AXES) - 1;
    desc.name = "Steam Controller";
    desc.userdata = slot;
    desc.Rumble = virtual_rumble;
    const int index = SDL_JoystickAttachVirtualEx(&desc);
    if (index < 0) {
        commons_log_warn("SteamTriton", "slot if%d: attach failed: %s", slot->interface_number, SDL_GetError());
        return;
    }
    SDL_Joystick *joy = SDL_JoystickOpen(index);
    if (joy == NULL) {
        commons_log_warn("SteamTriton", "slot if%d: open failed: %s", slot->interface_number, SDL_GetError());
        SDL_JoystickDetachVirtual(index);
        return;
    }
    /* Resting state before the first report: sticks centred, triggers released. */
    SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
    SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
    SDL_LockMutex(slot->joy_lock);
    slot->last_buttons = 0xFFFFFFFFu;   /* force the first report to set every button */
    slot->joystick = joy;
    SDL_UnlockMutex(slot->joy_lock);
    commons_log_info("SteamTriton", "slot if%d (28DE:%04X): controller connected, virtual controller %d attached",
                     slot->interface_number, slot->product_id, (int) SDL_JoystickInstanceID(joy));
}

/* Watcher thread only: stop the reader and free everything but the attachment (the pump owns that). */
static void retire_locked(triton_slot_t *slot) {
    SDL_AtomicSet(&slot->stop, 1);
    if (slot->thread != NULL) {
        SDL_WaitThread(slot->thread, NULL);
        slot->thread = NULL;
    }
    if (slot->dev != NULL) {
        SDL_hid_close(slot->dev);
        slot->dev = NULL;
    }
}

static bool path_known_locked(const char *path) {
    for (int i = 0; i < TRITON_MAX_SLOTS; i++) {
        if (slots[i].used && slots[i].path != NULL && strcmp(slots[i].path, path) == 0) {
            return true;
        }
    }
    return false;
}

/* Which HID entries are controller slots. Puck: interfaces 2..5 (SDL3's rule). On Windows each
 * interface splits into mouse / keyboard / vendor collections and only the vendor one (usage page
 * 0xFF00 and up) carries the protocol; Linux hidraw gives one node per interface. */
static bool wanted(const SDL_hid_device_info *d) {
    if (d->vendor_id != VALVE_VID || !is_triton(d->product_id)) {
        return false;
    }
    if (is_puck(d->product_id) && (d->interface_number < 2 || d->interface_number > 5)) {
        return false;
    }
#ifdef _WIN32
    return d->usage_page >= 0xFF00;
#else
    return true;
#endif
}

static void open_new_slots(void) {
    SDL_hid_device_info *list = SDL_hid_enumerate(VALVE_VID, 0);
    for (SDL_hid_device_info *d = list; d != NULL; d = d->next) {
        if (!wanted(d) || d->path == NULL) {
            continue;
        }
        SDL_LockMutex(slots_lock);
        if (!path_known_locked(d->path)) {
            int free_index = -1;
            for (int i = 0; i < TRITON_MAX_SLOTS; i++) {
                if (!slots[i].used) {
                    free_index = i;
                    break;
                }
            }
            SDL_hid_device *dev = free_index >= 0 ? SDL_hid_open_path(d->path, 0) : NULL;
            if (dev != NULL) {
                triton_slot_t *slot = &slots[free_index];
                SDL_mutex *keep = slot->joy_lock;
                SDL_zerop(slot);
                slot->joy_lock = keep != NULL ? keep : SDL_CreateMutex();
                slot->used = true;
                slot->path = SDL_strdup(d->path);
                slot->product_id = d->product_id;
                slot->interface_number = d->interface_number;
                slot->dev = dev;
                slot->thread = SDL_CreateThread(reader_main, "SteamTriton", slot);
                commons_log_info("SteamTriton", "opened 28DE:%04X if%d", d->product_id, d->interface_number);
            }
        }
        SDL_UnlockMutex(slots_lock);
    }
    SDL_hid_free_enumeration(list);
}

static int SDLCALL watcher_main(void *data) {
    (void) data;
    Uint32 last_enum = 0;
    while (!SDL_AtomicGet(&watcher_stop)) {
        const Uint32 now = SDL_GetTicks();
        if (feature_enabled() && (last_enum == 0 || now - last_enum >= TRITON_ENUM_INTERVAL_MS)) {
            open_new_slots();
            last_enum = now ? now : 1;
        }
        /* Slots whose reader failed (puck unplugged) or that the setting turned off: stop the reader,
         * close the device. The pump detaches the virtual controller when it sees `connected` = 0. */
        SDL_LockMutex(slots_lock);
        for (int i = 0; i < TRITON_MAX_SLOTS; i++) {
            triton_slot_t *slot = &slots[i];
            if (slot->used && slot->dev != NULL && (SDL_AtomicGet(&slot->failed) || !feature_enabled())) {
                SDL_AtomicSet(&slot->connected, 0);
                retire_locked(slot);
            }
        }
        SDL_UnlockMutex(slots_lock);
        SDL_Delay(100);
    }
    return 0;
}

void steam_triton_init(void) {
    if (started) {
        return;
    }
    if (SDL_hid_init() != 0) {
        commons_log_warn("SteamTriton", "SDL_hid_init failed: %s", SDL_GetError());
        return;
    }
    slots_lock = SDL_CreateMutex();
    SDL_AtomicSet(&watcher_stop, 0);
    watcher = SDL_CreateThread(watcher_main, "SteamTritonWatch", NULL);
    started = watcher != NULL;
}

void steam_triton_pump(void) {
    if (!started) {
        return;
    }
    /* Never wait on the UI thread: the watcher holds this lock while it opens a device or stops a
     * reader (tens of ms on Windows). Skipped loops just attach a moment later. */
    if (SDL_TryLockMutex(slots_lock) != 0) {
        return;
    }
    for (int i = 0; i < TRITON_MAX_SLOTS; i++) {
        triton_slot_t *slot = &slots[i];
        if (!slot->used) {
            continue;
        }
        const bool connected = SDL_AtomicGet(&slot->connected) != 0 && slot->dev != NULL;
        if (connected && slot->joystick == NULL) {
            attach(slot);
        } else if (!connected && slot->joystick != NULL) {
            detach(slot);
        }
        /* Fully retired (device closed by the watcher, controller detached): free the slot so a
         * re-plugged puck is opened again. */
        if (slot->dev == NULL && slot->joystick == NULL) {
            SDL_free(slot->path);
            slot->path = NULL;
            slot->used = false;
        }
    }
    SDL_UnlockMutex(slots_lock);
}

bool steam_triton_device_path(SDL_JoystickID id, char *out, size_t out_len) {
    if (!started || out == NULL || out_len == 0) {
        return false;
    }
    bool found = false;
    SDL_LockMutex(slots_lock);
    for (int i = 0; i < TRITON_MAX_SLOTS && !found; i++) {
        triton_slot_t *slot = &slots[i];
        if (slot->used && slot->joystick != NULL && slot->path != NULL &&
            SDL_JoystickInstanceID(slot->joystick) == id) {
            SDL_strlcpy(out, slot->path, out_len);
            found = true;
        }
    }
    SDL_UnlockMutex(slots_lock);
    return found;
}

void steam_triton_deinit(void) {
    if (!started) {
        return;
    }
    SDL_AtomicSet(&watcher_stop, 1);
    SDL_WaitThread(watcher, NULL);
    watcher = NULL;
    SDL_LockMutex(slots_lock);
    for (int i = 0; i < TRITON_MAX_SLOTS; i++) {
        triton_slot_t *slot = &slots[i];
        if (slot->used) {
            retire_locked(slot);
            detach(slot);
            SDL_free(slot->path);
            slot->path = NULL;
            slot->used = false;
        }
        if (slot->joy_lock != NULL) {
            SDL_DestroyMutex(slot->joy_lock);
            slot->joy_lock = NULL;
        }
    }
    SDL_UnlockMutex(slots_lock);
    SDL_DestroyMutex(slots_lock);
    slots_lock = NULL;
    SDL_hid_exit();
    started = false;
}

#endif /* STEAM_TRITON_ENABLED */
