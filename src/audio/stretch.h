/* The tempo stretcher's C face (stretch.cpp), shared by the mixer and the C++ that wraps Signalsmith Stretch. */
#ifndef CW_AUDIO_STRETCH_H
#define CW_AUDIO_STRETCH_H

#ifdef __cplusplus
extern "C" {
#endif

/* One configuration for every voice, so these are constants: the stretcher is fed CW__STRETCH_IN_LAT frames past
 * the point it is processing, and its output is CW__STRETCH_OUT_LAT frames behind that point. Starting it at a
 * position takes CW__STRETCH_SEEK frames of history. */
#define CW__STRETCH_IN_LAT 2880
#define CW__STRETCH_OUT_LAT 4320
#define CW__STRETCH_SEEK 7200
#define CW__STRETCH_CHUNK 512      /* output frames per process call */
#define CW__STRETCH_IN_MAX 8192    /* input frames per call (a seek: 7200; a chunk at tempo 2: 1025) */
#define CW__TRING 4096             /* stretched frames kept per voice (a power of two) */
#define CW__TEMPO_ENTER 2048       /* mixer frames over which a playing voice's stretcher warms up before it is heard */
#define CW__TSEGS 16               /* tempo changes remembered (the clock follows them CW__STRETCH_OUT_LAT late) */

typedef struct cw__stretch cw__stretch;
cw__stretch *cw__stretch_new(void);      /* [game]: allocates */
void cw__stretch_free(cw__stretch *s);  /* [game] */
float *cw__stretch_in(cw__stretch *s, int ch);  /* CW__STRETCH_IN_MAX frames per channel */
float *cw__stretch_out(cw__stretch *s, int ch); /* CW__STRETCH_CHUNK frames per channel */
void cw__stretch_reset(cw__stretch *s);
void cw__stretch_seek(cw__stretch *s, int n);             /* the history: n frames from the input buffers */
void cw__stretch_process(cw__stretch *s, int nin, int nout); /* nin input frames in, nout out */

#ifdef __cplusplus
}
#endif
#endif
