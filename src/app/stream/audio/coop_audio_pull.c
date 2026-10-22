#include "coop_audio_pull.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

void coop_audio_pull_destroy(coop_audio_pull_t *p) {
    if (!p) return;
    for (unsigned s=0;s<2;++s) {
        coop_audio_spsc_destroy(&p->source[s]);
        free(p->stamp[s]); free(p->raw[s]); free(p->work[s]);
    }
    free(p->mixed);
    coop_audio_spsc_destroy(&p->ready);
    free(p->drain);
    memset(p,0,sizeof(*p));
}

bool coop_audio_pull_init(coop_audio_pull_t *p, unsigned channels, unsigned block,
    unsigned capacity, coop_audio_pull_mix_fn mix, coop_audio_pull_observe_fn observe, void *ctx) {
    if (!p || !channels || channels>2 || !block || !mix || capacity<block) return false;
    memset(p,0,sizeof(*p));
    p->channels=channels; p->block=block; p->offset=block;
    p->mix=mix; p->observe=observe; p->ctx=ctx;
    for(unsigned s=0;s<2;++s) {
        if(!coop_audio_spsc_init(&p->source[s],capacity)) goto fail;
        p->stamp[s]=calloc(block,sizeof(*p->stamp[s]));
        p->raw[s]=calloc(block,channels*sizeof(int16_t));
        p->work[s]=calloc(block,channels*sizeof(int16_t));
        if(!p->stamp[s] || !p->raw[s] || !p->work[s])goto fail;
    }
    p->mixed=calloc(block,channels*sizeof(int16_t));
    if(!p->mixed)goto fail;
    return true;
fail:
    coop_audio_pull_destroy(p);
    return false;
}

static void render_block(coop_audio_pull_t *p) {
    unsigned valid=0,new_epoch=0;
    for(unsigned s=0;s<2;++s) {
        unsigned n=coop_audio_spsc_pop(&p->source[s],p->stamp[s],p->block);
        p->real[s]=n;
        bool ok=n==p->block;
        if(n && p->stamp[s][0].epoch!=p->stats.epoch[s]) {
            if(p->stats.epoch[s])new_epoch|=1u<<s;
            p->stats.epoch[s]=p->stamp[s][0].epoch;
            p->indexed[s]=false;
        }
        for(unsigned i=0;i<n;++i) {
            const coop_audio_frame_t *f=&p->stamp[s][i];
            if(p->indexed[s] && f->index!=p->next_index[s]) {
                ok=false; ++p->stats.discontinuities[s];
            }
            p->indexed[s]=true; p->next_index[s]=f->index+1;
            if(f->flags)ok=false;
            for(unsigned c=0;c<p->channels;++c)p->raw[s][(size_t)i*p->channels+c]=f->pcm[c];
        }
        memset(p->raw[s]+(size_t)n*p->channels,0,(size_t)(p->block-n)*p->channels*sizeof(int16_t));
        memcpy(p->work[s],p->raw[s],(size_t)p->block*p->channels*sizeof(int16_t));
        if(ok)valid|=1u<<s;
    }
    p->mix(p->ctx,p->work[0],p->work[1],p->mixed,p->block,valid,new_epoch);
    if(p->observe)p->observe(p->ctx,p->raw[0],p->raw[1],p->mixed,p->block,p->produced);
    p->produced+=p->block;
    p->offset=0;
}

/** Per-source accounting of frames [from, from + n) of the current block, handed on at handed_us. */
static void account(coop_audio_pull_t *p, unsigned from, unsigned n, uint64_t handed_us) {
    for(unsigned s=0;s<2;++s) for(unsigned i=from;i<from+n;++i) {
        if(i>=p->real[s]) { ++p->stats.missing[s]; continue; }
        const coop_audio_frame_t *f=&p->stamp[s][i];
        ++p->stats.consumed[s];
        if(f->flags)++p->stats.concealed[s];
        if(!f->ready_us || handed_us<f->ready_us) { ++p->stats.untimed[s]; continue; }
        ++p->stats.timed[s];
        uint64_t age=handed_us-f->ready_us;
        if(age>p->stats.age_max_us[s])p->stats.age_max_us[s]=age;
        const uint64_t bucket=age/1000u;
        ++p->stats.age_hist[s][bucket<COOP_AUDIO_AGE_BUCKETS-1u ? bucket : COOP_AUDIO_AGE_BUCKETS-1u];
    }
}

/* Produced ahead: the device thread only copies what the worker has mixed, and plays silence for what it has not. */
static unsigned render_ready(coop_audio_pull_t *p,int16_t *pcm,unsigned frames) {
    unsigned written=0;
    while(written<frames) {
        unsigned want=frames-written;
        if(want>p->drain_cap)want=p->drain_cap;
        const unsigned n=coop_audio_spsc_pop(&p->ready,p->drain,want);
        for(unsigned i=0;i<n;++i) for(unsigned c=0;c<p->channels;++c)
            pcm[(size_t)(written+i)*p->channels+c]=p->drain[i].pcm[c];
        written+=n;
        if(n<want) {
            memset(pcm+(size_t)written*p->channels,0,(size_t)(frames-written)*p->channels*sizeof(int16_t));
            atomic_fetch_add_explicit(&p->device_short,frames-written,memory_order_relaxed);
            written=frames;
        }
    }
    atomic_fetch_add_explicit(&p->device_frames,frames,memory_order_relaxed);
    return frames;
}

unsigned coop_audio_pull_render(coop_audio_pull_t *p,int16_t *pcm,unsigned frames,uint64_t now) {
    if(!p || !p->mixed || !pcm)return 0;
    if(p->ahead)return render_ready(p,pcm,frames);
    unsigned written=0;
    while(written<frames) {
        if(p->offset==p->block)render_block(p);
        unsigned n=p->block-p->offset;
        if(n>frames-written)n=frames-written;
        memcpy(pcm+(size_t)written*p->channels,p->mixed+(size_t)p->offset*p->channels,
            (size_t)n*p->channels*sizeof(int16_t));
        account(p,p->offset,n,p->now_us ? p->now_us(p->ctx) : now);
        p->offset+=n; written+=n;
    }
    p->stats.output_frames+=written;
    return written;
}

bool coop_audio_pull_set_ahead(coop_audio_pull_t *p,unsigned ahead_frames,unsigned trim_above,unsigned trim_to) {
    if(!p || !p->mixed || p->ahead || ahead_frames<p->block || trim_to>trim_above)return false;
    if(!coop_audio_spsc_init(&p->ready,ahead_frames+p->block))return false;
    p->drain_cap=1024;
    p->drain=calloc(p->drain_cap,sizeof(*p->drain));
    if(!p->drain) { coop_audio_spsc_destroy(&p->ready); return false; }
    atomic_init(&p->device_frames,0); atomic_init(&p->device_short,0);
    p->ahead=ahead_frames; p->trim_above=trim_above; p->trim_to=trim_to;
    return true;
}

unsigned coop_audio_pull_produce(coop_audio_pull_t *p,uint64_t now) {
    return coop_audio_pull_produce_max(p,now,UINT_MAX);
}

unsigned coop_audio_pull_produce_max(coop_audio_pull_t *p,uint64_t now,unsigned max_blocks) {
    if(!p || !p->ahead || !p->mixed)return 0;
    /* Bound the backlog: a queue that has grown past trim_above loses its oldest frames down to trim_to. */
    for(unsigned s=0;s<2;++s) {
        const unsigned depth=coop_audio_spsc_count(&p->source[s]);
        if(depth<=p->trim_above)continue;
        unsigned cut=depth-p->trim_to;
        while(cut) {
            const unsigned n=coop_audio_spsc_pop(&p->source[s],p->stamp[s],cut<p->block ? cut : p->block);
            if(!n)break;
            cut-=n; p->stats.trimmed[s]+=n;
        }
    }
    unsigned blocks=0;
    while(blocks<max_blocks && coop_audio_spsc_count(&p->ready)+p->block<=p->ahead) {
        render_block(p);
        account(p,0,p->block,p->now_us ? p->now_us(p->ctx) : now);
        if(!coop_audio_spsc_push(&p->ready,p->mixed,p->block,p->channels,p->produced-p->block,0,1,0))break;
        ++blocks;
    }
    p->stats.output_frames=atomic_load_explicit(&p->device_frames,memory_order_relaxed);
    p->stats.device_short=atomic_load_explicit(&p->device_short,memory_order_relaxed);
    return blocks;
}
