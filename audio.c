/*
 * Komposter
 *
 * Copyright (c) 2010 Noora Halme et al. (see AUTHORS)
 *
 * This code is licensed under the GNU General Public
 * License version 2. See LICENSE for full text.
 *
 * Audio playback and rendering
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "audio.h"
#include "buffermm.h"
#include "constants.h"
#include "modules.h"
#include "pattern.h"
#include "sequencer.h"
#include "synthesizer.h"

ALCdevice *dev;
ALCcontext *ctx;
ALuint buffers[3];
ALuint source;
ALenum format;
     
unsigned long playpos;
int audiomode=AUDIOMODE_COMPOSING; 
int oldtick=-1;
int audio_spinlock=0;
int restarts=0;
unsigned int audiomode_flags=0;


int voicepatch[MAX_CHANNELS];

short *render_buffer;
int render_state;
int render_oldtick;
int render_type;

// lengths and positions in 16-bit stereo samples
long render_bufferlen;
long render_pos;
long render_playpos;

// render start position and length in measures
int render_start;
int render_measures;

// looping play
int render_live_loop;
int render_loops;
int render_played_loops;

// from synthesizer.c
extern synthmodule mod[MAX_SYNTH][MAX_MODULES];
extern int signalfifo[MAX_SYNTH][MAX_MODULES];
extern int csynth;

extern int bpm; // from sequencer.c

// from pattern.c
extern u32 pattlen[MAX_PATTERN];
extern int cpatt;
extern u32 pattdata[MAX_PATTERN][MAX_PATTLENGTH];

// from modules.c
extern float pitch[MAX_SYNTH];
extern int accent[MAX_SYNTH];
extern int gate[MAX_SYNTH];
extern int restart[MAX_SYNTH];

// from patch.c
extern int cpatch[MAX_SYNTH]; // selected patch for each synth
extern float modvalue[MAX_SYNTH][MAX_PATCHES][MAX_MODULES];

// from sequencer.c
extern int seqch;
extern int seqsonglen;
extern int seq_pattern[MAX_CHANNELS][MAX_SONGLEN];
extern int seq_repeat[MAX_CHANNELS][MAX_SONGLEN];
extern int seq_transpose[MAX_CHANNELS][MAX_SONGLEN];
extern int seq_patch[MAX_CHANNELS][MAX_SONGLEN];
extern int seq_render_start;
extern int seq_render_end;
extern int seq_synth[MAX_CHANNELS];
extern int seq_restart[MAX_CHANNELS];
extern int seq_mute[MAX_CHANNELS];

// module instance data - module index is its mod structure index number, NOT signal stack position
float modulator[MAX_CHANNELS][MAX_MODULES];  // currently modulator value
float output[MAX_CHANNELS][MAX_MODULES];     // output "voltage" from each module
float localdata[MAX_CHANNELS][MAX_MODULES][16];  // 16 dwords of local data for each module

// audio peak values
float audio_peak, audio_latest_peak;

// macros for typecasting the localdata void ptr
#define mod_fdata(v,m)  ((float*)localdata[v][m])
#define mod_fpdata(v,m) ((float**)localdata[v][m])
#define mod_ldata(v,m)  ((u32*)localdata[v][m])
#define mod_lpdata(v,m) ((u32**)localdata[v][m])
#define mod_ddata(v,m)  ((double*)localdata[v][m])


// above which point to round audio peaks
#define AUDIO_SHAPER_THRESHOLD	0.9


// waveshaper to limit audio range
float audio_shape(float input)
{
  return 0.5 * (fabs(input + AUDIO_SHAPER_THRESHOLD) - fabs(input - AUDIO_SHAPER_THRESHOLD));  
}


int audio_initialize(void)
{
  int error, i;
  short data[AUDIOBUFFER_LEN*2]; //16bit stereo

  audio_peak=0.0f;
  audio_latest_peak=0.0f;

  playpos=0;

  render_buffer=NULL;
  render_state=RENDER_STOPPED;
  render_type=RENDER_LIVE;
  
  render_live_loop=0;
  render_loops=0;
  render_played_loops=0;

  dev=NULL;
  ctx=NULL;
  dev=alcOpenDevice(NULL);
  if (dev==NULL) { printf("alcOpenDevice() failed to return a device!\n"); return 0; }
  ctx=alcCreateContext(dev, NULL);
  if (ctx==NULL) { printf("alcCreateContext() failed to return a context!\n"); return 0; }
  alcMakeContextCurrent(ctx);
  format = AL_FORMAT_STEREO16;

  alGenBuffers(3, buffers);
  if (alGetError()!=AL_NO_ERROR) { printf("Failed to generate audio buffers!\n"); return 0; }

  alGenSources(1, &source);
  if (alGetError()!=AL_NO_ERROR) { printf("Failed to generate audio source\n"); return 0; }

  // set positions  
  alSource3f(source, AL_POSITION,        0.0, 0.0, 0.0);
  alSource3f(source, AL_VELOCITY,        0.0, 0.0, 0.0);
  alSource3f(source, AL_DIRECTION,       0.0, 0.0, 0.0);
  alSourcef (source, AL_ROLLOFF_FACTOR,  0.0          );
  alSourcei (source, AL_SOURCE_RELATIVE, AL_TRUE      );

  // set gain
  alSourcef(source, AL_GAIN, 1.0f);

  // queue three empty zeroed buffers
  for(i=0;i<AUDIOBUFFER_LEN;i++) { data[i*2]=0; data[i*2+1]=0; }
  alBufferData(buffers[0], format, data, AUDIOBUFFER_LEN*4, OUTPUTFREQ);  
  alBufferData(buffers[1], format, data, AUDIOBUFFER_LEN*4, OUTPUTFREQ);
  alBufferData(buffers[2], format, data, AUDIOBUFFER_LEN*4, OUTPUTFREQ);

  // start playback
  alSourceQueueBuffers(source, 3, buffers);
  error=alGetError();
  if (error!=AL_NO_ERROR) { printf("Failed to queue source buffers (err %d/0x%x)\n",error,error); return 0; }
  alSourcePlay(source);
  if (alGetError()!=AL_NO_ERROR) { printf("Failed to start source playback\n"); return 0; }

  return 1;
}



int audio_isplaying()
{
    ALenum state;
    
    alGetSourcei(source, AL_SOURCE_STATE, &state);
    return (state == AL_PLAYING);
}


void audio_release(void)
{
  ALuint buffer;
  int queued;

  alSourceStop(source);
  alGetSourcei(source, AL_BUFFERS_QUEUED, &queued);
  while(queued--)
    alSourceUnqueueBuffers(source, 1, &buffer);
  alDeleteSources(1, &source);
  alDeleteBuffers(1, buffers);
}


int audio_update(int cs)
{
  int processed, active;
  ALuint buffer;
  short data[AUDIOBUFFER_LEN*2]; //16bit stereo

  active=0;
  alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
  // if (processed) printf("refilling %d buffer(s)\n",processed);
  while(processed--)
  {
    alSourceUnqueueBuffers(source, 1, &buffer);
    if (alGetError()!=AL_NO_ERROR) return 0;

    // fill data and queue the buffer
    audio_latest_peak=0.0f;
    audio_process((short*)(&data), AUDIOBUFFER_LEN);
    alBufferData(buffer, format, data, AUDIOBUFFER_LEN*4, OUTPUTFREQ);
    active++;
 
    alSourceQueueBuffers(source, 1, &buffer);
    if (alGetError()!=AL_NO_ERROR) return 0;
  }

  // restart if both buffers ran out
  if (!audio_isplaying()) {
    // printf("audio: playback died, restarting(%d)..\n", restarts++);
    alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
    alSourcePlay(source);
  }
  
  return active; // number of buffers re-filled
}


// play into a buffer. bufferlen = number of 16-bit stereo samples
int audio_process(short *buffer, long bufferlen)
{
  int i, m, mi=0, mt, ii, pkey;
  float signals[4], p;
  short s;
  long ticks=0, copylen;
  int voice, pattpos;
  void *buf;

  // clear the buffer
  for(i=0;i<bufferlen*2;i++) buffer[i]=0;

  // if playback is muted, exit immediately
  if (audiomode==AUDIOMODE_MUTE) return bufferlen;

  // now we start the render for real - set the spin lock to signal other threads that
  // we're rendering and no changes should be made to synth data
  audio_spinlock=0;

  if (audiomode==AUDIOMODE_PLAY) {
    // start a new render     
    if (render_state==RENDER_START) {
      // start a new render, reset all synths and load patch 0
      for(i=0;i<seqch;i++) {
        audio_resetsynth(i); 
        audio_loadpatch(i, seq_synth[i], 0);
      }
      if (render_buffer) { free(render_buffer); render_buffer=NULL; }
      render_start=seq_render_start;
      if (render_type==RENDER_IN_PROGRESS) {
        render_measures=seq_render_end - seq_render_start;
      } else {
        if (seq_render_end > seq_render_start) {
          render_measures=seq_render_end - seq_render_start;
        } else {
          render_measures=seqsonglen - seq_render_start;
        }
      }
      render_bufferlen=((OUTPUTFREQ*60*render_measures*4)/bpm);
      render_buffer=calloc(2*render_bufferlen, sizeof(short));
      render_pos=0;
      render_state=render_type;
      render_playpos=0;
      render_oldtick=-1;
      render_loops=0;
      render_played_loops=0;
    }

    // if we're playing live, make sure there's enough unplayed audio in the render
    // buffer and then copy from render buffer to audio output buffer. if the computer
    // is too slow for the number of channels/synths used, the audio output will have
    // gaps as no data at all is copied to output if there's not enough to fill one
    // buffer.
    if (render_state==RENDER_LIVE && render_live_loop) {
      // copy as usual
      copylen=bufferlen;
      if ((render_playpos+copylen) >= render_bufferlen) {
        // can't get a full buffer
        copylen = render_bufferlen - render_playpos;
        memcpy(buffer, &render_buffer[render_playpos*2], copylen*4);
        render_playpos=0; //+=copylen;
        if (copylen < bufferlen) {
          // restart playing from start of render buffer, copy rest of buffer from there
          memcpy(&buffer[copylen], &render_buffer[0], (bufferlen-copylen)*4);
          render_playpos+=bufferlen-copylen;
        }
        render_played_loops++;
      } else {
        memcpy(buffer, &render_buffer[render_playpos*2], copylen*4);
        render_playpos+=copylen;        
      }
    }
    if (render_state==RENDER_PLAYBACK  ||
        ((render_state==RENDER_LIVE || render_state==RENDER_LIVE_COMPLETE) && render_pos>=(render_playpos+bufferlen) && !render_live_loop)
       )
    {
      copylen=bufferlen;
      if ((render_playpos+copylen) >= render_bufferlen) {
        // at end of renderbuffer - copy last full or partial buffer to playback buffer
        copylen = render_bufferlen - render_playpos;
        memcpy(buffer, &render_buffer[render_playpos*2], copylen*4);
        render_playpos+=copylen;
        if (copylen < bufferlen) {
          // fill the rest of the playback buffer with zero
          memset(&buffer[copylen], 0, (bufferlen-copylen)*4);
        }

        // stop playback and reset synths to clear any sounds left playing
        render_state=RENDER_COMPLETE; render_playpos=0;
        for(i=0;i<seqch;i++) audio_resetsynth(i);

      } else {
        // copy a full buffer from renderbuffer to playback buffer
        memcpy(buffer, &render_buffer[render_playpos*2], copylen*4);
        render_playpos+=copylen;        
      }
    }

    // stop live playback if no longer able to buffer data to output
    if (render_state==RENDER_LIVE_COMPLETE && ((render_bufferlen-render_playpos)<bufferlen)) {
        render_state=RENDER_COMPLETE;
        audiomode=AUDIOMODE_COMPOSING;
        for(i=0;i<seqch;i++) audio_resetsynth(i);
    }
    audio_spinlock=0;
    return bufferlen;
  }

  // loop for each sample in buffer
  for(i=0;i<bufferlen;i++) {
    voice=0;    
    if (audiomode==AUDIOMODE_PATTERNPLAY) {

      if (audiomode_flags&1) {
        playpos=0;
        oldtick=-1;
      }
      audiomode_flags=0;
      
      ticks=playpos / (OUTPUTFREQ/(bpm*256/60)); // calc tick from sample index
      pattpos=ticks>>6;

      // follow the pattern and play any notes
      if (ticks!=oldtick) { // new tick
        if (!(ticks&63)) {
          // tick 0/64/128/192 : trigger notes
          if (pattdata[cpatt][pattpos] && !(pattdata[cpatt][pattpos]&NOTE_LEGATO)) {
            pkey=pattdata[cpatt][pattpos]&0x7f;
            audio_trignote(voice, pkey);
            accent[voice] = (pattdata[cpatt][pattpos]&NOTE_ACCENT) ? 1 : 0;
          }
          // TODO: push a slide to stack if portamento
        }
        if ((ticks&63)==60) {
          // tick 60/124/188/252 : drop gate if following note is not legato
          m=pattpos+1;
          gate[voice]=0;
          if ( m<(pattlen[cpatt]*16) ) { // don't drop gate if next note is legato
            if (pattdata[cpatt][m]&NOTE_LEGATO) gate[voice]=1;
          }
        }
      }        
    }
    
    if (audiomode==AUDIOMODE_PATTERNPLAY || audiomode==AUDIOMODE_COMPOSING) {
      // copy modulator values from active patch when composing / previewing pattern
      audio_loadpatch(voice, csynth, cpatch[csynth]);

      // process the synthesizer signal stack
      m=0;
      while (signalfifo[csynth][m]>=0) {
        mi=signalfifo[csynth][m];
        mt=mod[csynth][mi].type;
        for(int i=0;i<4;i++) {
          ii = mod[csynth][mi].input[i];
          signals[i] = (ii>=0) ? output[0][ii] : 0.0f;
        }
        if (mt>=0 && modDataBufferLength[mt]) {
          memcpy(&buf, &localdata[voice][mi][0], sizeof(void*));
          if (!buf) {
            // !!!! this doesn't compile with xcode 4.1 LLVM
            buf=kmm_alloc(modDataBufferLength[mt], voice, csynth, mi, mt);
            memcpy(&localdata[voice][mi][0], &buf, sizeof(void*));
            // !!!!
          }
        }
        if (mt>=0)
          output[voice][mi]=mod_functable[mt](voice, &modulator[voice][mi], (void*)&localdata[voice][mi], (float*)&signals);
        m++;
      }
      restart[voice]=0; // did restart on this sample
      p=output[voice][mi];
       
      // update audio peaks
      if (fabs(p) > audio_peak) audio_peak=fabs(p);
      if (fabs(p) > audio_latest_peak) audio_latest_peak=fabs(p);
 
      p=audio_shape(p);
      s=(short)(32766*p);
      buffer[i*2]=s; buffer[i*2+1]=s; // output stream is in stereo

      // advance the play position
      oldtick=ticks;
      playpos++;
      if ((ticks>>10) >= pattlen[cpatt]) { ticks=0; playpos=0; }
    }
  }

  // ok, buffer is filled and we're done! release the spin lock
  audio_spinlock=0;
  return bufferlen;
}








long audio_render(void)
{
  int m, mi=0, mt, ii, pkey;
  float signals[4], p;
  short s;
  int i, voice;
  int synth;
  int pattern, pattstart, pattpos;
  long ticks=0;
  void *buf;
  short *buffer;
  long bufferlen;

  // render a block of audio
  bufferlen=AUDIOBUFFER_LEN;  
  if ((render_pos+bufferlen) > (render_bufferlen))
  bufferlen = (render_bufferlen) - render_pos;  
  buffer=&render_buffer[render_pos*2];

  // when playing live, we want to keep render_pos no more than than AUDIO_RENDER_AHEAD
  // buffers ahead of render_playpos to allow changes to patches during playback.
  if (render_live_loop) {
    if (render_loops > render_played_loops) {
      // render has looped back but playback is still rendering
      if (render_pos >= AUDIO_RENDER_AHEAD*bufferlen) return 0;
    } else {
      if (render_state==RENDER_LIVE && render_pos >= (render_playpos+AUDIO_RENDER_AHEAD*bufferlen)) return 0;      
    }
  } else {
    if (render_state==RENDER_LIVE && render_pos >= (render_playpos+AUDIO_RENDER_AHEAD*bufferlen)) return 0;
  }

  // loop for each sample in buffer
  for(i=0;i<bufferlen;i++) {
    p=0;
    ticks=render_pos / (OUTPUTFREQ/(bpm*256/60)); // calc tick from sample index
    ticks+=(render_start<<10);

    for(voice=0;voice<seqch;voice++) {
      synth=seq_synth[voice];

      // find if there's a pattern playing on this voice
      pattern=-1; // no pattern by default
      pattstart=0;
      if (sequencer_ispattern(voice, ticks>>10)) { // is there a pattern here?
        pattstart=sequencer_patternstart(voice, ticks>>10);
        pattern=seq_pattern[voice][pattstart];
        pattpos=(ticks/64) - (pattstart*16);
        while (pattpos>=(pattlen[pattern]*16)) pattpos-=(pattlen[pattern]*16);
        // pattpos is the play position inside the pattern
      }

      if (pattern>=0) {
        // follow the pattern and play any notes
        if (ticks!=render_oldtick) { // new tick
          if (!(ticks&63)) {
            if (pattpos==0 || render_pos==0) {
              // tick 0 on new pattern or first sample of a render run -> load patch to synth
              audio_loadpatch(voice, synth, seq_patch[voice][pattstart]);
            }
            // tick 0/64/128/192 : trigger notes
            if (pattdata[pattern][pattpos] && !(pattdata[pattern][pattpos]&NOTE_LEGATO)) {
              pkey=pattdata[pattern][pattpos]&0x7f;
              pkey+=seq_transpose[voice][pattstart];
 	      audio_trignote(voice, pkey);
              accent[voice] = (pattdata[pattern][pattpos]&NOTE_ACCENT) ? 1 : 0;
            }
            // TODO: push a slide to stack if portamento
          }
          if ((ticks&63)==60) {
            // tick 60/124/188/252 : drop gate if following note is not legato
            m=pattpos+1;
            gate[voice]=0;
            if ( m<(pattlen[pattern]*16) ) { // don't drop gate if next note is legato
              if (pattdata[pattern][m]&NOTE_LEGATO) gate[voice]=1;
            }
          }
        }        
      }

      // process the synthesizer signal stack
      m=0;
      while (signalfifo[synth][m]>=0 && m<MAX_MODULES) {
        mi=signalfifo[synth][m];
        mt=mod[synth][mi].type;
        for(int i=0;i<4;i++) {
          ii = mod[synth][mi].input[i];
          signals[i] = (ii>=0) ? output[voice][ii] : 0.0f;
        }
        
        if (mt>=0 && modDataBufferLength[mt]) {
          memcpy(&buf, &localdata[voice][mi][0], sizeof(void*));

          if (!buf) {
            // !!!! this does not compile with xcode 4.1 LLVM
            buf=kmm_alloc(modDataBufferLength[mt], voice, synth, mi, mt);
            memcpy(&localdata[voice][mi][0], &buf, sizeof(void*));
            // !!!!
          }

        }

        if (mt>=0)
          output[voice][mi]=mod_functable[mt](voice, &modulator[voice][mi], (void*)&localdata[voice][mi], (float*)&signals);
        m++;
      }

      restart[voice]=0; // did restart on this sample

      if (seq_mute[voice]) continue; // skip mixing into final output if voice is muted
      p+=output[voice][mi];
    }

    // update audio peaks
    if (fabs(p) > audio_peak) audio_peak=fabs(p);
    if (fabs(p) > audio_latest_peak) audio_latest_peak=fabs(p);

    p=audio_shape(p);
    s=(short)(32766*p);
    buffer[i*2]=s; buffer[i*2+1]=s; // output stream is in stereo

    oldtick=ticks;
    render_pos++;
    if (render_pos >= render_bufferlen) {
      if (render_state==RENDER_LIVE) {
        if (!render_live_loop) {
          render_state=RENDER_LIVE_COMPLETE;
          return i;
        } else {
          // loop back to start
          render_pos=0;
          render_loops++;
          audio_panic();
        }
      } else {
        render_state=RENDER_COMPLETE; return i;
      }
    }
    
    
  }

  // ok, buffer is filled and we're done!
  return bufferlen;
}



// loads a patch from the bank to the synth voice
void audio_loadpatch(int voice, int synth, int patch)
{
  int j;

  // copy modulator values form patch to synth modules
  for(j=0;j<MAX_MODULES;j++) if (mod[synth][j].type)
    modulator[ voice ][ j ] = modvalue[ synth ][ patch ][ j ];
}

// trigger a note
void audio_trignote(int voice, int note)
{
  int i;
  float freq;

  freq=8.1757989156; // c-0 in hz
  for(i=0;i<note;i++) freq*=1.059463094; // ratio between two seminotes
  pitch[voice]=freq;
  gate[voice]=1;
  restart[voice]=seq_restart[voice];
  // printf("note_on: voice %d midi note %d, hardrestart=%d\n",voice,note,hardrestart);
}


// panic reset - completely resets all modules
void audio_panic(void)
{
  int voice;
  for(voice=0;voice<seqch;voice++) audio_resetsynth(voice);
}


// reset a synth voice so that it no longer produces sound
void audio_resetsynth(int voice)
{
  int m,mi,mt,synth;
  long i;
  float *lbuf;

  gate[voice]=0;
  m=0;
  synth=seq_synth[voice];
  while (signalfifo[synth][m]>=0 && m<MAX_MODULES) {
    mi=signalfifo[synth][m];
    mt=mod[synth][mi].type;
    output[voice][mi]=0;
    pitch[voice]=110.0/OUTPUTFREQ;
    switch(mt) {
      case MOD_WAVEFORM:
        mod_fdata(voice, mi)[0]=0; // osc accu
        mod_fdata(voice, mi)[1]=0; // subosc accu
        break;
      case MOD_ADSR:
         mod_fdata(voice, mi)[0]=0; // accu
         mod_ldata(voice, mi)[1]=0;  // old gate
       break;
      case MOD_LFO:
        mod_fdata(voice, mi)[0]=0; // accu
        break;
      case MOD_DELAY:
//        lbuf=mod_fpdata(voice, mi)[0]; // bufferptr
        memcpy(&lbuf, &localdata[voice][mi][0], sizeof(void*));
        if (lbuf) memset(lbuf, 0, modDataBufferLength[MOD_DELAY]*sizeof(float));
        else printf("audio_resetsynth(): buffer for delay not yet allocated for v %d mi %d\n", voice, mi);
        mod_ldata(voice, mi)[2]=0; // write position
       break;
      case MOD_FILTER:
        mod_fdata(voice, mi)[0]=0; //lp
        mod_fdata(voice, mi)[1]=0; //bp
        mod_fdata(voice, mi)[2]=0; //hp        
        break;
      case MOD_LPF24:
        for(i=0;i<8;i++) mod_ddata(voice, mi)[0]=0;
        break;
      case MOD_RESAMPLE:
        mod_fdata(voice, mi)[0]=0; // accu
        mod_fdata(voice, mi)[1]=0; // held sample
        break;
      case MOD_SUPERSAW:
        for(i=0;i<8;i++) mod_fdata(voice, mi)[0]=0;
        mod_fdata(voice, mi)[8]=0; //lp
        mod_fdata(voice, mi)[9]=0; //bp
        mod_fdata(voice, mi)[10]=0; //hp
        break;
    }
    m++;
  }
}




// export the rendered audio clip as a wav file
typedef struct {
  char wav_chunkid[4];
  unsigned int wav_chunksize;
  char wav_format[4];
  char wav_sub1chunkid[4];
  unsigned int wav_sub1chunksize;
  unsigned short wav_audioformat;
  unsigned short wav_numchannels;
  unsigned long wav_samplerate;
  unsigned long wav_byterate;
  unsigned short wav_blockalign;
  unsigned short wav_bitspersample;
  char wav_sub2chunkid[4];
  unsigned long wav_sub2chunksize;
} wavheader;
int audio_exportwav() //char *filename)
{
  FILE *f;
  wavheader w;

  char *home, audiofile[512], logentry[1024];

  home=getenv("HOME");
  snprintf(audiofile, 511, "%s/Desktop/komposter_render_%u.wav", home, (int)time(NULL));
  
  strncpy((char*)&w.wav_chunkid, "RIFF", 4);
  w.wav_chunksize=0;
  strncpy((char*)&w.wav_format, "WAVE", 4);
  strncpy((char*)&w.wav_sub1chunkid, "fmt ", 4);
  w.wav_sub1chunksize=16;
  w.wav_audioformat=1;
  w.wav_numchannels=2;
  w.wav_samplerate=44100;    
  w.wav_byterate=44100*2*2;
  w.wav_blockalign=2*2;
  w.wav_bitspersample=16;
  strncpy((char*)&w.wav_sub2chunkid, "data", 4);
  w.wav_sub2chunksize=0;

  w.wav_chunksize=36+render_bufferlen*2*2;
  w.wav_sub2chunksize=render_bufferlen*2*2;

  f=fopen(audiofile, "wb");
  fwrite(&w, sizeof(wavheader), 1, f);
  fwrite(render_buffer, sizeof(short), render_bufferlen*2, f);
  fclose(f);

  snprintf(logentry, 1023, "Wrote rendered audio to %s", audiofile);
  console_post(logentry);

  return 0;
}

