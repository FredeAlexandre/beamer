// Real-time core of beamer-audio: lock-free SPSC ring buffers and the two Core Audio IOProcs.
// Kept in C so that nothing on the HAL IO thread touches the Swift runtime (no retain/release,
// no allocation, no locks). Swift only sees opaque pointers and plain-struct snapshots.
#pragma once
#include <CoreAudio/CoreAudio.h>
#include <stddef.h>
#include <stdint.h>

// ---- Float sample ring (single producer, single consumer). Counts are in samples. ----
typedef struct BRing BRing;
BRing *bring_create(size_t capacity_samples);
void bring_destroy(BRing *r);
size_t bring_readable(BRing *r);
size_t bring_writable(BRing *r);
size_t bring_write(BRing *r, const float *src, size_t n);
size_t bring_read(BRing *r, float *dst, size_t n);
uint64_t bring_total_written(BRing *r);
uint64_t bring_total_read(BRing *r);

// ---- Per-callback timing record, logged by the IOProc for measurements. ----
// index: frame index in the ring stream (input: first frame written by this callback;
//        output: first frame read from the ring by this callback).
// frames: frames moved through the ring by this callback (output: may be < buffer on underrun).
// sample_time/host_time: the device timestamp of the data (inInputTime / inOutputTime).
// now_time: inNow host time (when the callback ran).
typedef struct {
    uint64_t index;
    uint32_t frames;
    uint32_t buffer_frames;
    uint32_t flags;  // 1 = input data dropped (ring full)
    uint32_t _pad;
    double sample_time;
    uint64_t host_time;
    uint64_t now_time;
} BTimeRec;

typedef struct BRecRing BRecRing;
int brec_pop(BRecRing *r, BTimeRec *out);  // 1 if a record was popped

// ---- IO context shared by an IOProc and its Swift pump. ----
typedef struct {
    uint64_t callbacks;
    uint64_t frames;          // device frames seen (buffer frames summed over callbacks)
    uint64_t ring_frames;     // frames moved through the ring
    uint64_t overruns;        // input: callbacks whose data was dropped (ring full)
    uint64_t underruns;       // output: callbacks that ran dry while playing
    uint64_t underrun_frames; // output: frames replaced by silence while playing
    uint32_t min_buffer_frames;
    uint32_t max_buffer_frames;
    uint32_t last_buffer_frames;
    uint32_t primed;
    double first_sample_time;
    double last_sample_time;
    uint64_t first_host_time;
    uint64_t last_host_time;
    uint64_t first_now_time;
    uint64_t last_now_time;
    double last_rate_scalar;
} BIOStats;

typedef struct BIOCtx BIOCtx;
// is_input: 1 = capture IOProc (reads inInputData), 0 = playback IOProc (fills outOutputData).
// channels: total channels across all streams in the IOProc's direction (ring frame width).
// prefill_frames: playback only, frames to accumulate before (re)starting after silence.
// log_records: keep a BTimeRec per callback (capacity in records, 0 = off).
BIOCtx *bio_create(int is_input, int channels, size_t ring_frames, size_t prefill_frames,
                   size_t log_records);
void bio_destroy(BIOCtx *c);
BRing *bio_ring(BIOCtx *c);
BRecRing *bio_records(BIOCtx *c);
AudioDeviceIOProc bio_proc(BIOCtx *c);
void bio_set_draining(BIOCtx *c, int draining);  // playback: play out what is left, no re-prime
void bio_set_silent(BIOCtx *c, int silent);      // playback: empty ring is not an underrun
int bio_wait(BIOCtx *c, int timeout_ms);         // wait for the next callback (or timeout)
void bio_stats(BIOCtx *c, BIOStats *out);
