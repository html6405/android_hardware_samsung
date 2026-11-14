# AVC adaptive playback regression

Run `python3 exynos/multimedia/openmax/tests/test_h264_adaptive_csc.py` from
`hardware/samsung`. Requires a host C compiler with AddressSanitizer and
UndefinedBehaviorSanitizer.

The test extracts the production output-copy helper and links Samsung's
scalar tiled CSC reference in place of NEON. It checks all visible pixels,
untouched row padding and trailing allocation guards for planar and
semiplanar output. Cases include 960x544 and 704x400 sources in buffers with
a 1920-pixel pitch, native planar chroma pitch alignment, compact output,
and packed non-native planar output. The Android module build and device
playback provide coverage of the actual NEON implementation.

## N8000 validation, 2026-11-14

Prime Video reproduced visibly scrambled colors with `OMX.SEC.AVC.Decoder`.
An ADB screenshot was entirely black, so visual validation was performed
on the physical display by the device owner.

The FIMC diagnostic reported a 960x544 source with a destination Cb offset
of 2088960 bytes and Cr offset of 2611200 bytes. These are the planar
1920x1088 allocation offsets, rather than the source-sized allocation
expected by the single-buffer FIMC API. The fallback then replaced the
source width with the ANB pitch and passed that width to the tiled NEON
reader. That changes source tile addressing and can read beyond the source
frame. Source geometry and destination pitch must remain independent.

With the corrected helper, the owner confirmed the same Prime playback
had normal colors. Hardware decoding remained active. FIMC is retained
for compatible layouts; larger adaptive buffers use NEON conversion into
a reused compact image followed by copies at the destination plane pitches.
The FIMC geometry precheck avoids repeated rejected conversions and error logs.

Validation: AVC module build passed, all 11 host layout cases passed under
ASan/UBSan, and `git diff --check` passed. Both blocking and nonblocking AVC
paths use the helper; only the configured device path was tested in playback.
The corrected decoder was installed to the N8000 vendor partition after
a temporary bind-mount test, preserving an original-library backup. On-disk
SHA-256: `34c889a1cb0a1a9f4dabc1ba5fdd4992ac7c391444c169eef3cc635eb7ff25c0`.
No reboot verification was performed during this playback session.

Raw logs, screenshot, original library, and installation verification are
kept locally under `bringup-notes/exynos4-avc-20261114` in the Android tree.
They are not part of this source repository.
