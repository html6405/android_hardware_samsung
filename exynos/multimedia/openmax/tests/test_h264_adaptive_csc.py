#!/usr/bin/env python3
"""Exercise adaptive AVC output pitches against the scalar CSC reference.

The device build and playback exercise NEON; this host test checks plane
contents and untouched destination padding with ASan and UBSan enabled.
"""
from pathlib import Path
import subprocess, tempfile
openmax = Path(__file__).resolve().parents[1]
source = openmax / 'component/video/dec/h264/SEC_OMX_H264dec.c'
csc = openmax.parent / 'utils/csc/exynos4'
helper = source.read_text().split('static OMX_ERRORTYPE SEC_H264_CopyTiledOutput(', 1)[1].split('\n#ifdef USE_ANB', 1)[0]
helper = 'static OMX_ERRORTYPE SEC_H264_CopyTiledOutput(' + helper
prefix = r'''
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "color_space_convertor.h"
typedef unsigned OMX_U32;
typedef int OMX_BOOL;
typedef int OMX_ERRORTYPE;
#define OMX_TRUE 1
#define OMX_FALSE 0
#define OMX_ErrorNone 0
#define OMX_ErrorBadParameter 1
#define OMX_ErrorInsufficientResources 2
#define SEC_OSAL_Malloc malloc
#define SEC_OSAL_Free free
#define SEC_OSAL_Memcpy memcpy
#define csc_tiled_to_linear_y_neon csc_tiled_to_linear_y
#define csc_tiled_to_linear_uv_neon csc_tiled_to_linear_uv
#define csc_tiled_to_linear_uv_deinterleave_neon csc_tiled_to_linear_uv_deinterleave
typedef struct {void *cscScratch; unsigned cscScratchSize;} SEC_H264DEC_HANDLE;
typedef struct {unsigned img_width,img_height; unsigned char *YVirAddr,*CVirAddr;} SSBSIP_MFC_DEC_OUTPUT_INFO;
'''
test = r'''
static unsigned char pattern(unsigned x,unsigned y,unsigned p) {return (x*13+y*7+p*71)%251;}
static void check(unsigned w,unsigned h,unsigned stride,int planar,int native) {
 unsigned cs=planar?(native?((stride/2+15)&~15U):stride/2):stride;
 size_t yn=w*h,cn=yn/4,dy=stride*(h+32),dc=cs*(h/2+32);
 unsigned char *linear=malloc(yn+2*cn),*ty=calloc(1,8*1024*1024),*tc=calloc(1,8*1024*1024);
 for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++)linear[y*w+x]=pattern(x,y,0);
 for(unsigned p=1;p<=2;p++)for(unsigned y=0;y<h/2;y++)for(unsigned x=0;x<w/2;x++)linear[yn+(p-1)*cn+y*w/2+x]=pattern(x,y,p);
 csc_linear_to_tiled_y(ty,linear,w,h);
 csc_linear_to_tiled_uv(tc,linear+yn,linear+yn+cn,w,h/2);
 void *dst[3]={malloc(dy),malloc(dc),malloc(dc)};
 memset(dst[0],0xee,dy);memset(dst[1],0xee,dc);memset(dst[2],0xee,dc);
 SEC_H264DEC_HANDLE dec={0}; SSBSIP_MFC_DEC_OUTPUT_INFO src={w,h,ty,tc};
 assert(SEC_H264_CopyTiledOutput(&dec,dst,&src,stride,planar,native)==0);
 for(unsigned p=0;p<(planar?3:2);p++) {
  unsigned pitch=p?cs:stride,rows=p?h/2:h,bytes=p?(planar?w/2:w):w;
  size_t alloc=p?dc:dy;
  for(size_t i=0;i<alloc;i++) {
   unsigned y=i/pitch,x=i%pitch; unsigned char expected=0xee;
   if(y<rows&&x<bytes)expected=pattern(p&&!planar?x/2:x,y,p&&!planar?1+x%2:p);
   if(((unsigned char*)dst[p])[i]!=expected) {fprintf(stderr,"FAIL %ux%u stride %u planar %d native %d plane %u at %zu\n",w,h,stride,planar,native,p,i);abort();}
  }
 }
 free(dec.cscScratch);for(unsigned p=0;p<3;p++)free(dst[p]);free(linear);free(ty);free(tc);
 printf("PASS %ux%u stride=%u planar=%d native=%d\n",w,h,stride,planar,native);
}
int main(void){
 unsigned cases[][3]={{960,544,1920},{704,400,1920},{720,480,720},{640,480,640},{960,544,960}};
 for(unsigned i=0;i<5;i++)for(int p=0;p<2;p++)check(cases[i][0],cases[i][1],cases[i][2],p,1);
 check(720,480,720,1,0); return 0;
}
'''
with tempfile.TemporaryDirectory() as t:
 f=Path(t)/'test.c';f.write_text(prefix+helper+test)
 exe=Path(t)/'test'
 subprocess.run(['cc','-std=gnu11','-O1','-g','-fsanitize=address,undefined','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',f'-I{csc / "include"}',str(f),str(csc/'color_space_convertor.c'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
