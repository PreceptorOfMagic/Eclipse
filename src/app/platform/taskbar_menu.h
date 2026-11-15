#pragma once

/**
 * THE TASKBAR ICON'S RIGHT-CLICK MENU (Windows "jump list"; user 2026-11-04: "a backup/extra
 * option for the users"): Disconnect, Quit stream and Give back the mouse while a stream runs,
 * Resume last co-op session when none does.
 *
 * A jump-list entry cannot call into the running Eclipse: Windows starts the exe again with an
 * argument (--taskbar=<command>). That copy hands the command to the running Eclipse through a
 * hidden message-only window and exits before any window or stream is created. The running
 * Eclipse turns it into USER_TASKBAR_COMMAND on its own UI thread, where root.c does what the
 * stream menu does.
 *
 * IF THE RUNNING ECLIPSE DOES NOT ANSWER (a frozen stream -- the case this is a backup for), Quit
 * and Disconnect end that process after 5 s. In co-op its secondary then ends both panes and
 * undoes the controller hiding (coop_secondary.c, primary_gone); a single stream pauses on its
 * host, as any lost client does.
 *
 * Other platforms: no such menu; the stubs below do nothing.
 */

#include <stdbool.h>

typedef enum taskbar_cmd_t {
    TASKBAR_CMD_NONE = 0,
    TASKBAR_CMD_DISCONNECT,
    TASKBAR_CMD_QUIT,
    TASKBAR_CMD_GIVE_BACK_MOUSE,
    TASKBAR_CMD_RESUME_COOP,
} taskbar_cmd_t;

#ifdef _WIN32

/** First thing in main(). true = this process only carried a command: exit with *exit_code. A
 *  command for an Eclipse that is not running is kept for taskbar_menu_init (Resume) or dropped. */
bool taskbar_menu_forward(int argc, char *argv[], int *exit_code);

/** UI thread, after the UI exists: the command window, the menu, and any command kept above. */
void taskbar_menu_init(bool has_coop_pair);

/** UI thread: show the stream entries (true) or the launcher entry (false). */
void taskbar_menu_set_streaming(bool streaming);

/** UI thread, at exit: leave the launcher entry behind (an exited Eclipse has no stream). */
void taskbar_menu_deinit(void);

#else

static inline bool taskbar_menu_forward(int argc, char *argv[], int *exit_code) {
    (void) argc; (void) argv; (void) exit_code;
    return false;
}
static inline void taskbar_menu_init(bool has_coop_pair) { (void) has_coop_pair; }
static inline void taskbar_menu_set_streaming(bool streaming) { (void) streaming; }
static inline void taskbar_menu_deinit(void) {}

#endif
