// Real-time audio passthrough core. See PassthroughCore.h.
//
// Threads:
//   - control thread (Swift): start/stop, configuration
//   - input IO thread:  in_cb  -> ring_write
//   - output IO thread: out_cb -> AudioUnitRender(Varispeed) -> vs_pull -> ring read
//
// The ring is single-producer/single-consumer: only the input thread moves
// `wpos`, only the output thread moves `rpos`. The output thread steers the
// fill level toward a small target with a PI controller on the Varispeed
// playback rate, which absorbs the clock drift between two independent USB
// devices without audible artifacts (±1000 ppm max ≈ ±1.7 cents).
//
// Dropouts and restarts are faded so they do not click: a dropout plays out
// what is left with a fade-out, every (re)start and the first buffer after the
// HAL skipped output cycles fade in, and dropping a backlog crossfades from the
// old read position to the new one. (An input chunk lost while the ring still
// covers it remains an unfaded splice; it is counted as an input gap.)
#include "PassthroughCore.h"
#include <math.h>

// Older SDKs lack the real-time annotation; the code is the same without it.
#ifndef CA_REALTIME_API
#define CA_REALTIME_API
#endif
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define PT_CH 2
#define KP_PPM_PER_FRAME 4.0       // proportional gain
#define KI_PPM_PER_FRAME_S 0.2     // integral gain (ζ ≈ 1, ωn ≈ 0.1 rad/s at 48 kHz)
#define MAX_PPM 1000.0
#define FILL_EMA_SECONDS 0.5
#define RESYNC_MIN_SLACK_FRAMES 480 // 10 ms (+ one input buffer) above target => drop the backlog
#define TARGET_STEP_FRAMES 64      // added after each jitter underrun
#define TARGET_MAX_EXTRA 2048
#define TARGET_DECAY_SECONDS 60.0  // quiet time before giving some of it back
#define TARGET_DECAY_FRAMES 16
#define DECAY_HEADROOM_FRAMES 48   // spare frames that must remain after a decay step
#define FADE_FRAMES 96             // 2 ms fade-in / crossfade
#define FADE_RESERVE_FRAMES 32     // kept back so a dropout always has audio left to fade out

struct PTCore {
    float *ring[PT_CH];
    uint32_t cap, mask, maxIO;
    _Atomic int64_t wpos;
    _Atomic int64_t rpos;

    // Seqlock-published (write position, host time of that write), used to
    // extrapolate the fill to "now" so the controller does not see the
    // sawtooth of whole input chunks arriving.
    _Atomic uint32_t wseq;
    _Atomic int64_t wposStamp;
    _Atomic uint64_t wHostNs;

    // Input side. Written by the control thread while the input is stopped.
    AudioUnit inUnit;
    uint32_t inChannels;
    double inRate;
    float *inScratch[PT_CH];
    AudioBufferList *inABL;
    _Atomic uint32_t inChunk;      // input IO buffer size in frames
    double nextSampleTime;         // input thread only

    // Fade curves, computed once (no libm on the IO threads).
    float fadeIn[FADE_FRAMES];     // raised cosine 0 -> 1
    float xfadeNew[FADE_FRAMES];   // equal power: sin, for two unrelated stretches of audio
    float xfadeOld[FADE_FRAMES];   // equal power: cos

    // Output side. Written by the control thread while the output is stopped.
    AudioUnit outUnit, vsUnit;
    double outRate;
    double outChunk;               // output IO buffer, in input-rate frames
    double framesPerNs;            // input-rate frames per nanosecond

    // Output thread only.
    double avgFill, integ, quietSeconds;
    double minHeadroom;            // smallest spare fill seen since the last decay check
    uint32_t extraTarget;
    int running;
    int underrunPending;           // ran dry; classified once audio flows again
    uint64_t gapsAtUnderrun;
    uint32_t fadeInPos;            // frames into the fade-in after a (re)start
    float xfOld[PT_CH][FADE_FRAMES]; // backlog drop: the audio being faded out, copied
    uint32_t xfLeft;                 // before the read position moves (the writer may reuse it)
    float gain;                    // volume applied to the last buffer (ramps toward `volume`)
    double nextOutSampleTime;

    _Atomic uint32_t margin;
    _Atomic float volume;
    _Atomic uint64_t underruns, inputGaps, overruns, resyncs, inCb, outCb;
    _Atomic uint64_t dropouts, inputErrors, outputSkips;
    _Atomic double pubFill, pubPPM;
    _Atomic uint32_t pubTarget, pubRunning;
};

static uint64_t now_ns(void) CA_REALTIME_API {
    return AudioConvertHostTimeToNanos(AudioGetCurrentHostTime());
}

// MARK: - Input thread

static void ring_write(PTCore *c, float *const src[PT_CH], uint32_t n) CA_REALTIME_API {
    int64_t w = atomic_load_explicit(&c->wpos, memory_order_relaxed);
    int64_t r = atomic_load_explicit(&c->rpos, memory_order_acquire);
    if ((uint64_t)(w - r) + n > c->cap) {
        // The consumer is stalled (output stopped); drop this chunk, it resyncs later.
        atomic_fetch_add_explicit(&c->overruns, 1, memory_order_relaxed);
        return;
    }
    uint32_t off = (uint32_t)(w & c->mask);
    uint32_t first = c->cap - off;
    if (first > n) first = n;
    for (int ch = 0; ch < PT_CH; ch++) {
        memcpy(c->ring[ch] + off, src[ch], first * sizeof(float));
        memcpy(c->ring[ch], src[ch] + first, (n - first) * sizeof(float));
    }
    atomic_store_explicit(&c->wpos, w + n, memory_order_release);

    uint32_t q = atomic_load_explicit(&c->wseq, memory_order_relaxed);
    atomic_store_explicit(&c->wseq, q + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&c->wposStamp, w + n, memory_order_relaxed);
    atomic_store_explicit(&c->wHostNs, now_ns(), memory_order_relaxed);
    atomic_store_explicit(&c->wseq, q + 2, memory_order_release);
}

static OSStatus in_cb(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
                      UInt32 bus, UInt32 n, AudioBufferList *unused) CA_REALTIME_API {
    (void)unused;
    PTCore *c = ref;
    if (n > c->maxIO) return kAudioUnitErr_TooManyFramesToProcess;
    for (uint32_t ch = 0; ch < c->inChannels; ch++) {
        c->inABL->mBuffers[ch].mData = c->inScratch[ch];
        c->inABL->mBuffers[ch].mDataByteSize = n * sizeof(float);
    }
    OSStatus err = AudioUnitRender(c->inUnit, flags, ts, bus, n, c->inABL);
    if (err != noErr) {
        // This chunk is lost. nextSampleTime stays put, so the next callback
        // counts the hole as a gap and the dropout is not blamed on jitter.
        atomic_fetch_add_explicit(&c->inputErrors, 1, memory_order_relaxed);
        return err;
    }

    // A jump in device sample time means the device delivered nothing for a
    // while: the USB audio stream was restarted after a transfer error, the HAL
    // skipped a cycle, or Core Audio restarted the device's IO. Even one lost
    // buffer counts.
    if (ts->mFlags & kAudioTimeStampSampleTimeValid) {
        if (c->nextSampleTime >= 0 && fabs(ts->mSampleTime - c->nextSampleTime) > (double)n / 2) {
            atomic_fetch_add_explicit(&c->inputGaps, 1, memory_order_relaxed);
        }
        c->nextSampleTime = ts->mSampleTime + n;
    }

    float *src[PT_CH] = { c->inScratch[0], c->inChannels > 1 ? c->inScratch[1] : c->inScratch[0] };
    ring_write(c, src, n);
    atomic_fetch_add_explicit(&c->inCb, 1, memory_order_relaxed);
    return noErr;
}

// MARK: - Output thread

/// Write position extrapolated to "now", capped at one input chunk.
static double virtual_fill(PTCore *c, int64_t r) CA_REALTIME_API {
    double maxExtra = (double)atomic_load_explicit(&c->inChunk, memory_order_relaxed);
    for (int tries = 0; tries < 4; tries++) {
        uint32_t q0 = atomic_load_explicit(&c->wseq, memory_order_acquire);
        if (q0 & 1) continue;
        int64_t w = atomic_load_explicit(&c->wposStamp, memory_order_relaxed);
        uint64_t t = atomic_load_explicit(&c->wHostNs, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&c->wseq, memory_order_relaxed) != q0) continue;
        double extra = (double)(int64_t)(now_ns() - t) * c->framesPerNs;
        if (extra < 0) extra = 0;
        if (extra > maxExtra) extra = maxExtra;
        return (double)(w - r) + extra;
    }
    return (double)(atomic_load_explicit(&c->wpos, memory_order_acquire) - r);
}

static void zero_abl(AudioBufferList *io) CA_REALTIME_API {
    for (UInt32 i = 0; i < io->mNumberBuffers; i++) {
        memset(io->mBuffers[i].mData, 0, io->mBuffers[i].mDataByteSize);
    }
}

/// Called when audio flows again after the ring ran dry. If the input
/// reported a gap in the meantime, the device itself went quiet (a bigger
/// buffer would not have helped), so the latency is left alone. Otherwise our
/// margin was too small for the two IO threads' jitter: raise the target.
static void classify_underrun(PTCore *c) CA_REALTIME_API {
    c->underrunPending = 0;
    if (atomic_load_explicit(&c->inputGaps, memory_order_relaxed) != c->gapsAtUnderrun) return;
    atomic_fetch_add_explicit(&c->underruns, 1, memory_order_relaxed);
    c->extraTarget += TARGET_STEP_FRAMES;
    if (c->extraTarget > TARGET_MAX_EXTRA) c->extraTarget = TARGET_MAX_EXTRA;
    // Start a fresh decay window for the new target.
    c->quietSeconds = 0;
    c->minHeadroom = INFINITY;
}

// Varispeed input pull (output IO thread, from inside AudioUnitRender(vsUnit)).
static OSStatus vs_pull(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
                        UInt32 bus, UInt32 n, AudioBufferList *io) CA_REALTIME_API {
    (void)ts;
    (void)bus;
    PTCore *c = ref;
    int64_t r = atomic_load_explicit(&c->rpos, memory_order_relaxed);
    int64_t w = atomic_load_explicit(&c->wpos, memory_order_acquire);
    if (!c->running || w - r < (int64_t)n + FADE_RESERVE_FRAMES) {
        int64_t left = w - r;
        if (c->running) {
            c->running = 0;
            c->underrunPending = 1;
            c->gapsAtUnderrun = atomic_load_explicit(&c->inputGaps, memory_order_relaxed);
            atomic_fetch_add_explicit(&c->dropouts, 1, memory_order_relaxed);
            if (left > 0) {
                // Play out what is left with a fade instead of cutting it off.
                uint32_t a = left > (int64_t)n ? n : (uint32_t)left;
                for (UInt32 ch = 0; ch < io->mNumberBuffers && ch < PT_CH; ch++) {
                    float *dst = io->mBuffers[ch].mData;
                    for (uint32_t i = 0; i < a; i++) {
                        // Curve index for i / a, rounded up so it never goes below 0.
                        uint32_t k = (uint32_t)(((uint64_t)(i + 1) * FADE_FRAMES + a - 1) / a) - 1;
                        dst[i] = c->ring[ch][(r + i) & c->mask] * (1.0f - c->fadeIn[k]);
                    }
                    memset(dst + a, 0, (n - a) * sizeof(float));
                }
                // Leftovers beyond this slice are dropped: replayed after the gap
                // they would be spliced against the new audio.
                atomic_store_explicit(&c->rpos, r + left, memory_order_release);
                c->xfLeft = 0;
                return noErr;
            }
        }
        zero_abl(io);
        *flags |= kAudioUnitRenderAction_OutputIsSilence;
        return noErr;
    }
    uint32_t off = (uint32_t)(r & c->mask);
    uint32_t first = c->cap - off;
    if (first > n) first = n;
    for (UInt32 ch = 0; ch < io->mNumberBuffers && ch < PT_CH; ch++) {
        float *dst = io->mBuffers[ch].mData;
        memcpy(dst, c->ring[ch] + off, first * sizeof(float));
        memcpy(dst + first, c->ring[ch], (n - first) * sizeof(float));
    }
    for (uint32_t i = 0; i < n && c->xfLeft > 0; i++, c->xfLeft--) {
        // Backlog dropped: the old position fades out while the new one fades in.
        uint32_t k = FADE_FRAMES - c->xfLeft;
        for (UInt32 ch = 0; ch < io->mNumberBuffers && ch < PT_CH; ch++) {
            float *dst = io->mBuffers[ch].mData;
            dst[i] = dst[i] * c->xfadeNew[k] + c->xfOld[ch][k] * c->xfadeOld[k];
        }
    }
    for (uint32_t i = 0; i < n && c->fadeInPos < FADE_FRAMES; i++, c->fadeInPos++) {
        for (UInt32 ch = 0; ch < io->mNumberBuffers && ch < PT_CH; ch++) {
            ((float *)io->mBuffers[ch].mData)[i] *= c->fadeIn[c->fadeInPos];
        }
    }
    atomic_store_explicit(&c->rpos, r + n, memory_order_release);
    // Frames to spare before this pull would have been a dropout.
    double headroom = (double)(w - r - (int64_t)n - FADE_RESERVE_FRAMES);
    if (headroom < c->minHeadroom) c->minHeadroom = headroom;
    return noErr;
}

static double current_target(PTCore *c) CA_REALTIME_API {
    return (double)atomic_load_explicit(&c->inChunk, memory_order_relaxed) + c->outChunk
         + (double)atomic_load_explicit(&c->margin, memory_order_relaxed) + (double)c->extraTarget;
}

/// How far above the target the raw fill may go before the backlog is dropped.
/// A late input callback and its catch-up can add up to one input buffer. Never
/// so far that the ring overflows (and drops input) first.
static double resync_slack(PTCore *c, double target) CA_REALTIME_API {
    double chunk = (double)atomic_load_explicit(&c->inChunk, memory_order_relaxed);
    double slack = RESYNC_MIN_SLACK_FRAMES + chunk;
    double room = (double)c->cap - chunk - target - 1;
    if (slack > room) slack = room;
    return slack > 0 ? slack : 0;
}

// Output AUHAL render callback (output device IO thread).
static OSStatus out_cb(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
                       UInt32 bus, UInt32 n, AudioBufferList *io) CA_REALTIME_API {
    (void)bus;
    PTCore *c = ref;
    int64_t w = atomic_load_explicit(&c->wpos, memory_order_acquire);
    int64_t r = atomic_load_explicit(&c->rpos, memory_order_relaxed);
    double fill = (double)(w - r);
    double target = current_target(c);
    double dt = (double)n / c->outRate;

    // A jump in the output sample time means the HAL skipped output cycles
    // (an IO overload, or Core Audio restarting the device): the device played
    // silence, so this buffer fades in (through the volume ramp below).
    int skipped = 0;
    if (ts->mFlags & kAudioTimeStampSampleTimeValid) {
        if (c->nextOutSampleTime >= 0 && fabs(ts->mSampleTime - c->nextOutSampleTime) > (double)n / 2) {
            atomic_fetch_add_explicit(&c->outputSkips, 1, memory_order_relaxed);
            skipped = 1;
        }
        c->nextOutSampleTime = ts->mSampleTime + n;
    }

    if (!c->running) {
        // Resume on the same extrapolated fill the controller steers, so it
        // starts without an error to work off.
        double vfill = virtual_fill(c, r);
        if (vfill >= target && c->underrunPending) {
            // Decide before resuming so a raised target takes effect now.
            classify_underrun(c);
            target = current_target(c);
        }
        if (vfill >= target) {
            // Start exactly at the target (this also drops stale audio after a
            // long stall); the fade-in hides the frames skipped here.
            int64_t skip = (int64_t)(vfill - target);
            if (skip > 0) {
                atomic_store_explicit(&c->rpos, r + skip, memory_order_release);
                vfill = virtual_fill(c, r + skip);
            }
            c->running = 1;
            c->avgFill = vfill;
            c->fadeInPos = 0;
            c->xfLeft = 0;
        }
    } else if (fill > target + resync_slack(c, target)) {
        // Backlog after a stall, an overload or a burst: jump back to the target
        // instead of slowly draining it at +1000 ppm, crossfading so it does not
        // click. The integrator keeps its drift estimate. The audio being faded
        // out is copied first: once rpos moves, the writer may overwrite it.
        for (int ch = 0; ch < PT_CH; ch++) {
            for (uint32_t i = 0; i < FADE_FRAMES; i++) c->xfOld[ch][i] = c->ring[ch][(r + i) & c->mask];
        }
        c->xfLeft = FADE_FRAMES;
        // Land the extrapolated fill (what the controller steers) on the target,
        // as the resume path does, not the raw fill.
        int64_t nr = w - (int64_t)target;
        double over = virtual_fill(c, nr) - target;
        if (over > 0) nr += (int64_t)over;
        atomic_store_explicit(&c->rpos, nr, memory_order_release);
        c->avgFill = virtual_fill(c, nr);
        atomic_fetch_add_explicit(&c->resyncs, 1, memory_order_relaxed);
    }

    double a = dt / FILL_EMA_SECONDS;
    if (a > 1.0) a = 1.0;
    c->avgFill += (virtual_fill(c, atomic_load_explicit(&c->rpos, memory_order_relaxed)) - c->avgFill) * a;
    double e = c->avgFill - target;
    if (c->running) {
        // Conditional integration: no wind-up while the rate is pinned in the
        // direction of the error (draining a backlog), which would otherwise
        // overshoot the fill below the target and cause a second dropout later.
        double u = KP_PPM_PER_FRAME * e + KI_PPM_PER_FRAME_S * (c->integ + e * dt);
        if (fabs(u) < MAX_PPM || (u > 0) != (e > 0)) c->integ += e * dt;
        double limit = MAX_PPM / KI_PPM_PER_FRAME_S;
        if (c->integ > limit) c->integ = limit;
        if (c->integ < -limit) c->integ = -limit;

        // Give back some of the learned margin, but only if it was not needed:
        // otherwise this would step down until the next dropout, forever.
        c->quietSeconds += dt;
        if (c->quietSeconds >= TARGET_DECAY_SECONDS) {
            if (c->extraTarget > 0 && c->minHeadroom > TARGET_DECAY_FRAMES + DECAY_HEADROOM_FRAMES) {
                c->extraTarget = c->extraTarget > TARGET_DECAY_FRAMES ? c->extraTarget - TARGET_DECAY_FRAMES : 0;
            }
            c->quietSeconds = 0;
            c->minHeadroom = INFINITY;
        }
    }
    double ppm = KP_PPM_PER_FRAME * e + KI_PPM_PER_FRAME_S * c->integ;
    if (ppm > MAX_PPM) ppm = MAX_PPM;
    if (ppm < -MAX_PPM) ppm = -MAX_PPM;
    // Rate > 1 consumes input faster, draining the ring when the input clock runs fast.
    AudioUnitSetParameter(c->vsUnit, kVarispeedParam_PlaybackRate, kAudioUnitScope_Global, 0,
                          (AudioUnitParameterValue)(1.0 + ppm * 1e-6), 0);

    OSStatus err = AudioUnitRender(c->vsUnit, flags, ts, 0, n, io);
    if (err != noErr) {
        zero_abl(io);
        *flags |= kAudioUnitRenderAction_OutputIsSilence;
    }

    float volume = atomic_load_explicit(&c->volume, memory_order_relaxed);
    float from = skipped ? 0.0f : c->gain;
    if (volume != 1.0f || from != 1.0f) {
        // A volume change ramps across this buffer instead of stepping (a step clicks).
        for (UInt32 i = 0; i < io->mNumberBuffers; i++) {
            float *p = io->mBuffers[i].mData;
            UInt32 count = io->mBuffers[i].mDataByteSize / sizeof(float);
            float step = count > 0 ? (volume - from) / (float)count : 0;
            for (UInt32 k = 0; k < count; k++) p[k] *= from + step * (float)(k + 1);
        }
        c->gain = volume;
    }

    atomic_store_explicit(&c->pubFill, c->avgFill, memory_order_relaxed);
    atomic_store_explicit(&c->pubPPM, ppm, memory_order_relaxed);
    atomic_store_explicit(&c->pubTarget, (uint32_t)target, memory_order_relaxed);
    atomic_store_explicit(&c->pubRunning, (uint32_t)c->running, memory_order_relaxed);
    atomic_fetch_add_explicit(&c->outCb, 1, memory_order_relaxed);
    return noErr;
}

// MARK: - Control thread

static AudioStreamBasicDescription float_planar(double sampleRate, uint32_t channels) {
    AudioStreamBasicDescription d = {0};
    d.mSampleRate = sampleRate;
    d.mFormatID = kAudioFormatLinearPCM;
    d.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    d.mBytesPerPacket = 4;
    d.mFramesPerPacket = 1;
    d.mBytesPerFrame = 4;
    d.mChannelsPerFrame = channels;
    d.mBitsPerChannel = 32;
    return d;
}

static AudioUnit new_unit(OSType type, OSType subType) {
    AudioComponentDescription d = { type, subType, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent component = AudioComponentFindNext(NULL, &d);
    AudioUnit unit = NULL;
    if (component) AudioComponentInstanceNew(component, &unit);
    return unit;
}

static void dispose_unit(AudioUnit *unit) {
    if (*unit) {
        AudioOutputUnitStop(*unit);
        AudioUnitUninitialize(*unit);
        AudioComponentInstanceDispose(*unit);
        *unit = NULL;
    }
}

static double nominal_rate(AudioObjectID device) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    Float64 rate = 0;
    UInt32 size = sizeof(rate);
    AudioObjectGetPropertyData(device, &a, 0, NULL, &size, &rate);
    return rate;
}

PTCore *pt_create(uint32_t ringFrames, uint32_t maxIOFrames) {
    if (ringFrames == 0 || (ringFrames & (ringFrames - 1)) || maxIOFrames == 0) return NULL;
    PTCore *c = calloc(1, sizeof(PTCore));
    if (!c) return NULL;
    c->cap = ringFrames;
    c->mask = ringFrames - 1;
    c->maxIO = maxIOFrames;
    c->inABL = calloc(1, offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer) * PT_CH);
    bool ok = c->inABL != NULL;
    for (int ch = 0; ch < PT_CH; ch++) {
        c->ring[ch] = calloc(ringFrames, sizeof(float));
        c->inScratch[ch] = calloc(maxIOFrames, sizeof(float));
        ok = ok && c->ring[ch] && c->inScratch[ch];
    }
    if (!ok) {
        pt_destroy(c);
        return NULL;
    }
    for (int i = 0; i < FADE_FRAMES; i++) {
        double x = (double)(i + 1) / FADE_FRAMES;
        c->fadeIn[i] = (float)(0.5 - 0.5 * cos(M_PI * x));
        c->xfadeNew[i] = (float)sin(M_PI_2 * x);
        c->xfadeOld[i] = (float)cos(M_PI_2 * x);
    }
    c->gain = 1.0f;
    c->inRate = 48000;
    c->outRate = 48000;
    c->framesPerNs = 48000 / 1e9;
    atomic_init(&c->inChunk, 512);
    atomic_init(&c->margin, 128);
    atomic_init(&c->volume, 1.0f);
    return c;
}

void pt_destroy(PTCore *c) {
    pt_stop_input(c);
    pt_stop_output(c);
    for (int ch = 0; ch < PT_CH; ch++) {
        free(c->ring[ch]);
        free(c->inScratch[ch]);
    }
    free(c->inABL);
    free(c);
}

OSStatus pt_start_input(PTCore *c, AudioObjectID device, uint32_t ioFrames) {
    pt_stop_input(c);
    AudioUnit u = new_unit(kAudioUnitType_Output, kAudioUnitSubType_HALOutput);
    if (!u) return kAudioUnitErr_FailedInitialization;

    // TN2091: enable IO before choosing the device.
    UInt32 one = 1, zero = 0;
    OSStatus e = AudioUnitSetProperty(u, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &one, sizeof(one));
    if (!e) e = AudioUnitSetProperty(u, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &zero, sizeof(zero));
    if (!e) e = AudioUnitSetProperty(u, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device, sizeof(device));
    if (!e) AudioUnitSetProperty(u, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &ioFrames, sizeof(ioFrames));

    AudioStreamBasicDescription hw = {0};
    UInt32 size = sizeof(hw);
    if (!e) e = AudioUnitGetProperty(u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1, &hw, &size);
    if (!e) {
        c->inChannels = hw.mChannelsPerFrame >= 2 ? 2 : 1;
        // AUHAL cannot resample input: the client side must use the device rate.
        c->inRate = nominal_rate(device) > 0 ? nominal_rate(device) : hw.mSampleRate;
        c->inABL->mNumberBuffers = c->inChannels;
        for (uint32_t ch = 0; ch < c->inChannels; ch++) c->inABL->mBuffers[ch].mNumberChannels = 1;
        AudioStreamBasicDescription client = float_planar(c->inRate, c->inChannels);
        e = AudioUnitSetProperty(u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &client, sizeof(client));
    }
    UInt32 maxIO = c->maxIO;
    if (!e) e = AudioUnitSetProperty(u, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxIO, sizeof(maxIO));
    AURenderCallbackStruct callback = { in_cb, c };
    if (!e) e = AudioUnitSetProperty(u, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &callback, sizeof(callback));
    if (!e) e = AudioUnitInitialize(u);
    if (!e) {
        UInt32 actual = ioFrames;
        size = sizeof(actual);
        AudioUnitGetProperty(u, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &actual, &size);
        atomic_store_explicit(&c->inChunk, actual, memory_order_relaxed);
        c->nextSampleTime = -1;
    }
    c->inUnit = u;   // in_cb reads it: publish before starting
    if (!e) e = AudioOutputUnitStart(u);
    if (e) dispose_unit(&c->inUnit);
    return e;
}

void pt_stop_input(PTCore *c) {
    dispose_unit(&c->inUnit);
}

double pt_input_sample_rate(const PTCore *c) {
    return c->inRate;
}

double pt_output_sample_rate(const PTCore *c) {
    return c->outRate;
}

OSStatus pt_start_output(PTCore *c, AudioObjectID device, uint32_t ioFrames, double inputSampleRate) {
    pt_stop_output(c);
    double outRate = nominal_rate(device);
    if (outRate <= 0 || inputSampleRate <= 0) return kAudioUnitErr_InvalidPropertyValue;
    c->outRate = outRate;
    c->framesPerNs = inputSampleRate / 1e9;

    AudioUnit out = new_unit(kAudioUnitType_Output, kAudioUnitSubType_HALOutput);
    AudioUnit vs = new_unit(kAudioUnitType_FormatConverter, kAudioUnitSubType_Varispeed);
    if (!out || !vs) {
        if (out) AudioComponentInstanceDispose(out);
        if (vs) AudioComponentInstanceDispose(vs);
        return kAudioUnitErr_FailedInitialization;
    }
    OSStatus e = AudioUnitSetProperty(out, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device, sizeof(device));
    if (!e) AudioUnitSetProperty(out, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &ioFrames, sizeof(ioFrames));

    AudioStreamBasicDescription fin = float_planar(inputSampleRate, PT_CH);
    AudioStreamBasicDescription fout = float_planar(outRate, PT_CH);
    UInt32 maxIO = c->maxIO;
    // Measured: Medium = 0.33 ms of SRC latency (Max 1 ms), flat to 17 kHz and
    // no level modulation at any drift rate.
    UInt32 quality = kRenderQuality_Medium;
    if (!e) e = AudioUnitSetProperty(vs, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fin, sizeof(fin));
    if (!e) e = AudioUnitSetProperty(vs, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &fout, sizeof(fout));
    if (!e) e = AudioUnitSetProperty(vs, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxIO, sizeof(maxIO));
    if (!e) AudioUnitSetProperty(vs, kAudioUnitProperty_RenderQuality, kAudioUnitScope_Global, 0, &quality, sizeof(quality));
    AURenderCallbackStruct pull = { vs_pull, c };
    if (!e) e = AudioUnitSetProperty(vs, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &pull, sizeof(pull));
    if (!e) e = AudioUnitSetProperty(out, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fout, sizeof(fout));
    if (!e) e = AudioUnitSetProperty(out, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxIO, sizeof(maxIO));
    AURenderCallbackStruct render = { out_cb, c };
    if (!e) e = AudioUnitSetProperty(out, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &render, sizeof(render));
    if (!e) e = AudioUnitInitialize(vs);
    if (!e) e = AudioUnitInitialize(out);
    UInt32 outFrames = ioFrames;
    UInt32 size = sizeof(outFrames);
    if (!e) AudioUnitGetProperty(out, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &outFrames, &size);
    c->outChunk = ceil((double)outFrames * inputSampleRate / outRate);

    // The consumer is stopped, so its state and rpos can be reset safely.
    // Skipping to the newest audio keeps an output switch from adding latency.
    atomic_store_explicit(&c->rpos, atomic_load_explicit(&c->wpos, memory_order_acquire), memory_order_release);
    // A margin learned on another device or under a load burst does not carry over.
    c->running = 0;
    c->underrunPending = 0;
    c->fadeInPos = 0;
    c->xfLeft = 0;
    c->nextOutSampleTime = -1;
    c->extraTarget = 0;
    c->avgFill = 0;
    c->integ = 0;
    c->quietSeconds = 0;
    c->minHeadroom = INFINITY;
    c->vsUnit = vs;
    c->outUnit = out;
    if (!e) e = AudioOutputUnitStart(out);
    if (e) pt_stop_output(c);
    return e;
}

void pt_stop_output(PTCore *c) {
    dispose_unit(&c->outUnit);
    if (c->vsUnit) {
        AudioUnitUninitialize(c->vsUnit);
        AudioComponentInstanceDispose(c->vsUnit);
        c->vsUnit = NULL;
    }
}

void pt_set_margin_frames(PTCore *c, uint32_t frames) {
    atomic_store_explicit(&c->margin, frames, memory_order_relaxed);
}

void pt_set_volume(PTCore *c, float volume) {
    atomic_store_explicit(&c->volume, volume, memory_order_relaxed);
}

void pt_get_stats(PTCore *c, PTStats *o) {
    o->fillFrames = atomic_load_explicit(&c->pubFill, memory_order_relaxed);
    o->ratePPM = atomic_load_explicit(&c->pubPPM, memory_order_relaxed);
    o->targetFrames = atomic_load_explicit(&c->pubTarget, memory_order_relaxed);
    o->running = atomic_load_explicit(&c->pubRunning, memory_order_relaxed);
    o->underruns = atomic_load_explicit(&c->underruns, memory_order_relaxed);
    o->inputGaps = atomic_load_explicit(&c->inputGaps, memory_order_relaxed);
    o->overruns = atomic_load_explicit(&c->overruns, memory_order_relaxed);
    o->resyncs = atomic_load_explicit(&c->resyncs, memory_order_relaxed);
    o->inputCallbacks = atomic_load_explicit(&c->inCb, memory_order_relaxed);
    o->outputCallbacks = atomic_load_explicit(&c->outCb, memory_order_relaxed);
    o->dropouts = atomic_load_explicit(&c->dropouts, memory_order_relaxed);
    o->inputErrors = atomic_load_explicit(&c->inputErrors, memory_order_relaxed);
    o->outputSkips = atomic_load_explicit(&c->outputSkips, memory_order_relaxed);
}
