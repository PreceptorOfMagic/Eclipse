/* Reproduce the LIVE tiled composite offline: OUR pane in tile 0, the PEER's in tile 1.
 * The mirror harness (test_tile_compose) decodes cleanly, so pairing two DIFFERENT panes is the
 * only untested difference against the black live panel.
 *   ./test_tile_pair mine.h265 peer.h265 out.h265 [N] [--poc]
 * --poc also retimes both halves to a composite-owned POC, as the live path does. */
#include "../../src/app/stream/video/coop_splice.h"
#include "../../src/app/stream/video/coop_poc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char A[24u<<20], B[24u<<20], out[8u<<20], nv[64u<<10], pbuf[8u<<20], qbuf[8u<<20];
static size_t next_au(const unsigned char *b, size_t n, size_t off, size_t *end) {
    size_t e = off + 4;
    while (e + 4 <= n) { if (b[e]==0&&b[e+1]==0&&b[e+2]==0&&b[e+3]==1 &&
                            (((b[e+4]>>1)&0x3F)==32) && e>off) break; e++; }
    *end = (e + 4 > n) ? n : e; return *end;
}
int main(int argc,char**argv){
    if(argc<4){fprintf(stderr,"usage: %s mine.h265 peer.h265 out.h265 [N] [--poc]\n",argv[0]);return 2;}
    int want=(argc>4)?atoi(argv[4]):40; int dopoc=0;
    for(int i=1;i<argc;i++) if(!strcmp(argv[i],"--poc")) dopoc=1;
    FILE*f=fopen(argv[1],"rb"); size_t na=fread(A,1,sizeof(A),f); fclose(f);
    f=fopen(argv[2],"rb"); size_t nb=fread(B,1,sizeof(B),f); fclose(f);
    FILE*o=fopen(argv[3],"wb");
    coop_poc_params_t mp,pp,comp; size_t nvl=0,ao=0,bo=0,made=0,skip=0; unsigned poc=0;
    static const unsigned char sc[4]={0,0,0,1};
    while(ao<na&&bo<nb&&(int)made<want){
        size_t ae,be; next_au(A,na,ao,&ae); next_au(B,nb,bo,&be);
        coop_au_t am,bm;
        int oka=coop_au_parse(A+ao,ae-ao,&am)&&am.slice[0];
        int okb=coop_au_parse(B+bo,be-bo,&bm)&&bm.slice[0];
        if(oka&&okb){
            if(!nvl&&am.non_vcl&&bm.non_vcl){
                if(!coop_poc_params_parse(am.non_vcl,am.non_vcl_len,&mp)||!mp.valid){puts("mine params fail");return 1;}
                if(!coop_poc_params_parse(bm.non_vcl,bm.non_vcl_len,&pp)||!pp.valid){puts("peer params fail");return 1;}
                nvl=coop_nonvcl_tile_rows(am.non_vcl,am.non_vcl_len,nv,sizeof(nv));
                if(!nvl||!coop_poc_params_parse(nv,nvl,&comp)||!comp.valid){puts("composite fail");return 1;}
                printf("mine %ux%u  peer %ux%u  composite %ux%u  bottom=%u  poc_rewrite=%d\n",
                       mp.w_ctbs,mp.h_ctbs,pp.w_ctbs,pp.h_ctbs,comp.w_ctbs,comp.h_ctbs,
                       coop_bottom_half_addr(&comp),dopoc);
            }
            const int ta=(am.slice[0][0]>>1)&0x3F, tb=(bm.slice[0][0]>>1)&0x3F;
            if(nvl && ta==tb){       /* every VCL NAL of one picture shares nal_unit_type */
                const unsigned char *sa=am.slice[0],*sb=bm.slice[0];
                size_t la=am.slice_len[0],lb=bm.slice_len[0];
                if(dopoc && !(ta>=16&&ta<=23)){
                    poc++;
                    size_t r=coop_poc_rewrite(&mp,sa,la,poc,pbuf,sizeof(pbuf)); if(r){sa=pbuf;la=r;}
                    r=coop_poc_rewrite(&pp,sb,lb,poc,qbuf,sizeof(qbuf)); if(r){sb=qbuf;lb=r;}
                }
                size_t p=0; memcpy(out,nv,nvl); p=nvl;
                memcpy(out+p,sc,4); p+=4;
                size_t g=coop_slice_to_tile(&mp,&comp,sa,la,0,out+p,sizeof(out)-p);
                if(g){ p+=g; memcpy(out+p,sc,4); p+=4;
                    size_t g2=coop_slice_to_tile(&pp,&comp,sb,lb,coop_bottom_half_addr(&comp),
                                                 out+p,sizeof(out)-p);
                    if(g2){ p+=g2; fwrite(out,1,p,o); made++; } else skip++;
                } else skip++;
            } else if(nvl) skip++;
        }
        ao=ae; bo=be;
    }
    fclose(o); printf("composed %zu, skipped %zu\n",made,skip); return made?0:1;
}
