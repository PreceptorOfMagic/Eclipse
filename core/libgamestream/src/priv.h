#pragma once

#include "http.h"
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>

#define UNIQUE_FILE_NAME "uniqueid.dat"
#define UNIQUE_ID_DEFAULT "0123456789ABCDEF"

#define UNIQUEID_BYTES 8
#define UNIQUEID_CHARS (UNIQUEID_BYTES * 2)

struct GS_CLIENT_T {
    char unique_id[UNIQUEID_CHARS + 1];
    mbedtls_pk_context pk;
    mbedtls_x509_crt cert;
    char cert_hex[8192];
    HTTP *http;
    bool aurora_gpu_hevc;
    /* Umbra co-op pane (gs_set_coop): send umbraCoop on /launch and /resume, =1 with the host's co-op
     * tuning, =0 without (an Umbra host treats a MISSING argument as "tuning if it is the co-op pane"). */
    bool umbra_coop;
    bool umbra_coop_tuning;
    /* Umbra (gs_set_host_monitor_off): umbraMonitorOff=1 on /launch and /resume. */
    bool umbra_monitor_off;
    /* Umbra wired pin (gs_set_wired_pin): empty = not asked for. */
    char wired_mac[24];
    char own_macs[160];
    char own_ips[160];
};
