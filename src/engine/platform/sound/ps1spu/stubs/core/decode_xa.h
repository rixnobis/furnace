// Stands in for pcsx-redux core/decode_xa.h: xa_decode_t layout only (copied from Redux), no decoder.
#pragma once
#include <stdint.h>
struct ADPCM_Decode_t {
    int32_t y0, y1;
};
struct xa_decode_t {
    int freq;
    int nbits;
    int stereo;
    int nsamples;
    ADPCM_Decode_t left, right;
    short pcm[16384];
};
