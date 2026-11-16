#!/usr/bin/env python3
"""Verify production ANB physical plane addresses against the Mali handle ABI."""
from pathlib import Path
import subprocess
import tempfile
source=(Path(__file__).resolve().parents[1]/'osal/SEC_OSAL_Android.cpp').read_text()
function=source[source.index('OMX_ERRORTYPE SEC_OSAL_GetPhysANBHandle('):source.index('OMX_ERRORTYPE SEC_OSAL_GetANBInfoHandle(')]
prefix=r'''
#include <cstdint>
#include <cstddef>
#include <cassert>
#define OMX_IN
#define OMX_OUT
#define FunctionIn() ((void)0)
#define FunctionOut() ((void)0)
#define SEC_OSAL_Log(...) ((void)0)
using OMX_U32=uintptr_t;
using OMX_PTR=void *;
using OMX_ERRORTYPE=int;
constexpr int OMX_ErrorNone=0, OMX_ErrorBadParameter=1;
constexpr int HAL_PIXEL_FORMAT_YV12=0x32315659;
struct private_handle_t;
using buffer_handle_t=const private_handle_t *;
struct private_handle_t {
 uint32_t paddr,offset,uoffset,voffset; int format;
 static const private_handle_t *dynamicCast(buffer_handle_t h) {return h;}
};
'''
# The production ABI uses a 32-bit pointer; uintptr_t preserves it in this host harness.
function=function.replace('private_handle_t *hnd =','const private_handle_t *hnd =')
tests=r'''
int main() {
 private_handle_t h={0x100000,4096,1024*544,512*272,HAL_PIXEL_FORMAT_YV12};
 void *planes[3]={};
 assert(SEC_OSAL_GetPhysANBHandle(reinterpret_cast<uintptr_t>(&h),planes)==0);
 uintptr_t base=h.paddr+h.offset;
 assert(reinterpret_cast<uintptr_t>(planes[0])==base);
 assert(reinterpret_cast<uintptr_t>(planes[1])==base+h.uoffset+h.voffset);
 assert(reinterpret_cast<uintptr_t>(planes[2])==base+h.uoffset);
 h.format=0x101;
 assert(SEC_OSAL_GetPhysANBHandle(reinterpret_cast<uintptr_t>(&h),planes)==0);
 assert(reinterpret_cast<uintptr_t>(planes[1])==base+h.uoffset);
 assert(reinterpret_cast<uintptr_t>(planes[2])==base+h.uoffset+h.voffset);
 h.paddr=0;
 assert(SEC_OSAL_GetPhysANBHandle(reinterpret_cast<uintptr_t>(&h),planes)==OMX_ErrorBadParameter);
 assert(SEC_OSAL_GetPhysANBHandle(0,planes)==OMX_ErrorBadParameter);
 assert(SEC_OSAL_GetPhysANBHandle(reinterpret_cast<uintptr_t>(&h),nullptr)==OMX_ErrorBadParameter);
}
'''
with tempfile.TemporaryDirectory(prefix='exynos-anb-') as d:
 f=Path(d)/'planes.cpp'; exe=Path(d)/'planes'
 f.write_text(prefix+function+tests)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(f),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: YV12/I420 physical planes, allocation offset and invalid handles')
