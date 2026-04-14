/**
 * Furnace Tracker - multi-system chiptune tracker
 * Copyright (C) 2021-2026 tildearrow and contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "spudump.h"
#include "../engine.h"
#include "../../ta-log.h"
#include <thread>
#include <map>

// SPU dump packet types
#define SPUD_PKT_REG_WRITE   0x00
#define SPUD_PKT_WAIT        0x01
#define SPUD_PKT_END_PATTERN 0x02
#define SPUD_PKT_LOOP_POINT  0x03
#define SPUD_PKT_TRACE_BEGIN 0x04
#define SPUD_PKT_ORDER_TABLE 0x10
#define SPUD_PKT_PATTERN_HDR 0x11
#define SPUD_PKT_SUBSONG_TBL 0x12
#define SPUD_PKT_MACRO_DEF   0x13
#define SPUD_PKT_SAMPLE_DIR  0x20
#define SPUD_PKT_SAMPLE_DATA 0x21
#define SPUD_PKT_TICK_RATE   0x30
#define SPUD_PKT_TITLE       0x40
#define SPUD_PKT_AUTHOR      0x41
#define SPUD_PKT_GAME_ID     0x42
#define SPUD_PKT_COMMENT     0x43
#define SPUD_PKT_SUBSONG_NAM 0x44
#define SPUD_PKT_VOICE_COUNT 0x45

// write packet header: length in words (3 bytes LE) + type (1 byte)
static void writePacketHeader(SafeWriter* w, unsigned char type, unsigned int lengthWords) {
  w->writeC(lengthWords&0xff);
  w->writeC((lengthWords>>8)&0xff);
  w->writeC((lengthWords>>16)&0xff);
  w->writeC(type);
}

// write a zero-padded string packet (title, author, comment, etc.)
static void writeStringPacket(SafeWriter* w, unsigned char type, const char* str) {
  size_t len=strlen(str);
  // pad to 4-byte alignment
  size_t padded=(len+4)&(~3);
  unsigned int lengthWords=padded/4;
  writePacketHeader(w,type,lengthWords);
  w->write(str,len);
  // zero-pad remainder
  for (size_t i=len; i<padded; i++) {
    w->writeC(0);
  }
}

// write a single-word packet
static void writeWordPacket(SafeWriter* w, unsigned char type, unsigned int val) {
  writePacketHeader(w,type,1);
  w->writeI(val);
}

// write a wait packet
static void writeWaitPacket(SafeWriter* w, unsigned int ticks) {
  writePacketHeader(w,SPUD_PKT_WAIT,1);
  w->writeI(ticks);
}

// write a register write packet from a batch of writes
// each write is encoded as (addr16 << 16) | val16
static void writeRegWritePacket(SafeWriter* w, std::vector<DivRegWrite>& writes, const std::map<int,int>* insMacroMap=NULL) {
  if (writes.empty()) return;
  // pre-build output words, remapping macro invocations
  std::vector<unsigned int> words;
  words.reserve(writes.size());
  for (DivRegWrite& wr: writes) {
    unsigned int addr=wr.addr&0xffff;
    unsigned int val=wr.val&0xffff;
    if (addr>=0xF000 && insMacroMap!=NULL) {
      int insIdx=addr&0x0FFF;
      auto it=insMacroMap->find(insIdx);
      if (it!=insMacroMap->end()) {
        addr=0xF000|it->second;
      } else {
        continue; // no macro for this instrument, skip
      }
    }
    words.push_back((addr<<16)|val);
  }
  if (words.empty()) return;
  writePacketHeader(w,SPUD_PKT_REG_WRITE,(unsigned int)words.size());
  for (unsigned int word: words) {
    w->writeI(word);
  }
}

// write an empty packet (end of pattern, loop point, trace begin)
static void writeEmptyPacket(SafeWriter* w, unsigned char type) {
  writePacketHeader(w,type,0);
}

bool DivExportSPUDump::go(DivEngine* eng) {
  progress[0].name="Export";
  progress[0].amount=0.0f;
  e=eng;
  running=true;
  failed=false;
  mustAbort=false;
  exportThread=new std::thread(&DivExportSPUDump::run,this);
  return true;
}

void DivExportSPUDump::run() {
  // read config
  bool loop=conf.getBool("loop",true);
  int voiceCount=conf.getInt("voiceCount",24);

  // find PS1 SPU dispatch
  int SPU=-1;
  for (int i=0; i<e->song.systemLen; i++) {
    if (e->song.system[i]==DIV_SYSTEM_PS1_SPU) {
      SPU=i;
      break;
    }
  }
  if (SPU<0) {
    logAppend("ERROR: no PS1 SPU system found!");
    failed=true;
    running=false;
    return;
  }

  SafeWriter* w=new SafeWriter;
  w->init();

  // Phase 1: magic header (16 bytes)
  w->write("PSXS",4);
  w->write("PUDU",4);
  w->write("MPv1",4);
  w->write("r1\0\0",4);

  // Phase 2: metadata packets
  if (!e->song.name.empty()) {
    writeStringPacket(w,SPUD_PKT_TITLE,e->song.name.c_str());
  }
  if (!e->song.author.empty()) {
    writeStringPacket(w,SPUD_PKT_AUTHOR,e->song.author.c_str());
  }
  writeStringPacket(w,SPUD_PKT_COMMENT,"Exported by Furnace");

  // voice count
  writeWordPacket(w,SPUD_PKT_VOICE_COUNT,(unsigned int)voiceCount);

  // Phase 3: tick rate
  // derive from engine's effective Hz
  double tickHz=e->curSubSong->hz;
  unsigned int tickRate=(unsigned int)(tickHz*65536.0);
  writeWordPacket(w,SPUD_PKT_TICK_RATE,tickRate);

  // Phase 4: sample data
  // collect all samples and write them contiguously
  {
    // first, calculate total sample data size
    size_t totalSampleSize=0;
    for (int i=0; i<e->song.sampleLen; i++) {
      DivSample* s=e->song.sample[i];
      if (s->dataPS1SPU!=NULL && s->lengthPS1SPU>0) {
        totalSampleSize+=s->lengthPS1SPU;
      }
    }

    if (totalSampleSize>0) {
      // SPU RAM base address for samples (in 8-byte units)
      // start at 0x1010 (after capture buffers, 0x1000 = 4096 bytes = 512 8-byte units)
      unsigned int baseAddr8=0x202; // 0x1010 / 8

      // sample data packet: base address word + raw ADPCM data
      unsigned int dataWords=1+(unsigned int)((totalSampleSize+3)/4);
      writePacketHeader(w,SPUD_PKT_SAMPLE_DATA,dataWords);
      w->writeI(baseAddr8);

      // track per-sample offsets for directory
      std::vector<unsigned int> sampleAddr8;
      std::vector<unsigned int> sampleLen8;
      std::vector<unsigned int> sampleLoopAddr8;
      std::vector<bool> sampleHasLoop;

      unsigned int curAddr8=baseAddr8;
      for (int i=0; i<e->song.sampleLen; i++) {
        DivSample* s=e->song.sample[i];
        if (s->dataPS1SPU!=NULL && s->lengthPS1SPU>0) {
          sampleAddr8.push_back(curAddr8);
          sampleLen8.push_back((unsigned int)(s->lengthPS1SPU/8));

          if (s->isLoopable()) {
            unsigned int loopBlock=(s->loopStart/28)*16;
            sampleLoopAddr8.push_back(curAddr8+(loopBlock/8));
            sampleHasLoop.push_back(true);
          } else {
            sampleLoopAddr8.push_back(0);
            sampleHasLoop.push_back(false);
          }

          w->write(s->dataPS1SPU,s->lengthPS1SPU);
          curAddr8+=(unsigned int)(s->lengthPS1SPU/8);
        } else {
          sampleAddr8.push_back(0);
          sampleLen8.push_back(0);
          sampleLoopAddr8.push_back(0);
          sampleHasLoop.push_back(false);
        }
      }

      // pad to 4-byte alignment
      size_t written=4+totalSampleSize; // base addr word + data
      while (written%4!=0) {
        w->writeC(0);
        written++;
      }

      // sample directory packet
      int sampleCount=(int)sampleAddr8.size();
      writePacketHeader(w,SPUD_PKT_SAMPLE_DIR,1+(unsigned int)(sampleCount*2));
      w->writeI((unsigned int)sampleCount);
      for (int i=0; i<sampleCount; i++) {
        // word 0: addr8 (upper 16) | len8 (lower 16)
        w->writeI((sampleAddr8[i]<<16)|(sampleLen8[i]&0xffff));
        // word 1: loopAddr8 (upper 16) | flags (lower 16)
        unsigned int flags=sampleHasLoop[i]?1:0;
        w->writeI((sampleLoopAddr8[i]<<16)|(flags&0xffff));
      }
    }
  }

  // Phase 5: macro definitions
  // Build a macro for each instrument that maps to a set of voice-relative register writes.
  // Voice-relative means offsets are from voice 0 (0x00); the player adds voice*0x10 at invocation.
  struct MacroDef {
    std::vector<unsigned int> writes; // each word: (offset16 << 16) | value16
  };
  std::vector<MacroDef> macroDefs;
  // map instrument index -> macro index
  std::map<int,int> insMacroMap;

  for (int i=0; i<e->song.insLen; i++) {
    DivInstrument* ins=e->song.ins[i];
    if (ins->type!=DIV_INS_PS1) continue;

    MacroDef md;
    // ADSR registers (voice-relative offsets 0x08, 0x0A)
    unsigned short adsrLo=0;
    unsigned short adsrHi=0;
    int attackShift=(ins->ps1.a>=15)?0:(15-ins->ps1.a);
    adsrLo|=(attackShift&0x1f);
    adsrLo|=((ins->ps1.d&0xf)<<6);
    adsrHi|=((ins->ps1.s&0x7)<<1);
    adsrHi|=((ins->ps1.r&0x1f)<<4);
    md.writes.push_back((0x0008<<16)|(adsrLo&0xffff));
    md.writes.push_back((0x000A<<16)|(adsrHi&0xffff));

    // sample start address (voice-relative offset 0x06)
    int sampleIdx=ins->amiga.initSample;
    if (sampleIdx>=0 && sampleIdx<e->song.sampleLen) {
      DivSample* s=e->song.sample[sampleIdx];
      if (s->dataPS1SPU!=NULL && s->lengthPS1SPU>0) {
        // find this sample's SPU RAM address from the sample directory we built earlier
        // sampleOff is not available here, but we can compute from the directory
        // use the same base address calculation as Phase 4
        unsigned int baseAddr8=0x202;
        unsigned int addr8=baseAddr8;
        for (int j=0; j<sampleIdx; j++) {
          DivSample* prev=e->song.sample[j];
          if (prev->dataPS1SPU!=NULL && prev->lengthPS1SPU>0) {
            addr8+=(unsigned int)(prev->lengthPS1SPU/8);
          }
        }
        md.writes.push_back((0x0006<<16)|(addr8&0xffff));
      }
    }

    if (!md.writes.empty()) {
      int macroIdx=(int)macroDefs.size();
      insMacroMap[i]=macroIdx;
      macroDefs.push_back(md);
    }
  }

  // emit macro definition packets
  for (int i=0; i<(int)macroDefs.size(); i++) {
    MacroDef& md=macroDefs[i];
    writePacketHeader(w,SPUD_PKT_MACRO_DEF,1+(unsigned int)md.writes.size());
    w->writeI((unsigned int)i); // macro index
    for (unsigned int wr: md.writes) {
      w->writeI(wr);
    }
  }

  // Phase 6: order table
  {
    DivSubSong* sub=e->song.subsong[0];
    int orderLen=sub->ordersLen;
    bool songLoop=loop;

    // find loop point via timestamps
    e->calcSongTimestamps();
    int loopOrder=e->curSubSong->ts.loopStart.order;

    // order table: flags + loop target + pattern indices
    writePacketHeader(w,SPUD_PKT_ORDER_TABLE,2+(unsigned int)orderLen);
    w->writeI(songLoop?1:0); // flags: bit 0 = loop
    w->writeI(songLoop?(unsigned int)loopOrder:0); // loop target order
    for (int i=0; i<orderLen; i++) {
      w->writeI((unsigned int)i); // pattern index = order index for now (1:1 mapping)
    }

    // pattern headers (placeholders - will backfill offsets)
    std::vector<size_t> patternHeaderOffsets;
    for (int i=0; i<orderLen; i++) {
      patternHeaderOffsets.push_back(w->tell());
      writePacketHeader(w,SPUD_PKT_PATTERN_HDR,1);
      w->writeI(0); // placeholder offset
    }

    // Phase 6: pattern data
    e->stop();
    e->repeatPattern=false;
    e->setOrder(0);
    e->synchronizedSoft([&]() {
      double origRate=e->got.rate;

      // reset playback state
      e->curOrder=0;
      e->freelance=false;
      e->playing=false;
      e->extValuePresent=false;
      e->remainingLoops=-1;

      e->playSub(false);

      e->disCont[SPU].dispatch->toggleRegisterDump(true);

      bool done=false;
      int lastOrder=-1;
      int patternIdx=0;
      unsigned int pendingWait=0;

      while (!done && !mustAbort) {
        int curOrder=e->curOrder;

        // detect pattern boundary
        if (curOrder!=lastOrder) {
          // flush pending wait
          if (pendingWait>0 && lastOrder>=0) {
            writeWaitPacket(w,pendingWait);
            pendingWait=0;
          }

          // end previous pattern
          if (lastOrder>=0) {
            writeEmptyPacket(w,SPUD_PKT_END_PATTERN);
          }

          // backfill pattern header with current file offset
          if (patternIdx<(int)patternHeaderOffsets.size()) {
            size_t curPos=w->tell();
            w->seek(patternHeaderOffsets[patternIdx]+4,SEEK_SET); // skip packet header
            w->writeI((unsigned int)curPos);
            w->seek(curPos,SEEK_SET);
          }

          lastOrder=curOrder;
          patternIdx++;

          // update progress
          if (e->song.subsong[0]->ordersLen>0) {
            progress[0].amount=(float)curOrder/(float)e->song.subsong[0]->ordersLen;
          }
        }

        // advance one tick
        if (e->nextTick() || !e->playing) {
          done=true;
          if (!loop) {
            e->disCont[SPU].dispatch->getRegisterWrites().clear();
            break;
          }
        }

        // collect register writes
        std::vector<DivRegWrite>& writes=e->disCont[SPU].dispatch->getRegisterWrites();
        if (!writes.empty()) {
          // flush any pending wait before writing registers
          if (pendingWait>0) {
            writeWaitPacket(w,pendingWait);
            pendingWait=0;
          }
          writeRegWritePacket(w,writes,&insMacroMap);
        }
        writes.clear();

        // accumulate wait
        int totalWait=e->cycles;
        if (totalWait>0 && !done) {
          pendingWait+=totalWait;
        }
      }

      // flush final wait and end last pattern
      if (pendingWait>0) {
        writeWaitPacket(w,pendingWait);
      }
      if (lastOrder>=0) {
        writeEmptyPacket(w,SPUD_PKT_END_PATTERN);
      }

      e->disCont[SPU].dispatch->toggleRegisterDump(false);
      e->got.rate=origRate;
      e->remainingLoops=-1;
      e->playing=false;
      e->freelance=false;
      e->extValuePresent=false;
    });
  }

  progress[0].amount=1.0f;
  logAppend("finished!");

  output.push_back(DivROMExportOutput("out.spudump",w));
  running=false;
}

bool DivExportSPUDump::isRunning() {
  return running;
}

bool DivExportSPUDump::hasFailed() {
  return failed;
}

void DivExportSPUDump::abort() {
  mustAbort=true;
  wait();
}

void DivExportSPUDump::wait() {
  if (exportThread!=NULL) {
    exportThread->join();
    delete exportThread;
    exportThread=NULL;
  }
}

DivROMExportProgress DivExportSPUDump::getProgress(int index) {
  if (index<0 || index>1) return progress[0];
  return progress[index];
}
