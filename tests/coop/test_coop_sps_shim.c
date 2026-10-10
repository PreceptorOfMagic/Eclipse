#include <string.h>
typedef struct { const unsigned char *d; size_t len, bit; int overrun; } bits_t;
static void bi(bits_t*b,const unsigned char*d,size_t l){b->d=d;b->len=l;b->bit=0;b->overrun=0;}
static unsigned bu(bits_t*b,unsigned n){unsigned v=0;for(unsigned i=0;i<n;i++){size_t p=b->bit++;if((p>>3)>=b->len){b->overrun=1;return v;}v=(v<<1)|((b->d[p>>3]>>(7-(p&7)))&1);}return v;}
static unsigned bue(bits_t*b){unsigned z=0;while(!bu(b,1)&&!b->overrun&&z<32)z++;if(z==0)return 0;return ((1u<<z)|bu(b,z))-1;}
static size_t rbsp(const unsigned char*in,size_t l,unsigned char*o,size_t c){size_t j=0,z=0;for(size_t i=0;i<l&&j<c;i++){if(z>=2&&in[i]==3){z=0;continue;}o[j++]=in[i];z=in[i]?0:z+1;}return j;}
static void sptl(bits_t*b,unsigned m){bu(b,8);bu(b,32);bu(b,4);bu(b,43);bu(b,1);bu(b,8);
 unsigned pf[8],lf[8];for(unsigned i=0;i<m;i++){pf[i]=bu(b,1);lf[i]=bu(b,1);}
 if(m>0)for(unsigned i=m;i<8;i++)bu(b,2);
 for(unsigned i=0;i<m;i++){if(pf[i]){bu(b,8);bu(b,32);bu(b,4);bu(b,43);bu(b,1);}if(lf[i])bu(b,8);}}
unsigned sps_height(const unsigned char*nal,size_t len){unsigned char rb[512];size_t n=rbsp(nal,len<512?len:512,rb,512);
 if(n<=2)return 0;bits_t b;bi(&b,rb+2,n-2);bu(&b,4);unsigned ms=bu(&b,3);bu(&b,1);sptl(&b,ms);bue(&b);
 unsigned c=bue(&b);if(c==3)bu(&b,1);bue(&b);return bue(&b);}
