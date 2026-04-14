/* ps1SpuAdpcm - PS1 SPU ADPCM audio codec utilities
 * Copyright (C) 2026 tildearrow and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _PS1_SPU_ADPCM_H
#define _PS1_SPU_ADPCM_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * read len samples from buf, encode in PS1 SPU ADPCM and output to out.
 * @param buf input data.
 * @param out output buffer. shall be at least 16*((27+len)/28) bytes in size (16 more if loopStart is not -1).
 * @param len input length (should be a multiple of 28. if it isn't, the output will be padded).
 * @param loopStart beginning of loop area (may be -1 for no loop). the respective block gets the loop-start flag set.
 * @return number of written bytes.
 */
long ps1SpuAdpcmEncode(short* buf, unsigned char* out, long len, long loopStart);

/**
 * read len bytes from buf, decode PS1 SPU ADPCM and output to out.
 * @param buf input data.
 * @param out output buffer. shall be at least 28*(len/16) shorts in size.
 * @param len input length (shall be a multiple of 16).
 * @return number of written samples.
 */
long ps1SpuAdpcmDecode(unsigned char* buf, short* out, long len);

#ifdef __cplusplus
}
#endif

#endif
