/* Independent slice-header reader, so a bug shared by writer and parser cannot pass. */
#include <string.h>
typedef struct { const unsigned char *d; size_t len, bit; int ov; } B;
static void bi(B*b,const unsigned char*d,size_t l){b->d=d;b->len=l;b->bit=0;b->ov=0;}
static unsigned bu(B*b,unsigned n){unsigned v=0;for(unsigned i=0;i<n;i++){size_t p=b->bit++;if((p>>3)>=b->len){b->ov=1;return v;}v=(v<<1)|((b->d[p>>3]>>(7-(p&7)))&1);}return v;}
static unsigned bue(B*b){unsigned z=0;while(!bu(b,1)&&!b->ov&&z<32)z++;if(!z)return 0;return ((1u<<z)|bu(b,z))-1;}
static size_t rbsp(const unsigned char*in,size_t l,unsigned char*o,size_t c){size_t j=0,z=0;for(size_t i=0;i<l&&j<c;i++){if(z>=2&&in[i]==3){z=0;continue;}o[j++]=in[i];z=in[i]?0:z+1;}return j;}
unsigned slice_addr_of(const unsigned char*nal,size_t len,int irap,int dep_enabled,unsigned addr_bits,unsigned*first_out){
  static unsigned char rb[1<<20];size_t n=rbsp(nal,len<sizeof(rb)?len:sizeof(rb),rb,sizeof(rb));
  if(n<=2){if(first_out)*first_out=9;return 0;}
  B b;bi(&b,rb+2,n-2);unsigned first=bu(&b,1);if(first_out)*first_out=first;
  if(irap)bu(&b,1);bue(&b);
  if(first)return 0;
  if(dep_enabled)bu(&b,1);
  return bu(&b,addr_bits);}
