#!/usr/bin/env python3
"""Check the production layout against Mali's legacy planar handle ABI."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'backend/BufferManager.cpp').read_text()
layout = source[source.index('bool computeLayout('):source.index('size_t metadataOffset(')]
def extract_function(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    cursor = opening + 1
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    return source[start:cursor] + '\n'

helpers = extract_function('bool align16(') + extract_function('bool fitsLegacySize(')
prefix = r'''
#include <cstdint>
#include <cstddef>
#include <climits>
#include <limits>
#include <cassert>
enum class PixelFormat : int32_t {
 RGBA_8888=1, RGBX_8888=2, RGB_888=3, RGB_565=4, BGRA_8888=5,
 YCBCR_422_SP=16, YCRCB_420_SP=17, BLOB=33, YCBCR_420_888=35, YV12=0x32315659
};
constexpr int32_t kExynosI420=0x101, kExynosNv12=0x105;
struct BufferLayout {
 uint32_t stride=0, bytesPerPixel=0; size_t size=0;
 uint32_t chromaOffset=0, vOffset=0;
};
'''
tests = r'''
int main() {
 // Legacy gralloc aligns luma height and chroma height independently to 16.
 // Cover software-video resolutions and non-macroblock-aligned dimensions.
 for (uint32_t w : {18U, 352U, 704U, 960U, 1024U, 1920U}) {
  for (uint32_t h : {18U, 270U, 396U, 540U, 544U, 1080U}) {
   BufferLayout b;
   assert(computeLayout(w,h,static_cast<int32_t>(PixelFormat::YV12),&b));
   uint32_t pitch=(w+15)&~15U, cpitch=(pitch/2+15)&~15U;
   uint32_t ybytes=pitch*((h+15)&~15U);
   uint32_t cbytes=cpitch*((h/2+15)&~15U);
   assert(b.stride==pitch && b.chromaOffset==ybytes && b.vOffset==cbytes);
   assert(b.size==ybytes+2*cbytes);
   // A client writing only visible rows stays in its own padded plane.
   assert((h-1)*pitch+w <= b.chromaOffset);
   assert((h/2-1)*cpitch+w/2 <= b.vOffset);
   assert(b.chromaOffset+b.vOffset+(h/2-1)*cpitch+w/2 <= b.size);
  }
 }
 BufferLayout b;
 assert(computeLayout(1920,1080,kExynosI420,&b));
 assert(b.chromaOffset==1920U*1088 && b.vOffset==960U*544);
 assert(computeLayout(1280,800,1,&b) && b.size==1280U*800*4);
 assert(!computeLayout(1023,540,static_cast<int32_t>(PixelFormat::YV12),&b));
 assert(!computeLayout(UINT32_MAX,540,static_cast<int32_t>(PixelFormat::YV12),&b));
 assert(!computeLayout(0,540,static_cast<int32_t>(PixelFormat::YV12),&b));
}
'''
with tempfile.TemporaryDirectory(prefix='exynos4-layout-') as directory:
    cpp = Path(directory) / 'layout.cpp'
    binary = Path(directory) / 'layout'
    cpp.write_text('#include <initializer_list>\n' + prefix + helpers + layout + tests)
    subprocess.run(['c++', '-std=c++17', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: 36 YV12 layouts, plane bounds, I420/RGBA and invalid dimensions')
