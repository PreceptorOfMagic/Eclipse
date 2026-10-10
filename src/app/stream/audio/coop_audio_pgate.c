/* Co-op audio passive gate -- see coop_audio_pgate.h for what and why. The default duplicate filter since 2026-10-06. */
#include "coop_audio_pgate.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PGATE_TIMING   /* offline measurement only: time spent per phase */
#include <time.h>
double pgate_t_block = 0, pgate_t_search = 0, pgate_t_total = 0;
static double pg_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
#endif

#define PG_PI 3.14159265358979323846
#define NB COOP_PGATE_BANDS
#define PG_BLOCK_MS 5
static const double PG_XOVER_HZ[COOP_PGATE_XOVERS] = {250.0, 600.0, 1500.0, 3500.0, 8000.0};
/* window (ms) and stride (samples) per band. A narrow band holds few independent samples in a short window (about
 * bandwidth x duration): 600-1500 Hz over 5 ms is ~4.5, which made its match noisy both ways (measured: v2 with
 * 20/10/5 ms windows missed 7 lines where 3 wider bands missed 4). So the low bands look longer -- prediction
 * still closes them on time -- and each band is sampled at a few times its top frequency. */
static int PG_WIN_MS[NB] = {40, 30, 10, 5, 5, 5};
static const int PG_STRIDE[NB] = {8, 8, 4, 2, 1, 1};
#define PG_FLOOR_MS2 (32768.0 * 32768.0 * 1e-7)   /* -70 dBFS mean square: quieter windows decide nothing */
#define PG_SEARCH_MS PG_BLOCK_MS   /* discover first-presentation offsets each complete block */
#define PG_DEC 8                   /* mono candidate search downsampling; sample refinement stays full-rate */
#define PG_PEAKS 3
#define PG_ADMIT 0.64f             /* a candidate must reach this match over a 20 ms window (available startup prefix when shorter);
                                    * admission never closes anything by itself */
#define PG_SAME_MS 12              /* offsets this close are one offset (speech repeats at its pitch period) */
#define PG_TWIN_MIN_MS 1           /* closer than this is sample refinement, left to drift/fast tracking */
#define PG_TWIN_WIN_MS 60          /* the twin test's window: long enough for a voice's pitch to move */
#define PG_TWIN_MARGIN 0.20f       /* how clearly (share, 0..1) a candidate must beat the working lag it sits next to */
#define PG_STICKY_BLOCKS 3         /* blocks a challenger must out-match the stream holding a band before taking it */
#define PG_STICKY_AVG_MS 300       /* time constant of the per-band "closed" average */
#define PG_EXPIRE_S 60.0
#define PG_BASS_ALONE 0.90f        /* the bass may close without a witness on a match this strong ... */
#define PG_BASS_RUN 3              /* ... held for this many consecutive blocks */

static float P_CLOSE_REL = 0.70f;  /* close at this fraction of the band's learned ceiling (PGATE_CLOSE_REL) */
static float P_OPEN_REL = 0.45f;   /* open below this fraction, after the hold (PGATE_OPEN_REL) */
static float P_CLOSE_MIN = 0.55f;  /* absolute floor of the close threshold, whatever the ceiling (PGATE_CLOSE_MIN) */
static float P_RISE = 4.0f;        /* PGATE_RISE: master energy rise (x) that predicts a line (4 = +6 dB) */
static int P_PRED = 1;             /* PGATE_PRED: close ahead using the earlier copy */
static int P_HOLD_MS = 200;        /* PGATE_HOLD_MS: stays closed this long after the last match */
static int P_CEIL = 1;             /* PGATE_CEIL: thresholds relative to the learned ceiling (0 = ceiling fixed 0.81) */
static int P_STEREO = 1;           /* PGATE_STEREO: match each channel (0 = match the mono mix, gate both alike) */
static int P_BALANCE = 1;          /* PGATE_BALANCE: balance the hosts' levels from clean matches */
static int P_BASSALONE = 1;        /* PGATE_BASSALONE: a strong sustained bass match may close without a witness */
static int P_CONFLICT = 1;         /* PGATE_CONFLICT: a band never closes in both streams at once */
static int P_SIBILANT = 1;         /* PGATE_SIBILANT: the treble may close on an ENVELOPE match during a line */
static float P_ENV = 0.60f;        /* PGATE_ENV: envelope correlation that counts as a match */
static float P_PRE_GAIN = 0.0f;    /* PGATE_PRE_GAIN: gain while a PREDICTED close awaits its copy (0 = mute; 0.25 = -12 dB delayed the -20 dB onset to ~30 ms) */
static int P_PRED_BANDS = 2;       /* PGATE_PRED_BANDS: bands that must rise together to predict a line */
static int P_PRED_FAILS = 0;       /* PGATE_PRED_FAILS: stop predicting on an offset after this many unconfirmed in a row (0 = never; 3 slowed real line onsets 5 -> 35 ms) */
static int P_PRED_ONE = 0;         /* PGATE_PRED_ONE: one outstanding prediction per offset (1 slowed onsets 5 -> 35 ms) */
static int P_DRIFT_VOTES = 3;      /* PGATE_DRIFT_VOTES: blocks a drift direction must win before the offset moves */
static int P_LANDING = 1;          /* PGATE_LANDING: drop a prediction at once if the slave's band does not rise when the copy should land */
static int P_LANDING_AGE = 2;      /* PGATE_LANDING_AGE: blocks after the guess at which the landing is checked */
static int P_DROPOUT = 1;          /* PGATE_DROPOUT: when one stream drops to digital silence, let the other's copy fill it */
static int P_ENV_ALL = 1;          /* PGATE_ENV_ALL: during a line, EVERY band above 250 Hz may close on an envelope match */
static int P_ENV_LINE = 1;         /* PGATE_ENV_LINE: >= 3 bands with the same varying envelope at a known offset = a line */
static int P_FAST_TRACK = 1;       /* PGATE_FAST_TRACK: an active offset that loses its match searches +-8 samples per block */
static int P_NASAL_WITNESS = 1;    /* PGATE_NASAL_WITNESS: the 250-600 Hz band may witness the bass */
static int P_SENTINEL = 0;         /* PGATE_SENTINEL: an offset idle > 2 s checks 2 mono mid bands first (saves ~0.5% of a TV core but slowed the slowest 10% of line onsets 265 -> 860 ms; a candidate for a lower quality setting) */
static int P_LINK = 1;             /* PGATE_LINK: a channel closed on a strong match pulls the other one with it */
static int P_ANYWITNESS = 1;       /* PGATE_ANYWITNESS: a higher band at ANY offset witnesses the bass */
static float P_RESTORE_DB = 0.0f;  /* PGATE_RESTORE_DB: while a band is closed on a copy, lift the OTHER stream's same band by up to
                                     * this many dB so shared content keeps two copies' loudness (0 = off; runtime: coop_pgate_set_restore_db) */
static int P_LOOKAHEAD_MS = 0;     /* PGATE_LOOKAHEAD_MS: decide each block with this much of the FUTURE in view (output delayed by it) */
static int P_TWIN = 1;             /* PGATE_TWIN: a candidate near a working offset that wins the long-window twin test gets its own slot */
static float P_STICKY_BED = 0.3f;  /* PGATE_STICKY_BED: a band closed this much of the recent time is a sustained bed: no prediction against it */
static int P_PLAYED_GUARD = 1;     /* PGATE_PLAYED_GUARD: never close a band on a copy whose earlier version was itself closed (not
                                    * played): "both copies cut" -- a repeat matched against an earlier repeat (2026-10-10 game-sound score) */
static float P_COPY_MAX_DB = 6.0f; /* PGATE_COPY_MAX_DB: refuse a match whose earlier copy is this much quieter than the later one, after the
                                    * level balance (0 = off): a faint far copy must not remove a loud near sound */
static int P_HOLD_RELEASE = 1;     /* PGATE_HOLD_RELEASE: end a band's hold as soon as the block it would remove carries sound the other
                                    * stream did not play at the held lag (> 6 dB more): the hold cut reloads and gunfire (user, 2026-10-10) */
static int P_STICKY = 3;           /* PGATE_STICKY bits: 1 = a band closed in one stream stays with it against a weak or brief
                                    * challenge; 2 = no prediction takes a band the other stream holds (0 = neither) */

typedef struct {
    bool used;
    int lag;                       /* samples, > 0 peer later; includes the tracked drift */
    int home;                      /* where lines START at this offset; fast tracking may leave it within a line */
    double last_match_s, created_s, last_hi_match_s;
    double last_mid_match_s;       /* last WAVEFORM match in 250-3500 Hz (a line is being removed at this offset) */
    int bass_run[2];               /* consecutive strong bass matches, per channel */
    int pred_fails;                /* predictions in a row that the copy never confirmed */
    double last_pred_fail_s;
    int pred_pending;              /* blocks left on this offset's latest prediction */
    bool pred_confirmed;
    int drift_vote, drift_dir;     /* drift hysteresis: the same direction must win several blocks running */
} pg_offset_t;

typedef struct {
    int n;
    float *cos_t, *sin_t;
    int *rev;
    float *tw;   /* per stage (len >= 8), contiguous (cos, -sin) pairs for k = 0..len/2-1 */
} pg_fft_t;

struct coop_pgate {
    int rate, ch, block, max_lag, ld, search_win;
    int ahead;                                  /* look-ahead (frames, whole blocks): decisions apply to audio this much older */
    int ahead_want;                             /* look-ahead asked for; becomes `ahead` on the first streaming call */
    bool pred, pred_want;                       /* predictions running / asked for (settled with `ahead`) */
    int io_mode; /* 0 unused, 1 legacy delayed stream, 2 caller-complete blocks */
    unsigned valid_len, valid_mask;
    unsigned char *valid_blocks; /* source-valid bits per complete analysis block */
    int win[NB];
    unsigned rlen, rmask;
    float *bandp[2][2][NB];                     /* band signals per stream, CHANNEL (planar: contiguous for vector loads), band */
    float *bandm[2][NB];                        /* mono band signals (stereo matching off, and the search) */
    coop_pgate_xover_t xo[2][2];                /* [stream][channel] */
    unsigned long long pos, next_block, out_pos;
    float gain[2][2][NB];                       /* [stream][channel][band] gate gain */
    int hold[2][2][NB], armed[2][2][NB];
    int challenge[2][2][NB];                    /* blocks in a row this stream has out-matched the one holding the band */
    float closed_avg[2][2][NB];                 /* ~300 ms moving average of "this stream has the band closed" (0..1) */
    int armed_age[2][2][NB];                    /* blocks since a predicted close was armed */
    double arm_ref[2][2][NB];                   /* the slave band's level before the predicted copy */
    float ceil[NB];
    int close_lag[2][2][NB];                    /* |lag| of the match that last closed this band (the hold's copy) */
    unsigned played_len, played_mask;           /* what each output block finally played, by output block number */
    float (*played)[2][2][NB];                  /* final gain per output block, stream, channel, band */
    unsigned long long *played_tag;             /* output block number + 1 held in that slot (0 = empty) */
    double last_hi_any_s;                       /* a band >= 2 matched at any offset */
    pg_offset_t bank[COOP_PGATE_MAX_OFFSETS];
    double bal_db;                              /* level of peer relative to self for the same audio, dB */
    unsigned bal_n;
    float bal_g[2];                             /* output gain per stream */
    float restore_gain;                         /* shared-content restore: largest lift of the open copy (1 = off) */
    float restore_prev[2][2][NB];               /* restore lift at the end of the previous block (ramp start) */
    unsigned dlen, dmask;
    float *dec[2];
    unsigned long long dpos;
    float dec_acc[2];
    int dec_n;
    unsigned long long next_search;
    pg_fft_t fft;
    float *fr, *fi, *gr, *gi;
    unsigned olen, omask;
    float *out[2];
    /* 1 ms log-energy frames per stream, channel (and mono), band: envelopes cost a lookup instead of a recount */
    unsigned flen, fmask;
    float *fen[2][3][NB];                       /* [stream][0,1 = channel, 2 = mono][band] */
    float *fel[2][3][NB];                       /* the same frames as LINEAR energy: loudness sums are lookups */
    float *raw[2];                              /* the input, interleaved: open gates pass it straight through */
    float facc[2][3][NB];                       /* 48-sample sums: single precision is ample */
    int fpos_in;                                /* samples into the current frame */
    unsigned long long fcount;                  /* frames completed */
    coop_pgate_stats_t stats;
    coop_pgate_hook_t hook;
    void *hook_ctx;
};

/* ---------------------------------------------------------------- crossover */

void coop_pgate_xover_init(coop_pgate_xover_t *x, int rate) {
    memset(x, 0, sizeof(*x));
    for (int k = 0; k < COOP_PGATE_XOVERS; ++k) {   /* 2nd-order Butterworth low-passes, each fed the INPUT */
        const double w = 2 * PG_PI * PG_XOVER_HZ[k] / rate, c = cos(w), s = sin(w), al = s / (2 * 0.70710678),
                     a0 = 1 + al;
        x->b0[k] = (float) ((1 - c) / 2 / a0);
        x->b1[k] = (float) ((1 - c) / a0);
        x->b2[k] = x->b0[k];
        x->a1[k] = (float) (-2 * c / a0);
        x->a2[k] = (float) ((1 - al) / a0);
    }
}

void coop_pgate_xover_run(coop_pgate_xover_t *x, float in, float *bands) {
    float lp[COOP_PGATE_XOVERS];
    for (int k = 0; k < COOP_PGATE_XOVERS; ++k) {   /* transposed direct form II */
        lp[k] = x->b0[k] * in + x->z1[k];
        x->z1[k] = x->b1[k] * in - x->a1[k] * lp[k] + x->z2[k];
        x->z2[k] = x->b2[k] * in - x->a2[k] * lp[k];
    }
    /* differences of low-passes: band k = lp[k] - lp[k-1]; the top band = in - lp[last]. Telescopes to `in`. */
    bands[0] = lp[0];
    for (int k = 1; k < COOP_PGATE_XOVERS; ++k) {
        bands[k] = lp[k] - lp[k - 1];
    }
    bands[NB - 1] = in - lp[COOP_PGATE_XOVERS - 1];
}

/* ---------------------------------------------------------------- FFT for the search */

static bool fft_init(pg_fft_t *f, int n) {
    f->n = n;
    f->cos_t = malloc(sizeof(float) * (size_t) (n / 2));
    f->sin_t = malloc(sizeof(float) * (size_t) (n / 2));
    f->rev = malloc(sizeof(int) * (size_t) n);
    f->tw = malloc(sizeof(float) * (size_t) (2 * n));   /* sum over stages of len/2 pairs < n pairs */
    if (!f->cos_t || !f->sin_t || !f->rev || !f->tw) {
        return false;
    }
    for (int i = 0; i < n / 2; ++i) {
        f->cos_t[i] = (float) cos(2.0 * PG_PI * i / n);
        f->sin_t[i] = (float) -sin(2.0 * PG_PI * i / n);
    }
    float *t = f->tw;
    for (int len = 8; len <= n; len <<= 1) {
        const int step = n / len;
        for (int k = 0; k < len / 2; ++k) {
            *t++ = f->cos_t[k * step];
            *t++ = f->sin_t[k * step];
        }
    }
    int bits = 0;
    while ((1 << bits) < n) {
        bits++;
    }
    for (int i = 0; i < n; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b) {
            r |= ((i >> b) & 1) << (bits - 1 - b);
        }
        f->rev[i] = r;
    }
    return true;
}

/* Radix-2, in place. The first two stages need no multiplications (twiddles 1 and -i); later stages read their
 * twiddles from one contiguous table per stage instead of striding through the full-length one (measured on the TV:
 * the search was 26% of the gate's time). */
static void fft_run(const pg_fft_t *f, float *re, float *im, bool inverse) {
    const int n = f->n;
    for (int i = 0; i < n; ++i) {
        int j = f->rev[i];
        if (j > i) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int i = 0; i < n; i += 2) {   /* len 2 */
        const float ar = re[i], ai = im[i], br = re[i + 1], bi = im[i + 1];
        re[i] = ar + br; im[i] = ai + bi;
        re[i + 1] = ar - br; im[i + 1] = ai - bi;
    }
    for (int i = 0; i < n; i += 4) {   /* len 4: twiddles 1 and -i (forward) / +i (inverse) */
        float ar = re[i], ai = im[i], br = re[i + 2], bi = im[i + 2];
        re[i] = ar + br; im[i] = ai + bi;
        re[i + 2] = ar - br; im[i + 2] = ai - bi;
        ar = re[i + 1]; ai = im[i + 1]; br = re[i + 3]; bi = im[i + 3];
        const float tr = inverse ? -bi : bi, ti = inverse ? br : -br;   /* b * (-i) forward, b * (+i) inverse */
        re[i + 1] = ar + tr; im[i + 1] = ai + ti;
        re[i + 3] = ar - tr; im[i + 3] = ai - ti;
    }
    const float *t = f->tw;
    for (int len = 8; len <= n; len <<= 1) {
        const int half = len / 2;
        for (int i = 0; i < n; i += len) {
            float *r0 = re + i, *i0 = im + i, *r1 = re + i + half, *i1 = im + i + half;
            for (int k = 0; k < half; ++k) {
                const float wr = t[2 * k], wi = inverse ? -t[2 * k + 1] : t[2 * k + 1];
                const float xr = r1[k], xi = i1[k];
                const float tr = xr * wr - xi * wi, ti = xr * wi + xi * wr;
                r1[k] = r0[k] - tr;
                i1[k] = i0[k] - ti;
                r0[k] += tr;
                i0[k] += ti;
            }
        }
        t += len;
    }
}

static unsigned pow2_at_least(unsigned v) {
    unsigned p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

/* ---------------------------------------------------------------- lifecycle */

/* PGATE_* environment overrides are OFFLINE TUNING knobs for the simulator and benches, compiled in only with
 * -DCOOP_PGATE_ENV_TUNING. Shipping builds always run the evaluated defaults above and read no environment. */
#ifndef COOP_PGATE_ENV_TUNING
static void read_env(void) {}
#else
static void read_env(void) {
    const char *e;
    if ((e = getenv("PGATE_CLOSE_REL")) != NULL) P_CLOSE_REL = (float) atof(e);
    if ((e = getenv("PGATE_OPEN_REL")) != NULL) P_OPEN_REL = (float) atof(e);
    if ((e = getenv("PGATE_CLOSE_MIN")) != NULL) P_CLOSE_MIN = (float) atof(e);
    if ((e = getenv("PGATE_RISE")) != NULL) P_RISE = (float) atof(e);
    if ((e = getenv("PGATE_PRED")) != NULL) P_PRED = atoi(e);
    if ((e = getenv("PGATE_HOLD_MS")) != NULL) P_HOLD_MS = atoi(e);
    if ((e = getenv("PGATE_CEIL")) != NULL) P_CEIL = atoi(e);
    if ((e = getenv("PGATE_STEREO")) != NULL) P_STEREO = atoi(e);
    if ((e = getenv("PGATE_BALANCE")) != NULL) P_BALANCE = atoi(e);
    if ((e = getenv("PGATE_BASSALONE")) != NULL) P_BASSALONE = atoi(e);
    if ((e = getenv("PGATE_ANYWITNESS")) != NULL) P_ANYWITNESS = atoi(e);
    if ((e = getenv("PGATE_CONFLICT")) != NULL) P_CONFLICT = atoi(e);
    if ((e = getenv("PGATE_SIBILANT")) != NULL) P_SIBILANT = atoi(e);
    if ((e = getenv("PGATE_LINK")) != NULL) P_LINK = atoi(e);
    if ((e = getenv("PGATE_SENTINEL")) != NULL) P_SENTINEL = atoi(e);
    if ((e = getenv("PGATE_ENV_ALL")) != NULL) P_ENV_ALL = atoi(e);
    if ((e = getenv("PGATE_ENV_LINE")) != NULL) P_ENV_LINE = atoi(e);
    if ((e = getenv("PGATE_FAST_TRACK")) != NULL) P_FAST_TRACK = atoi(e);
    if ((e = getenv("PGATE_NASAL_WITNESS")) != NULL) P_NASAL_WITNESS = atoi(e);
    if ((e = getenv("PGATE_DROPOUT")) != NULL) P_DROPOUT = atoi(e);
    if ((e = getenv("PGATE_LANDING")) != NULL) P_LANDING = atoi(e);
    if ((e = getenv("PGATE_LANDING_AGE")) != NULL) P_LANDING_AGE = atoi(e);
    if ((e = getenv("PGATE_PRED_ONE")) != NULL) P_PRED_ONE = atoi(e);
    if ((e = getenv("PGATE_DRIFT_VOTES")) != NULL) P_DRIFT_VOTES = atoi(e);
    if ((e = getenv("PGATE_PRE_GAIN")) != NULL) P_PRE_GAIN = (float) atof(e);
    if ((e = getenv("PGATE_PRED_BANDS")) != NULL) P_PRED_BANDS = atoi(e);
    if ((e = getenv("PGATE_PRED_FAILS")) != NULL) P_PRED_FAILS = atoi(e);
    if ((e = getenv("PGATE_ENV")) != NULL) P_ENV = (float) atof(e);
    if ((e = getenv("PGATE_RESTORE_DB")) != NULL) P_RESTORE_DB = (float) atof(e);
    if ((e = getenv("PGATE_TWIN")) != NULL) P_TWIN = atoi(e);
    if ((e = getenv("PGATE_LOOKAHEAD_MS")) != NULL) P_LOOKAHEAD_MS = atoi(e);
    if ((e = getenv("PGATE_STICKY")) != NULL) P_STICKY = atoi(e);
    if ((e = getenv("PGATE_PLAYED_GUARD")) != NULL) P_PLAYED_GUARD = atoi(e);
    if ((e = getenv("PGATE_COPY_MAX_DB")) != NULL) P_COPY_MAX_DB = (float) atof(e);
    if ((e = getenv("PGATE_HOLD_RELEASE")) != NULL) P_HOLD_RELEASE = atoi(e);
    if ((e = getenv("PGATE_STICKY_BED")) != NULL) P_STICKY_BED = (float) atof(e);
    if ((e = getenv("PGATE_WINS")) != NULL) {   /* "40,30,10,5,5,5" */
        sscanf(e, "%d,%d,%d,%d,%d,%d", &PG_WIN_MS[0], &PG_WIN_MS[1], &PG_WIN_MS[2], &PG_WIN_MS[3], &PG_WIN_MS[4], &PG_WIN_MS[5]);
    }
}
#endif

coop_pgate_t *coop_pgate_create(int rate, int channels, int max_lag_ms) {
    if (rate < 8000 || rate > 96000 || channels < 1 || channels > 2 || max_lag_ms < 5 || max_lag_ms > 1000) {
        return NULL;
    }
    read_env();
    coop_pgate_t *g = calloc(1, sizeof(*g));
    if (g == NULL) {
        return NULL;
    }
    g->rate = rate;
    g->ch = channels;
    g->block = rate / 1000 * PG_BLOCK_MS;
    g->max_lag = rate / 1000 * max_lag_ms;
    g->search_win = g->block / PG_DEC;
    for (int b = 0; b < NB; ++b) {
        g->win[b] = rate / 1000 * PG_WIN_MS[b];
        g->ceil[b] = 0.81f;   /* until learned: an exact copy through good coding (rho 0.9) */
    }
    g->ld = g->max_lag / PG_DEC + 1;
    g->rlen = pow2_at_least((unsigned) (g->max_lag + 2 * g->rate / 1000 * 80 + 4 * g->block + 256));
    g->rmask = g->rlen - 1;
    g->valid_len = pow2_at_least(g->rlen / (unsigned) g->block + 2);
    g->valid_mask = g->valid_len - 1;
    g->valid_blocks = calloc(g->valid_len, sizeof(*g->valid_blocks));
    g->dlen = pow2_at_least((unsigned) (g->search_win + g->ld + 64));
    g->dmask = g->dlen - 1;
    {   /* LOOK-AHEAD (opt-in). Each block's decision is made, as always, on windows ending at the newest sample, but it
         * is APPLIED to the block `ahead` frames older: the gate then sees that much of a copy's future before playing
         * it (its onset, whether it continues, where it ends), and the output is that much later. Whole blocks only. */
        const int want = (int) ((long long) P_LOOKAHEAD_MS * rate / 1000);
        g->ahead_want = want > 0 ? (want + g->block - 1) / g->block * g->block : 0;
        if (g->ahead_want > rate / 10) g->ahead_want = rate / 10 / g->block * g->block;   /* <= 100 ms */
        g->ahead = 0;
        g->pred = g->pred_want = P_PRED != 0;
    }
    g->olen = pow2_at_least((unsigned) (4 * g->block + rate / 10 + 4096));   /* room for any look-ahead (<= 100 ms) */
    g->omask = g->olen - 1;
    const unsigned nf = pow2_at_least((unsigned) (g->search_win + g->ld));
    bool ok = fft_init(&g->fft, (int) nf) && g->valid_blocks != NULL;
    for (int s = 0; s < 2 && ok; ++s) {
        g->out[s] = calloc((size_t) g->olen * (size_t) channels, sizeof(float));
        g->dec[s] = calloc(g->dlen, sizeof(float));
        ok = g->out[s] && g->dec[s];
        for (int b = 0; b < NB && ok; ++b) {
            g->bandp[s][0][b] = calloc(g->rlen, sizeof(float));
            g->bandp[s][1][b] = channels > 1 ? calloc(g->rlen, sizeof(float)) : NULL;
            g->bandm[s][b] = calloc(g->rlen, sizeof(float));
            ok = g->bandp[s][0][b] && (channels < 2 || g->bandp[s][1][b]) && g->bandm[s][b];
        }
        for (int c = 0; c < channels; ++c) {
            coop_pgate_xover_init(&g->xo[s][c], rate);
        }
        for (int c = 0; c < 2; ++c) {
            for (int b = 0; b < NB; ++b) {
                g->gain[s][c][b] = 1.0f;
            }
        }
        g->bal_g[s] = 1.0f;
        for (int c = 0; c < 2; ++c) for (int b = 0; b < NB; ++b) g->restore_prev[s][c][b] = 1.0f;
    }
    g->restore_gain = 1.0f;
    coop_pgate_set_restore_db(g, P_RESTORE_DB);
    g->flen = pow2_at_least((unsigned) (g->max_lag / (rate / 1000) + 256));
    g->fmask = g->flen - 1;
    for (int s2 = 0; s2 < 2 && ok; ++s2) {
        for (int c = 0; c < 3 && ok; ++c) {
            for (int b = 0; b < NB && ok; ++b) {
                g->fen[s2][c][b] = calloc(g->flen, sizeof(float));
                g->fel[s2][c][b] = calloc(g->flen, sizeof(float));
                ok = g->fen[s2][c][b] != NULL && g->fel[s2][c][b] != NULL;
            }
        }
    }
    for (int s2 = 0; s2 < 2 && ok; ++s2) {
        g->raw[s2] = calloc((size_t) g->rlen * (size_t) channels, sizeof(float));
        ok = g->raw[s2] != NULL;
    }
    /* the played record must reach back the largest lag plus the largest look-ahead (100 ms) */
    g->played_len = pow2_at_least((unsigned) ((g->max_lag + rate / 10) / g->block + 4));
    g->played_mask = g->played_len - 1;
    g->played = calloc(g->played_len, sizeof(*g->played));
    g->played_tag = calloc(g->played_len, sizeof(*g->played_tag));
    ok = ok && g->played && g->played_tag;
    g->fr = calloc(nf, sizeof(float));
    g->fi = calloc(nf, sizeof(float));
    g->gr = calloc(nf, sizeof(float));
    g->gi = calloc(nf, sizeof(float));
    if (!ok || !g->fr || !g->fi || !g->gr || !g->gi) {
        coop_pgate_destroy(g);
        return NULL;
    }
    g->last_hi_any_s = -1e9;
    g->next_block = (unsigned long long) g->block;
    g->next_search = (unsigned long long) g->block;
    return g;
}

void coop_pgate_destroy(coop_pgate_t *g) {
    if (g == NULL) {
        return;
    }
    for (int s = 0; s < 2; ++s) {
        free(g->out[s]);
        free(g->dec[s]);
        for (int b = 0; b < NB; ++b) {
            free(g->bandp[s][0][b]);
            free(g->bandp[s][1][b]);
            free(g->bandm[s][b]);
        }
    }
    for (int s2 = 0; s2 < 2; ++s2) {
        for (int c = 0; c < 3; ++c) {
            for (int b = 0; b < NB; ++b) {
                free(g->fen[s2][c][b]);
                free(g->fel[s2][c][b]);
            }
        }
    }
    free(g->raw[0]);
    free(g->raw[1]);
    free(g->valid_blocks);
    free(g->played);
    free(g->played_tag);
    free(g->fft.tw);
    free(g->fft.cos_t);
    free(g->fft.sin_t);
    free(g->fft.rev);
    free(g->fr);
    free(g->fi);
    free(g->gr);
    free(g->gi);
    free(g);
}

int coop_pgate_latency_frames(const coop_pgate_t *g) { return g->block + g->ahead; }

coop_pgate_t *coop_pgate_create_quality(int rate, int channels, int max_lag_ms, int quality, int lookahead_ms) {
    coop_pgate_t *g = coop_pgate_create(rate, channels, max_lag_ms);
    if (g != NULL && quality == COOP_PGATE_ACCURATE) {
        const int ms = lookahead_ms < 0 ? 0 : lookahead_ms > 100 ? 100 : lookahead_ms;
        g->ahead_want = (rate / 1000 * ms + g->block - 1) / g->block * g->block;
        g->pred_want = false;
    }
    return g;
}

int coop_pgate_quality(const coop_pgate_t *g) {
    return g != NULL && g->io_mode == 1 && !g->pred ? COOP_PGATE_ACCURATE : COOP_PGATE_FAST;
}

void coop_pgate_set_restore_db(coop_pgate_t *g, float db) {
    if (g == NULL) return;
    if (!(db > 0.0f)) db = 0.0f;   /* also catches NaN */
    if (db > 6.0f) db = 6.0f;
    g->restore_gain = (float) pow(10.0, db / 20.0);
}

int coop_pgate_block_frames(const coop_pgate_t *g) { return g ? g->block : 0; }

bool coop_pgate_reset(coop_pgate_t *g) {
    if (!g) return false;
    g->io_mode = 0;
    g->pos = g->out_pos = g->dpos = g->fcount = 0;
    g->next_block = (unsigned) g->block;
    g->next_search = (unsigned long long) g->block;
    g->dec_n = g->fpos_in = 0;
    g->bal_db = 0; g->bal_n = 0; g->last_hi_any_s = -1e9;
    memset(&g->stats, 0, sizeof(g->stats));
    memset(g->bank, 0, sizeof(g->bank));
    memset(g->hold, 0, sizeof(g->hold)); memset(g->armed, 0, sizeof(g->armed));
    memset(g->armed_age, 0, sizeof(g->armed_age)); memset(g->arm_ref, 0, sizeof(g->arm_ref));
    memset(g->challenge, 0, sizeof(g->challenge)); memset(g->closed_avg, 0, sizeof(g->closed_avg));
    memset(g->facc, 0, sizeof(g->facc)); memset(g->dec_acc, 0, sizeof(g->dec_acc));
    memset(g->valid_blocks, 0, g->valid_len * sizeof(*g->valid_blocks));
    memset(g->played_tag, 0, g->played_len * sizeof(*g->played_tag));
    memset(g->close_lag, 0, sizeof(g->close_lag));
    for (int b = 0; b < NB; ++b) g->ceil[b] = 0.81f;
    for (int s = 0; s < 2; ++s) {
        g->bal_g[s] = 1.0f;
        for (int c = 0; c < 2; ++c) for (int b = 0; b < NB; ++b) g->restore_prev[s][c][b] = 1.0f;
        memset(g->raw[s], 0, (size_t) g->rlen * g->ch * sizeof(float));
        memset(g->out[s], 0, (size_t) g->olen * g->ch * sizeof(float));
        memset(g->dec[s], 0, g->dlen * sizeof(float));
        for (int c = 0; c < 2; ++c) {
            memset(g->xo[s][c].z1, 0, sizeof(g->xo[s][c].z1));
            memset(g->xo[s][c].z2, 0, sizeof(g->xo[s][c].z2));
            for (int b = 0; b < NB; ++b) {
                g->gain[s][c][b] = 1.0f;
                if (g->bandp[s][c][b]) memset(g->bandp[s][c][b], 0, g->rlen * sizeof(float));
            }
        }
        for (int b = 0; b < NB; ++b) {
            memset(g->bandm[s][b], 0, g->rlen * sizeof(float));
            for (int c = 0; c < 3; ++c) {
                memset(g->fen[s][c][b], 0, g->flen * sizeof(float));
                memset(g->fel[s][c][b], 0, g->flen * sizeof(float));
            }
        }
    }
    return true;
}

void coop_pgate_set_hook(coop_pgate_t *g, coop_pgate_hook_t hook, void *ctx) {
    g->hook = hook;
    g->hook_ctx = ctx;
}

void coop_pgate_get_stats(const coop_pgate_t *g, coop_pgate_stats_t *out) {
    *out = g->stats;
    out->offsets = 0;
    for (int i = 0; i < COOP_PGATE_MAX_OFFSETS; ++i) {
        if (g->bank[i].used) {
            out->offset_samples[out->offsets++] = g->bank[i].lag;
        }
    }
    out->balance_db = (float) g->bal_db;
    out->balance_applied = P_BALANCE && g->bal_n >= 20;
    out->balance_samples = g->bal_n;
    for (int b = 0; b < NB; ++b) {
        out->ceiling[b] = g->ceil[b];
    }
}

static double now_s(const coop_pgate_t *g) { return (double) g->pos / g->rate; }

/* Complete-block validity is conservative for partially overlapped matching windows. */
static bool range_valid(const coop_pgate_t *g, int stream, unsigned long long start, int frames) {
    if (frames <= 0 || start > g->pos || (unsigned long long) frames > g->pos - start ||
        g->pos - start >= g->rlen) return false;
    const unsigned long long first = start / (unsigned) g->block;
    const unsigned long long last = (start + (unsigned) frames - 1) / (unsigned) g->block;
    for (unsigned long long i = first; i <= last; ++i)
        if (!(g->valid_blocks[i & g->valid_mask] & (1u << stream))) return false;
    return true;
}

/* ---------------------------------------------------------------- matching */

/* Number of consecutive valid retained samples immediately before end, capped
 * at limit. Partial source blocks remain invalid; no missing PCM is reclassified. */
static int valid_suffix(const coop_pgate_t *g, int stream, unsigned long long end, int limit) {
    if (!end || end > g->pos || limit <= 0) return 0;
    const unsigned long long oldest = g->pos >= g->rlen ? g->pos - g->rlen + 1 : 0;
    if (end <= oldest) return 0;
    unsigned long long start = end - oldest < (unsigned) limit ? oldest : end - (unsigned) limit;
    unsigned long long cursor = end;
    while (cursor > start) {
        const unsigned long long index = (cursor - 1) / (unsigned) g->block;
        if (!(g->valid_blocks[index & g->valid_mask] & (1u << stream))) break;
        const unsigned long long block_start = index * (unsigned) g->block;
        cursor = block_start > start ? block_start : start;
    }
    return (int) (end - cursor);
}

/* Epoch start and missing-input gaps both limit usable history. Require at least
 * one complete block from BOTH target and lagged reference. Bass keeps its full
 * window: a few cycles are not a unique witness. Final range checks still apply. */
static int matching_window(const coop_pgate_t *g, int band, int slave, unsigned long long end, int lag) {
    if (lag < 0 || end <= (unsigned) lag) return 0;
    const int requested = g->win[band];
    int w = valid_suffix(g, slave, end, requested);
    const int reference = valid_suffix(g, 1 - slave, end - (unsigned) lag, requested);
    if (reference < w) w = reference;
    if (band == 0 && w != requested) return 0;
    return w >= g->block ? w : 0;
}


/* Share of `slave`'s band window (channel ch, or the mono mix when ch < 0) ending at `end` that is a copy of the
 * master `lag` samples earlier: rho^2 when the correlation is positive, else 0; -1 when the window is too quiet to
 * decide anything. Optionally returns the least-squares gain slave/master. */
/* The slave window's energy sum, split the way share() splits it (element j -> sum j % 8), so a cached value gives the
 * same bits as computing it inside share(). */
static double slave_energy(const coop_pgate_t *g, int band, int slave, int ch, unsigned long long end) {
    const int w = g->win[band], st = PG_STRIDE[band];
    const float *x = ch < 0 ? g->bandm[slave][band] : g->bandp[slave][ch][band];
    const unsigned rm = g->rmask, xa = (unsigned) ((end - (unsigned long long) w) & rm);
    float xx[8] = {0};
    for (int j = 0; j < w / st; ++j) {
        const float xv = x[(xa + (unsigned) (j * st)) & rm];
        xx[j & 7] += xv * xv;
    }
    return (double) ((xx[0] + xx[4]) + (xx[1] + xx[5])) + ((xx[2] + xx[6]) + (xx[3] + xx[7]));
}

/* sxx_known >= 0: the slave window's energy from slave_energy() for this block (the same for every offset, so it is
 * computed once per block instead of once per offset -- a third of the matching work). */
static float share_x(const coop_pgate_t *g, int band, int slave, int ch, unsigned long long end, int lag, float *gain_out,
                     double sxx_known) {
    const int w = matching_window(g, band, slave, end, lag), st = PG_STRIDE[band];
    if (!w) return -1.0f;
    if (w != g->win[band]) sxx_known = -1.0;
    const unsigned long long a = end - (unsigned long long) w;
    if (!range_valid(g, slave, a, w) ||
        !range_valid(g, 1 - slave, a - (unsigned long long) lag, w)) return -1.0f;
    /* eight independent single-precision sums: the TV's ARM core pipelines them (one double-precision chain stalled on
     * every add -- profiled 2026-10-06). NEON was tried and was not faster on the TV's core for these short windows. */
    float xy[8] = {0}, xx[8] = {0}, yy[8] = {0};
    const float *x = ch < 0 ? g->bandm[slave][band] : g->bandp[slave][ch][band];
    const float *y = ch < 0 ? g->bandm[1 - slave][band] : g->bandp[1 - slave][ch][band];
    const unsigned rm = g->rmask;
    const unsigned xa = (unsigned) (a & rm), ya = (unsigned) ((a - (unsigned long long) lag) & rm);
    const int n = w / st;
    if (sxx_known >= 0) {
        for (int j = 0; j < n; ++j) {
            const float xv = x[(xa + (unsigned) (j * st)) & rm], yv = y[(ya + (unsigned) (j * st)) & rm];
            xy[j & 7] += xv * yv;
            yy[j & 7] += yv * yv;
        }
    } else {
        for (int j = 0; j < n; ++j) {
            const float xv = x[(xa + (unsigned) (j * st)) & rm], yv = y[(ya + (unsigned) (j * st)) & rm];
            xy[j & 7] += xv * yv;
            xx[j & 7] += xv * xv;
            yy[j & 7] += yv * yv;
        }
    }
    const double sxy = (double) ((xy[0] + xy[4]) + (xy[1] + xy[5])) + ((xy[2] + xy[6]) + (xy[3] + xy[7])),
                 sxx = sxx_known >= 0 ? sxx_known : (double) ((xx[0] + xx[4]) + (xx[1] + xx[5])) + ((xx[2] + xx[6]) + (xx[3] + xx[7])),
                 syy = (double) ((yy[0] + yy[4]) + (yy[1] + yy[5])) + ((yy[2] + yy[6]) + (yy[3] + yy[7]));
    if (sxx < PG_FLOOR_MS2 * (w / st)) {
        return -1.0f;
    }
    if (sxy <= 0 || syy <= 0) {
        return 0.0f;
    }
    if (gain_out != NULL) {
        *gain_out = (float) (sxy / syy);
    }
    return (float) (sxy * sxy / (sxx * syy));
}

static float share(const coop_pgate_t *g, int band, int slave, int ch, unsigned long long end, int lag, float *gain_out) {
    return share_x(g, band, slave, ch, end, lag, gain_out, -1.0);
}

/* The 1 ms frame containing sample position `p` (floor(p / frame)), without a 64-bit division -- the TV's 32-bit ARM
 * has no instruction for it and the library routine was 5% of the gate's time. Positions are within the ring of
 * the current one, so the difference fits in 32 bits. */
static unsigned long long frame_of(const coop_pgate_t *g, unsigned long long p) {
    const int fl = g->rate / 1000;
    const long long d = (long long) g->fpos_in - (long long) (g->pos - p);   /* p relative to the current frame start */
    const int di = (int) d;
    const int q = di >= 0 ? di / fl : -((-di + fl - 1) / fl);
    return (unsigned long long) ((long long) g->fcount + q);
}

/* Energy of stream `s`, channel ch (< 0: the mono mix), band, over [a, a+len) -- from the 1 ms frame record, so it
 * costs len/1 ms additions instead of len multiply-adds. Exact for windows on 1 ms boundaries (every block is); the
 * prediction's lag-shifted windows are rounded to the nearest frame, well inside the onset test's resolution.
 * (Profiled 2026-10-06: the per-sample version was 46% of the gate's CPU.) */
static double band_energy(const coop_pgate_t *g, int s, int ch, int band, unsigned long long a, int len) {
    const int fl = g->rate / 1000;
    unsigned long long f0 = frame_of(g, a + (unsigned long long) (fl / 2)), f1 = frame_of(g, a + (unsigned long long) (len + fl / 2));
    if (f1 > g->fcount) {
        f1 = g->fcount;
    }
    const float *x = g->fel[s][ch < 0 ? 2 : ch][band];
    double e = 0;
    for (unsigned long long f = f0; f < f1; ++f) {
        e += x[f & g->fmask];
    }
    return e;
}

/* Correlation of the log-energy ENVELOPES (20 frames of 1 ms ending at `end`) of the slave's band and the master's
 * band `lag` earlier. An "s" is noise: during the narrator lines each game generated its hiss as a DIFFERENT noise
 * pattern (waveform share 0.16-0.57 above 3.5 kHz) with the SAME loudness shape (envelope 0.84-0.90), measured
 * 2026-10-05 -- so in the treble the envelope is the copy that survives. */
static float env_corr(const coop_pgate_t *g, int band, int slave, int ch, unsigned long long end, int lag) {
    const int F = 20;
    const int span = F * (g->rate / 1000);
    if (lag < 0 || !range_valid(g, slave, end - (unsigned) span, span) ||
        !range_valid(g, 1 - slave, end - (unsigned) span - (unsigned) lag, span)) return 0.0f;
    const unsigned long long ef = frame_of(g, end);                            /* frames up to the block end */
    const unsigned long long mf = frame_of(g, end - (unsigned long long) lag);
    if (ef > g->fcount || ef < (unsigned long long) F || mf < (unsigned long long) F) {
        return 0.0f;
    }
    const int cc = ch < 0 ? 2 : ch;
    const float *xs = g->fen[slave][cc][band], *xm = g->fen[1 - slave][cc][band];
    double ma = 0, mb = 0, ea[20], eb[20];
    for (int f = 0; f < F; ++f) {
        ea[f] = xs[(ef - (unsigned long long) (F - f)) & g->fmask];
        eb[f] = xm[(mf - (unsigned long long) (F - f)) & g->fmask];
        ma += ea[f];
        mb += eb[f];
    }
    ma /= F;
    mb /= F;
    double xy = 0, xx = 0, yy = 0;
    for (int f = 0; f < F; ++f) {
        xy += (ea[f] - ma) * (eb[f] - mb);
        xx += (ea[f] - ma) * (ea[f] - ma);
        yy += (eb[f] - mb) * (eb[f] - mb);
    }
    return xx > 1e-6 && yy > 1e-6 ? (float) (xy / sqrt(xx * yy)) : 0.0f;
}

/* Envelope dynamic range (max - min of the slave's 1 ms log-energy over 20 ms), in dB: a near-flat envelope (steady
 * noise, a hum) correlates by chance, so an envelope match only counts when the sound actually moves. */
static float env_range_db(const coop_pgate_t *g, int band, int slave, int ch, unsigned long long end) {
    const int F = 20;
    const unsigned long long ef = frame_of(g, end);
    if (ef > g->fcount || ef < (unsigned long long) F) {
        return 0.0f;
    }
    const float *xs = g->fen[slave][ch < 0 ? 2 : ch][band];
    float lo = 1e30f, hi = -1e30f;
    for (int f = 0; f < F; ++f) {
        const float d = xs[(ef - (unsigned long long) (F - f)) & g->fmask];
        if (d < lo) lo = d;
        if (d > hi) hi = d;
    }
    return 10.0f * (hi - lo);
}

static float close_thr(const coop_pgate_t *g, int b) {
    const float t = P_CLOSE_REL * (P_CEIL ? g->ceil[b] : 0.81f);
    return t < P_CLOSE_MIN ? P_CLOSE_MIN : t;
}

static float open_thr(const coop_pgate_t *g, int b) { return P_OPEN_REL * (P_CEIL ? g->ceil[b] : 0.81f); }

/* ---------------------------------------------------------------- the bank */

/* The NEAREST entry within PG_SAME_MS of `lag` (current lag or home). Nearest, not first: the twin test below can leave
 * two entries inside one tolerance, and a candidate must land on its own. */
static pg_offset_t *bank_near(coop_pgate_t *g, int lag) {
    const int tol = g->rate / 1000 * PG_SAME_MS;
    pg_offset_t *near = NULL;
    int near_d = tol + 1;
    for (int i = 0; i < COOP_PGATE_MAX_OFFSETS; ++i) {
        if (!g->bank[i].used) {
            continue;
        }
        const int d1 = abs(g->bank[i].lag - lag), d2 = abs(g->bank[i].home - lag), d = d1 < d2 ? d1 : d2;
        if (d < near_d) {
            near = &g->bank[i];
            near_d = d;
            if (!P_TWIN) {
                break;   /* the first entry within tolerance, exactly as before the twin test existed */
            }
        }
    }
    return near;
}

/* Mean mono 600-3500 Hz match of a signed lag (> 0 peer later) over the last PG_TWIN_WIN_MS; -1 when unmeasurable. */
static float twin_match(coop_pgate_t *g, int lag) {
    const int slave = lag > 0 ? 1 : 0, l = abs(lag);
    if (l < 1 || l > g->max_lag) {
        return -1.0f;
    }
    float sum = 0;
    for (int band = 2; band <= 3; ++band) {
        const int save = g->win[band];
        g->win[band] = g->rate / 1000 * PG_TWIN_WIN_MS;
        const float s = share(g, band, slave, -1, g->pos, l, NULL);
        g->win[band] = save;
        if (s < 0) {
            return -1.0f;
        }
        sum += s;
    }
    return sum * 0.5f;
}

static void bank_admit(coop_pgate_t *g, int lag) {
    pg_offset_t *o = bank_near(g, lag);
    if (o != NULL) {
        if (now_s(g) - o->last_match_s > 0.3) {   /* a pitch twin cannot pull a working offset off its sample */
            if (o->lag != lag) {
                g->stats.recentres++;
            }
            o->lag = o->home = lag;
            return;
        }
        /* PITCH TWIN, OR THE TWIN IS THE ONE WORKING. A lag one voice period off also matches a vowel, which is why a
         * candidate this close may not pull a working offset. But the working offset can itself be the twin -- locked
         * one period off, it matches vowels and loses every consonant and word ending -- or belong to another source a
         * few ms away (music). Over PG_TWIN_WIN_MS the pitch moves, so a twin's mid-band match decays while the true
         * lag's holds: a candidate that wins that test clearly gets its OWN slot; the working entry is left alone, so a
         * second source keeps its lag. Measured 2026-10-09 (a marine's "yes sir", 7760420a session): the copy sat at
         * 385.9 ms, the bank held 381.0 (one period off) and 396.6, and the 1.5-3.5 kHz match was 0.99 at the true lag
         * and ~0 at both; the end of the line leaked as an audible echo. */
        if (!P_TWIN || abs(o->lag - lag) < g->rate / 1000 * PG_TWIN_MIN_MS) {
            return;
        }
        const float mc = twin_match(g, lag);
        if (mc < PG_ADMIT) {
            return;
        }
        /* it must beat EVERY entry within the tolerance, or twins would split off one another */
        const int tol = g->rate / 1000 * PG_SAME_MS;
        float rival = -1.0f;
        for (int i = 0; i < COOP_PGATE_MAX_OFFSETS; ++i) {
            if (g->bank[i].used && abs(g->bank[i].lag - lag) <= tol) {
                const float m = twin_match(g, g->bank[i].lag);
                if (m > rival) {
                    rival = m;
                }
            }
        }
        if (mc < rival + PG_TWIN_MARGIN) {
            return;
        }
#ifdef COOP_PGATE_ENV_TUNING
        if (getenv("PGATE_TWIN_DEBUG")) {
            fprintf(stderr, "TWIN %.3f s: candidate %+.2f ms match %.2f beats best neighbour %.2f (nearest %+.2f ms)\n",
                    now_s(g), lag * 1000.0 / g->rate, mc, rival, o->lag * 1000.0 / g->rate);
        }
#endif
        g->stats.twin_splits++;
        o = NULL;   /* fall through to a new slot */
    }
    for (int i = 0; i < COOP_PGATE_MAX_OFFSETS && o == NULL; ++i) {
        if (!g->bank[i].used) {
            o = &g->bank[i];
        }
    }
    if (o == NULL) {
        o = &g->bank[0];
        for (int i = 1; i < COOP_PGATE_MAX_OFFSETS; ++i) {
            if (g->bank[i].last_match_s < o->last_match_s) {
                o = &g->bank[i];
            }
        }
        g->stats.evicted++;
    }
    memset(o, 0, sizeof(*o));
    o->used = true;
    o->lag = o->home = lag;
    o->created_s = o->last_match_s = now_s(g);
    o->last_hi_match_s = o->last_mid_match_s = -1e9;
    g->stats.admitted++;
}

static void search(coop_pgate_t *g) {
    if (g->dpos <= (unsigned) g->search_win) return;
    g->stats.searches++;
    const unsigned long long available = g->dpos - g->search_win;
    const int ld = available < (unsigned)g->ld ? (int)available : g->ld, n = g->fft.n;
    /* BOTH DIRECTIONS IN THREE TRANSFORMS (was six): direction d correlates stream d's newest window (x_d) with the
     * other stream's history (y_d). One complex FFT of x_0 + i*x_1 and one of y_0 + i*y_1 give all four spectra
     * (real signals: X0[k] = (Z[k] + conj Z[n-k]) / 2, X1[k] = (Z[k] - conj Z[n-k]) / 2i); the two ordinary
     * cross-spectra are Hermitian, so one inverse FFT of C0 + i*C1 returns both correlations (real / imaginary). */
    memset(g->fr, 0, sizeof(float) * (size_t) n);
    memset(g->fi, 0, sizeof(float) * (size_t) n);
    memset(g->gr, 0, sizeof(float) * (size_t) n);
    memset(g->gi, 0, sizeof(float) * (size_t) n);
    const unsigned long long x0 = g->dpos - g->search_win, y0 = g->dpos - g->search_win - (unsigned long long) ld;
    double ex[2] = {0, 0}, ey[2] = {0, 0};
    for (int i = 0; i < g->search_win; ++i) {
        g->fr[i] = g->dec[0][(x0 + (unsigned) i) & g->dmask];
        g->fi[i] = g->dec[1][(x0 + (unsigned) i) & g->dmask];
        ex[0] += (double) g->fr[i] * g->fr[i];
        ex[1] += (double) g->fi[i] * g->fi[i];
    }
    /* Both directions would fail the identical energy test below. Still expire idle offsets. */
    if (ex[0] < PG_FLOOR_MS2 * g->search_win && ex[1] < PG_FLOOR_MS2 * g->search_win)
        goto expire_offsets;
    for (int i = 0; i < g->search_win + ld; ++i) {
        g->gr[i] = g->dec[1][(y0 + (unsigned) i) & g->dmask];   /* history of the OTHER stream, for direction 0 */
        g->gi[i] = g->dec[0][(y0 + (unsigned) i) & g->dmask];   /* ... and for direction 1 */
        ey[0] += (double) g->gr[i] * g->gr[i];
        ey[1] += (double) g->gi[i] * g->gi[i];
    }
    fft_run(&g->fft, g->fr, g->fi, false);
    fft_run(&g->fft, g->gr, g->gi, false);
    for (int k = 0; k <= n / 2; ++k) {
        const int kn = (n - k) & (n - 1);
        const float zr = g->fr[k], zi = g->fi[k], zr2 = g->fr[kn], zi2 = g->fi[kn];
        const float wr = g->gr[k], wi = g->gi[k], wr2 = g->gr[kn], wi2 = g->gi[kn];
        /* X0 = (Z + conj Zn)/2, X1 = (Z - conj Zn)/(2i); same for Y0, Y1 from W */
        const float x0r = 0.5f * (zr + zr2), x0i = 0.5f * (zi - zi2), x1r = 0.5f * (zi + zi2), x1i = -0.5f * (zr - zr2);
        const float y0r = 0.5f * (wr + wr2), y0i = 0.5f * (wi - wi2), y1r = 0.5f * (wi + wi2), y1i = -0.5f * (wr - wr2);
        /* Unweighted cross-spectra; candidate scores normalized by local time-domain energy below. */
        float c0r = x0r * y0r + x0i * y0i, c0i = x0r * y0i - x0i * y0r;
        float c1r = x1r * y1r + x1i * y1i, c1i = x1r * y1i - x1i * y1r;

        /* pack C0 + i*C1 at k, and its Hermitian mirror at n-k */
        g->fr[k] = c0r - c1i;
        g->fi[k] = c0i + c1r;
        if (kn != k) {
            g->fr[kn] = c0r + c1i;
            g->fi[kn] = -c0i + c1r;
        }
    }
    fft_run(&g->fft, g->fr, g->fi, true);   /* fr = direction 0's correlation, fi = direction 1's */
    for (int later = 0; later < 2; ++later) {
        if (ex[later] < PG_FLOOR_MS2 * g->search_win || ey[later] < 1.0) {
            continue;
        }
        float *corr = later == 0 ? g->fr : g->fi;
        /* LOCAL HISTORICAL ENERGY: normalize every candidate using exactly its matched window.
         * Ranks candidates; sample refinement and final gain rules are unchanged. Admission additionally
         * requires the same-lag corroboration below. */
        double history_energy = 0;
        for (int j = 0; j < g->search_win; ++j) {
            const double v = g->dec[1 - later][(y0 + (unsigned) j) & g->dmask];
            history_energy += v * v;
        }
        for (int q = 0; q <= ld; ++q) {
            const double cross = (double) corr[q] / n; /* fft_run inverse is unnormalized */
            if (cross > 0 && history_energy >= PG_FLOOR_MS2 * g->search_win) {
                const double score = cross * cross / (ex[later] * history_energy);
                corr[q] = (float) (score < 1.0 ? score : 1.0);
            } else {
                corr[q] = -1.0f;
            }
            if (q < ld) {
                const double before = g->dec[1 - later][(y0 + (unsigned) q) & g->dmask];
                const double after = g->dec[1 - later][(y0 + (unsigned) q + g->search_win) & g->dmask];
                history_energy += after * after - before * before;
            }
        }
        int found[PG_PEAKS], nf = 0;
        const int sep = g->rate / 1000 * 3 / PG_DEC + 1;
        for (int p = 0; p < PG_PEAKS; ++p) {
            int bs = -1;
            float best = -1e30f;
            for (int s = 0; s <= ld; ++s) {
                bool taken = false;
                for (int q = 0; q < nf && !taken; ++q) {
                    taken = abs(s - found[q]) <= sep;
                }
                if (!taken && corr[s] > best) {
                    best = corr[s];
                    bs = s;
                }
            }
            if (bs < 0 || best <= 0) {
                break;
            }
            found[nf++] = bs;
            /* refine to the sample on the mono mid bands (600-3500 Hz), 20 ms window ending now */
            const int coarse = (ld - bs) * PG_DEC;
            float bsh = -1;
            int blag = coarse;
            for (int band = 2; band <= 3; ++band) {
                const int w_save = g->win[band];
                g->win[band] = g->rate / 1000 * 20;
                /* coarse-to-fine: every 2nd sample across +-PG_DEC, then the neighbours of the best (11 matches
                 * instead of 17; the 600-3500 Hz match varies smoothly over 2 samples) */
                float bb2 = -1;
                int bl2 = coarse;
                for (int l = coarse - PG_DEC; l <= coarse + PG_DEC; l += 2) {
                    if (l < 1 || l > g->max_lag) {
                        continue;
                    }
                    const float sh = share(g, band, later, -1, g->pos, l, NULL);
                    if (sh > bb2) {
                        bb2 = sh;
                        bl2 = l;
                    }
                }
                for (int l = bl2 - 1; l <= bl2 + 1; l += 2) {
                    if (l < 1 || l > g->max_lag) {
                        continue;
                    }
                    const float sh = share(g, band, later, -1, g->pos, l, NULL);
                    if (sh > bb2) {
                        bb2 = sh;
                        bl2 = l;
                    }
                }
                if (bb2 > bsh) {
                    bsh = bb2;
                    blag = bl2;
                }
                g->win[band] = w_save;
            }
            /* ... or on the bass alone when it is a near-exact copy over 40 ms (run 3's music layer had no higher
             * band at its offset); the bass needs the stronger bar because a hum matches at many offsets */
            if (bsh < PG_ADMIT) {
                const int w_save = g->win[0];
                g->win[0] = g->rate / 1000 * 40;
                for (int l = coarse - PG_DEC; l <= coarse + PG_DEC; ++l) {
                    if (l < 1 || l > g->max_lag) {
                        continue;
                    }
                    const float sh = share(g, 0, later, -1, g->pos, l, NULL);
                    if (sh >= PG_BASS_ALONE && sh > bsh) {
                        bsh = sh;
                        blag = l;
                    }
                }
                g->win[0] = w_save;
            }
            /* Candidate corroboration: both mid bands at the SAME refined lag must
             * support closure. The original near-exact bass-only fallback remains valid. */
            bool corroborated = false;
            if (bsh >= PG_ADMIT) {
                const int save2 = g->win[2], save3 = g->win[3];
                g->win[2] = g->win[3] = g->rate / 1000 * 20;
                const float s2 = share(g, 2, later, -1, g->pos, blag, NULL);
                const float s3 = share(g, 3, later, -1, g->pos, blag, NULL);
                g->win[2] = save2; g->win[3] = save3;
                corroborated = s2 >= close_thr(g, 2) && s3 >= close_thr(g, 3);
                if (!corroborated) {
                    const int saved = g->win[0];
                    g->win[0] = g->rate / 1000 * 40;
                    corroborated = share(g, 0, later, -1, g->pos, blag, NULL) >= PG_BASS_ALONE;
                    g->win[0] = saved;
                }
            }
            if (bsh >= PG_ADMIT && corroborated) {
                bank_admit(g, later == 1 ? blag : -blag);
            }
        }
    }
expire_offsets:
    {
        const double t = now_s(g);
        for (int i = 0; i < COOP_PGATE_MAX_OFFSETS; ++i) {
            if (g->bank[i].used && t - g->bank[i].last_match_s > PG_EXPIRE_S) {
                g->bank[i].used = false;
                g->stats.evicted++;
            }
        }
    }
}

/* ---------------------------------------------------------------- one block */

static void process_block(coop_pgate_t *g, unsigned long long end) {
    const int B = g->block, ch = g->ch, nch = P_STEREO ? ch : 1;
    const unsigned long long start = end - (unsigned long long) B;
    const double t = now_s(g);
    const double hold_s = P_HOLD_MS / 1000.0;
    g->stats.blocks++;
    const bool valid_now[2] = {range_valid(g, 0, start, B), range_valid(g, 1, start, B)};
    for (int s = 0; s < 2; ++s) if (!valid_now[s]) {
        memset(g->hold[s], 0, sizeof(g->hold[s]));
        memset(g->armed[s], 0, sizeof(g->armed[s]));
        memset(g->challenge[s], 0, sizeof(g->challenge[s]));
        for (int i = 0; i < COOP_PGATE_MAX_OFFSETS; ++i)
            if ((g->bank[i].lag > 0 ? 1 : 0) == s) g->bank[i].pred_pending = 0;
    }
    float best[2][2][NB];
    bool pred[2][2][NB];
    bool reference_ok[2][NB] = {{false}};
    for (int s = 0; s < 2; ++s) {
        for (int c = 0; c < 2; ++c) {
            for (int b = 0; b < NB; ++b) {
                best[s][c][b] = -1.0f;
                pred[s][c][b] = false;
            }
        }
    }
    /* QUIET BANDS: a band whose window is silent in a stream cannot match at any offset (share() would return "too
     * quiet"), so it is decided once here from the 1 ms record instead of once per offset */
    bool loud[2][3][NB];
    double sxx_c[2][3][NB];   /* each stream's window energy per channel/mono and band, as share() would sum it */
    for (int s = 0; s < 2; ++s) {
        for (int c = 0; c < 3; ++c) {
            for (int b = 0; b < NB; ++b) {
                const double e = band_energy(g, s, c == 2 ? -1 : c, b, end - (unsigned long long) g->win[b], g->win[b]);
                loud[s][c][b] = (c < 2 && c >= ch) ? false : e >= 0.5 * PG_FLOOR_MS2 * g->win[b];
                sxx_c[s][c][b] = -1.0;   /* computed on first use: most combinations are never needed */
            }
        }
    }
    /* the slave/master gain of a copy, normalised by the learned balance: [slave] multiplies the least-squares gain */
    const float copy_max = P_COPY_MAX_DB > 0 ? powf(10.0f, P_COPY_MAX_DB / 20.0f) : 0.0f;
    const float bal_norm[2] = {powf(10.0f, (float) g->bal_db / 20.0f), powf(10.0f, (float) -g->bal_db / 20.0f)};
    for (int i = 0; i < COOP_PGATE_MAX_OFFSETS; ++i) {
        pg_offset_t *o = &g->bank[i];
        if (!o->used || !valid_now[o->lag > 0 ? 1 : 0]) {
            continue;
        }
        if (o->lag != o->home && t - o->last_match_s > hold_s) {
            o->lag = o->home;   /* the line is over: the next one starts where lines start */
        }
        const int slave = o->lag > 0 ? 1 : 0;
        const int lag = abs(o->lag);
        if (end < (unsigned long long) (lag + B)) {
            continue;
        }
        for (int b = 0; b < NB; ++b) {
            const int w = matching_window(g, b, slave, end, lag);
            if (w) reference_ok[slave][b] |= range_valid(g, 1 - slave,
                end - (unsigned) lag - (unsigned) w, w);
        }
        /* SENTINEL: most offsets are idle most of the time. One that has not matched for 2 s checks only the mono
         * 600-3500 Hz bands (where a line starts) and does the full per-channel check only when either shows the copy
         * (>= the open threshold) -- in the same block, so a line is not delayed. */
        bool skip_full = false;
        if (P_SENTINEL && t - o->last_match_s > 2.0 && o->pred_pending == 0) {
            const float s2 = loud[slave][2][2] ? share(g, 2, slave, -1, end, lag, NULL) : -1.0f;
            const float s3 = loud[slave][2][3] ? share(g, 3, slave, -1, end, lag, NULL) : -1.0f;
            skip_full = s2 < open_thr(g, 2) && s3 < open_thr(g, 3);
            if (skip_full) {
                g->stats.sentinel_skips++;
            }
        }
        float v[2][NB];
        bool forced[2][NB];
        memset(forced, 0, sizeof(forced));
        bool hi_match = false, any_match = false;
        int track_band = -1;
        float track_best = -1;
        for (int c = 0; c < nch; ++c) {
            const int cc = P_STEREO ? c : -1;
            for (int b = 0; b < NB; ++b) {
                float gl = 0;
                if (!skip_full && loud[slave][cc < 0 ? 2 : cc][b]) {
                    double *sx = &sxx_c[slave][cc < 0 ? 2 : cc][b];
                    if (*sx < 0) {
                        *sx = slave_energy(g, b, slave, cc, end);
                    }
                    v[c][b] = share_x(g, b, slave, cc, end, lag, &gl, *sx);
                    /* PLAYED-COPY GUARD: the earlier copy this block would be removed for must itself have been played.
                     * When it was closed (a repeat matched against an earlier repeat), closing this one too loses the sound. */
                    if (P_PLAYED_GUARD && v[c][b] >= 0 && lag >= B && end >= (unsigned long long) (B + g->ahead + lag)) {
                        const unsigned long long ob = (end - (unsigned) B - (unsigned) g->ahead - (unsigned) lag) / (unsigned) B;
                        const unsigned k = (unsigned) (ob & g->played_mask);
                        if (g->played_tag[k] == ob + 1 && g->played[k][1 - slave][c][b] < 0.5f) {
                            v[c][b] = 0.0f;
                        }
                    }
                    /* COPY LEVEL: a copy much quieter than the later sound (after the hosts' learned balance) is not what
                     * that sound is made of -- a distant version of a near gunshot. Refuse it. */
                    if (copy_max > 0 && v[c][b] >= 0 && gl > 0 && gl * bal_norm[slave] > copy_max) {
                        v[c][b] = 0.0f;
                    }
                } else {
                    v[c][b] = -1.0f;
                }
                if (b >= 2 && v[c][b] >= close_thr(g, b)) {
                    hi_match = true;
                }
                if (b >= 1 && b <= 4 && v[c][b] > track_best) {
                    track_best = v[c][b];
                    track_band = b;
                }
                /* LEVEL BALANCE from clean matches only: a near-exact copy in a mid band gives the hosts' level
                 * ratio with little dilution from either side's own sound */
                if (b >= 1 && b <= 4 && v[c][b] >= 0.90f && gl > 0) {
                    double ratio_db = 20.0 * log10(gl);   /* slave / master */
                    if (slave == 0) {
                        ratio_db = -ratio_db;            /* expressed as peer / self */
                    }
                    const double a = g->bal_n < 50 ? 1.0 / (g->bal_n + 1) : 0.01;
                    g->bal_db += a * (ratio_db - g->bal_db);
                    g->bal_n++;
                }
            }
        }
        /* FAST TRACKING: a line played at a slightly different speed on one PC (random pitch variation on barks is
         * common) moves the offset ~5 samples per block at 2%; the drift tracker below follows 1 sample per 3 blocks.
         * So an offset that matched within the last 100 ms but has lost its match while the master is still sounding
         * searches +-8 samples on the 600-1500 Hz band and jumps to a real match. */
        if (P_FAST_TRACK && t - o->last_match_s <= 0.1 && t - o->last_match_s > 0.0 && track_best < close_thr(g, 2)) {
            float fb = -1;
            int fl = 0;
            /* the copy moves ~1 sample per ms at 2%, so the search widens with the time since the last match */
            const int R = 8 + (int) ((t - o->last_match_s) * 1000.0 * 1.2);
            const int Rm = R > 48 ? 48 : R;
            for (int d = -Rm; d <= Rm; ++d) {
                if (d == 0 || lag + d < 1) {
                    continue;
                }
                const float vv = share(g, 2, slave, P_STEREO ? 0 : -1, end, lag + d, NULL);
                if (vv > fb) {
                    fb = vv;
                    fl = d;
                }
            }
            if (fb >= close_thr(g, 2)) {
                o->lag += o->lag > 0 ? fl : -fl;
                g->stats.fast_tracks++;
                for (int c = 0; c < nch; ++c) {   /* re-measure every band at the new offset */
                    for (int b = 0; b < NB; ++b) {
                        v[c][b] = share(g, b, slave, P_STEREO ? c : -1, end, abs(o->lag), NULL);
                    }
                }
                track_best = fb;
                track_band = 2;
                hi_match = true;
            }
        }
        /* drift: followed on whichever mid band matches best (not one fixed band -- run 3's layers sat in different
         * bands) */
        if (track_band >= 0 && track_best >= close_thr(g, track_band)) {
            float bd = track_best + (P_DRIFT_VOTES > 1 ? 0.02f : 0.0f);   /* a neighbour must be clearly better ... */
            int dl = 0;
            for (int d = -1; d <= 1; d += 2) {
                const float vv = share(g, track_band, slave, P_STEREO ? 0 : -1, end, lag + d, NULL);
                if (vv > bd) {
                    bd = vv;
                    dl = d;
                }
            }
            /* ... three blocks running in the same direction (measured: without this the offset stepped ~20x/s on
             * noise, while the real clock drift is under 0.25 samples/s) */
            if (dl != 0 && dl == o->drift_dir) {
                o->drift_vote++;
            } else {
                o->drift_vote = dl != 0 ? 1 : 0;
                o->drift_dir = dl;
            }
            if (dl != 0 && o->drift_vote >= P_DRIFT_VOTES) {
                o->lag += o->lag > 0 ? dl : -dl;
                if (abs(o->lag - o->home) <= 2) {   /* slow clock drift: the line start moves with it */
                    o->home = o->lag;
                }
                o->drift_vote = 0;
                g->stats.drift_steps++;
            }
        }
        if (hi_match) {
            o->last_hi_match_s = t;
            g->last_hi_any_s = t;
        }
        for (int c = 0; c < nch; ++c) {
            for (int b = 1; b <= 3; ++b) {
                if (v[c][b] >= close_thr(g, b)) {
                    o->last_mid_match_s = t;
                }
            }
            if (P_NASAL_WITNESS && v[c][1] >= close_thr(g, 1)) {   /* "m", "n": almost nothing above 600 Hz */
                o->last_hi_match_s = t;
            }
        }
        /* ENVELOPE LINE: a whispered line or a breath has no waveform copy anywhere (each game renders the noise
         * afresh), so nothing above can start a line. At a KNOWN offset, three or more bands whose envelopes move
         * (>= 6 dB over 20 ms) and match (>= 0.85) together count as a line: unrelated sounds do not line up across
         * three bands at one exact offset, and steady noise is excluded by the movement requirement. */
        if (P_ENV_LINE) {
            for (int c = 0; c < nch; ++c) {
                int nb_env = 0;
                bool envm[NB] = {false};
                for (int b = 1; b < NB; ++b) {
                    const int cc = P_STEREO ? c : -1;
                    if (!loud[slave][cc < 0 ? 2 : cc][b]) {
                        continue;
                    }
                    if (env_range_db(g, b, slave, cc, end) >= 6.0f && env_corr(g, b, slave, cc, end, lag) >= 0.85f) {
                        envm[b] = true;
                        nb_env++;
                    }
                }
                if (nb_env >= 3) {
                    g->stats.env_lines++;
                    o->last_mid_match_s = t;
                    for (int b = 1; b < NB; ++b) {
                        if (envm[b] && (v[c][b] < close_thr(g, b))) {
                            v[c][b] = close_thr(g, b);
                            forced[c][b] = true;
                        }
                    }
                }
            }
        }
        /* THE TREBLE DURING A LINE: while this offset is matching in the mid bands (a line is being removed here),
         * the treble bands also close on an ENVELOPE match -- the hiss of an "s" keeps its loudness shape but not
         * its waveform. Outside a line it never applies, so a gunshot's envelope cannot close anything. */
        if (P_SIBILANT && t - o->last_mid_match_s <= hold_s) {
            for (int c = 0; c < nch; ++c) {
                for (int b = P_ENV_ALL ? 1 : 4; b < NB; ++b) {
                    if (v[c][b] >= 0 && v[c][b] < close_thr(g, b) &&
                        env_corr(g, b, slave, P_STEREO ? c : -1, end, lag) >= P_ENV) {
                        v[c][b] = close_thr(g, b);
                        forced[c][b] = true;
                        g->stats.env_forced++;
                    }
                }
            }
        }
        /* the BASS needs a witness: a higher band matching at this offset or -- since run 3, where the treble was a
         * copy at a DIFFERENT offset from the bass -- at any offset; or, on its own, a near-exact match held for
         * PG_BASS_RUN blocks (a hum matches at many offsets, so alone it needs both) */
        const bool witness = t - o->last_hi_match_s <= hold_s || (P_ANYWITNESS && t - g->last_hi_any_s <= hold_s);
        for (int c = 0; c < nch; ++c) {
            if (v[c][0] >= PG_BASS_ALONE) {
                o->bass_run[c]++;
            } else {
                o->bass_run[c] = 0;
            }
            const bool bass_ok = witness || (P_BASSALONE && o->bass_run[c] >= PG_BASS_RUN);
            if (!bass_ok) {
                v[c][0] = -1.0f;
            }
            for (int b = 0; b < NB; ++b) {
                if (v[c][b] > best[slave][c][b]) {
                    best[slave][c][b] = v[c][b];
                    if (v[c][b] >= close_thr(g, b)) {
                        g->close_lag[slave][c][b] = lag;
                    }
                }
                any_match |= v[c][b] >= close_thr(g, b);
            }
        }
        if (any_match) {
            o->last_match_s = t;
            if (o->pred_pending > 0) {
                o->pred_confirmed = true;
            }
        }
        /* an offset's PREDICTION RECORD resets only on a confirmed prediction -- not on any match: the gameplay bed's
         * offset matches all the time, yet nearly every onset it predicted (gunfire) never arrived as a copy */
        if (o->pred_pending > 0 && --o->pred_pending == 0) {
            if (!o->pred_confirmed) {
                o->pred_fails++;
                o->last_pred_fail_s = t;
                g->stats.pred_failed++;
            } else {
                o->pred_fails = 0;
                g->stats.pred_confirmed++;
            }
        }
        if (o->pred_fails >= P_PRED_FAILS && t - o->last_pred_fail_s > 5.0) {
            o->pred_fails = P_PRED_FAILS - 1;   /* after 5 s quiet, one more chance */
        }
        /* CEILING: each band's best achievable match, learned from blocks where some higher band matched; rises
         * fast and decays slowly, so partial copies in mixed audio cannot drag it down */
        /* ...and only from NEAR-PURE moments (some band >= 0.85): measured, learning from every witnessed block let a
         * long stretch of partial copies drag every ceiling to ~0.6 and the threshold with it, and chance matches in
         * loading/gameplay audio began closing (-2.4..-3 dB, 50 flips/s) */
        bool pure = false;
        for (int c = 0; c < nch && !pure; ++c) {
            for (int b = 1; b < NB && !pure; ++b) {
                pure = v[c][b] >= 0.85f;
            }
        }
        if (P_CEIL && hi_match && pure) {
            for (int c = 0; c < nch; ++c) {
                for (int b = 0; b < NB; ++b) {
                    if (v[c][b] < 0 || forced[c][b]) {   /* envelope-forced matches say nothing about the ceiling */
                        continue;
                    }
                    const float a = v[c][b] > g->ceil[b] ? 0.05f : 0.002f;
                    g->ceil[b] += a * (v[c][b] - g->ceil[b]);
                    if (g->ceil[b] < 0.75f) {
                        g->ceil[b] = 0.75f;
                    }
                }
            }
        }
        /* PREDICTION: the earlier copy is |lag| ahead; an onset in its next two blocks closes the slave now */
        if (g->pred && lag >= 3 * g->block && t - o->last_match_s < 60.0 && o->last_match_s > o->created_s &&
            (P_PRED_FAILS <= 0 || o->pred_fails < P_PRED_FAILS) && (!P_PRED_ONE || o->pred_pending == 0)) {
            const unsigned long long na = end - (unsigned long long) lag;
            if (!range_valid(g, 1 - slave, na - (unsigned) (4 * B), 6 * B)) continue;
            for (int c = 0; c < nch; ++c) {
                const int cc = P_STEREO ? c : -1;
                bool rise[NB] = {false};
                int nrise = 0;
                for (int b = 1; b < NB; ++b) {
                    const double en = band_energy(g, 1 - slave, cc, b, na, 2 * g->block) / 2.0;
                    const double ep = band_energy(g, 1 - slave, cc, b, na - (unsigned long long) (4 * g->block), 4 * g->block) / 4.0;
                    if (en > PG_FLOOR_MS2 * g->block * 10.0 && en > P_RISE * ep) {
                        rise[b] = true;
                        nrise++;
                    }
                }
                /* a line starts across several bands at once; a single-band bump mostly did not bring a copy
                 * (measured: 4324 of 4676 single-band predictions failed in runs 1-2) */
                if (nrise >= P_PRED_BANDS) {
                    for (int b = 1; b < NB; ++b) {
                        if (rise[b]) {
                            pred[slave][c][b] = true;
                            if (b <= 2) {
                                pred[slave][c][0] = true;   /* a low-mid onset witnesses the bass */
                            }
                        }
                    }
                    o->pred_pending = 8;   /* the copy must match within 40 ms or this counts as a failure */
                    o->pred_confirmed = false;
                }
            }
        }
    }
#ifdef COOP_PGATE_ENV_TUNING
    {   /* Offline trace: PGATE_TRACE="from,to" seconds -> stderr: bank, peer ch0 best shares, gate state */
        static int init = 0; static double tf = -1, tt = -1;
        if (!init) { const char *e = getenv("PGATE_TRACE"); if (e) sscanf(e, "%lf,%lf", &tf, &tt); init = 1; }
        if (t >= tf && t <= tt) {
            fprintf(stderr, "%.3f bank:", t);
            for (int i = 0; i < COOP_PGATE_MAX_OFFSETS; ++i) if (g->bank[i].used) fprintf(stderr, " %+.1f(m%.2f)", g->bank[i].lag * 1000.0 / g->rate, t - g->bank[i].last_match_s);
            fprintf(stderr, " | peer best:");
            for (int b = 0; b < NB; ++b) fprintf(stderr, " %.2f", best[1][0][b]);
            fprintf(stderr, " | thr:");
            for (int b = 0; b < NB; ++b) fprintf(stderr, " %.2f", close_thr(g, b));
            fprintf(stderr, " | self best:");
            for (int b = 0; b < NB; ++b) fprintf(stderr, " %.2f", best[0][0][b]);
            fprintf(stderr, " | peer gain L:");
            for (int b = 0; b < NB; ++b) fprintf(stderr, "%d", g->gain[1][0][b] < 0.5f ? 0 : 1);
            fprintf(stderr, " R:");
            for (int b = 0; b < NB; ++b) fprintf(stderr, "%d", g->gain[1][1][b] < 0.5f ? 0 : 1);
            fprintf(stderr, " | self gain L:");
            for (int b = 0; b < NB; ++b) fprintf(stderr, "%d", g->gain[0][0][b] < 0.5f ? 0 : 1);
            fprintf(stderr, " | peer R best:");
            for (int b = 0; b < NB; ++b) fprintf(stderr, " %.2f", best[1][1][b]);
            fprintf(stderr, "\n");
        }
    }
#endif
    /* LINKED CHANNELS: a centred copy (the narrator) is in both channels, but one channel's own positional sound can
     * hide it from that channel's match -- measured 2026-10-05, run 2 at 120.8 s: left 0.9+, right 0.1-0.7, and the
     * right channel's treble reopened mid-"s" and leaked it. So when one channel matches a band outright, the other
     * channel counts as matching too if its own share at least shows the copy is there (>= the open threshold). */
    if (P_LINK && nch == 2) {
        for (int s = 0; s < 2; ++s) {
            for (int b = 0; b < NB; ++b) {
                const float ct = close_thr(g, b), ot = open_thr(g, b);
                if (best[s][0][b] >= ct && best[s][1][b] >= ot && best[s][1][b] < ct) {
                    best[s][1][b] = ct;
                } else if (best[s][1][b] >= ct && best[s][0][b] >= ot && best[s][0][b] < ct) {
                    best[s][0][b] = ct;
                }
            }
        }
    }
    /* a stream in DROPOUT: this block is digital silence (< -90 dBFS) right after audible blocks (> -60 dBFS) */
    bool dropout[2] = {false, false};
    for (int s = 0; s < 2; ++s) {
        double now_e = 0, prev_e = 0;
        for (int b = 0; b < NB; ++b) {
            now_e += band_energy(g, s, -1, b, start, B);
            prev_e += band_energy(g, s, -1, b, start - (unsigned long long) (4 * B), 4 * B) / 4.0;
        }
        const double fs = 32768.0 * 32768.0 * B;
        /* ...or the block ENDS in 2 ms of digital silence (a dropout starting mid-block) */
        const int tail = g->rate / 500;
        double tail_e = 0;
        for (int b = 0; b < NB; ++b) {
            tail_e += band_energy(g, s, -1, b, end - (unsigned long long) tail, tail);
        }
        dropout[s] = prev_e > fs * 1e-6 && (now_e < fs * 1e-9 || tail_e < 32768.0 * 32768.0 * tail * 1e-9);
        if (dropout[s]) {
            g->stats.dropouts++;
        }
    }
    /* gates per stream, channel, band */
    const int hold_blocks = P_HOLD_MS / PG_BLOCK_MS;
    float g0[2][2][NB], g1[2][2][NB];
    unsigned char why[2][2][NB];
    for (int c = 0; c < 2; ++c) {
        const int cs = P_STEREO ? (c < nch ? c : 0) : 0;   /* mono matching: both channels follow the mono decision */
        for (int b = 0; b < NB; ++b) {
            bool want[2];
            for (int s = 0; s < 2; ++s) {
                const float bv = best[s][cs][b];
                if (g->io_mode == 2 && !reference_ok[s][b]) {
                    g->hold[s][c][b] = g->armed[s][c][b] = 0;
                    pred[s][cs][b] = false;
                }
                if (g->armed[s][c][b] > 0) {   /* confirming a predicted close: a modest match is enough */
                    g->armed_age[s][c][b]++;
                    if (bv >= open_thr(g, b)) {
                        g->hold[s][c][b] = hold_blocks;
                        g->armed[s][c][b] = 0;
                    } else {
                        g->armed[s][c][b]--;
                        /* LANDING CHECK: the copy lands within two blocks of the prediction, and when it does the
                         * slave's band rises just as the master's did. No rise by the third block = no copy: reopen
                         * now instead of holding the band shut until the waveform check gives up (measured: thousands
                         * of guesses per 3 minutes never arrived, each muting a band for 15-50 ms) */
                        if (P_LANDING && g->armed_age[s][c][b] >= P_LANDING_AGE) {
                            const double now_e = band_energy(g, s, P_STEREO ? c : -1, b, start, B);
                            if (now_e < 2.0 * g->arm_ref[s][c][b] + PG_FLOOR_MS2 * B) {
                                g->armed[s][c][b] = 0;
                            }
                        }
                    }
                }
                /* a PREDICTION never takes a band the other stream has been holding closed for a while (a sustained
                 * bed such as music): that stream is the copy being removed there, and a guess the other way round is
                 * what flipped sustained music between the two copies (180-187 s trace: 736 of 839 immediate swaps
                 * were predictions). "A while", not "recently": brief false closes are everywhere in gameplay and
                 * blocking on those delayed real line onsets (truth bench median 10 -> 20 ms). */
                const bool other_holds = (P_STICKY & 2) && g->closed_avg[1 - s][c][b] >= P_STICKY_BED;
                if (pred[s][cs][b] && g->hold[s][c][b] == 0 && g->armed[s][c][b] == 0 && !other_holds) {
                    /* the copy lands within two blocks; confirmation needs the band's window to fill with it */
                    g->armed[s][c][b] = 2 + (g->win[b] + B - 1) / B;
                    g->armed_age[s][c][b] = 0;
                    g->arm_ref[s][c][b] = band_energy(g, s, P_STEREO ? c : -1, b, start - (unsigned long long) (4 * B), 4 * B) / 4.0;
                }
                if (bv >= close_thr(g, b)) {
                    g->hold[s][c][b] = hold_blocks;
                } else if (g->hold[s][c][b] > 0 && bv < open_thr(g, b)) {
                    g->hold[s][c][b]--;
                }
                /* HOLD RELEASE: a band held shut after its match reopens as soon as the block it is about to remove carries
                 * sound the other stream did not play at the held lag (> 6 dB more over the band's window) -- a reload or a
                 * shot right after a shared sound. */
                if (P_HOLD_RELEASE && g->hold[s][c][b] > 0 && bv < close_thr(g, b)) {
                    const int lagh = g->close_lag[s][cs][b], w = g->win[b] > B ? g->win[b] : B;
                    if (lagh > 0 && start >= (unsigned long long) (g->ahead + lagh + w + B)) {
                        const unsigned long long os = start - (unsigned long long) g->ahead + (unsigned) B - (unsigned) w;
                        const int chh = P_STEREO ? c : -1;
                        const double es = band_energy(g, s, chh, b, os, w);
                        const double eo = band_energy(g, 1 - s, chh, b, os - (unsigned) lagh, w);
                        if (es > 4.0 * eo + PG_FLOOR_MS2 * w) {
                            g->hold[s][c][b] = 0;
                        }
                    }
                }
                want[s] = g->hold[s][c][b] > 0 || g->armed[s][c][b] > 0;
            }
            /* DROPOUT FILL: if the OTHER stream has just dropped to digital silence (a lost stretch of its audio --
             * measured run 3 at 302.60 s: PC2 zeros for ~20 ms while VEGA, a 143 ms-late copy, was gated, leaving
             * a hole), this stream's copy is the only sound there is: let it through for those blocks */
            if (P_DROPOUT && g->io_mode == 1) {
                for (int s = 0; s < 2; ++s) {
                    if (want[s] && dropout[1 - s]) {
                        want[s] = false;
                    }
                }
            }
            if (P_CONFLICT && want[0] && want[1]) {   /* never both streams: the better match stays closed */
                int lose = best[0][cs][b] >= best[1][cs][b] ? 1 : 0;
                /* INCUMBENCY. "Better match" is this block's: a stream holding the band (between matches, or on a
                 * prediction) lost it to ANY momentary match in the other one, and the band then played the other
                 * copy -- hundreds of ms earlier or later. On sustained shared music that is constant: music matches
                 * itself at many lags, and the stream being removed flipped back and forth (user 2026-10-09: "rather
                 * than just picking the one audio track it is jumping between them leaving audible distortions each
                 * time"; trace 180-187 s of the 7760420a combat excerpt). The stream that held the band keeps it
                 * unless the challenger matches strongly while the holder does not, PG_STICKY_BLOCKS in a row -- a
                 * real change of leader (the next line from the other PC) still switches within 15 ms. */
                const bool held0 = g->gain[0][c][b] < 1.0f, held1 = g->gain[1][c][b] < 1.0f;
                if ((P_STICKY & 1) && held0 != held1) {
                    const int inc = held0 ? 0 : 1, chal = 1 - inc;
                    if (best[chal][cs][b] >= close_thr(g, b) && best[inc][cs][b] < open_thr(g, b)) {
                        g->challenge[chal][c][b]++;
                    } else {
                        g->challenge[chal][c][b] = 0;
                    }
                    if (g->challenge[chal][c][b] >= PG_STICKY_BLOCKS) {
                        lose = inc;
                        g->challenge[chal][c][b] = 0;
                        g->stats.side_switches++;
                    } else {
                        lose = chal;
                        g->stats.sticky_holds++;
                    }
                }
                want[lose] = false;
                g->hold[lose][c][b] = g->armed[lose][c][b] = 0;
            } else {
                g->challenge[0][c][b] = g->challenge[1][c][b] = 0;
            }
            for (int s = 0; s < 2; ++s) {
                g0[s][c][b] = valid_now[s] && (g->io_mode == 1 || reference_ok[s][b]) ? g->gain[s][c][b] : 1.0f;
                /* a close that only a PREDICTION holds (copy not yet confirmed) dips instead of muting: a wrong guess
                 * costs -12 dB for a few ms, not a dropout */
                g1[s][c][b] = !want[s] ? 1.0f
                            : (g->hold[s][c][b] == 0 && g->armed[s][c][b] > 0 && best[s][cs][b] < close_thr(g, b)) ? P_PRE_GAIN
                            : 0.0f;
                why[s][c][b] = !want[s] ? 0 : best[s][cs][b] >= close_thr(g, b) ? 1 : g->armed[s][c][b] > 0 ? 3 : 2;
                g->gain[s][c][b] = g1[s][c][b];
                g->closed_avg[s][c][b] += ((g1[s][c][b] < 1.0f ? 1.0f : 0.0f) - g->closed_avg[s][c][b]) *
                                          ((float) PG_BLOCK_MS / PG_STICKY_AVG_MS);
            }
        }
    }
    /* the block these gains are applied to: `ahead` frames behind the newest (see coop_pgate_create) */
    const unsigned long long ostart = start - (unsigned long long) g->ahead;
    if (start >= (unsigned long long) g->ahead) {   /* what this output block finally plays: the played-copy guard reads it */
        const unsigned long long ob = ostart / (unsigned) B;
        const unsigned k = (unsigned) (ob & g->played_mask);
        g->played_tag[k] = ob + 1;
        memcpy(g->played[k], g1, sizeof(g->played[k]));
    }
    if (g->hook != NULL && start >= (unsigned long long) g->ahead) {
        g->hook(g->hook_ctx, ostart, B, (const float (*)[2][NB]) g0, (const float (*)[2][NB]) g1,
                (const unsigned char (*)[2][NB]) why);
    }
    /* level balance: meet in the middle once enough clean matches have been seen; clamped to +-12 dB */
    const float bg0[2] = {g->bal_g[0], g->bal_g[1]};
    if (P_BALANCE && g->bal_n >= 20) {
        const double d = g->bal_db > 12 ? 12 : (g->bal_db < -12 ? -12 : g->bal_db);
        g->bal_g[0] = (float) pow(10.0, d / 40.0);
        g->bal_g[1] = (float) pow(10.0, -d / 40.0);
    }
    /* SHARED-CONTENT RESTORE (opt-in, restore_gain > 1): while one stream's band is closed on a copy, that band's
     * shared content plays once instead of twice. Lift the OTHER stream's same band by up to restore_gain, scaled by how
     * closed the copy is, so shared content such as dialogue keeps the loudness two copies had, without the echo. Unique
     * sound in the lifted band is lifted too. Ramped over the block like the gate gains; 1.0 everywhere when off, so the
     * output is unchanged. Not part of the hook, which describes echo removal only. */
    float r0[2][2][NB], r1[2][2][NB];
    for (int s = 0; s < 2; ++s) {
        for (int c = 0; c < 2; ++c) {
            for (int b = 0; b < NB; ++b) {
                const float closed = valid_now[1 - s] ? 1.0f - g1[1 - s][c][b] : 0.0f;
                r0[s][c][b] = g->restore_prev[s][c][b];
                r1[s][c][b] = 1.0f + (g->restore_gain - 1.0f) * (closed > 0.0f ? closed : 0.0f);
                g->restore_prev[s][c][b] = r1[s][c][b];
            }
        }
    }
    bool open_all[2][2];
    for (int s = 0; s < 2; ++s) {
        for (int c = 0; c < 2; ++c) {
            open_all[s][c] = true;
            for (int b = 0; b < NB; ++b) {
                open_all[s][c] = open_all[s][c] && g0[s][c][b] == 1.0f && g1[s][c][b] == 1.0f &&
                                 r0[s][c][b] == 1.0f && r1[s][c][b] == 1.0f;
            }
        }
    }
    for (int s = 0; s < 2 && start >= (unsigned long long) g->ahead; ++s) {
        for (int i = 0; i < B; ++i) {
            const float f = (float) (i + 1) / (float) B;
            const float bal = valid_now[s] ? bg0[s] + (g->bal_g[s] - bg0[s]) * f : 1.0f;
            const unsigned long long t2 = ostart + (unsigned) i;
            for (int c = 0; c < ch; ++c) {
                float val;
                if (open_all[s][c]) {   /* every gate open all block: the bands sum back to the input exactly */
                    val = g->raw[s][(t2 & g->rmask) * (unsigned) ch + (unsigned) c];
                } else {
                    val = 0;
                    for (int b = 0; b < NB; ++b) {
                        const float gg = g0[s][c][b] + (g1[s][c][b] - g0[s][c][b]) * f;
                        const float rr = r0[s][c][b] + (r1[s][c][b] - r0[s][c][b]) * f;
                        val += gg * rr * g->bandp[s][c][b][t2 & g->rmask];
                    }
                }
                g->out[s][(t2 & g->omask) * (unsigned) ch + (unsigned) c] = val * bal;
            }
        }
    }
}

/* ---------------------------------------------------------------- per call */

static void ingest(coop_pgate_t *g, const int16_t *self, const int16_t *peer, int frames, unsigned valid_streams) {
    const int ch = g->ch;
    const int16_t *io[2] = {self, peer};
    for (int i = 0; i < frames; ++i) {
        const unsigned long long t = g->pos;
        if (t % (unsigned) g->block == 0)
            g->valid_blocks[(t / (unsigned) g->block) & g->valid_mask] = (unsigned char) valid_streams;
        for (int s = 0; s < 2; ++s) {
            float m[NB] = {0}, mono = 0;
            const unsigned idx = (unsigned) (t & g->rmask) * (unsigned) ch;
            for (int c = 0; c < ch; ++c) {
                const float v = (float) io[s][i * ch + c];
                float bb[NB];
                coop_pgate_xover_run(&g->xo[s][c], v, bb);
                g->raw[s][idx + (unsigned) c] = v;
                for (int b = 0; b < NB; ++b) {
                    g->bandp[s][c][b][t & g->rmask] = bb[b];
                    g->facc[s][c][b] += bb[b] * bb[b];
                    m[b] += bb[b];
                }
                mono += v;
            }
            const float inv = 1.0f / (float) ch;
            for (int b = 0; b < NB; ++b) {
                const float mm = m[b] * inv;
                g->bandm[s][b][t & g->rmask] = mm;
                g->facc[s][2][b] += mm * mm;
            }
            g->dec_acc[s] += mono / (float) ch;
        }
        g->pos++;
        if (++g->fpos_in == g->rate / 1000) {   /* a 1 ms frame is complete */
            for (int s = 0; s < 2; ++s) {
                for (int c = 0; c < 3; ++c) {
                    for (int b = 0; b < NB; ++b) {
                        g->fen[s][c][b][g->fcount & g->fmask] = log10f(g->facc[s][c][b] + 1.0f);
                        g->fel[s][c][b][g->fcount & g->fmask] = g->facc[s][c][b];
                        g->facc[s][c][b] = 0;
                    }
                }
            }
            g->fcount++;
            g->fpos_in = 0;
        }
        if (++g->dec_n == PG_DEC) {
            for (int s = 0; s < 2; ++s) {
                g->dec[s][g->dpos & g->dmask] = g->dec_acc[s] / PG_DEC;
                g->dec_acc[s] = 0;
            }
            g->dpos++;
            g->dec_n = 0;
        }
        if (g->pos >= g->next_search) {
#ifdef PGATE_TIMING
            const double t0 = pg_now();
            search(g);
            pgate_t_search += pg_now() - t0;
#else
            search(g);
#endif
            g->next_search += (unsigned long long) (g->rate / 1000 * PG_SEARCH_MS);
        }
        if (g->pos >= g->next_block) {
#ifdef PGATE_TIMING
            const double t0 = pg_now();
            process_block(g, g->next_block);
            pgate_t_block += pg_now() - t0;
#else
            process_block(g, g->next_block);
#endif
            g->next_block += (unsigned long long) g->block;
        }
    }
}

void coop_pgate_process(coop_pgate_t *g, int16_t *self, int16_t *peer, int frames) {
    if (!g || !self || !peer || frames <= 0 || g->io_mode == 2) return;
    if (g->io_mode == 0) {   /* first streaming call: the asked-for quality starts (see coop_pgate_create_quality) */
        g->ahead = g->ahead_want;
        g->pred = g->pred_want;
    }
    g->io_mode = 1;
    ingest(g, self, peer, frames, COOP_PGATE_VALID_BOTH);
    const int ch = g->ch;
    int16_t *io[2] = {self, peer};
    for (int i = 0; i < frames; ++i) {
        const unsigned long long t = g->out_pos;
        for (int s = 0; s < 2; ++s) {
            for (int c = 0; c < ch; ++c) {
                float v = 0.0f;
                const unsigned long long lat = (unsigned long long) (g->block + g->ahead);
                if (t >= lat) {
                    v = g->out[s][((t - lat) & g->omask) * (unsigned) ch + (unsigned) c];
                }
                const long r = lrintf(v);
                io[s][i * ch + c] = (int16_t) (r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
            }
        }
        g->out_pos++;
    }
}

bool coop_pgate_process_ready_block(coop_pgate_t *g, int16_t *self, int16_t *peer, int frames) {
    return coop_pgate_process_ready_block_valid(g, self, peer, frames, COOP_PGATE_VALID_BOTH);
}

bool coop_pgate_process_ready_block_valid(coop_pgate_t *g, int16_t *self, int16_t *peer, int frames, unsigned valid_streams) {
    if (valid_streams & ~COOP_PGATE_VALID_BOTH) return false;
    if (g != NULL && g->io_mode == 0) {   /* the ready-block entry cannot hold blocks back: FAST, whatever was asked */
        g->ahead = 0;
        g->pred = P_PRED != 0;
    }
    if (!g || !self || !peer || frames != g->block || g->io_mode == 1 || g->ahead != 0 || g->pos != g->out_pos ||
        g->pos % (unsigned) g->block != 0) return false;
    const unsigned long long start = g->pos;
    g->io_mode = 2;
    ingest(g, self, peer, frames, valid_streams);
    int16_t *io[2] = {self, peer};
    for (int i = 0; i < frames; ++i) {
        for (int stream = 0; stream < 2; ++stream) {
            for (int c = 0; c < g->ch; ++c) {
                const float v = g->out[stream][((start + (unsigned) i) & g->omask) * (unsigned) g->ch + (unsigned) c];
                const long value = lrintf(v);
                io[stream][i * g->ch + c] = (int16_t) (value > 32767 ? 32767 : (value < -32768 ? -32768 : value));
            }
        }
    }
    g->out_pos = g->pos;
    return true;
}
