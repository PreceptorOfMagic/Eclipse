#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <SDL.h>

/**
 * The 2026 Steam Controller ("Triton") on its wireless receiver puck (Valve 28DE:1304 "Proteus",
 * 28DE:1305 "Nereid"), presented to the rest of Eclipse as ordinary SDL game controllers.
 *
 * WHY THIS EXISTS: Eclipse links SDL2, and SDL2 does not know these devices (no 0x1304 in its
 * usb_ids.h, release-2.32.10 and the SDL2 branch). Windows sees each puck slot as a KEYBOARD and a
 * MOUSE ("lizard mode"), so without this the controller types and clicks instead of being a
 * gamepad. SDL3 has a driver for it; steam_triton.c is a port of that driver onto SDL2's public
 * HID API (SDL_hid_*) and virtual joysticks (SDL_JoystickAttachVirtualEx). Every connected slot
 * becomes one virtual game controller, so input_gamepad.c and the stream need no changes.
 *
 * Threads: one reader thread per puck slot (blocking reads, lowest latency); a watcher thread that
 * finds pucks (HID enumeration is slow on Windows, so it never runs on the UI thread); the UI
 * thread only attaches and detaches the virtual joysticks in steam_triton_pump().
 *
 * Setting: `steam_controller` (app_settings.h), default on. Off = nothing is opened, lizard mode
 * is left alone, and Windows' keyboard/mouse behaviour is untouched.
 */

/** Start the watcher. Call after SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER. */
void steam_triton_init(void);

/** UI thread, every loop: attach/detach virtual controllers for slots that (dis)connected. Cheap. */
void steam_triton_pump(void);

/** Stop every thread, detach every virtual controller, close every device. */
void steam_triton_deinit(void);

/** Is @p id one of our virtual Steam Controllers? Then copy its slot's HID device path (the vendor
 *  collection Steam itself reads) into @p out -- what pad ownership hides from this PC. UI thread. */
bool steam_triton_device_path(SDL_JoystickID id, char *out, size_t out_len);
