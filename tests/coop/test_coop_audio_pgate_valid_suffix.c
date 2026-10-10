/* White-box helper regression against range_valid, including retained-history wrap.
 * Compile this test alone: it includes the production translation unit below. */
#include "../../src/app/stream/audio/coop_audio_pgate.c"
#include <assert.h>
#include <stdio.h>

static void compare(const coop_pgate_t *g, int stream, unsigned long long end, int limit) {
    int expected=0;
    for (int n=1;n<=limit && (unsigned long long)n<=end;++n) {
        if (!range_valid(g,stream,end-(unsigned)n,n)) break;
        expected=n;
    }
    assert(valid_suffix(g,stream,end,limit)==expected);
}
int main(void) {
    coop_pgate_t *g=coop_pgate_create(48000,2,1000); assert(g);
    const int B=coop_pgate_block_frames(g), N=48000*8;
    int16_t a[480]={0}, b[480]={0}; assert(B==240);
    for (int frame=0;frame<N;frame+=B) {
        /* Different per-source PLC/missing patterns; no fabricated valid samples. */
        unsigned mask=0, block=(unsigned)(frame/B);
        if (block%7!=0) mask|=COOP_PGATE_VALID_SELF;
        if (block%11!=0) mask|=COOP_PGATE_VALID_PEER;
        assert(coop_pgate_process_ready_block_valid(g,a,b,B,mask));
    }
    assert((unsigned)N>g->rlen);
    for (int s=0;s<2;++s) {
        for (unsigned long long end=0;end<=(unsigned)N+B;end+=317) {
            compare(g,s,end,B);
            compare(g,s,end,B*8);
        }
        const unsigned long long oldest=(unsigned)N-g->rlen+1;
        /* Include the exact overwritten-history boundary inside a validity block. */
        for (unsigned long long end=oldest-1;end<=oldest+(unsigned)(2*B);++end)
            compare(g,s,end,B*8);
        compare(g,s,N,B*8);
        compare(g,s,N+1,B*8);
    }
    assert(coop_pgate_reset(g));
    compare(g,0,0,B); compare(g,1,B,B);
    puts("valid suffix boundary/reset assertions PASS");
    coop_pgate_destroy(g);
    return 0;
}
