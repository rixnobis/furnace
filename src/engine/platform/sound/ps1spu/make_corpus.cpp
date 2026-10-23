// Ours. Builds the small PS1 test corpus (papers/ps1-corpus/) through DivEngine's own API:
// one DIV_SYSTEM_PS1 chip per song, synthetic samples generated here, saved with saveFur().
// usage: ps1spu_corpus <outdir>
// songs (60 Hz ticks, speed 6 -> 0.1 s per row unless noted):
//   pitch_ladder.fur: ch0, C-3..C-6 by semitones, one row each; 1 s 1 kHz sine @44100 Hz (C-4 = 1 kHz)
//   adsr.fur:         ch0, four PS1 instruments with note-offs between, last one a decaying noise burst
//   loop.fur:         ch0, 84-sample sine (56 samples/cycle @56000 Hz), loop 28..84, held 2 s
//   poly.fur:         ch0-7, a chord of different notes with different 08xy pans, held 2 s
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../../../engine.h"

// normally defined in main.cpp, which this tool replaces.
void reportError(String what) {
  logE("%s",what);
}

#define NOTE(oct,semi) (60+(oct)*12+(semi)) // 60 is C-0, so C-4 is 108

static DivSample* makeSine(const char* name, int len, int rate, double hz, double amp) {
  DivSample* s=new DivSample;
  s->name=name;
  s->depth=DIV_SAMPLE_DEPTH_16BIT;
  s->centerRate=rate;
  s->init(len);
  for (int i=0; i<len; i++) {
    s->data16[i]=(short)round(amp*sin(2.0*M_PI*hz*i/rate));
  }
  return s;
}

static DivSample* makeNoiseBurst(int len, int rate) {
  DivSample* s=new DivSample;
  s->name="noise burst";
  s->depth=DIV_SAMPLE_DEPTH_16BIT;
  s->centerRate=rate;
  s->init(len);
  unsigned int lcg=12345;
  for (int i=0; i<len; i++) {
    lcg=lcg*1103515245u+12345u;
    double n=((double)((lcg>>16)&0x7fff)/16383.5)-1.0;
    s->data16[i]=(short)round(16000.0*n*exp(-5.0*i/len));
  }
  return s;
}

static DivInstrument* makeIns(const char* name, int sample) {
  DivInstrument* ins=new DivInstrument;
  ins->type=DIV_INS_PS1;
  ins->name=name;
  ins->amiga.initSample=sample;
  return ins;
}

// fresh song with one PS1 chip, speed 6 @ 60 Hz, one order, patLen rows.
static void newSong(DivEngine& e, const char* name, int patLen) {
  e.createNew("id0=225\n","Sony PlayStation",false);
  e.song.name=name;
  e.song.author="ps1-corpus generator";
  DivSubSong* sub=e.song.subsong[0];
  sub->hz=60.0f;
  sub->speeds.val[0]=6;
  sub->speeds.len=1;
  sub->patLen=patLen;
  sub->ordersLen=1;
}

static DivPattern* pat(DivEngine& e, int ch) {
  return e.song.subsong[0]->pat[ch].getPattern(0,true);
}

static void cell(DivEngine& e, int ch, int row, int note, int ins, int vol=-1) {
  DivPattern* p=pat(e,ch);
  p->newData[row][DIV_PAT_NOTE]=note;
  if (ins>=0) p->newData[row][DIV_PAT_INS]=ins;
  if (vol>=0) p->newData[row][DIV_PAT_VOL]=vol;
}

static void fx(DivEngine& e, int ch, int row, int col, int fx, int val) {
  DivPattern* p=pat(e,ch);
  p->newData[row][DIV_PAT_FX(col)]=fx;
  p->newData[row][DIV_PAT_FXVAL(col)]=val;
}

static bool save(DivEngine& e, const String& dir, const char* file) {
  e.renderSamples();
  SafeWriter* w=e.saveFur();
  if (w==NULL) {
    fprintf(stderr,"saveFur failed for %s\n",file);
    return false;
  }
  String path=dir+"/"+file;
  FILE* f=fopen(path.c_str(),"wb");
  if (f==NULL) {
    fprintf(stderr,"cannot open %s\n",path.c_str());
    w->finish(); delete w;
    return false;
  }
  fwrite(w->getFinalBuf(),1,w->size(),f);
  fclose(f);
  printf("wrote %s (%u bytes)\n",path.c_str(),(unsigned)w->size());
  w->finish();
  delete w;
  return true;
}

int main(int argc, char** argv) {
  if (argc<2) {
    fprintf(stderr,"usage: %s <outdir>\n",argv[0]);
    return 1;
  }
  String dir=argv[1];
  DivEngine e;
  e.preInit(true);
  e.setAudio(DIV_AUDIO_DUMMY);
  if (!e.init()) {
    fprintf(stderr,"engine init failed\n");
    return 1;
  }
  bool ok=true;

  // a. pitch ladder: C-3 (row 0) .. C-6 (row 36), one row (0.1 s) each, then stop.
  {
    newSong(e,"PS1 pitch ladder",40);
    e.addSamplePtr(makeSine("sine 1k 44100",44100,44100,1000.0,16000.0));
    e.addInstrumentPtr(makeIns("sine",0));
    for (int n=0; n<=36; n++) cell(e,0,n,NOTE(3,0)+n,n==0?0:-1);
    cell(e,0,37,DIV_NOTE_OFF,-1);
    fx(e,0,39,0,0xff,0x00);
    ok&=save(e,dir,"pitch_ladder.fur");
  }

  // b. ADSR: four instruments on a looped 1 kHz sine, note-offs between, then a noise burst.
  {
    newSong(e,"PS1 ADSR",64);
    // 1764 samples = 40 whole cycles of 1 kHz at 44100 Hz and 63 ADPCM blocks of 28.
    DivSample* s=makeSine("sine 1k loop",1764,44100,1000.0,16000.0);
    s->loop=true; s->loopStart=0; s->loopEnd=1764;
    e.addSamplePtr(s);
    e.addSamplePtr(makeNoiseBurst(8820,44100));
    DivInstrument* i0=makeIns("fast attack, slow release",0);
    i0->ps1.ar=0; i0->ps1.sl=15; i0->ps1.sr=0; i0->ps1.rr=16; i0->ps1.rrExp=false;
    DivInstrument* i1=makeIns("slow linear attack",0);
    i1->ps1.ar=60; i1->ps1.arExp=false; i1->ps1.sl=15; i1->ps1.sr=0; i1->ps1.rr=8;
    DivInstrument* i2=makeIns("exp decay to mid sustain",0);
    i2->ps1.ar=0; i2->ps1.dr=13; i2->ps1.sl=7; i2->ps1.sr=127; i2->ps1.rr=8;
    DivInstrument* i3=makeIns("noise burst",1);
    i3->ps1.ar=0; i3->ps1.sl=15; i3->ps1.sr=0; i3->ps1.rr=4;
    e.addInstrumentPtr(i0); e.addInstrumentPtr(i1); e.addInstrumentPtr(i2); e.addInstrumentPtr(i3);
    cell(e,0,0,NOTE(4,0),0);  cell(e,0,6,DIV_NOTE_OFF,-1);   // 0.6 s on, 1.0 s release
    cell(e,0,16,NOTE(4,0),1); cell(e,0,30,DIV_NOTE_OFF,-1);  // 1.4 s slow attack
    cell(e,0,34,NOTE(4,0),2); cell(e,0,48,DIV_NOTE_OFF,-1);  // 1.4 s decay/sustain
    cell(e,0,52,NOTE(4,0),3); cell(e,0,60,DIV_NOTE_OFF,-1);  // noise burst
    fx(e,0,63,0,0xff,0x00);
    ok&=save(e,dir,"adsr.fur");
  }

  // c. loop: 84 samples (1.5 cycles at 56 samples/cycle, 56000 Hz -> 1 kHz at C-4), loop 28..84.
  {
    newSong(e,"PS1 loop",24);
    DivSample* s=makeSine("one-cycle loop",84,56000,1000.0,16000.0);
    s->loop=true; s->loopStart=28; s->loopEnd=84;
    e.addSamplePtr(s);
    DivInstrument* i0=makeIns("loop",0);
    i0->ps1.rr=0;
    e.addInstrumentPtr(i0);
    cell(e,0,0,NOTE(4,0),0);
    cell(e,0,20,DIV_NOTE_OFF,-1); // held 2 s
    fx(e,0,23,0,0xff,0x00);
    ok&=save(e,dir,"loop.fur");
  }

  // d. poly: 8 channels, C major-ish spread, different pans, held 2 s.
  {
    newSong(e,"PS1 poly",24);
    DivSample* s=makeSine("sine 1k loop",1764,44100,1000.0,16000.0);
    s->loop=true; s->loopStart=0; s->loopEnd=1764;
    e.addSamplePtr(s);
    DivInstrument* i0=makeIns("poly",0);
    i0->ps1.rr=0;
    e.addInstrumentPtr(i0);
    const int notes[8]={NOTE(3,0),NOTE(3,4),NOTE(3,7),NOTE(4,0),NOTE(4,4),NOTE(4,7),NOTE(5,0),NOTE(5,4)};
    const int pans[8]={0xf0,0x0f,0xff,0xf8,0x8f,0xf4,0x4f,0x88};
    for (int c=0; c<8; c++) {
      cell(e,c,0,notes[c],0,0x20);
      fx(e,c,0,0,0x08,pans[c]);
      cell(e,c,20,DIV_NOTE_OFF,-1);
    }
    fx(e,0,23,0,0xff,0x00);
    ok&=save(e,dir,"poly.fur");
  }

  // e.quit() never returns here (observed: >5 min after all four files were written,
  // dummy audio backend, no playback started), so leave without tearing the engine down.
  fflush(stdout);
  _exit(ok?0:1);
}
