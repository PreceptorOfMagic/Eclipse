/* Component evidence: production output owner with two indexed production
 * FIFOs and a simulated device. No device/perceptual latency assertion. */
#include "coop_audio_output.h"
#include "coop_audio_fifo.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    coop_audio_fifo_t source[2];
    uint64_t generated[2], heard[2], rendered;
    unsigned queued, quantum, waits;
    int16_t pending[480];
    bool retrying;
} rig_t;

static void render(void *ctx, int16_t *out, unsigned frames) {
    rig_t *r=ctx; int16_t a[240],b[240]; assert(frames==240); assert(!r->retrying);
    coop_audio_fifo_render(&r->source[0],a,frames,1000000);
    coop_audio_fifo_render(&r->source[1],b,frames,1000000);
    for(unsigned i=0;i<frames;++i){out[2*i]=a[i];out[2*i+1]=b[i];}
    ++r->rendered;
}
static int feed(void *ctx,const int16_t *pcm,unsigned frames) {
    rig_t *r=ctx;
    if(r->retrying) assert(memcmp(r->pending,pcm,sizeof(r->pending))==0);
    if(r->waits) {
        memcpy(r->pending,pcm,sizeof(r->pending)); r->retrying=true; --r->waits;
        return COOP_OUTPUT_FEED_WAIT;
    }
    r->retrying=false;
    for(unsigned i=0;i<frames;++i) for(unsigned s=0;s<2;++s) {
        int value=pcm[2*i+s];
        if(value) {++r->heard[s]; assert(value==(int)r->heard[s]);}
    }
    r->queued+=frames;
    return COOP_OUTPUT_FEED_OK;
}
static void push(rig_t *r,unsigned source,unsigned frames) {
    int16_t pcm[480]; assert(frames<=480);
    for(unsigned i=0;i<frames;++i) pcm[i]=(int16_t)(r->generated[source]+i+1);
    assert(coop_audio_fifo_push(&r->source[source],pcm,frames,r->generated[source],1,0));
    r->generated[source]+=frames;
}
static void exercise(unsigned quantum) {
    rig_t r={.quantum=quantum}; coop_output_t out;
    assert(coop_output_init(&out,48000,2,240,7));
    for(int s=0;s<2;++s)assert(coop_audio_fifo_init(&r.source[s],24000,1));
    /* Source zero stops for 25 packets, then resumes. Peer timing and device
     * demand continue. Packet bursts and device periods deliberately differ. */
    for(unsigned t=0;t<60000;t+=48) {
        if(t<24000 && t%240==0) {
            if(t<4800||t>=10800)push(&r,0,240);
            if(t%480==0)push(&r,1,480);
        }
        /* Model actual callbacks on their own frame boundaries, not packet time. */
        if(t/quantum!=(t+48)/quantum)r.queued=r.queued>quantum?r.queued-quantum:0;
        if(t==12000)r.waits=3;
        for(unsigned calls=0;calls<16;++calls) {
            coop_output_clock_t c={48000,quantum,7,r.queued};
            int rc=coop_output_step(&out,&c,render,feed,&r);
            assert(rc!=COOP_OUTPUT_ERROR);
            unsigned target=quantum>240?quantum:240;
            assert(r.queued<target+240);
            if(rc!=COOP_OUTPUT_PROGRESS)break;
        }
    }
    for(int s=0;s<2;++s){
        assert(r.heard[s]==r.generated[s]);assert(r.source[s].count==0);
        assert(r.source[s].accepted==r.source[s].consumed);
        assert(!r.source[s].rejected&&!r.source[s].discarded);
        coop_audio_fifo_destroy(&r.source[s]);
    }
    assert(out.submitted_frames==r.rendered*240);assert(out.retries==3);
    /* A generation change must never feed old rendered audio to a new device. */
    uint64_t rendered=r.rendered,submitted=out.submitted_frames;
    coop_output_clock_t changed={48000,quantum,8,0};
    assert(coop_output_step(&out,&changed,render,feed,&r)==COOP_OUTPUT_ERROR);
    assert(r.rendered==rendered&&out.submitted_frames==submitted&&out.failed);
    coop_output_destroy(&out);
    printf("output owner: period=%u, independent silence/resume, indexed continuity and retry passed\n",quantum);
}
static void fifo_boundaries(void) {
    coop_audio_fifo_t q;assert(coop_audio_fifo_init(&q,7,1));
    int16_t a[]={1,2,3,4,5},b[8]={0};coop_audio_stamp_t stamp;
    assert(coop_audio_fifo_push(&q,a,5,100,10,0));
    assert(!coop_audio_fifo_push(&q,a,3,105,20,1));assert(q.count==5);
    coop_audio_fifo_consume(&q,3,30);
    assert(coop_audio_fifo_push(&q,a,5,105,20,1));
    assert(coop_audio_fifo_peek(&q,b,7,&stamp)==2&&stamp.index==103&&stamp.flags==0);
    assert(b[0]==4&&b[1]==5);coop_audio_fifo_consume(&q,2,40);
    assert(coop_audio_fifo_peek(&q,b,7,&stamp)==5&&stamp.index==105&&stamp.flags==1);
    coop_audio_fifo_clear(&q);
    assert(q.accepted==q.consumed+q.discarded+q.count);assert(q.rejected==3);
    coop_audio_fifo_destroy(&q);
}
int main(void){fifo_boundaries();exercise(127);exercise(240);exercise(480);return 0;}
