/* Test the actual socket wrapper, including its unchanged legacy return values.
 * Syscalls are scripted; no network or device is opened. */
#include "../../core/moonlight-common-c/src/Limelight-internal.h"
#include <assert.h>
static int receiveResult, receiveError, pollResult, pollError, receives, polls;
static bool retryOnce;
#ifdef LC_WINDOWS
static int WSAAPI scripted_recvfrom(SOCKET s,char *b,int n,int flags,struct sockaddr *a,int *l)
#else
static ssize_t scripted_recvfrom(int s,void *b,size_t n,int flags,struct sockaddr *a,socklen_t *l)
#endif
{
    (void)s;(void)b;(void)n;(void)flags;(void)a;(void)l; ++receives;
    if(retryOnce && receives==1) {
#ifdef LC_WINDOWS
        SetLastSocketError(WSAECONNRESET);
#else
        SetLastSocketError(ECONNREFUSED);
#endif
        return -1;
    }
    SetLastSocketError(receiveError);return receiveResult;
}
#ifdef LC_WINDOWS
static int WSAAPI scripted_select(int n,fd_set *r,fd_set *w,fd_set *e,const struct timeval *t) {
    (void)n;(void)r;(void)w;(void)e;(void)t;++polls;SetLastSocketError(pollError);return pollResult;
}
#define select scripted_select
#else
static int scripted_poll(struct pollfd *p,nfds_t n,int t) {
    (void)p;(void)n;assert(t==UDP_RECV_POLL_TIMEOUT_MS);++polls;SetLastSocketError(pollError);return pollResult;
}
#define poll scripted_poll
#endif
#define recvfrom scripted_recvfrom
#include "../../core/moonlight-common-c/src/PlatformSockets.c"
#undef recvfrom

static void check(bool selectMode,int returned,bool timeout) {
    char b[16];bool timedOut=true; receives=polls=0;
    assert(recvUdpSocketWithTimeoutStatus(3,b,sizeof(b),selectMode,&timedOut)==returned);
    assert(timedOut==timeout);
    if(retryOnce)assert(receives==2);
    receives=polls=0;
    assert(recvUdpSocket(3,b,sizeof(b),selectMode)==returned);
}
int main(void) {
    pollResult=1;receiveResult=16;check(false,16,false);check(true,16,false);
    receiveResult=0;check(false,0,false);check(true,0,false); /* empty datagram */
    receiveResult=-1;
    int timeouts[]={EWOULDBLOCK,EAGAIN,ETIMEDOUT};
    for(unsigned i=0;i<sizeof(timeouts)/sizeof(timeouts[0]);++i) {
        receiveError=timeouts[i];check(false,0,true);
        check(true,-1,false); /* Existing select path does not reinterpret errors. */
    }
    receiveError=EINTR;check(false,0,false);check(true,-1,false);
#ifdef LC_WINDOWS
    receiveError=WSA_IO_PENDING;check(false,0,false);check(true,-1,false);
#endif
    receiveError=0;receiveResult=16;
    pollResult=0;check(true,0,true);
    pollResult=-1;pollError=EINTR;check(true,-1,false);
    pollResult=1;pollError=0;retryOnce=true;
    check(false,16,false);check(true,16,false);
    receiveResult=-1;receiveError=ETIMEDOUT;check(false,0,true);
    puts("UDP timeout classification PASS (scripted syscalls)");
    return 0;
}
