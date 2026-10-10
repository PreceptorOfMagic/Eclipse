#pragma once

#include "libgamestream/client.h"
#include "uuidstr.h"

typedef enum SERVER_STATE_ENUM {
    /** Initial state */
    SERVER_STATE_NONE = 0x0,
    /** PCManager is requesting server info */
    SERVER_STATE_QUERYING = 0x10,
    /** Server is online */
    SERVER_STATE_ONLINE = 0x20,
    /** Server is online and paired */
    SERVER_STATE_AVAILABLE = SERVER_STATE_ONLINE | 0x01,
    /** Server is online but not paired */
    SERVER_STATE_NOT_PAIRED = SERVER_STATE_ONLINE | 0x02,
    /** Can't reach server */
    SERVER_STATE_OFFLINE = 0x40,
    /** Server returned error */
    SERVER_STATE_ERROR = 0x80,
} SERVER_STATE_ENUM;

typedef union SERVER_STATE {
    SERVER_STATE_ENUM code;
    struct {
        SERVER_STATE_ENUM code;
        int errcode;
        const char *errmsg;
    } error;
} SERVER_STATE;

typedef struct appid_list_t {
    int id;
    struct appid_list_t *prev;
    struct appid_list_t *next;
} appid_list_t;

/** Controllers that default to a host. NAMES, not indices or GUIDs: a name is what the
 *  user picks from and what the assignment engine already matches on, and it survives a
 *  pad being re-paired or plugged into a different port -- which an index or an
 *  instance id does not. Stored inline so the list frees like the appid lists above. */
#define PADNAME_MAX 64

typedef struct padname_list_t {
    char name[PADNAME_MAX];
    struct padname_list_t *prev;
    struct padname_list_t *next;
} padname_list_t;

typedef struct pclist_t {
    uuidstr_t id;
    bool known, selected;
    SERVER_STATE state;
    /* DO NOT HOLD reference to this field*/
    SERVER_DATA *server;
    appid_list_t *favs;
    appid_list_t *hidden;
    /* Per-host Windows login PIN for Wake-on-LAN auto-entry (NULL = none; plaintext).
     * Persisted in hosts.ini; survives SERVER_DATA replacement on host updates. */
    char *pin_code;
    /* See known_host_t.pin_is_password. Persisted in hosts.ini beside pin_code. */
    bool pin_is_password;
    /* Controllers that default to THIS host in a co-op session. Any number of them:
     * two people on one PC is an ordinary case, and so is the same pad defaulting to
     * both machines. Persisted in hosts.ini as repeated `pad=` keys, beside the
     * favourites, because this is per-host state and hosts.ini is where per-host state
     * lives. */
    padname_list_t *pads;
    /** THE GPU MAKE, AS STATED BY THE USER (coop_gpu_vendor_t; 0 = not stated).
     *
     *  Co-op needs each host's encoder family to pick a codec the two panes can share, and a
     *  stock Apollo or Sunshine host does not report it. Rather than guess from capability
     *  flags -- which separate our own machines cleanly but misread a pre-Turing NVIDIA as AMD
     *  and an Intel Arc as NVIDIA -- the app asks, once, and remembers the answer here.
     *
     *  A HOST THAT REPORTS ITS OWN ENCODER OVERRULES THIS. Measurement beats recollection: if
     *  the Eclipse/Umbra build is installed later, its `EncoderVendor` wins and this becomes a
     *  fallback nobody reads. Persisted in hosts.ini beside pin_code and the pad list. */
    int gpu_vendor;
    struct pclist_t *prev;
    struct pclist_t *next;
} pclist_t;
