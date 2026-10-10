/* Co-op audio echo gate (multi-offset) -- see coop_audio_gate.h for what and why. PROTOTYPE. */
#include "coop_audio_gate.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define GT_PI 3.14159265358979323846
#define GT_N 512                 /* STFT size; latency = GT_N samples (10.7 ms at 48 kHz) */
#define GT_H 256
#define GT_BINS (GT_N / 2 + 1)
#define GT_DEC 4                 /* offset search runs on a 4x decimated mono copy (12 kHz) */
#define GT_WIN_D 2048            /* search window, decimated samples (~170 ms) */
#define GT_SEARCH_MS 50          /* how often the bank is offered new offsets */
#define GT_PEAKS 3               /* PHAT peaks examined per direction per search (one per layer) */
#define GT_PEAK_SEP_MS 3         /* peaks closer than this are the same layer */
#define GT_ADMIT_RHO 0.35f       /* a search peak needs at least this correlation to be tried as an offset */
#define GT_TRACK_MS 8            /* a sighting this close to a banked offset is that offset (layers seen 20+ ms apart) */
#define GT_RECENTRE 12           /* ... and re-centres it (samples) when it is not currently explaining anything */
#define GT_WARMUP 4              /* hops before an offset's coherence means anything (1 frame always reads 1.0) */
#define GT_PROVE_HOPS 16         /* a NEW offset must explain the audio for ~85 ms before it may cut anything --
                                  * the guard against one coincidental match (two identical gunshots) */
#define GT_BANK 8                /* proven offsets kept */
#define GT_TRIALS 6              /* candidates being tried; a new one displaces the weakest */
#define GT_SLOTS (GT_BANK + GT_TRIALS)
#define GT_TRIAL_S 0.5           /* an offset that has not proven itself within this long is dropped */
#define GT_EXPIRE_S 20.0         /* a proven offset that explains nothing for this long leaves the bank */
#define GT_LOUD_LO 2.0f          /* later/earlier power ratio per bin: up to +3 dB the later copy is cut fully, */
#define GT_LOUD_HI 4.0f          /* from +6 dB not at all -- a player's own loud sound is kept rather than traded
                                  * for the partner game's quieter, distant copy of it */
#define GT_SMOOTH 0.7f           /* per-hop smoothing of each offset's spectra (~6 frames, ~30 ms); less reads chance as coherence */
static int P_MODE = 4;   /* 4 = the configuration evaluated 10-04 (coherence + joint prediction, band gate) */
static float P_DOM_LO = 0.5f, P_DOM_HI = 0.7f, P_DOM_SMOOTH = 0.9f;
static float P_SH_LO = 0.3f, P_SH_HI = 0.6f;
static float P_COH_LO = 0.45f, P_COH_SPAN = 0.40f, P_ACT_ON = 0.50f, P_ACT_OFF = 0.32f, P_SMOOTH = 0.7f;
#define GT_COH_LO_DEFAULT 0.45f          /* coherence below this is chance (the ~6-frame estimate is biased up ~0.2) */
#define GT_COH_SPAN 0.40f        /* ... and at LO+SPAN a bin is a sure copy */
#define GT_ACT_ON 0.50f          /* energy-weighted coherence at which an offset engages */
#define GT_ACT_OFF 0.32f         /* ... and below which it lets go */
#define GT_ATTACK 0.35f          /* per hop: engages in ~3 hops (~15 ms) */
#define GT_RELEASE 0.03f         /* per hop: lets go over ~0.2 s */
#define GT_FLOOR 0.01f           /* deepest cut: -40 dB */

typedef struct {
    int n;
    float *cos_t, *sin_t;
    int *rev;
} gt_fft_t;

typedef struct {
    bool used, admitted, proven;
    int lag;                     /* > 0 peer later */
    int frames, prove_hops;
    float rho;                   /* correlation at the sighting that created it */
    float level;                 /* usual later/earlier power ratio of THIS copy (the hosts' volumes differ) */
    double created_s, last_sighting_s, last_active_s;
    float amount, activity;
    bool valid;
    float s_xy_re[GT_BINS], s_xy_im[GT_BINS], s_xx[GT_BINS], s_yy[GT_BINS];
    float q[GT_BINS];
    float hh[GT_BINS], hconf[GT_BINS];   /* mode 2: |H|^2 per bin (copy level vs the earlier stream) + its confidence */
    float l_xy_re[GT_BINS], l_xy_im[GT_BINS], l_yy[GT_BINS];   /* mode 3: slow spectra while the layer is present */
    bool l_valid;
    float d_xy_re[GT_BINS], d_xy_im[GT_BINS], d_xx[GT_BINS], d_yy[GT_BINS];   /* slow spectra for DOMINANCE */
    bool d_valid;
    float band_rho2[7], band_yy[7], band_cr[7], band_ci[7];
    float slow_cr[7], slow_ci[7], slow_bx[7], slow_by[7];   /* ~250 ms band sums: is the match SUSTAINED? */
    bool slow_valid;
} gt_slot_t;

struct coop_gate {
    int rate, ch, max_lag, rho_win, search_every, ld;
    unsigned rlen, rmask;
    float *raw[2], *mono[2];
    unsigned long long pos;
    unsigned dlen, dmask;
    float *dec[2];
    unsigned long long dpos;
    float dec_acc[2];
    int dec_n;
    gt_fft_t big, small;
    float *bx_re, *bx_im, *by_re, *by_im;
    unsigned long long next_search, next_hop, out_pos;
    float win[GT_N];
    unsigned olen, omask;
    float *ola[2];
    gt_slot_t slot[GT_SLOTS];
    coop_gate_stats_t stats;
    float band_xx[2][GT_BINS];
    bool band_valid;
    coop_gate_gain_hook_t hook;
    void *hook_ctx;
};

/* ---------------------------------------------------------------- FFT (radix 2, in place) */

static bool fft_init(gt_fft_t *f, int n) {
    f->n = n;
    f->cos_t = malloc(sizeof(float) * (size_t) (n / 2));
    f->sin_t = malloc(sizeof(float) * (size_t) (n / 2));
    f->rev = malloc(sizeof(int) * (size_t) n);
    if (f->cos_t == NULL || f->sin_t == NULL || f->rev == NULL) {
        return false;
    }
    for (int i = 0; i < n / 2; ++i) {
        f->cos_t[i] = (float) cos(2.0 * GT_PI * i / n);
        f->sin_t[i] = (float) -sin(2.0 * GT_PI * i / n);
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

static void fft_free(gt_fft_t *f) {
    free(f->cos_t);
    free(f->sin_t);
    free(f->rev);
}

static void fft_run(const gt_fft_t *f, float *re, float *im, bool inverse) {
    const int n = f->n;
    for (int i = 0; i < n; ++i) {
        int j = f->rev[i];
        if (j > i) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len / 2, step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; ++k) {
                float wr = f->cos_t[k * step], wi = inverse ? -f->sin_t[k * step] : f->sin_t[k * step];
                float xr = re[i + k + half], xi = im[i + k + half];
                float tr = xr * wr - xi * wi, ti = xr * wi + xi * wr;
                re[i + k + half] = re[i + k] - tr;
                im[i + k + half] = im[i + k] - ti;
                re[i + k] += tr;
                im[i + k] += ti;
            }
        }
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

coop_gate_t *coop_gate_create(int rate, int channels, int max_lag_ms) {
    if (rate < 8000 || rate > 96000 || channels < 1 || channels > 2 || max_lag_ms < 5 || max_lag_ms > 1000) {
        return NULL;
    }
    {   /* PROTOTYPE: tunables from the environment for offline sweeps */
        const char *e;
        if ((e = getenv("GATE_COH_LO")) != NULL) P_COH_LO = (float) atof(e);
        if ((e = getenv("GATE_COH_SPAN")) != NULL) P_COH_SPAN = (float) atof(e);
        if ((e = getenv("GATE_ACT_ON")) != NULL) P_ACT_ON = (float) atof(e);
        if ((e = getenv("GATE_ACT_OFF")) != NULL) P_ACT_OFF = (float) atof(e);
        if ((e = getenv("GATE_SMOOTH")) != NULL) P_SMOOTH = (float) atof(e);
        if ((e = getenv("GATE_MODE")) != NULL) P_MODE = atoi(e);
        if ((e = getenv("GATE_DOM_LO")) != NULL) P_DOM_LO = (float) atof(e);
        if ((e = getenv("GATE_DOM_HI")) != NULL) P_DOM_HI = (float) atof(e);
        if ((e = getenv("GATE_DOM_SMOOTH")) != NULL) P_DOM_SMOOTH = (float) atof(e);
        if ((e = getenv("GATE_SH_LO")) != NULL) P_SH_LO = (float) atof(e);
        if ((e = getenv("GATE_SH_HI")) != NULL) P_SH_HI = (float) atof(e);
    }
    coop_gate_t *g = calloc(1, sizeof(*g));
    if (g == NULL) {
        return NULL;
    }
    g->rate = rate;
    g->ch = channels;
    g->max_lag = rate / 1000 * max_lag_ms;
    g->rho_win = rate / 10;
    g->search_every = rate / 1000 * GT_SEARCH_MS;
    g->ld = g->max_lag / GT_DEC + 1;
    g->rlen = pow2_at_least((unsigned) (g->max_lag + g->rho_win + 2 * GT_N + 4096));
    g->rmask = g->rlen - 1;
    g->dlen = pow2_at_least((unsigned) (GT_WIN_D + g->ld + 64));
    g->dmask = g->dlen - 1;
    g->olen = 4096;
    g->omask = g->olen - 1;
    unsigned nf = pow2_at_least((unsigned) (GT_WIN_D + g->ld));
    bool ok = fft_init(&g->big, (int) nf) && fft_init(&g->small, GT_N);
    for (int s = 0; s < 2 && ok; ++s) {
        g->raw[s] = calloc((size_t) g->rlen * (size_t) channels, sizeof(float));
        g->mono[s] = calloc(g->rlen, sizeof(float));
        g->dec[s] = calloc(g->dlen, sizeof(float));
        g->ola[s] = calloc((size_t) g->olen * (size_t) channels, sizeof(float));
        ok = g->raw[s] && g->mono[s] && g->dec[s] && g->ola[s];
    }
    g->bx_re = calloc(nf, sizeof(float));
    g->bx_im = calloc(nf, sizeof(float));
    g->by_re = calloc(nf, sizeof(float));
    g->by_im = calloc(nf, sizeof(float));
    if (!ok || !g->bx_re || !g->bx_im || !g->by_re || !g->by_im) {
        coop_gate_destroy(g);
        return NULL;
    }
    for (int i = 0; i < GT_N; ++i) {
        g->win[i] = (float) sqrt(0.5 - 0.5 * cos(2.0 * GT_PI * i / GT_N));
    }
    g->next_hop = GT_N;
    g->next_search = (unsigned long long) (GT_WIN_D + g->ld) * GT_DEC;
    return g;
}

void coop_gate_destroy(coop_gate_t *g) {
    if (g == NULL) {
        return;
    }
    for (int s = 0; s < 2; ++s) {
        free(g->raw[s]);
        free(g->mono[s]);
        free(g->dec[s]);
        free(g->ola[s]);
    }
    free(g->bx_re);
    free(g->bx_im);
    free(g->by_re);
    free(g->by_im);
    fft_free(&g->big);
    fft_free(&g->small);
    free(g);
}

int coop_gate_latency_frames(void) { return GT_N; }

void coop_gate_set_gain_hook(coop_gate_t *g, coop_gate_gain_hook_t hook, void *ctx) {
    g->hook = hook;
    g->hook_ctx = ctx;
}

void coop_gate_get_stats(const coop_gate_t *g, coop_gate_stats_t *out) {
    *out = g->stats;
    out->count = 0;
    for (int i = 0; i < GT_SLOTS && out->count < COOP_GATE_MAX_LAGS; ++i) {
        if (g->slot[i].used && g->slot[i].proven) {
            out->lag_samples[out->count] = g->slot[i].lag;
            out->amount[out->count] = g->slot[i].amount;
            out->activity[out->count] = g->slot[i].activity;
            out->count++;
        }
    }
}

static double now_s(const coop_gate_t *g) { return (double) g->pos / g->rate; }

/* Analysis bands of at least 4 bins (94-375 Hz, then octaves): a band of 1-2 bins gives a coherence so noisy that
 * the best of eight of them read "active" for offsets that did not exist (measured 2026-10-04, gameplay). */
#define GT_NBANDS 7   /* also the size of gt_slot_t band arrays */
static int band_of(int k) {
    if (k <= 4) {
        return 0;
    }
    int b = 1;
    while (b < GT_NBANDS - 1 && k > (4 << b)) {
        b++;
    }
    return b;
}

/* ---------------------------------------------------------------- offset search */

static float rho_at(const coop_gate_t *g, int lag) {
    const float *later = g->mono[lag >= 0 ? 1 : 0], *earlier = g->mono[lag >= 0 ? 0 : 1];
    const unsigned long long a = (unsigned long long) (lag >= 0 ? lag : -lag);
    double sab = 0, saa = 0, sbb = 0;
    for (unsigned long long t = g->pos - (unsigned long long) g->rho_win; t < g->pos; ++t) {
        float x = later[t & g->rmask], y = earlier[(t - a) & g->rmask];
        sab += (double) x * y;
        saa += (double) x * x;
        sbb += (double) y * y;
    }
    if (saa < 1e-3 || sbb < 1e-3) {
        return 0.0f;
    }
    double r = sab / sqrt(saa * sbb);
    return r > 0 ? (float) r : 0.0f;
}

/* One-sided GCC-PHAT on the newest audio: up to GT_PEAKS separate delays (decimated) at which `later` repeats
 * `earlier`, strongest first. Several peaks because several layers can be present at once. */
static int phat_peaks(coop_gate_t *g, int later, int *delays) {
    const int ld = g->ld, n = g->big.n;
    const unsigned long long dnow = g->dpos;
    memset(g->bx_re, 0, sizeof(float) * (size_t) n);
    memset(g->bx_im, 0, sizeof(float) * (size_t) n);
    memset(g->by_re, 0, sizeof(float) * (size_t) n);
    memset(g->by_im, 0, sizeof(float) * (size_t) n);
    const unsigned long long x0 = dnow - GT_WIN_D, y0 = dnow - GT_WIN_D - (unsigned long long) ld;
    double ex = 0, ey = 0;
    for (int i = 0; i < GT_WIN_D; ++i) {
        g->bx_re[i] = g->dec[later][(x0 + (unsigned) i) & g->dmask];
        ex += (double) g->bx_re[i] * g->bx_re[i];
    }
    for (int i = 0; i < GT_WIN_D + ld; ++i) {
        g->by_re[i] = g->dec[1 - later][(y0 + (unsigned) i) & g->dmask];
        ey += (double) g->by_re[i] * g->by_re[i];
    }
    if (ex < 1.0 || ey < 1.0) {
        return 0;
    }
    fft_run(&g->big, g->bx_re, g->bx_im, false);
    fft_run(&g->big, g->by_re, g->by_im, false);
    for (int k = 0; k < n; ++k) {
        float re = g->bx_re[k] * g->by_re[k] + g->bx_im[k] * g->by_im[k];
        float im = g->bx_re[k] * g->by_im[k] - g->bx_im[k] * g->by_re[k];
        float mag = sqrtf(re * re + im * im) + 1e-9f;
        g->bx_re[k] = re / mag;
        g->bx_im[k] = im / mag;
    }
    fft_run(&g->big, g->bx_re, g->bx_im, true);
    const int sep = g->rate / 1000 * GT_PEAK_SEP_MS / GT_DEC + 1;
    int found = 0;
    for (int p = 0; p < GT_PEAKS; ++p) {
        int best_s = -1;
        float best = -1e30f;
        for (int s = 0; s <= ld; ++s) {
            bool taken = false;
            for (int q = 0; q < found; ++q) {
                if (abs((ld - s) - delays[q]) <= sep) {
                    taken = true;
                    break;
                }
            }
            if (!taken && g->bx_re[s] > best) {
                best = g->bx_re[s];
                best_s = s;
            }
        }
        if (best_s < 0) {
            break;
        }
        delays[found++] = ld - best_s;
    }
    return found;
}

static int refine(const coop_gate_t *g, int coarse, float *rho_out) {
    int best = coarse;
    float best_rho = -1.0f;
    for (int l = coarse - GT_DEC; l <= coarse + GT_DEC; ++l) {
        if (l > g->max_lag || l < -g->max_lag) {
            continue;
        }
        float r = rho_at(g, l);
        if (r > best_rho) {
            best_rho = r;
            best = l;
        }
    }
    *rho_out = best_rho;
    return best;
}

static gt_slot_t *slot_near(coop_gate_t *g, int lag, int tol) {
    gt_slot_t *best = NULL;
    for (int i = 0; i < GT_SLOTS; ++i) {
        if (g->slot[i].used && abs(g->slot[i].lag - lag) <= tol &&
            (best == NULL || abs(g->slot[i].lag - lag) < abs(best->lag - lag))) {
            best = &g->slot[i];
        }
    }
    return best;
}

/* How well a trial matches NOW: its live coherence once it has any, else the correlation it was found with. Ranking by
 * the sighting correlation alone let stale candidates from a loading screen hold the whole pool while the cutscene's
 * real offset waited 1.4 s for a place (measured 2026-10-04). */
static float trial_score(const gt_slot_t *c) {
    return c->frames >= GT_WARMUP ? c->activity : c->rho;
}

static void admit(coop_gate_t *g, int lag, float rho) {
    const double t = now_s(g);
    int trials = 0;
    gt_slot_t *free_slot = NULL, *weakest = NULL;
    for (int i = 0; i < GT_SLOTS; ++i) {
        gt_slot_t *c = &g->slot[i];
        if (!c->used) {
            if (free_slot == NULL) {
                free_slot = c;
            }
        } else if (!c->proven) {
            trials++;
            if (weakest == NULL || trial_score(c) < trial_score(weakest)) {
                weakest = c;
            }
        }
    }
    gt_slot_t *s = NULL;
    if (trials < GT_TRIALS && free_slot != NULL) {
        s = free_slot;
    } else if (weakest != NULL && rho > trial_score(weakest)) {   /* pool full: displace the weakest CURRENT match */
        s = weakest;
        g->stats.expired++;
    }
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));
    s->used = s->admitted = true;
    s->lag = lag;
    s->rho = rho;
    s->created_s = s->last_sighting_s = s->last_active_s = t;
    g->stats.admitted++;
}

/* A trial has proven itself: make room in the bank by dropping the proven offset idle longest. */
static void promote(coop_gate_t *g, gt_slot_t *sl) {
    int proven = 0;
    gt_slot_t *idle = NULL;
    for (int i = 0; i < GT_SLOTS; ++i) {
        gt_slot_t *c = &g->slot[i];
        if (c->used && c->proven) {
            proven++;
            if (idle == NULL || c->last_active_s < idle->last_active_s) {
                idle = c;
            }
        }
    }
    if (proven >= GT_BANK && idle != NULL) {
        idle->used = false;
        g->stats.expired++;
    }
    sl->proven = true;
}

static void sighting(coop_gate_t *g, int lag, float rho) {
    gt_slot_t *s = slot_near(g, lag, g->rate / 1000 * GT_TRACK_MS);
    if (s == NULL) {
        admit(g, lag, rho);
        return;
    }
    s->last_sighting_s = now_s(g);
    if (rho > s->rho && !s->proven) {
        s->rho = rho;
    }
    /* the layer drifted: follow it -- but only once it has stopped explaining anything at its old place, or every
     * neighbouring search peak would keep resetting a working offset */
    if (abs(s->lag - lag) >= GT_RECENTRE && s->frames >= GT_WARMUP &&
        (now_s(g) - s->last_active_s > 0.3 || rho > rho_at(g, s->lag) + 0.05f)) {
        s->lag = lag;
        s->valid = false;
        s->frames = 0;
    }
}

static void search(coop_gate_t *g) {
    g->stats.searches++;
    for (int later = 0; later < 2; ++later) {
        int d[GT_PEAKS];
        const int n = phat_peaks(g, later, d);
        for (int i = 0; i < n; ++i) {
            const int coarse = (later == 1 ? 1 : -1) * d[i] * GT_DEC;
            float rho;
            const int lag = refine(g, coarse, &rho);
            {
                static double tf = -2, tt = -2;
                if (tf == -2) { const char *e = getenv("GATE_STRACE"); tf = tt = -1; if (e) sscanf(e, "%lf,%lf", &tf, &tt); }
                const double tn = now_s(g);
                if (tn >= tf && tn <= tt) fprintf(stderr, "S %.3f cand %+.1f rho %.2f\n", tn, lag * 1000.0 / g->rate, rho);
            }
            if (rho >= GT_ADMIT_RHO && lag != 0) {
                sighting(g, lag, rho);
            }
        }
    }
    const double t = now_s(g);
    /* two offsets within GT_TRACK_MS are one layer: keep the better-matching, or both claim (and count) the same copy */
    for (int i = 0; i < GT_SLOTS; ++i) {
        for (int j = i + 1; j < GT_SLOTS; ++j) {
            gt_slot_t *a = &g->slot[i], *b = &g->slot[j];
            if (a->used && b->used && abs(a->lag - b->lag) <= g->rate / 1000 * GT_TRACK_MS) {
                gt_slot_t *loser = (a->proven != b->proven) ? (a->proven ? b : a) : (a->activity >= b->activity ? b : a);
                loser->used = false;
                g->stats.expired++;
            }
        }
    }
    for (int i = 0; i < GT_SLOTS; ++i) {
        gt_slot_t *s = &g->slot[i];
        const bool stale = s->proven ? t - s->last_active_s > GT_EXPIRE_S && t - s->last_sighting_s > GT_EXPIRE_S
                                     : t - s->created_s > GT_TRIAL_S;
        if (s->used && stale) {
            s->used = false;
            g->stats.expired++;
        }
    }
}

/* ---------------------------------------------------------------- STFT frame */

static void mono_fft(const coop_gate_t *g, int s, unsigned long long start, float *re, float *im) {
    for (int i = 0; i < GT_N; ++i) {
        re[i] = g->mono[s][(start + (unsigned) i) & g->rmask] * g->win[i];
        im[i] = 0.0f;
    }
    fft_run(&g->small, re, im, false);
}

static void process_frame(coop_gate_t *g, unsigned long long end) {
    const int ch = g->ch;
    const unsigned long long start = end - GT_N;
    const double t = now_s(g);
    float cur_re[2][GT_N], cur_im[2][GT_N];
    mono_fft(g, 0, start, cur_re[0], cur_im[0]);
    mono_fft(g, 1, start, cur_re[1], cur_im[1]);
    for (int s2 = 0; s2 < 2; ++s2) {   /* slow per-bin power of each stream, same smoothing as the dominance spectra */
        for (int k = 0; k < GT_BINS; ++k) {
            const float pw = cur_re[s2][k] * cur_re[s2][k] + cur_im[s2][k] * cur_im[s2][k];
            g->band_xx[s2][k] = g->band_valid ? P_DOM_SMOOTH * g->band_xx[s2][k] + (1 - P_DOM_SMOOTH) * pw : pw;
        }
    }
    g->band_valid = true;
    /* per stream, per bin: the strongest removal any offset asks for */
    float want[2][GT_BINS], claim[2][GT_BINS], pe[2][GT_BINS];
    double dom_num[2][GT_NBANDS], dom_den[2][GT_NBANDS];   /* (diagnostic) */
    float open_b[2][GT_NBANDS];   /* per stream, per band: how far the gate is open, 0..1 */
    float sus_b[2][GT_NBANDS];    /* per stream, per band: summed SUSTAINED share of offsets that are each clearly real */
    memset(open_b, 0, sizeof(open_b));
    memset(sus_b, 0, sizeof(sus_b));
    memset(dom_num, 0, sizeof(dom_num));
    memset(dom_den, 0, sizeof(dom_den));
    memset(want, 0, sizeof(want));
    memset(claim, 0, sizeof(claim));
    memset(pe, 0, sizeof(pe));
    for (int i = 0; i < GT_SLOTS; ++i) {
        gt_slot_t *sl = &g->slot[i];
        if (!sl->used || !sl->admitted) {
            continue;
        }
        const int later = sl->lag > 0 ? 1 : 0, earlier = 1 - later;
        const unsigned long long a = (unsigned long long) abs(sl->lag);
        if (start < a) {
            continue;
        }
        float yr[GT_N], yi[GT_N];
        mono_fft(g, earlier, start - a, yr, yi);
        const float *xr = cur_re[later], *xi = cur_im[later];
        const float al = sl->valid ? P_SMOOTH : 0.0f;
        double num_w = 0, den_w = 0, lx = 0, ly = 0;
        float coh_k[GT_BINS];
        for (int k = 0; k < GT_BINS; ++k) {
            float cre = xr[k] * yr[k] + xi[k] * yi[k], cim = xi[k] * yr[k] - xr[k] * yi[k];
            float pxx = xr[k] * xr[k] + xi[k] * xi[k];
            sl->s_xy_re[k] = al * sl->s_xy_re[k] + (1 - al) * cre;
            sl->s_xy_im[k] = al * sl->s_xy_im[k] + (1 - al) * cim;
            sl->s_xx[k] = al * sl->s_xx[k] + (1 - al) * pxx;
            sl->s_yy[k] = al * sl->s_yy[k] + (1 - al) * (yr[k] * yr[k] + yi[k] * yi[k]);
            float num = sl->s_xy_re[k] * sl->s_xy_re[k] + sl->s_xy_im[k] * sl->s_xy_im[k];
            float den = sl->s_xx[k] * sl->s_yy[k];
            float coh = den > 1e-6f ? num / den : 0.0f;
            if (coh > 1.0f) {
                coh = 1.0f;
            }
            coh_k[k] = coh;
            if (coh > 0.8f) {   /* sure-copy bins say how loud this copy usually is in the later stream */
                lx += sl->s_xx[k];
                ly += sl->s_yy[k];
            }
            num_w += (double) coh * sl->s_xx[k];
            den_w += sl->s_xx[k];
        }
        {   /* DOMINANCE: with ~10-20 frames of averaging, coherence is a fair estimate of how much of the later stream
             * IS this copy (a 30 ms estimate reads chance as copy -- measured: gameplay bands with 2% copy were cut
             * 6-14 dB). Summed per octave band below; cutting is only allowed where copies are the clear majority. */
            const float d = sl->d_valid ? P_DOM_SMOOTH : 0.0f;
            for (int k = 0; k < GT_BINS; ++k) {
                float cre = xr[k] * yr[k] + xi[k] * yi[k], cim = xi[k] * yr[k] - xr[k] * yi[k];
                sl->d_xy_re[k] = d * sl->d_xy_re[k] + (1 - d) * cre;
                sl->d_xy_im[k] = d * sl->d_xy_im[k] + (1 - d) * cim;
                sl->d_xx[k] = d * sl->d_xx[k] + (1 - d) * (xr[k] * xr[k] + xi[k] * xi[k]);
                sl->d_yy[k] = d * sl->d_yy[k] + (1 - d) * (yr[k] * yr[k] + yi[k] * yi[k]);
            }
            sl->d_valid = true;
            if (sl->proven && sl->frames >= 8 && sl->amount >= 0.5f) {
                /* THE GATE, per band, from THIS offset alone (never a sum across offsets -- phantom offsets each add a
                 * chance share): open when the waveform match is SUSTAINED over ~250 ms (rho^2 0.5 -> 0.7), or when it
                 * is near-total right now (rho^2 0.85 -> 0.95) so a cutscene opens it at once. Busy gameplay has brief
                 * music-dominated moments, which pass neither test. */
                for (int bb = 0; bb < GT_NBANDS; ++bb) {
                    const float sr2 = sl->slow_bx[bb] > 1e-3f && sl->slow_by[bb] > 1e-3f
                        ? (sl->slow_cr[bb] * sl->slow_cr[bb] + sl->slow_ci[bb] * sl->slow_ci[bb]) / (sl->slow_bx[bb] * sl->slow_by[bb])
                        : 0.0f;
                    if (sr2 >= 0.10f) {   /* each a real layer on its own; chance waveform matches sit far below */
                        sus_b[later][bb] += sr2;
                    }
                    float o = (sl->band_rho2[bb] - 0.85f) / 0.10f;   /* near-total right now: open at once */
                    o = o < 0 ? 0 : (o > 1 ? 1 : o);
                    if (o > open_b[later][bb]) {
                        open_b[later][bb] = o;
                    }
                }
                /* energy this copy explains in each band = |band cross-spectrum|^2 / earlier band energy (rho^2 * Exx),
                 * from last hop's band sums (the dominance spectra were just updated; one hop late is harmless) */
                for (int bb = 0; bb < GT_NBANDS; ++bb) {
                    if (sl->band_yy[bb] > 1e-3f) {
                        dom_num[later][bb] += (sl->band_cr[bb] * sl->band_cr[bb] + sl->band_ci[bb] * sl->band_ci[bb])
                                              / sl->band_yy[bb];
                    }
                }
            }
        }
        if (ly > 1e-3 && lx > 1e-3) {
            const float r = (float) (lx / ly);
            sl->level = sl->level <= 0 ? r : 0.97f * sl->level + 0.03f * r;   /* ~0.15 s */
        }
        if ((P_MODE == 3 || P_MODE == 4) && sl->proven) {
            /* MODE 3 -- JOINT PREDICTION ACROSS LAYERS. Several copies at different offsets can share one bin (music
             * at 60 ms under dialogue at 272 ms), so no single offset ever sees that bin as a clean match and each cuts
             * only part of it. Instead each offset learns its copy's transfer H per bin from SLOW spectra, updated only
             * while its layer is present (the other layers average out of a cross-spectrum), and adds its predicted
             * echo |H|^2|Y|^2 for this frame; the bin is then cut by the TOTAL predicted share of what arrived. */
            const bool present = sl->activity >= P_ACT_OFF;
            for (int k = 0; k < GT_BINS; ++k) {
                if (present) {
                    float cre = xr[k] * yr[k] + xi[k] * yi[k], cim = xi[k] * yr[k] - xr[k] * yi[k];
                    float pyy = yr[k] * yr[k] + yi[k] * yi[k];
                    if (!sl->l_valid) {   /* start from the fast estimate, not from one frame */
                        sl->l_xy_re[k] = sl->s_xy_re[k];
                        sl->l_xy_im[k] = sl->s_xy_im[k];
                        sl->l_yy[k] = sl->s_yy[k];
                    }
                    const float b = 0.95f;
                    sl->l_xy_re[k] = b * sl->l_xy_re[k] + (1 - b) * cre;
                    sl->l_xy_im[k] = b * sl->l_xy_im[k] + (1 - b) * cim;
                    sl->l_yy[k] = b * sl->l_yy[k] + (1 - b) * pyy;
                }
                if (sl->l_valid && present) {   /* only a layer that is there NOW predicts (a stale one only steals bins) */
                    const float h = sl->l_yy[k] > 1e-6f
                        ? (sl->l_xy_re[k] * sl->l_xy_re[k] + sl->l_xy_im[k] * sl->l_xy_im[k]) / (sl->l_yy[k] * sl->l_yy[k])
                        : 0.0f;
                    pe[later][k] += sl->amount * h * (yr[k] * yr[k] + yi[k] * yi[k]);
                }
            }
            if (present) {
                sl->l_valid = true;
            }
        }
        for (int k = 0; k < GT_BINS && P_MODE == 2; ++k) {
            /* MODE 2 -- PREDICT THIS FRAME'S ECHO. |H|^2 (how loud the copy is relative to the earlier stream, per bin)
             * changes slowly, so it is learned only where the match is clear; the echo expected in THIS frame is then
             * |H|^2 * |Y|^2 from the earlier stream's matching frame, and the bin is cut by how much of what arrived
             * that prediction accounts for. Anything louder than the prediction -- this player's own sound -- passes,
             * frame by frame, even while a copy is running underneath. */
            const float sxy2 = sl->s_xy_re[k] * sl->s_xy_re[k] + sl->s_xy_im[k] * sl->s_xy_im[k];
            if (coh_k[k] > 0.6f && sl->s_yy[k] > 1e-6f) {
                const float h = sxy2 / (sl->s_yy[k] * sl->s_yy[k]);
                sl->hh[k] = sl->hconf[k] <= 0.0f ? h : 0.9f * sl->hh[k] + 0.1f * h;
                sl->hconf[k] = 0.9f * sl->hconf[k] + 0.1f;
            } else {
                sl->hconf[k] *= 0.97f;
            }
            const float py = yr[k] * yr[k] + yi[k] * yi[k], px = xr[k] * xr[k] + xi[k] * xi[k];
            float share = px > 1e-6f ? sl->hh[k] * py / px : 0.0f;
            float v = (share - P_SH_LO) / (P_SH_HI - P_SH_LO);
            v = v < 0 ? 0 : (v > 1 ? 1 : v);
            const float c = sl->hconf[k] * 2.0f;
            sl->q[k] = v * (c > 1 ? 1 : c);
        }
        for (int k = 0; k < GT_BINS && (P_MODE == 1 || P_MODE == 4); ++k) {
            float v = (coh_k[k] - P_COH_LO) / P_COH_SPAN;
            v = v < 0 ? 0 : (v > 1 ? 1 : v);
            /* LOUDER THAN ITS COPY USUALLY IS = this player's own sound on top: keep it */
            const float ratio = sl->s_xx[k] / ((sl->s_yy[k] + 1e-6f) * (sl->level > 0 ? sl->level : 1.0f));
            if (ratio > GT_LOUD_LO) {
                v *= ratio >= GT_LOUD_HI ? 0.0f : (GT_LOUD_HI - ratio) / (GT_LOUD_HI - GT_LOUD_LO);
            }
            sl->q[k] = v;
        }
        sl->valid = true;
        sl->frames++;
        /* ACTIVITY PER BAND, best band wins: layers sit in different bands (music bass at one offset, dialogue at
         * another), and a broadband energy-weighted figure is all bass. Taken from the SLOW spectra (computed below,
         * so this uses last hop's -- one hop late is harmless) and over bands of >= 4 bins: per-bin coherence over a
         * few frames is mostly chance. A band counts only while it carries >= 1% of the frame's energy. */
        float act = 0.0f;
        if (sl->d_valid && sl->frames >= GT_WARMUP) {
            /* BAND WAVEFORM CORRELATION: the cross-spectrum is SUMMED across the band's bins before its magnitude is
             * taken, so the copy must line up in waveform -- every bin at the same delay -- not merely bin by bin. Per-bin
             * coherence also scores sustained tones (coherent at ANY delay) and the same event rendered differently by
             * each game (3D panning, distance filtering); measured 2026-10-04, that made every offset look active in
             * busy gameplay and band shares exceed 100%. */
            double cr[GT_NBANDS] = {0}, ci[GT_NBANDS] = {0}, bx[GT_NBANDS] = {0}, by[GT_NBANDS] = {0}, tot = 0;
            for (int k = 1; k < GT_BINS; ++k) {
                const int bb = band_of(k);
                cr[bb] += sl->d_xy_re[k];
                ci[bb] += sl->d_xy_im[k];
                bx[bb] += sl->d_xx[k];
                by[bb] += sl->d_yy[k];
                tot += sl->d_xx[k];
            }
            const bool slow_was = sl->slow_valid;
            sl->slow_valid = true;
            for (int bb = 0; bb < GT_NBANDS; ++bb) {
                sl->band_rho2[bb] = bx[bb] > 1e-3 && by[bb] > 1e-3
                    ? (float) ((cr[bb] * cr[bb] + ci[bb] * ci[bb]) / (bx[bb] * by[bb])) : 0.0f;
                sl->band_yy[bb] = (float) by[bb];
                {
                    const float a2 = slow_was ? 0.98f : 0.0f;
                    sl->slow_cr[bb] = a2 * sl->slow_cr[bb] + (1 - a2) * (float) cr[bb];
                    sl->slow_ci[bb] = a2 * sl->slow_ci[bb] + (1 - a2) * (float) ci[bb];
                    sl->slow_bx[bb] = a2 * sl->slow_bx[bb] + (1 - a2) * (float) bx[bb];
                    sl->slow_by[bb] = a2 * sl->slow_by[bb] + (1 - a2) * (float) by[bb];
                }
                sl->band_cr[bb] = (float) cr[bb];
                sl->band_ci[bb] = (float) ci[bb];
                if (bx[bb] >= 0.01 * tot) {
                    const float v = sqrtf(sl->band_rho2[bb]);
                    if (v > act) {
                        act = v;
                    }
                }
            }
        }
        sl->activity = act;
        if (!sl->proven) {
            sl->prove_hops = sl->activity >= P_ACT_ON ? sl->prove_hops + 1 : (sl->prove_hops > 0 ? sl->prove_hops - 1 : 0);
            /* an unambiguous find (whole-window correlation >= 0.9) proves itself in ~30 ms instead of ~85 */
            if (sl->prove_hops >= (sl->rho >= 0.9f ? 6 : GT_PROVE_HOPS)) {
                promote(g, sl);
            }
        }
        float target = sl->activity >= P_ACT_ON ? 1.0f : (sl->activity <= P_ACT_OFF ? 0.0f : sl->amount);
        if (!sl->proven) {
            target = 0.0f;
        }
        sl->amount += (target - sl->amount) * (target > sl->amount ? GT_ATTACK : GT_RELEASE);
        if (sl->activity >= P_ACT_ON) {   /* chance coherence sits near ACT_OFF; only a clear match counts */
            sl->last_active_s = t;
        }
        if (sl->amount > 0.01f) {
            for (int k = 0; k < GT_BINS; ++k) {
                float qs = sl->q[k];
                if (k > 0 && k < GT_BINS - 1) {
                    qs = 0.25f * sl->q[k - 1] + 0.5f * sl->q[k] + 0.25f * sl->q[k + 1];
                }
                const float w = sl->amount * qs;
                if (w > want[later][k]) {
                    want[later][k] = w;
                }
                if (sl->activity * qs > claim[later][k]) {
                    claim[later][k] = sl->activity * qs;
                }
            }
        }
    }
    if (P_MODE == 3 || P_MODE == 4) {   /* 4: the deeper of the coherence cut (already in want) and the joint one */
        for (int s2 = 0; s2 < 2; ++s2) {
            for (int k = 0; k < GT_BINS; ++k) {
                const float px = cur_re[s2][k] * cur_re[s2][k] + cur_im[s2][k] * cur_im[s2][k];
                const float share = px > 1e-6f ? pe[s2][k] / px : 0.0f;
                float v = (share - P_SH_LO) / (P_SH_HI - P_SH_LO);
                v = v < 0 ? 0 : (v > 1 ? 1 : v);
                if (P_MODE == 3 || v > want[s2][k]) {
                    want[s2][k] = v;
                }
                if (P_MODE == 3 || share > claim[s2][k]) {
                    claim[s2][k] = share;
                }
            }
        }
    }
    if (P_DOM_HI > 0.0f) {
        /* THE GATE: a band may be cut only while the copies explain most of what arrives in it. In busy gameplay a
         * shared music bed is a minority of each band, so the gate stays shut and the player's own sound is untouched;
         * in a cutscene or over narration the copy dominates and the gate opens. */
        for (int s2 = 0; s2 < 2; ++s2) {
            for (int k = 1; k < GT_BINS; ++k) {
                dom_den[s2][band_of(k)] += g->band_xx[s2][k];
            }
            for (int k = 0; k < GT_BINS; ++k) {
                const int b = band_of(k);
                /* sustained: the layers that are each clearly real, summed (two copies can share a band -- music under
                 * dialogue -- and neither alone is the majority) */
                float o1 = (sus_b[s2][b] - P_DOM_LO) / (P_DOM_HI - P_DOM_LO);
                o1 = o1 < 0 ? 0 : (o1 > 1 ? 1 : o1);
                want[s2][k] *= open_b[s2][b] > o1 ? open_b[s2][b] : o1;
            }
        }
    }
    /* never cut the same bin in both streams: the weaker claim yields */
    float gain[2][GT_BINS];
    double e_in[2] = {0, 0}, e_out[2] = {0, 0};
    for (int k = 0; k < GT_BINS; ++k) {
        if (want[0][k] > 0 && want[1][k] > 0) {
            if (claim[0][k] >= claim[1][k]) {
                want[1][k] = 0;
            } else {
                want[0][k] = 0;
            }
        }
        for (int s = 0; s < 2; ++s) {
            float g2 = 1.0f - want[s][k];
            if (g2 < GT_FLOOR * GT_FLOOR) {
                g2 = GT_FLOOR * GT_FLOOR;
            }
            gain[s][k] = sqrtf(g2);
            const double p = (double) cur_re[s][k] * cur_re[s][k] + (double) cur_im[s][k] * cur_im[s][k];
            e_in[s] += p;
            e_out[s] += p * g2;
        }
    }
    {   /* PROTOTYPE trace: GATE_TRACE="from,to" seconds */
        static int init = 0; static double tf = -1, tt = -1;
        if (!init) { const char *e = getenv("GATE_TRACE"); if (e) sscanf(e, "%lf,%lf", &tf, &tt); init = 1; }
        if (t >= tf && t <= tt) {
            fprintf(stderr, "%.3f", t);
            for (int i = 0; i < GT_SLOTS; ++i) {
                const gt_slot_t *sl = &g->slot[i];
                if (sl->used) fprintf(stderr, " [%+.1f a=%.2f act=%.2f]", sl->lag * 1000.0 / g->rate, sl->amount, sl->activity);
            }
            double ei = 0, eo = 0;
            for (int k = 0; k < GT_BINS; ++k) { double p = (double) cur_re[1][k] * cur_re[1][k] + (double) cur_im[1][k] * cur_im[1][k]; ei += p; eo += p * gain[1][k] * gain[1][k]; }
            if (getenv("GATE_TRACE_BAND")) {
                const int tb = atoi(getenv("GATE_TRACE_BAND"));
                for (int i = 0; i < GT_SLOTS; ++i) {
                    const gt_slot_t *sl = &g->slot[i];
                    if (!sl->used || !sl->proven) continue;
                    const float sr2 = sl->slow_bx[tb] > 1e-3f && sl->slow_by[tb] > 1e-3f
                        ? (sl->slow_cr[tb] * sl->slow_cr[tb] + sl->slow_ci[tb] * sl->slow_ci[tb]) / (sl->slow_bx[tb] * sl->slow_by[tb]) : 0.0f;
                    fprintf(stderr, " {%+.1f slow%.2f fast%.2f bx%.0f by%.0f}", sl->lag * 1000.0 / g->rate, sr2, sl->band_rho2[tb], 10 * log10(sl->slow_bx[tb] + 1), 10 * log10(sl->slow_by[tb] + 1));
                }
            }
            fprintf(stderr, " peer=%.1fdB dom:", 10 * log10((eo + 1e-3) / (ei + 1e-3)));
            for (int b2 = 0; b2 < GT_NBANDS; ++b2) fprintf(stderr, " %.2f", open_b[1][b2]);
            fprintf(stderr, "\n");
        }
    }
    if (g->hook != NULL) {
        g->hook(g->hook_ctx, start, gain[0], gain[1], GT_BINS);
    }
    g->stats.self_cut_db = (float) (10.0 * log10((e_out[0] + 1e-3) / (e_in[0] + 1e-3)));
    g->stats.peer_cut_db = (float) (10.0 * log10((e_out[1] + 1e-3) / (e_in[1] + 1e-3)));
    for (int s = 0; s < 2; ++s) {
        bool apply = false;
        for (int k = 0; k < GT_BINS && !apply; ++k) {
            apply = gain[s][k] < 0.999f;
        }
        for (int c = 0; c < ch; ++c) {
            float re[GT_N], im[GT_N];
            for (int i = 0; i < GT_N; ++i) {
                re[i] = g->raw[s][((start + (unsigned) i) & g->rmask) * (unsigned) ch + (unsigned) c] * g->win[i];
                im[i] = 0.0f;
            }
            if (apply) {
                fft_run(&g->small, re, im, false);
                for (int k = 0; k < GT_BINS; ++k) {
                    re[k] *= gain[s][k];
                    im[k] *= gain[s][k];
                    if (k > 0 && k < GT_N / 2) {
                        re[GT_N - k] = re[k];
                        im[GT_N - k] = -im[k];
                    }
                }
                fft_run(&g->small, re, im, true);
                for (int i = 0; i < GT_N; ++i) {
                    re[i] /= (float) GT_N;
                }
            }
            for (int i = 0; i < GT_N; ++i) {
                g->ola[s][((start + (unsigned) i) & g->omask) * (unsigned) ch + (unsigned) c] += re[i] * g->win[i];
            }
        }
    }
}

/* ---------------------------------------------------------------- per call */

void coop_gate_process(coop_gate_t *g, int16_t *self, int16_t *peer, int frames) {
    const int ch = g->ch;
    int16_t *io[2] = {self, peer};
    for (int i = 0; i < frames; ++i) {
        const unsigned long long t = g->pos;
        for (int s = 0; s < 2; ++s) {
            float m = 0;
            for (int c = 0; c < ch; ++c) {
                float v = (float) io[s][i * ch + c];
                g->raw[s][(t & g->rmask) * (unsigned) ch + (unsigned) c] = v;
                m += v;
            }
            m /= (float) ch;
            g->mono[s][t & g->rmask] = m;
            g->dec_acc[s] += m;
        }
        g->pos++;
        if (++g->dec_n == GT_DEC) {
            for (int s = 0; s < 2; ++s) {
                g->dec[s][g->dpos & g->dmask] = g->dec_acc[s] / GT_DEC;
                g->dec_acc[s] = 0;
            }
            g->dpos++;
            g->dec_n = 0;
        }
        if (g->pos >= g->next_search) {
            search(g);
            g->next_search += (unsigned long long) g->search_every;
        }
        if (g->pos >= g->next_hop) {
            process_frame(g, g->next_hop);
            g->next_hop += GT_H;
        }
    }
    for (int i = 0; i < frames; ++i) {
        const unsigned long long t = g->out_pos;
        for (int s = 0; s < 2; ++s) {
            for (int c = 0; c < ch; ++c) {
                float v = 0.0f;
                if (t >= GT_N) {
                    float *cell = &g->ola[s][(((t - GT_N) & g->omask) * (unsigned) ch) + (unsigned) c];
                    v = *cell;
                    *cell = 0.0f;
                }
                long r = lrintf(v);
                io[s][i * ch + c] = (int16_t) (r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
            }
        }
        g->out_pos++;
    }
}
