/* Exercise the actual upstream receive loop with a deterministic UDP script.
 * No sockets, host, audio device or alternate copy of its startup policy.
 * Compile with function sections + gc-sections; unused stream lifecycle is not linked.
 * AUDIO_STREAM_SOURCE permits running the identical fixture against the frozen
 * pre-change source; EXPECT_OLD_POLICY documents that regression reproduction. */
#ifndef AUDIO_STREAM_SOURCE
#define AUDIO_STREAM_SOURCE "../../core/moonlight-common-c/src/AudioStream.c"
#endif
#include AUDIO_STREAM_SOURCE
#include <assert.h>
#include <stdarg.h>

CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
AUDIO_RENDERER_CALLBACKS AudioCallbacks;
STREAM_CONFIGURATION StreamConfig;
int AudioPacketDuration;
int AppVersionQuad[4];
bool AudioEncryptionEnabled;

static int events[2048], eventCount, cursor, forwarded, dataCount, fecCount;
static bool selectFallback;
static char finalLog[1024];

static void logMessage(const char *format, ...) {
    if (strstr(format,"[AUDIO-RECEIVE]")) {
        va_list ap; va_start(ap,format);
        vsnprintf(finalLog,sizeof(finalLog),format,ap); va_end(ap);
    }
}
bool PltIsThreadInterrupted(PLT_THREAD *thread) { (void)thread; return cursor == eventCount; }
uint64_t PltGetMillis(void) { return 999999; }
int setNonFatalRecvTimeoutMs(SOCKET s,int ms) { (void)s; assert(ms==UDP_RECV_POLL_TIMEOUT_MS); return selectFallback?-1:0; }
int recvUdpSocket(SOCKET s,char *buffer,int size,bool useSelect) {
    (void)s; assert(useSelect==selectFallback && size>=(int)sizeof(RTP_PACKET));
    int kind=events[cursor++];
    if (kind<=0) return 0;
    if (kind==1) return sizeof(RTP_PACKET)-1;
    PRTP_PACKET p=(PRTP_PACKET)buffer;
    memset(p,0,sizeof(*p)); p->packetType=(uint8_t)kind;
    p->sequenceNumber=BE16((uint16_t)cursor);
    return sizeof(*p);
}
int recvUdpSocketWithTimeoutStatus(SOCKET s,char *buffer,int size,bool useSelect,bool *timedOut) {
    *timedOut=events[cursor]==0;
    return recvUdpSocket(s,buffer,size,useSelect);
}
int RtpaAddPacket(PRTP_AUDIO_QUEUE q,PRTP_PACKET p,uint16_t length) {
    (void)q; assert(length==sizeof(*p)); ++forwarded;
    if(p->packetType==97)++dataCount;
    if(p->packetType==127)++fecCount;
    return 0; /* Sink at the receiver->RTP-queue boundary; no decoder claim. */
}
PRTP_PACKET RtpaGetQueuedPacket(PRTP_AUDIO_QUEUE q,uint16_t h,uint16_t *n) {
    (void)q;(void)h;(void)n; abort();
}
int LbqOfferQueueItem(PLINKED_BLOCKING_QUEUE q,void *d,PLINKED_BLOCKING_QUEUE_ENTRY e) {
    (void)q;(void)d;(void)e;abort();
}
PLINKED_BLOCKING_QUEUE_ENTRY LbqFlushQueueItems(PLINKED_BLOCKING_QUEUE q) { (void)q;abort(); }
bool PltDecryptMessage(PPLT_CRYPTO_CONTEXT c,int a,int f,unsigned char*k,int kl,
    unsigned char*iv,int il,unsigned char*t,int tl,unsigned char*in,int inl,unsigned char*out,int*ol) {
    (void)c;(void)a;(void)f;(void)k;(void)kl;(void)iv;(void)il;
    (void)t;(void)tl;(void)in;(void)inl;(void)out;(void)ol;abort();
}
static void reset(int duration,bool fallback) {
    AudioPacketDuration=duration;selectFallback=fallback;
    eventCount=cursor=forwarded=dataCount=fecCount=0;
    receivedDataFromPeer=false;firstReceiveTime=0;finalLog[0]=0;
    ListenerCallbacks.logMessage=logMessage;
}
static void add(int kind,int n) { while(n--) { assert(eventCount<2048);events[eventCount++]=kind; } }
static void run(int expectedData,int expectedFec,int expectedDropped) {
    AudioReceiveThreadProc(NULL);
    assert(dataCount==expectedData && fecCount==expectedFec);
    assert(forwarded==dataCount+fecCount);
#ifndef EXPECT_OLD_POLICY
    char token[96];snprintf(token,sizeof(token),"startup_dropped_data_packets=%d ",expectedDropped);
    assert(strstr(finalLog,token));
#else
    (void)expectedDropped;
#endif
}
int main(void) {
    for(int duration=5;duration<=10;duration+=5)for(int fallback=0;fallback<2;++fallback) {
        int budget=500/duration;
        /* A first sound after either 100 ms or minutes of silence is future
         * content, not socket backlog. FEC before data must not re-arm dropping. */
        for(int idle=1;idle<=1200;idle+=1199) {
            reset(duration,fallback);add(0,idle);add(127,1);add(97,budget+4);
#ifdef EXPECT_OLD_POLICY
            run(4,0,budget);
#else
            run(budget+4,1,0);
#endif
        }
        /* Retain compatibility for an uninterrupted startup backlog. */
        reset(duration,fallback);add(127,1);add(97,budget+4);run(4,0,budget);
        /* Draining a short initial backlog must end catch-up permanently. */
        reset(duration,fallback);add(97,3);add(0,1);add(97,budget+4);run(budget+4,0,3);
        /* A runt packet alone is not evidence that the socket drained. */
        reset(duration,fallback);add(1,1);add(97,budget+4);run(4,0,budget);
        reset(duration,fallback);add(0,1);add(1,1);add(97,2);
#ifdef EXPECT_OLD_POLICY
        run(0,0,2);
#else
        run(2,0,0);
#endif
        /* Interrupted receive/empty datagrams are not timeout evidence before
         * the first packet; after it they clear the budget exactly as upstream
         * does, so an IO_PENDING rig is never worse than upstream. */
        reset(duration,fallback);add(-1,1);add(97,budget+4);run(4,0,budget);
        reset(duration,fallback);add(97,3);add(-1,1);add(97,budget+4);run(budget+4,0,3);
    }
    puts("audio receive startup scenarios PASS (receiver boundary only)");
    return 0;
}
