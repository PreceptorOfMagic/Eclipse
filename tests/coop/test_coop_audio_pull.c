/* Offline pull ownership/metadata regression. No sound device or DAC-time claim. */
#include "coop_audio_pull.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint64_t age_hist_total(const uint64_t *hist) {
    uint64_t total=0;
    for(unsigned b=0;b<COOP_AUDIO_AGE_BUCKETS;++b)total+=hist[b];
    return total;
}

enum { BLOCK=240, CHANNELS=2, SAMPLES=BLOCK*CHANNELS, TOTAL=BLOCK*20 };
typedef struct {
    unsigned expected_valid[64], expected_epoch[64], expected, mixed, observed;
    int16_t original[2][SAMPLES], output[SAMPLES];
    uint64_t fake_now;
    unsigned fake_dsp_us, clock_calls;
} context;

static int16_t signal(unsigned role,unsigned index,unsigned channel) {
    return (int16_t)(100+role*300+(index%97)*2+channel);
}
static void expect(context *c,unsigned valid,unsigned epoch) {
    assert(c->expected<64);
    c->expected_valid[c->expected]=valid;
    c->expected_epoch[c->expected++]=epoch;
}
static void mix(void *opaque,int16_t *a,int16_t *b,int16_t *out,
                unsigned frames,unsigned valid,unsigned epoch) {
    context *c=opaque;
    assert(frames==BLOCK && c->mixed<c->expected);
    assert(valid==c->expected_valid[c->mixed]);
    assert(epoch==c->expected_epoch[c->mixed]);
    memcpy(c->original[0],a,sizeof c->original[0]);
    memcpy(c->original[1],b,sizeof c->original[1]);
    for(unsigned i=0;i<SAMPLES;++i) {
        out[i]=c->output[i]=(int16_t)(a[i]+b[i]);
        /* Observe must retain the untouched raw sources despite mix mutation. */
        a[i]=1234; b[i]=-1234;
    }
    ++c->mixed;
    c->fake_now+=c->fake_dsp_us;
}
static void observe(void *opaque,const int16_t *a,const int16_t *b,
                    const int16_t *out,unsigned frames,uint64_t index) {
    context *c=opaque;
    assert(frames==BLOCK && index==(uint64_t)c->observed*BLOCK);
    assert(c->mixed==c->observed+1);
    assert(!memcmp(a,c->original[0],sizeof c->original[0]));
    assert(!memcmp(b,c->original[1],sizeof c->original[1]));
    assert(!memcmp(out,c->output,sizeof c->output));
    ++c->observed;
}
static void push(coop_audio_pull_t *p,unsigned role,unsigned index,unsigned frames,
                 uint64_t ready,unsigned epoch,unsigned flags) {
    int16_t pcm[TOTAL*CHANNELS]; assert(frames<=TOTAL);
    for(unsigned i=0;i<frames;++i) for(unsigned ch=0;ch<CHANNELS;++ch)
        pcm[i*CHANNELS+ch]=signal(role,index+i,ch);
    assert(coop_audio_spsc_push(&p->source[role],pcm,frames,CHANNELS,index,ready,epoch,flags));
}
static void finish(coop_audio_pull_t *p,context *c) {
    assert(c->mixed==c->expected && c->observed==c->mixed);
    coop_audio_pull_destroy(p);
}

static void callback_quantum(unsigned quantum) {
    context c={0}; coop_audio_pull_t p;
    assert(coop_audio_pull_init(&p,CHANNELS,BLOCK,TOTAL,mix,observe,&c));
    for(unsigned i=0;i<TOTAL/BLOCK;++i)expect(&c,3,0);
    push(&p,0,0,TOTAL,1000,1,0); push(&p,1,0,TOTAL,2000,1,0);
    int16_t storage[480*CHANNELS+2], *out=storage+1;
    unsigned done=0,call=0; uint64_t now=0;
    while(done<TOTAL) {
        unsigned n=quantum; if(n>TOTAL-done)n=TOTAL-done;
        now=10000+(uint64_t)call++*1000;
        for(unsigned i=0;i<sizeof storage/sizeof storage[0];++i)storage[i]=7777;
        assert(coop_audio_pull_render(&p,out,n,now)==n);
        assert(storage[0]==7777 && storage[n*CHANNELS+1]==7777);
        for(unsigned i=0;i<n;++i)for(unsigned ch=0;ch<CHANNELS;++ch)
            assert(out[i*CHANNELS+ch]==signal(0,done+i,ch)+signal(1,done+i,ch));
        done+=n;
        for(unsigned s=0;s<2;++s) {
            assert(p.stats.consumed[s]==done && p.stats.timed[s]==done);
            assert(age_hist_total(p.stats.age_hist[s])==p.stats.timed[s]);
            assert(p.stats.age_max_us[s]==now-(s?2000:1000));
            assert(!p.stats.missing[s] && !p.stats.concealed[s] && !p.stats.discontinuities[s]);
        }
        assert(p.stats.output_frames==done);
    }
    assert(!coop_audio_spsc_count(&p.source[0]) && !coop_audio_spsc_count(&p.source[1]));
    finish(&p,&c);
}

static void independent_arrival(unsigned role) {
    context c={0}; coop_audio_pull_t p; int16_t out[480*CHANNELS];
    assert(coop_audio_pull_init(&p,CHANNELS,BLOCK,1000,mix,observe,&c));
    expect(&c,0,0);
    assert(coop_audio_pull_render(&p,out,127,2000)==127);
    push(&p,role,0,BLOCK,1000,1,0);
    /* New arrival cannot overwrite the already-produced missing tail. */
    assert(coop_audio_pull_render(&p,out,113,3000)==113);
    for(unsigned i=0;i<113*CHANNELS;++i)assert(out[i]==0);
    assert(coop_audio_spsc_count(&p.source[role])==BLOCK);
    expect(&c,1u<<role,0);
    assert(coop_audio_pull_render(&p,out,BLOCK,4000)==BLOCK);
    for(unsigned i=0;i<BLOCK;++i)for(unsigned ch=0;ch<CHANNELS;++ch)
        assert(out[i*CHANNELS+ch]==signal(role,i,ch));
    push(&p,role,BLOCK,BLOCK,3500,1,0);
    push(&p,1-role,0,192,3500,1,1); /* PLC samples play, but cannot validate analysis. */
    expect(&c,1u<<role,0); expect(&c,0,0);
    assert(coop_audio_pull_render(&p,out,480,5000)==480);
    for(unsigned i=0;i<480;++i)for(unsigned ch=0;ch<CHANNELS;++ch) {
        int expected=i<BLOCK?signal(role,BLOCK+i,ch):0;
        if(i<192)expected+=signal(1-role,i,ch);
        assert(out[i*CHANNELS+ch]==expected);
    }
    assert(p.stats.output_frames==960);
    assert(p.stats.consumed[role]==480 && p.stats.consumed[1-role]==192);
    assert(p.stats.missing[role]==480 && p.stats.missing[1-role]==768);
    assert(p.stats.concealed[1-role]==192 && !p.stats.concealed[role]);
    finish(&p,&c);
}

static void epoch_and_cached_age(unsigned role) {
    context c={0}; coop_audio_pull_t p; int16_t out[480*CHANNELS];
    assert(coop_audio_pull_init(&p,CHANNELS,BLOCK,1000,mix,observe,&c));
    push(&p,role,0,150,1000,7,0);
    push(&p,role,0,BLOCK,4500,8,0);
    push(&p,1-role,0,480,1000,9,0);
    expect(&c,1u<<(1-role),0);
    assert(coop_audio_pull_render(&p,out,127,2000)==127);
    assert(p.stats.consumed[role]==127 && p.stats.age_max_us[role]==1000);
    assert(p.stats.epoch[role]==7);
    assert(coop_audio_pull_render(&p,out,113,5000)==113);
    assert(p.stats.consumed[role]==150 && p.stats.missing[role]==90);
    assert(p.stats.age_max_us[role]==4000); /* Cached real tail uses this callback's time. */
    for(unsigned i=0;i<113;++i)for(unsigned ch=0;ch<CHANNELS;++ch) {
        int expected=signal(1-role,127+i,ch);
        if(i<23)expected+=signal(role,127+i,ch);
        assert(out[i*CHANNELS+ch]==expected);
    }
    expect(&c,3,1u<<role);
    assert(coop_audio_pull_render(&p,out,BLOCK,6000)==BLOCK);
    assert(p.stats.epoch[role]==8 && !p.stats.discontinuities[role]);
    for(unsigned i=0;i<BLOCK;++i)for(unsigned ch=0;ch<CHANNELS;++ch)
        assert(out[i*CHANNELS+ch]==signal(role,i,ch)+signal(1-role,BLOCK+i,ch));
    /* Same-epoch index gap passes its real PCM but invalidates that analysis block. */
    push(&p,role,1000,10,0,8,0);
    expect(&c,0,0);
    assert(coop_audio_pull_render(&p,out,BLOCK,7000)==BLOCK);
    assert(p.stats.discontinuities[role]==1 && p.stats.untimed[role]==10);
    for(unsigned i=0;i<BLOCK;++i)for(unsigned ch=0;ch<CHANNELS;++ch)
        assert(out[i*CHANNELS+ch]==(i<10?signal(role,1000+i,ch):0));
    push(&p,role,1010,BLOCK,9000,8,0); /* Future ready stamp is untimed, not negative age. */
    expect(&c,1u<<role,0);
    assert(coop_audio_pull_render(&p,out,BLOCK,8000)==BLOCK);
    assert(p.stats.untimed[role]==250 && p.stats.age_max_us[role]==4000);
    finish(&p,&c);
}

static uint64_t handoff_now(void *opaque) {
    context *c=opaque; ++c->clock_calls; return c->fake_now;
}
static void post_dsp_clock(void) {
    context c={0}; coop_audio_pull_t p; int16_t out[480*CHANNELS];
    assert(coop_audio_pull_init(&p,CHANNELS,BLOCK,1000,mix,observe,&c));
    p.now_us=handoff_now;
    for(unsigned i=0;i<3;++i)expect(&c,3,0);
    for(unsigned s=0;s<2;++s)push(&p,s,0,3*BLOCK,1000,1,0);
    c.fake_now=2000; c.fake_dsp_us=700;
    assert(coop_audio_pull_render(&p,out,127,2000)==127);
    assert(p.stats.age_max_us[0]==1700 && p.stats.age_max_us[1]==1700);
    assert(c.clock_calls==1 && c.mixed==1);
    c.fake_now=5000;
    assert(coop_audio_pull_render(&p,out,113,5000)==113);
    assert(p.stats.age_max_us[0]==4000 && p.stats.age_max_us[1]==4000);
    assert(c.clock_calls==2 && c.mixed==1); /* Cached tail still gets a fresh handoff clock. */
    c.fake_now=6000;
    assert(coop_audio_pull_render(&p,out,480,6000)==480);
    assert(p.stats.age_max_us[0]==6400 && p.stats.age_max_us[1]==6400);
    assert(c.clock_calls==4 && c.mixed==3); /* Each DSP portion is included. */
    assert(p.stats.consumed[0]==720 && p.stats.timed[0]==720);
    finish(&p,&c);
}

/* Produced ahead (Android): the worker mixes blocks into the ready ring, the callback only copies them out. */
static void produced_ahead(void) {
    context c={0}; coop_audio_pull_t p; int16_t out[480*CHANNELS];
    assert(coop_audio_pull_init(&p,CHANNELS,BLOCK,TOTAL,mix,observe,&c));
    assert(coop_audio_pull_set_ahead(&p,3*BLOCK,TOTAL,BLOCK));
    assert(!coop_audio_pull_set_ahead(&p,3*BLOCK,TOTAL,BLOCK)); /* once */
    push(&p,0,0,3*BLOCK,1000,1,0); push(&p,1,0,3*BLOCK,1000,1,0);
    for(unsigned i=0;i<3;++i)expect(&c,3,0);
    assert(coop_audio_pull_produce_max(&p,2000,1)==1);      /* capped: one block, the rest on the next call */
    assert(coop_audio_spsc_count(&p.ready)==BLOCK && c.mixed==1);
    assert(coop_audio_pull_produce(&p,2000)==2);
    assert(coop_audio_spsc_count(&p.ready)==3*BLOCK && c.mixed==3);
    assert(coop_audio_pull_produce(&p,2100)==0);            /* full: nothing more */
    assert(p.stats.consumed[0]==3*BLOCK && p.stats.age_max_us[0]==1000);
    /* The callback copies, mixes nothing, and is never short while the ring holds enough. */
    assert(coop_audio_pull_render(&p,out,256,3000)==256 && c.mixed==3);
    for(unsigned i=0;i<256;++i)for(unsigned ch=0;ch<CHANNELS;++ch)
        assert(out[i*CHANNELS+ch]==signal(0,i,ch)+signal(1,i,ch));
    assert(atomic_load(&p.device_frames)==256 && !atomic_load(&p.device_short));
    /* Topped up as the device drains it; with the sources empty the new block is missing, so invalid. */
    expect(&c,0,0);
    assert(coop_audio_pull_produce(&p,4000)==1 && coop_audio_spsc_count(&p.ready)==3*BLOCK-256+BLOCK);
    assert(p.stats.missing[0]==BLOCK && p.stats.output_frames==256);
    /* Drained past what is ready: the rest is silence, counted short. */
    const unsigned ready=coop_audio_spsc_count(&p.ready);
    assert(coop_audio_pull_render(&p,out,480,5000)==480);
    assert(coop_audio_pull_render(&p,out,480,5100)==480);
    assert(atomic_load(&p.device_short)==960-ready);
    for(unsigned i=ready>480?ready-480:0;i<480;++i)for(unsigned ch=0;ch<CHANNELS;++ch)assert(out[i*CHANNELS+ch]==0);
    finish(&p,&c);
}

/* A queue grown past trim_above loses its oldest frames down to trim_to before the next block is mixed. */
static void trims_backlog(void) {
    context c={0}; coop_audio_pull_t p;
    assert(coop_audio_pull_init(&p,CHANNELS,BLOCK,TOTAL,mix,observe,&c));
    assert(coop_audio_pull_set_ahead(&p,BLOCK,600,BLOCK));
    push(&p,0,0,1000,1000,1,0); push(&p,1,0,BLOCK,1000,1,0);
    expect(&c,3,0);
    assert(coop_audio_pull_produce(&p,2000)==1);
    assert(p.stats.trimmed[0]==1000-BLOCK && !p.stats.trimmed[1]);
    for(unsigned i=0;i<BLOCK;++i)for(unsigned ch=0;ch<CHANNELS;++ch)   /* the newest frames played */
        assert(c.output[i*CHANNELS+ch]==signal(0,1000-BLOCK+i,ch)+signal(1,i,ch));
    /* Within the limit nothing is cut; the later gap in the indices reads as a discontinuity. */
    push(&p,0,2000,BLOCK,1000,1,0); push(&p,1,BLOCK,BLOCK,1000,1,0);
    int16_t out[BLOCK*CHANNELS];
    assert(coop_audio_pull_render(&p,out,BLOCK,3000)==BLOCK);
    expect(&c,2,0);
    assert(coop_audio_pull_produce(&p,3000)==1);
    assert(p.stats.trimmed[0]==1000-BLOCK && p.stats.discontinuities[0]==1);
    finish(&p,&c);
}

int main(void) {
    produced_ahead(); trims_backlog();
    callback_quantum(127); callback_quantum(240); callback_quantum(480);
    for(unsigned role=0;role<2;++role) {
        independent_arrival(role); epoch_and_cached_age(role);
    }
    post_dsp_clock();
    puts("pull: produced-ahead ring + backlog trim, callback quanta, independent arrival, cached-tail age, PLC/index validity and epoch conservation PASS");
    return 0;
}
