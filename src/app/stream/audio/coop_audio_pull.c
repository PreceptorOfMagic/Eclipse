#include "coop_audio_pull.h"
#include <stdlib.h>
#include <string.h>

void coop_audio_pull_destroy(coop_audio_pull_t *p) {
    if (!p) return;
    for (unsigned s=0;s<2;++s) {
        coop_audio_spsc_destroy(&p->source[s]);
        free(p->stamp[s]); free(p->raw[s]); free(p->work[s]);
    }
    free(p->mixed);
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

unsigned coop_audio_pull_render(coop_audio_pull_t *p,int16_t *pcm,unsigned frames,uint64_t now) {
    if(!p || !p->mixed || !pcm)return 0;
    unsigned written=0;
    while(written<frames) {
        if(p->offset==p->block)render_block(p);
        unsigned n=p->block-p->offset;
        if(n>frames-written)n=frames-written;
        memcpy(pcm+(size_t)written*p->channels,p->mixed+(size_t)p->offset*p->channels,
            (size_t)n*p->channels*sizeof(int16_t));
        const uint64_t handed_us=p->now_us ? p->now_us(p->ctx) : now;
        for(unsigned s=0;s<2;++s) for(unsigned i=p->offset;i<p->offset+n;++i) {
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
        p->offset+=n; written+=n;
    }
    p->stats.output_frames+=written;
    return written;
}
