#pragma once

#include <stdbool.h>

#include "uuidstr.h"

typedef struct app_t app_t;

/**
 * PIN auto-entry over the stream after a Wake-on-LAN.
 *
 * Flow: the user presses Wake on an offline host -> pin_autoentry_arm(uuid)
 * records which host was woken -> when the following stream opens for that same
 * host, pin_autoentry_on_stream_open() runs the login sequence once (dismiss the
 * Windows lock screen, type the PIN, optionally submit) and disarms. It is tied
 * to the Wake press and the host UUID so it never types a PIN into an
 * already-logged-in session / game, or into the wrong PC.
 *
 * The PIN is per-host (pclist_t.pin_code, from hosts.ini), falling back to the
 * global app_configuration->pin_code if a host has none. Timing/behaviour
 * (pin_predelay_ms, pin_dismiss, pin_dismiss_ms, pin_submit, ...) stays global.
 */

/** Arm auto-entry for the given host (call on Wake). Records the woken host UUID.
 *
 * SEVERAL HOSTS CAN BE ARMED AT ONCE. A co-op launch may have to wake BOTH PCs, and
 * a single armed slot meant the second wake overwrote the first -- one machine would
 * log in and the other would sit on its lock screen streaming a black panel. */
void pin_autoentry_arm(const uuidstr_t *uuid);

/** Mark an already-armed host as one that was genuinely OFFLINE and got a magic packet.
 *
 * Arming is unconditional -- a host that is already up may still be sitting on a lock
 * screen -- so the arm alone no longer says whether the machine was asleep. This does,
 * and it is what lets the login gate act on a host whose Apollo is too old to report
 * `SessionLocked`: nobody is playing a game on a PC that was not answering the network
 * when the launch started. Without this the only safe answer to silence is to skip. */
void pin_autoentry_mark_woken(const uuidstr_t *uuid);

/** Was this host marked woken? Does NOT disarm. */
bool pin_autoentry_was_woken(const uuidstr_t *uuid);

/**
 * If armed for this host, schedule the login sequence and disarm (call when a
 * stream opens). No-op if the opened stream is not an armed host.
 */
void pin_autoentry_on_stream_open(app_t *global, const uuidstr_t *uuid);

/**
 * Was this host armed? Answers and DISARMS, like pin_autoentry_on_stream_open does
 * for the host it fires on.
 *
 * For the co-op secondary, which is a separate headless process with its own globals
 * and would otherwise never learn its host had just been woken. It has to consume the
 * arm, not merely read it: an arm left behind would make the NEXT co-op launch type a
 * PIN into a machine that was already logged in -- i.e. into whatever game is on
 * screen.
 */
bool pin_autoentry_take(const uuidstr_t *uuid);
