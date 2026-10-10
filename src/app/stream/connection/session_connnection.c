#include "ui/streaming/streaming.controller.h"
#include "stream/input/coop_input.h"
#include "stream/input/session_input.h"

#include "util/i18n.h"
#include "logging.h"
#include "input/input_gamepad.h"
#include "app.h"
#include "stream/session_priv.h"

#include <stdio.h>

static void conn_write_diag(const char *msg) {
    FILE *f = fopen(AURORA_DIAG_LOG, "a");
    if (f) { fputs(msg, f); fclose(f); }
}

static session_t *current_session = NULL;

static void connection_terminated(int errorCode) {
    char buf[112];
    snprintf(buf, sizeof(buf), "[CONN@%u] connection_terminated errorCode=0x%x (%s)\n",
             (unsigned) SDL_GetTicks(), errorCode,
             errorCode == ML_ERROR_GRACEFUL_TERMINATION ? "GRACEFUL" : "NETWORK_ERROR");
    conn_write_diag(buf);
    if (!current_session) {
        return;
    }
    current_session->conn_error = errorCode;
    current_session->conn_error_set = true;
    if (errorCode == ML_ERROR_GRACEFUL_TERMINATION) {
        session_interrupt(current_session, false, STREAMING_INTERRUPT_HOST);
    } else {
        commons_log_error("Session", "Connection terminated, errorCode = 0x%x", errorCode);
        streaming_error(current_session, 0, "Connection terminated, errorCode = 0x%x", errorCode);
        session_interrupt(current_session, false, STREAMING_INTERRUPT_NETWORK);
    }
}

static void connection_log_message(const char *format, ...) {
    va_list arglist;
    va_start(arglist, format);
    commons_log_vprintf(COMMONS_LOG_LEVEL_INFO, "Limelight", format, arglist);
    va_end(arglist);
    /* Also capture moonlight's own log into the diag file with a tick timestamp.
     * Its "Loss Stats: Transaction failed: <errno>", "Server notified termination
     * reason", and RTT/ping lines pinpoint WHY/WHEN the control channel died —
     * the smooth-video/clean-Wi-Fi control-plane drop we're chasing. */
    FILE *f = fopen(AURORA_DIAG_LOG, "a");
    if (f) {
        va_list ap;
        va_start(ap, format);
        fprintf(f, "[LL@%u] ", (unsigned) SDL_GetTicks());
        vfprintf(f, format, ap);
        va_end(ap);
        fclose(f);
    }
}

static void connection_status_update(int status) {
    char buf[80];
    snprintf(buf, sizeof(buf), "[CONN@%u] status_update status=%d (%s)\n",
             (unsigned) SDL_GetTicks(), status,
             status == CONN_STATUS_OKAY ? "OKAY" : status == CONN_STATUS_POOR ? "POOR" : "?");
    conn_write_diag(buf);
    switch (status) {
        case CONN_STATUS_OKAY:
            commons_log_info("Session", "Connection is okay");
            streaming_notice_show(NULL);
            break;
        case CONN_STATUS_POOR:
            commons_log_warn("Session", "Connection is poor");
            streaming_notice_show(locstr("Unstable connection."));
            break;
        default:
            break;
    }
}

static void connection_stage_failed(int stage, int errorCode) {
    if (!current_session) {
        return;
    }
    const char *stageName = LiGetStageName(stage);
    commons_log_error("Session", "Connection failed at stage %d (%s), errorCode = %d (%s)", stage, stageName, errorCode,
                      strerror(errorCode));
    streaming_error(current_session, errorCode, "Connection failed at stage %d (%s), errorCode = %d (%s)", stage,
                    stageName, errorCode, strerror(errorCode));
}

static void connection_rumble(unsigned short controllerNumber, unsigned short lowFreqMotor,
                              unsigned short highFreqMotor) {
    if (!current_session) {
        return;
    }
    const int source = coop_input_primary_source_slot(controllerNumber);
    if (source >= 0) {
        app_input_gamepad_rumble(&current_session->app->input, (unsigned short) source,
                                 lowFreqMotor, highFreqMotor);
    }
}

static void connection_rumble_triggers(unsigned short controllerNumber, unsigned short leftTrigger,
                                       unsigned short rightTrigger) {
    if (!current_session) {
        return;
    }
    const int source = coop_input_primary_source_slot(controllerNumber);
    if (source >= 0) {
        app_input_gamepad_rumble_triggers(&current_session->app->input,
                                          (unsigned short) source,
                                          leftTrigger, rightTrigger);
    }
}

static void connection_set_motion_event_state(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz) {
    if (!current_session) {
        return;
    }
    const int source = coop_input_primary_source_slot(controllerNumber);
    if (source >= 0) {
        app_input_gamepad_set_motion_event_state(&current_session->app->input,
                                                 (unsigned short) source,
                                                 motionType, reportRateHz);
    }
}

static void connection_set_controller_led(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b) {
    if (!current_session) {
        return;
    }
    const int source = coop_input_primary_source_slot(controllerNumber);
    if (source >= 0) {
        app_input_gamepad_set_controller_led(&current_session->app->input,
                                             (unsigned short) source, r, g, b);
    }
}

static void connection_set_hdr(bool hdrEnabled) {
    if (!current_session) {
        return;
    }
    streaming_set_hdr(current_session, hdrEnabled);
}

CONNECTION_LISTENER_CALLBACKS connection_callbacks = {
        .stageStarting = NULL,
        .stageComplete = NULL,
        .stageFailed = connection_stage_failed,
        .connectionStarted = NULL,
        .connectionTerminated = connection_terminated,
        .logMessage = connection_log_message,
        .rumble = connection_rumble,
        .rumbleTriggers = connection_rumble_triggers,
        .setMotionEventState = connection_set_motion_event_state,
        .setControllerLED = connection_set_controller_led,
        .connectionStatusUpdate = connection_status_update,
        .setHdrMode = connection_set_hdr
};


CONNECTION_LISTENER_CALLBACKS *session_connection_callbacks_prepare(session_t *session) {
    current_session = session;
    return &connection_callbacks;
}

void session_connection_callbacks_reset(session_t *session) {
    current_session = NULL;
}
