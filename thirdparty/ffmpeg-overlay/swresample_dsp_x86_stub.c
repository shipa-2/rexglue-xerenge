// This build does not vendor libswresample's x86 SIMD assembly (matching
// libavcodec above, which only takes the plain-C x86 dispatch/init files, not
// the .asm-implemented kernels): assembling and linking those would need a
// yasm/nasm toolchain this project does not otherwise require. resample_dsp.c
// calls swri_resample_dsp_x86_init() unconditionally on x86 regardless of
// whether SIMD is compiled in, so it needs a definition; leaving it empty
// keeps the DSP function pointers at the portable C implementations resample.c
// already set, i.e. correct output without vector acceleration.
#include "libswresample/resample.h"

void swri_resample_dsp_x86_init(ResampleContext *c) {
  (void)c;
}
