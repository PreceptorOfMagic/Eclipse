/* Mixer component contract; real DSP with synthetic indexed blocks, no device. */
#ifdef NDEBUG
#error "Mixer contract assertions must be enabled"
#endif
#include <assert.h>
#undef AURORA_DIAG_LOG
#undef AURORA_SECONDARY_LOG
#define AURORA_DIAG_LOG "/dev/null"
#define AURORA_SECONDARY_LOG "/dev/null"
#include "../../src/app/stream/audio/coop_audio.c"
const char *coop_secondary_mode(void) { return NULL; }
static void block(int16_t *self,int16_t *peer,int a,int b) {
    for(int i=0;i<480;++i){self[i]=(int16_t)a;peer[i]=(int16_t)b;}
}
static void input_push(uint64_t index,unsigned frames,unsigned flags) {
    int16_t pcm[4]; assert(frames<=4);
    for(unsigned i=0;i<frames;++i)pcm[i]=(int16_t)(index+i+1);
    assert(coop_audio_fifo_push(&input_fifo[0],pcm,frames,index,1,flags));
}
static void input_take(unsigned mask,const int16_t expected[4]) {
    int16_t out[4]={-1,-1,-1,-1};
    assert(render_input(&input_fifo[0],out,4,100,COOP_PGATE_VALID_SELF)==mask);
    assert(memcmp(out,expected,sizeof(out))==0);
}
static void input_validity(void) {
    assert(coop_audio_fifo_init(&input_fifo[0],16,1));
    output_source_indexed[0]=false;output_source_next[0]=0;output_invalid_index_blocks[0]=0;
    input_push(0,4,0);input_take(COOP_PGATE_VALID_SELF,(int16_t[]){1,2,3,4});
    input_push(8,4,0);input_take(0,(int16_t[]){9,10,11,12}); /* Inter-block gap. */
    assert(output_invalid_index_blocks[0]==1);
    input_push(12,4,0);input_take(COOP_PGATE_VALID_SELF,(int16_t[]){13,14,15,16});
    input_push(16,2,0);input_push(19,2,0);input_take(0,(int16_t[]){17,18,20,21});
    assert(output_invalid_index_blocks[0]==2); /* Intra-block gap; PCM retained. */
    input_push(21,4,1);input_take(0,(int16_t[]){22,23,24,25}); /* PLC. */
    input_push(25,2,0);input_take(0,(int16_t[]){26,27,0,0}); /* Missing tail only. */
    input_push(27,4,0);input_take(COOP_PGATE_VALID_SELF,(int16_t[]){28,29,30,31});
    assert(output_invalid_index_blocks[0]==2);
    assert(input_fifo[0].accepted==26&&input_fifo[0].consumed==26&&input_fifo[0].missing==2);
    coop_audio_fifo_destroy(&input_fifo[0]);
}
int main(void) {
    input_validity();
    coord_rate=48000;coord_channels=2;output_clocked=true;
    dedupe_on=false;gate_on=true;filter_active=false;listen_mode=COOP_AUDIO_MIX;
    limiter_gain=1;output_role_gain[0]=output_role_gain[1]=COOP_AUDIO_MIX_GAIN;
    int16_t a[480],b[480];block(a,b,2000,4000);
    mix_blocks(a,b,240,true,COOP_PGATE_VALID_BOTH);
    for(int i=0;i<480;++i)assert(a[i]==4500);
    listen_mode=COOP_AUDIO_PRIMARY_ONLY;block(a,b,2000,4000);
    mix_blocks(a,b,240,true,COOP_PGATE_VALID_BOTH);
    assert(a[0]>=4488&&a[0]<=4491&&a[478]==2000);
    for(int i=2;i<480;i+=2){assert(a[i]<=a[i-2]);assert(a[i]==a[i+1]);}
    listen_mode=COOP_AUDIO_SECONDARY_ONLY;block(a,b,2000,4000);
    mix_blocks(a,b,240,true,COOP_PGATE_VALID_BOTH);
    assert(a[0]>=2007&&a[0]<=2009&&a[478]==4000);
    for(int i=2;i<480;i+=2){assert(a[i]>=a[i-2]);assert(a[i]==a[i+1]);}
    /* Counterexample for the old attack interpolation: .825*40000 exceeds
     * int16 range before the gain ramp reaches its safe endpoint. */
    listen_mode=COOP_AUDIO_MIX;
    output_role_gain[0]=output_role_gain[1]=COOP_AUDIO_MIX_GAIN;
    for(int sign=-1;sign<=1;sign+=2) {
        limiter_gain=.825f;block(a,b,sign*26667,sign*26667);
        mix_blocks(a,b,240,true,COOP_PGATE_VALID_BOTH);
        for(int i=0;i<480;++i) {
            assert(a[i]!=32767&&a[i]!=-32768);
            assert(abs(a[i])<=(int)COOP_AUDIO_LIMIT);
            assert(abs(a[i])>=31999); /* A limiter bound, not arbitrary muting. */
        }
        assert(limiter_gain>=.7999f&&limiter_gain<=.8001f);
    }
    block(a,b,1000,1000);mix_blocks(a,b,240,true,COOP_PGATE_VALID_BOTH);
    assert(a[0]>=1199&&a[0]<=1200&&a[478]>=1201&&a[478]<=1202);
    for(int i=2;i<480;i+=2){assert(a[i]>=a[i-2]);assert(a[i]==a[i+1]);}
    /* The mix ceiling must not reduce a settled single source's full range.
     * Settle role transition and limiter recovery before testing steady state. */
    const int solo_modes[]={COOP_AUDIO_PRIMARY_ONLY,COOP_AUDIO_SECONDARY_ONLY};
    for(unsigned mode=0;mode<2;++mode) for(int sign=-1;sign<=1;sign+=2) {
        listen_mode=solo_modes[mode];
        for(int pass=0;pass<3;++pass) {
            block(a,b,mode==0?sign*32767:-sign*23456,mode==1?sign*32767:-sign*23456);
            mix_blocks(a,b,240,true,COOP_PGATE_VALID_BOTH);
            if(pass==2) for(int i=0;i<480;++i)assert(a[i]==sign*32767);
        }
    }
    /* Analysis continues through role/policy changes; no hidden held block. */
    gate=coop_pgate_create(48000,2,1000);assert(gate);
    coop_pgate_stats_t stats;
    for(int n=0;n<12;++n) {
        listen_mode=n%3;dedupe_on=n%2;block(a,b,0,0);
        mix_blocks(a,b,240,true,COOP_PGATE_VALID_BOTH);
        coop_pgate_get_stats(gate,&stats);assert(stats.blocks==(unsigned)n+1);
        for(int i=0;i<480;++i)assert(a[i]==0);
    }
    coop_pgate_destroy(gate);gate=NULL;
    puts("mixer: role ramps preserve block timing; safe mix limiter and bit-exact settled solo; bypass/roles keep gate history advancing; real input bridge preserves PCM across invalid gap/PLC/missing blocks");
    return 0;
}
