#include "rt.h"

#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------
// Float ring

struct BRing {
    float *buf;
    size_t cap;
    _Atomic uint64_t w;  // total samples written
    _Atomic uint64_t r;  // total samples read
};

BRing *bring_create(size_t cap) {
    BRing *r = calloc(1, sizeof(BRing));
    r->buf = calloc(cap ? cap : 1, sizeof(float));
    r->cap = cap ? cap : 1;
    atomic_init(&r->w, 0);
    atomic_init(&r->r, 0);
    return r;
}

void bring_destroy(BRing *r) {
    if (!r) return;
    free(r->buf);
    free(r);
}

size_t bring_readable(BRing *r) {
    uint64_t w = atomic_load_explicit(&r->w, memory_order_acquire);
    uint64_t rd = atomic_load_explicit(&r->r, memory_order_acquire);
    return (size_t)(w - rd);
}

size_t bring_writable(BRing *r) { return r->cap - bring_readable(r); }

size_t bring_write(BRing *r, const float *src, size_t n) {
    uint64_t w = atomic_load_explicit(&r->w, memory_order_relaxed);
    uint64_t rd = atomic_load_explicit(&r->r, memory_order_acquire);
    size_t space = r->cap - (size_t)(w - rd);
    if (n > space) n = space;
    size_t pos = (size_t)(w % r->cap);
    size_t first = r->cap - pos;
    if (first > n) first = n;
    if (src) {
        memcpy(r->buf + pos, src, first * sizeof(float));
        memcpy(r->buf, src + first, (n - first) * sizeof(float));
    } else {
        memset(r->buf + pos, 0, first * sizeof(float));
        memset(r->buf, 0, (n - first) * sizeof(float));
    }
    atomic_store_explicit(&r->w, w + n, memory_order_release);
    return n;
}

size_t bring_read(BRing *r, float *dst, size_t n) {
    uint64_t rd = atomic_load_explicit(&r->r, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&r->w, memory_order_acquire);
    size_t avail = (size_t)(w - rd);
    if (n > avail) n = avail;
    size_t pos = (size_t)(rd % r->cap);
    size_t first = r->cap - pos;
    if (first > n) first = n;
    memcpy(dst, r->buf + pos, first * sizeof(float));
    memcpy(dst + first, r->buf, (n - first) * sizeof(float));
    atomic_store_explicit(&r->r, rd + n, memory_order_release);
    return n;
}

uint64_t bring_total_written(BRing *r) { return atomic_load_explicit(&r->w, memory_order_acquire); }
uint64_t bring_total_read(BRing *r) { return atomic_load_explicit(&r->r, memory_order_acquire); }

// ---------------------------------------------------------------------------------------------
// Record ring

struct BRecRing {
    BTimeRec *recs;
    size_t cap;
    _Atomic uint64_t w;
    _Atomic uint64_t r;
};

static BRecRing *brec_create(size_t cap) {
    BRecRing *r = calloc(1, sizeof(BRecRing));
    r->recs = calloc(cap, sizeof(BTimeRec));
    r->cap = cap;
    return r;
}

static void brec_push(BRecRing *r, const BTimeRec *rec) {
    uint64_t w = atomic_load_explicit(&r->w, memory_order_relaxed);
    uint64_t rd = atomic_load_explicit(&r->r, memory_order_acquire);
    if (w - rd >= r->cap) return;  // consumer too slow: drop the record
    r->recs[w % r->cap] = *rec;
    atomic_store_explicit(&r->w, w + 1, memory_order_release);
}

int brec_pop(BRecRing *r, BTimeRec *out) {
    if (!r) return 0;
    uint64_t rd = atomic_load_explicit(&r->r, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&r->w, memory_order_acquire);
    if (rd == w) return 0;
    *out = r->recs[rd % r->cap];
    atomic_store_explicit(&r->r, rd + 1, memory_order_release);
    return 1;
}

// ---------------------------------------------------------------------------------------------
// IO context

#define SCRATCH_FRAMES 16384

struct BIOCtx {
    int is_input;
    int channels;
    size_t prefill_samples;
    BRing *ring;
    BRecRing *recs;
    float *scratch;
    dispatch_semaphore_t sem;
    _Atomic int draining;
    _Atomic int silent;
    int primed;
    // stats, published with a seqlock (writer: IO thread only)
    _Atomic uint32_t seq;
    BIOStats st;
};

static void stats_begin(BIOCtx *c) {
    atomic_fetch_add_explicit(&c->seq, 1, memory_order_acq_rel);
    atomic_thread_fence(memory_order_release);
}
static void stats_end(BIOCtx *c) {
    atomic_thread_fence(memory_order_release);
    atomic_fetch_add_explicit(&c->seq, 1, memory_order_acq_rel);
}

void bio_stats(BIOCtx *c, BIOStats *out) {
    for (;;) {
        uint32_t s1 = atomic_load_explicit(&c->seq, memory_order_acquire);
        if (s1 & 1) continue;
        atomic_thread_fence(memory_order_acquire);
        memcpy(out, (const void *)&c->st, sizeof(BIOStats));
        atomic_thread_fence(memory_order_acquire);
        uint32_t s2 = atomic_load_explicit(&c->seq, memory_order_acquire);
        if (s1 == s2) return;
    }
}

static void stats_common(BIOCtx *c, const AudioTimeStamp *now, const AudioTimeStamp *ts,
                         uint32_t frames) {
    BIOStats *s = &c->st;
    if (s->callbacks == 0) {
        s->first_sample_time = ts ? ts->mSampleTime : 0;
        s->first_host_time = ts ? ts->mHostTime : 0;
        s->first_now_time = now ? now->mHostTime : 0;
        s->min_buffer_frames = frames;
        s->max_buffer_frames = frames;
    }
    s->callbacks++;
    s->frames += frames;
    if (frames < s->min_buffer_frames) s->min_buffer_frames = frames;
    if (frames > s->max_buffer_frames) s->max_buffer_frames = frames;
    s->last_buffer_frames = frames;
    s->last_sample_time = ts ? ts->mSampleTime : 0;
    s->last_host_time = ts ? ts->mHostTime : 0;
    s->last_now_time = now ? now->mHostTime : 0;
    s->last_rate_scalar = ts ? ts->mRateScalar : 0;
}

static OSStatus in_proc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in,
                        const AudioTimeStamp *in_time, AudioBufferList *out,
                        const AudioTimeStamp *out_time, void *client) {
    (void)dev; (void)out; (void)out_time;
    BIOCtx *c = client;
    if (!in || in->mNumberBuffers == 0) return noErr;
    UInt32 ch0 = in->mBuffers[0].mNumberChannels ? in->mBuffers[0].mNumberChannels : 1;
    UInt32 frames = in->mBuffers[0].mDataByteSize / (UInt32)(sizeof(float) * ch0);
    if (frames > SCRATCH_FRAMES) frames = SCRATCH_FRAMES;
    const int C = c->channels;
    float *s = c->scratch;
    int base = 0;
    for (UInt32 b = 0; b < in->mNumberBuffers && base < C; b++) {
        const AudioBuffer *ab = &in->mBuffers[b];
        int bc = (int)ab->mNumberChannels;
        const float *src = ab->mData;
        for (int ch = 0; ch < bc && base + ch < C; ch++) {
            float *d = s + base + ch;
            if (src) {
                for (UInt32 f = 0; f < frames; f++) d[(size_t)f * C] = src[(size_t)f * bc + ch];
            } else {
                for (UInt32 f = 0; f < frames; f++) d[(size_t)f * C] = 0.f;
            }
        }
        base += bc;
    }
    for (int ch = base; ch < C; ch++)
        for (UInt32 f = 0; f < frames; f++) s[(size_t)f * C + ch] = 0.f;

    size_t n = (size_t)frames * C;
    uint64_t index = bring_total_written(c->ring) / (uint64_t)C;
    int dropped = 0;
    if (bring_writable(c->ring) < n) dropped = 1;
    else bring_write(c->ring, s, n);

    stats_begin(c);
    stats_common(c, now, in_time, frames);
    if (dropped) c->st.overruns++;
    else c->st.ring_frames += frames;
    stats_end(c);

    if (c->recs) {
        BTimeRec r = {index, dropped ? 0 : frames, frames, (uint32_t)dropped, 0,
                      in_time ? in_time->mSampleTime : 0, in_time ? in_time->mHostTime : 0,
                      now ? now->mHostTime : 0};
        brec_push(c->recs, &r);
    }
    dispatch_semaphore_signal(c->sem);
    return noErr;
}

static OSStatus out_proc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in,
                         const AudioTimeStamp *in_time, AudioBufferList *out,
                         const AudioTimeStamp *out_time, void *client) {
    (void)dev; (void)in; (void)in_time;
    BIOCtx *c = client;
    if (!out || out->mNumberBuffers == 0) return noErr;
    UInt32 ch0 = out->mBuffers[0].mNumberChannels ? out->mBuffers[0].mNumberChannels : 1;
    UInt32 frames = out->mBuffers[0].mDataByteSize / (UInt32)(sizeof(float) * ch0);
    if (frames > SCRATCH_FRAMES) frames = SCRATCH_FRAMES;
    const int C = c->channels;
    float *s = c->scratch;
    int draining = atomic_load_explicit(&c->draining, memory_order_acquire);
    int silent = atomic_load_explicit(&c->silent, memory_order_acquire);

    size_t avail = bring_readable(c->ring);
    if (!c->primed && (avail >= c->prefill_samples || (draining && avail > 0))) c->primed = 1;
    uint64_t index = bring_total_read(c->ring) / (uint64_t)C;
    UInt32 got = 0;
    int underrun = 0;
    if (c->primed) {
        size_t want = (size_t)frames * C;
        size_t take = avail < want ? avail - (avail % C) : want;
        bring_read(c->ring, s, take);
        got = (UInt32)(take / C);
        if (got < frames) {
            if (!silent && !draining) {
                underrun = 1;
                c->primed = 0;  // refill to the prefill level before resuming
            }
        }
    }
    memset(s + (size_t)got * C, 0, (size_t)(frames - got) * C * sizeof(float));

    int base = 0;
    for (UInt32 b = 0; b < out->mNumberBuffers; b++) {
        AudioBuffer *ab = &out->mBuffers[b];
        int bc = (int)ab->mNumberChannels;
        float *dst = ab->mData;
        if (!dst) { base += bc; continue; }
        for (int ch = 0; ch < bc; ch++) {
            if (base + ch < C) {
                const float *src = s + base + ch;
                for (UInt32 f = 0; f < frames; f++) dst[(size_t)f * bc + ch] = src[(size_t)f * C];
            } else {
                for (UInt32 f = 0; f < frames; f++) dst[(size_t)f * bc + ch] = 0.f;
            }
        }
        base += bc;
    }

    stats_begin(c);
    stats_common(c, now, out_time, frames);
    c->st.ring_frames += got;
    if (underrun) {
        c->st.underruns++;
        c->st.underrun_frames += frames - got;
    }
    c->st.primed = (uint32_t)c->primed;
    stats_end(c);

    if (c->recs) {
        BTimeRec r = {index, got, frames, 0, 0, out_time ? out_time->mSampleTime : 0,
                      out_time ? out_time->mHostTime : 0, now ? now->mHostTime : 0};
        brec_push(c->recs, &r);
    }
    dispatch_semaphore_signal(c->sem);
    return noErr;
}

BIOCtx *bio_create(int is_input, int channels, size_t ring_frames, size_t prefill_frames,
                   size_t log_records) {
    BIOCtx *c = calloc(1, sizeof(BIOCtx));
    c->is_input = is_input;
    c->channels = channels > 0 ? channels : 1;
    c->ring = bring_create(ring_frames * (size_t)c->channels);
    c->prefill_samples = prefill_frames * (size_t)c->channels;
    if (c->prefill_samples > c->ring->cap) c->prefill_samples = c->ring->cap;
    c->recs = log_records ? brec_create(log_records) : NULL;
    c->scratch = calloc((size_t)SCRATCH_FRAMES * c->channels, sizeof(float));
    c->sem = dispatch_semaphore_create(0);
    return c;
}

void bio_destroy(BIOCtx *c) {
    if (!c) return;
    bring_destroy(c->ring);
    if (c->recs) { free(c->recs->recs); free(c->recs); }
    free(c->scratch);
    free(c);
}

BRing *bio_ring(BIOCtx *c) { return c->ring; }
BRecRing *bio_records(BIOCtx *c) { return c->recs; }
AudioDeviceIOProc bio_proc(BIOCtx *c) { return c->is_input ? in_proc : out_proc; }
void bio_set_draining(BIOCtx *c, int d) { atomic_store_explicit(&c->draining, d, memory_order_release); }
void bio_set_silent(BIOCtx *c, int s) { atomic_store_explicit(&c->silent, s, memory_order_release); }

int bio_wait(BIOCtx *c, int timeout_ms) {
    return dispatch_semaphore_wait(c->sem, dispatch_time(DISPATCH_TIME_NOW,
                                                         (int64_t)timeout_ms * 1000000)) == 0;
}
