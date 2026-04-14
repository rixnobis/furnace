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

#include "ps1SpuAdpcm.h"
#include <string.h>

// PS1 SPU ADPCM uses 5 filter coefficient pairs.
// these are the fixed-point values divided by 64 in the decode formula.
static const int ps1SpuFilterCoeffs[5][2]={
  {  0,   0},
  { 60,   0},
  {115, -52},
  { 98, -55},
  {122, -60}
};

// PS1 SPU ADPCM block layout:
//   byte 0: shift (bits 0-3) | filter (bits 4-7)
//   byte 1: flags  (bit 0=loop end, bit 1=loop repeat, bit 2=loop start)
//   bytes 2-15: 28 nibbles packed 2 per byte (low nibble first)
//
// each block produces 28 samples.

#define PS1_SPU_BLOCK_SIZE 16
#define PS1_SPU_SAMPLES_PER_BLOCK 28

// decode one nibble using the IIR filter.
// nibble is sign-extended 4-bit, shift is 0-12, filter is 0-4.
// s1/s2 are previous samples (updated in place).
static short ps1SpuDecodeNibble(int nibble, int shift, int filter, int* s1, int* s2) {
  int sample;

  // sign-extend 4-bit nibble to 16-bit, then apply shift
  sample=(nibble<<12)&0xFFFF;
  if (sample&0x8000) sample|=0xFFFF0000;
  if (shift<=12) {
    sample>>=shift;
  } else {
    // shift 13-15: result is 0 or -1 depending on sign
    sample=(sample<0)?-1:0;
  }

  // apply IIR filter
  sample+=(*s1*ps1SpuFilterCoeffs[filter][0]+*s2*ps1SpuFilterCoeffs[filter][1]+32)>>6;

  // clamp to 16-bit signed
  if (sample>32767) sample=32767;
  if (sample<-32768) sample=-32768;

  *s2=*s1;
  *s1=sample;

  return (short)sample;
}

long ps1SpuAdpcmDecode(unsigned char* buf, short* out, long len) {
  long outPos=0;
  long pos;

  if (len<PS1_SPU_BLOCK_SIZE) return 0;

  for (pos=0; pos<=len-PS1_SPU_BLOCK_SIZE; pos+=PS1_SPU_BLOCK_SIZE) {
    int shift=buf[pos]&0x0F;
    int filter=(buf[pos]>>4)&0x07;
    // byte 1 is flags (loop end/repeat/start) - not needed for decode
    int s1=0, s2=0;
    int i;

    if (filter>4) filter=4;

    // carry filter state from previous block if we have output
    if (outPos>=2) {
      s1=out[outPos-1];
      s2=out[outPos-2];
    } else if (outPos==1) {
      s1=out[outPos-1];
      s2=0;
    }

    // decode 28 nibbles from 14 data bytes
    for (i=0; i<14; i++) {
      unsigned char dataByte=buf[pos+2+i];

      // low nibble first
      int nibble=dataByte&0x0F;
      if (nibble>=8) nibble-=16;
      out[outPos++]=ps1SpuDecodeNibble(nibble,shift,filter,&s1,&s2);

      // high nibble second
      nibble=(dataByte>>4)&0x0F;
      if (nibble>=8) nibble-=16;
      out[outPos++]=ps1SpuDecodeNibble(nibble,shift,filter,&s1,&s2);
    }
  }

  return outPos;
}

// try encoding one block with a given shift and filter.
// returns the total squared error.
static long long ps1SpuEncodeBlockTrial(const short* buf, int len, unsigned char shift, unsigned char filter, int s1In, int s2In) {
  long long errorSum=0;
  int s1=s1In;
  int s2=s2In;
  int i;

  for (i=0; i<len; i++) {
    int sample=buf[i];
    int predicted;
    int residual;
    int nibble;
    int decoded;
    int error;

    // compute predicted value from filter
    predicted=(s1*ps1SpuFilterCoeffs[filter][0]+s2*ps1SpuFilterCoeffs[filter][1]+32)>>6;

    // residual to encode
    residual=sample-predicted;

    // quantize: shift residual to 4-bit nibble
    if (shift<=12) {
      nibble=(residual+(1<<(shift-1)))>>shift;
    } else {
      nibble=0;
    }

    // clamp to 4-bit signed range
    if (nibble>7) nibble=7;
    if (nibble<-8) nibble=-8;

    // decode to compute actual reconstruction (matches hardware decode)
    decoded=(nibble<<12);
    if (decoded&0x8000) decoded|=0xFFFF0000;
    if (shift<=12) {
      decoded>>=shift;
    } else {
      decoded=(decoded<0)?-1:0;
    }
    decoded+=(s1*ps1SpuFilterCoeffs[filter][0]+s2*ps1SpuFilterCoeffs[filter][1]+32)>>6;

    if (decoded>32767) decoded=32767;
    if (decoded<-32768) decoded=-32768;

    s2=s1;
    s1=decoded;

    error=sample-decoded;
    errorSum+=(long long)error*error;
  }

  return errorSum;
}

// encode one block with a given shift and filter, writing output nibbles.
static void ps1SpuEncodeBlock(const short* buf, unsigned char* out, int len, unsigned char shift, unsigned char filter, int* s1, int* s2) {
  int i;

  for (i=0; i<len; i++) {
    int sample=buf[i];
    int predicted;
    int residual;
    int nibble;
    int decoded;

    predicted=(*s1*ps1SpuFilterCoeffs[filter][0]+*s2*ps1SpuFilterCoeffs[filter][1]+32)>>6;
    residual=sample-predicted;

    if (shift<=12) {
      nibble=(residual+(1<<(shift-1)))>>shift;
    } else {
      nibble=0;
    }

    if (nibble>7) nibble=7;
    if (nibble<-8) nibble=-8;

    // pack nibble into output byte (low nibble first within each byte)
    if (i&1) {
      out[i>>1]|=(nibble&0x0F)<<4;
    } else {
      out[i>>1]=(nibble&0x0F);
    }

    // decode to update filter state
    decoded=(nibble<<12);
    if (decoded&0x8000) decoded|=0xFFFF0000;
    if (shift<=12) {
      decoded>>=shift;
    } else {
      decoded=(decoded<0)?-1:0;
    }
    decoded+=(*s1*ps1SpuFilterCoeffs[filter][0]+*s2*ps1SpuFilterCoeffs[filter][1]+32)>>6;

    if (decoded>32767) decoded=32767;
    if (decoded<-32768) decoded=-32768;

    *s2=*s1;
    *s1=decoded;
  }

  // pad remaining nibbles with zero if len < 28
  for (i=len; i<PS1_SPU_SAMPLES_PER_BLOCK; i++) {
    if (i&1) {
      out[i>>1]|=0;
    } else {
      out[i>>1]=0;
    }
  }
}

long ps1SpuAdpcmEncode(short* buf, unsigned char* out, long len, long loopStart) {
  long outPos=0;
  long inPos=0;
  int s1=0, s2=0;
  int isFirstBlock=1;

  if (len==0) return 0;

  while (inPos<len) {
    int blockLen=PS1_SPU_SAMPLES_PER_BLOCK;
    int bestFilter=0;
    int bestShift=0;
    long long bestError=0x7FFFFFFFFFFFFFFFLL;
    int filter, shift;
    unsigned char flags=0;
    int isLoopStartBlock=0;
    int isLastBlock=0;

    if (inPos+blockLen>len) blockLen=(int)(len-inPos);

    // determine flags
    if (loopStart>=0 && loopStart>=inPos && loopStart<inPos+blockLen) {
      isLoopStartBlock=1;
    }
    if (inPos+blockLen>=len) {
      isLastBlock=1;
    }

    // try all filter/shift combinations to find lowest error
    for (filter=0; filter<5; filter++) {
      // first block should use filter 0 for clean start
      if (isFirstBlock && filter!=0) continue;
      // loop start block should use filter 0 for clean loop
      if (isLoopStartBlock && filter!=0) continue;

      for (shift=0; shift<=12; shift++) {
        long long error=ps1SpuEncodeBlockTrial(buf+inPos,blockLen,shift,filter,s1,s2);
        if (error<bestError) {
          bestError=error;
          bestFilter=filter;
          bestShift=shift;
        }
      }
    }

    // write block header
    out[outPos]=(unsigned char)(bestShift|(bestFilter<<4));

    // build flags byte
    flags=0;
    if (isLoopStartBlock) flags|=0x04; // loop start
    if (isLastBlock) {
      flags|=0x01; // loop end
      if (loopStart>=0) {
        flags|=0x02; // loop repeat (keep playing from loop point)
      }
      // if no loop, flags=0x01 means end+mute
    }
    out[outPos+1]=flags;

    // encode the block data
    memset(out+outPos+2,0,14);
    ps1SpuEncodeBlock(buf+inPos,out+outPos+2,blockLen,bestShift,bestFilter,&s1,&s2);

    outPos+=PS1_SPU_BLOCK_SIZE;
    inPos+=blockLen;
    isFirstBlock=0;
  }

  return outPos;
}
