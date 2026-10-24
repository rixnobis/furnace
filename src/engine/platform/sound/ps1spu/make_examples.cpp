// Ours. Builds the PS1 example songs (papers/ps1-examples/) from the intermediate written by
// papers/ps1-examples/tools/score2ps1.py, through DivEngine's own API (same approach as
// make_corpus.cpp): one DIV_SYSTEM_PS1 chip, one engine tick per score tick, speed = score
// ticks per row (key-offs between rows as FCxx delayed note release), samples as 16-bit PCM,
// PS1 instruments with the bank's ADSR words.
// usage: ps1_examples <intermediate dir> <out.fur>
//   <dir>/song.txt: song/sample/ins/cell lines (format in score2ps1.py), <dir>/sNN.raw: PCM
// regenerating both examples (from papers/ps1-examples/):
//   python3 tools/score2ps1.py src/keynote.mid src/keynote.vab /tmp/k "One More Thing (keynote)"
//   python3 tools/score2ps1.py src/forty-one-cycles.mid src/forty-one-cycles.vab /tmp/f "Forty-One Cycles" --names kitb
//   ps1_examples /tmp/k keynote.fur && ps1_examples /tmp/f forty-one-cycles.fur
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>
#include "../../../engine.h"

// normally defined in main.cpp, which this tool replaces.
void reportError(String what) {
  logE("%s",what);
}

static String restOf(const char* line, int skipWords) {
  const char* p=line;
  for (int i=0; i<skipWords; i++) {
    while (*p && *p!=' ') p++;
    while (*p==' ') p++;
  }
  String s=p;
  while (!s.empty() && (s.back()=='\n' || s.back()=='\r')) s.pop_back();
  return s;
}

static bool loadPCM(const String& path, DivSample* s) {
  FILE* f=fopen(path.c_str(),"rb");
  if (f==NULL) return false;
  fseek(f,0,SEEK_END);
  long len=ftell(f)/2;
  fseek(f,0,SEEK_SET);
  s->depth=DIV_SAMPLE_DEPTH_16BIT;
  s->init(len);
  size_t got=fread(s->data16,2,len,f);
  fclose(f);
  return got==(size_t)len;
}

int main(int argc, char** argv) {
  if (argc<3) {
    fprintf(stderr,"usage: %s <intermediate dir> <out.fur>\n",argv[0]);
    return 1;
  }
  String dir=argv[1];
  FILE* in=fopen((dir+"/song.txt").c_str(),"r");
  if (in==NULL) {
    fprintf(stderr,"cannot open %s/song.txt\n",dir.c_str());
    return 1;
  }
  DivEngine e;
  e.preInit(true);
  e.setAudio(DIV_AUDIO_DUMMY);
  if (!e.init()) {
    fprintf(stderr,"engine init failed\n");
    return 1;
  }
  e.createNew("id0=225\n","Sony PlayStation",false);
  DivSubSong* sub=e.song.subsong[0];
  int patLen=0, rows=0, cells=0, speed=1, lastSpeed=1, perBeat=4;
  char line[1024];
  while (fgets(line,sizeof(line),in)) {
    char kind[16];
    if (sscanf(line,"%15s",kind)!=1) continue;
    if (strcmp(kind,"song")==0) {
      double hz;
      if (sscanf(line,"song %lf %d %d %d %d %d",&hz,&patLen,&rows,&speed,&lastSpeed,&perBeat)!=6 || patLen<1 || patLen>DIV_MAX_ROWS ||
          speed<1 || speed>255 || lastSpeed<1 || lastSpeed>speed || perBeat<1 || perBeat>255) {
        fprintf(stderr,"bad song line\n"); return 1;
      }
      int pats=(rows+patLen-1)/patLen;
      if (pats>DIV_MAX_PATTERNS) {
        fprintf(stderr,"%d patterns do not fit\n",pats); return 1;
      }
      e.song.name=restOf(line,7);
      e.song.author="rixnobis";
      e.song.systemName="Sony PlayStation";
      sub->hz=(float)hz;
      sub->speeds.val[0]=speed;
      sub->speeds.len=1;
      // highlights at the beat and the bar: the tracker shows 60*hz/(hilightA*speed) = the score's BPM.
      sub->hilightA=perBeat;
      sub->hilightB=perBeat*4;
      sub->patLen=patLen;
      sub->ordersLen=pats;
      for (int ch=0; ch<e.getTotalChannelCount(); ch++) {
        sub->pat[ch].effectCols=3;
        for (int o=0; o<pats; o++) sub->orders.ord[ch][o]=o;
      }
      // the last row jumps back to the start: the song loops where the PS1 player loops. when
      // the loop point is not on the grid the last row is shorter (0Fxx), and row 0 restores the
      // speed. these go on the last voice, which the songs never use (the converter allocates
      // voices from 0 up).
      int last=rows-1, fxCh=e.getTotalChannelCount()-1;
      DivPattern* p=sub->pat[fxCh].getPattern(last/patLen,true);
      p->newData[last%patLen][DIV_PAT_FX(0)]=0x0d;
      p->newData[last%patLen][DIV_PAT_FXVAL(0)]=0x00;
      if (lastSpeed!=speed) {
        p->newData[last%patLen][DIV_PAT_FX(1)]=0x0f;
        p->newData[last%patLen][DIV_PAT_FXVAL(1)]=lastSpeed;
        p=sub->pat[fxCh].getPattern(0,true);
        p->newData[0][DIV_PAT_FX(1)]=0x0f;
        p->newData[0][DIV_PAT_FXVAL(1)]=speed;
      }
    } else if (strcmp(kind,"sample")==0) {
      int id, rate, ls, le;
      if (sscanf(line,"sample %d %d %d %d",&id,&rate,&ls,&le)!=4 || id!=e.song.sampleLen) {
        fprintf(stderr,"bad sample line: %s",line); return 1;
      }
      DivSample* s=new DivSample;
      s->name=restOf(line,5);
      s->centerRate=rate;
      char fn[32];
      snprintf(fn,sizeof(fn),"/s%02d.raw",id);
      if (!loadPCM(dir+fn,s) || s->samples!=(unsigned int)le) {
        fprintf(stderr,"bad PCM for sample %d\n",id); return 1;
      }
      if (ls>=0) {
        s->loop=true;
        s->loopStart=ls;
        s->loopEnd=le;
        s->loopMode=DIV_SAMPLE_LOOP_FORWARD;
      }
      e.addSamplePtr(s);
    } else if (strcmp(kind,"ins")==0) {
      int id, smp, rev;
      unsigned int a1, a2;
      if (sscanf(line,"ins %d %d %x %x %d",&id,&smp,&a1,&a2,&rev)!=5 || id!=e.song.insLen) {
        fprintf(stderr,"bad ins line: %s",line); return 1;
      }
      DivInstrument* ins=new DivInstrument;
      ins->type=DIV_INS_PS1;
      ins->name=restOf(line,6);
      ins->amiga.initSample=smp;
      ins->ps1.setADSR1((unsigned short)a1);
      ins->ps1.setADSR2((unsigned short)a2);
      ins->ps1.reverb=rev;
      e.addInstrumentPtr(ins);
    } else if (strcmp(kind,"cell")==0) {
      int ch, row, ins, vol, pl, pr, delay;
      char note[8];
      if (sscanf(line,"cell %d %d %7s %d %d %d %d %d",&ch,&row,note,&ins,&vol,&pl,&pr,&delay)!=8 || row>=rows || ch>=e.getTotalChannelCount()-1 ||
          delay<0 || delay>=speed) {
        fprintf(stderr,"bad cell line: %s",line); return 1;
      }
      DivPattern* p=sub->pat[ch].getPattern(row/patLen,true);
      short* c=p->newData[row%patLen];
      if (strcmp(note,"off")==0) {
        if (delay>0) {
          // key-off inside the row: FCxx releases the note (SPU key-off, ADSR release) xx ticks in.
          c[DIV_PAT_FX(2)]=0xfc; c[DIV_PAT_FXVAL(2)]=delay;
        } else {
          c[DIV_PAT_NOTE]=DIV_NOTE_OFF;
        }
      } else if (delay!=0) {
        fprintf(stderr,"note-on off the grid: %s",line); return 1;
      } else {
        // MIDI note 60 is C-4; the pattern note value 60 is C-0.
        c[DIV_PAT_NOTE]=atoi(note)+48;
        c[DIV_PAT_INS]=ins;
        c[DIV_PAT_VOL]=vol;
        if (pl>=0) {
          c[DIV_PAT_FX(0)]=0x81; c[DIV_PAT_FXVAL(0)]=pl;
          c[DIV_PAT_FX(1)]=0x82; c[DIV_PAT_FXVAL(1)]=pr;
        }
      }
      cells++;
    }
  }
  fclose(in);
  printf("%s: %d samples, %d instruments, %d cells, %d rows in %d orders\n",e.song.name.c_str(),
         e.song.sampleLen,e.song.insLen,cells,rows,sub->ordersLen);

  e.renderSamples();
  SafeWriter* w=e.saveFur();
  bool ok=false;
  if (w!=NULL) {
    // zlib-compressed, as the GUI saves .fur files.
    uLongf zlen=compressBound(w->size());
    unsigned char* zbuf=new unsigned char[zlen];
    FILE* f=NULL;
    if (compress2(zbuf,&zlen,w->getFinalBuf(),w->size(),Z_BEST_COMPRESSION)==Z_OK) f=fopen(argv[2],"wb");
    if (f!=NULL) {
      ok=fwrite(zbuf,1,zlen,f)==zlen;
      fclose(f);
      printf("wrote %s (%u bytes, %u uncompressed)\n",argv[2],(unsigned)zlen,(unsigned)w->size());
    }
    delete[] zbuf;
    w->finish();
    delete w;
  }
  if (!ok) fprintf(stderr,"saving %s failed\n",argv[2]);
  // DivEngine::quit() does not return here (see make_corpus.cpp), so leave without it.
  fflush(stdout);
  _exit(ok?0:1);
}
