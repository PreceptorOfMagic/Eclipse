/*
 * Co-op split-screen: the secondary session's video sink.
 *
 * Eclipse launched in secondary mode runs a second moonlight session (its own
 * process, because moonlight-common-c is a singleton) and installs these
 * callbacks instead of the SS4S decoder ones. They decode nothing, open no
 * player, touch no surface and claim no media pipeline -- they only reassemble
 * each access unit and hand it to the coordinator over coop_ipc.
 *
 * That is deliberate: a process with no pipeline and no surface is the one
 * shape that never failed in any TV experiment.
 */
#pragma once

#include <stdbool.h>

/**
 * Install the forwarding callbacks and connect to the coordinator's socket.
 * Returns false if the socket cannot be reached, in which case the caller
 * should abort the secondary session rather than start a pointless stream.
 */
bool coop_secondary_init(const char *socket_path);

/** Tear down the connection. Safe to call if init failed. */
void coop_secondary_shutdown(void);

/** Decoder callbacks that forward instead of decoding.
 * Limelight.h typedefs this without a struct tag, so include it before use. */
#include <Limelight.h>
extern DECODER_RENDERER_CALLBACKS coop_secondary_callbacks;

/** Log-only connection listener: the app's one needs UI/player/input state. */
extern CONNECTION_LISTENER_CALLBACKS coop_secondary_conn_callbacks;

/**
 * Secondary mode is selected by `--coop-secondary <socket>` on the command line.
 * Returns the socket path, or NULL for a normal (coordinator) launch. Parsed
 * once at startup; safe to call from anywhere afterwards.
 */
const char *coop_secondary_mode(void);

/** Record the mode from argv. Call once, before any session starts. */
void coop_secondary_parse_args(int argc, char *argv[]);

/** The secondary's launch request (gs_start_app) was refused: tell the primary why, now, instead
 *  of leaving it to time out (issue #33). @p gs_error is libgamestream's error text, may be NULL. */
void coop_secondary_launch_failed(const char *gs_error);

/**
 * Run the secondary with NO UI: no window, no foreground, no surface.
 * Blocks until the coordinator disconnects or the process is stopped.
 * Requires app_init to have run so settings, certificates and host data exist.
 */
struct app_t;
int coop_secondary_run_headless(struct app_t *app);

/** Frames forwarded / dropped, for diagnostics. */
unsigned coop_secondary_forwarded(void);
unsigned coop_secondary_dropped(void);
