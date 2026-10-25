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
#define RESYNC_SLACK_FRAMES 2400   // this far above target => drop the backlog
#define TARGET_STEP_FRAMES 64      // added after each jitter underrun
#define TARGET_MAX_EXTRA 2048
#define TARGET_DECAY_SECONDS 60.0  // quiet time before giving some of it back
#define TARGET_DECAY_FRAMES 16
#define DECAY_HEADROOM_FRAMES 48   // spare frames that must remain after a decay step

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

    _Atomic uint32_t margin;
    _Atomic float volume;
    _Atomic uint64_t underruns, inputGaps, overruns, resyncs, inCb, outCb;
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
    if (err != noErr) return err;

    // A jump in device sample time means the device delivered nothing for a
    // while (e.g. the USB audio stream was restarted after a transfer error).
    if (ts->mFlags & kAudioTimeStampSampleTimeValid) {
        if (c->nextSampleTime >= 0 && fabs(ts->mSampleTime - c->nextSampleTime) > (double)n) {
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
    if (!c->running || w - r < (int64_t)n) {
        if (c->running) {
            c->running = 0;
            c->underrunPending = 1;
            c->gapsAtUnderrun = atomic_load_explicit(&c->inputGaps, memory_order_relaxed);
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
    atomic_store_explicit(&c->rpos, r + n, memory_order_release);
    double headroom = (double)(w - r - (int64_t)n);
    if (headroom < c->minHeadroom) c->minHeadroom = headroom;
    return noErr;
}

static double current_target(PTCore *c) CA_REALTIME_API {
    return (double)atomic_load_explicit(&c->inChunk, memory_order_relaxed) + c->outChunk
         + (double)atomic_load_explicit(&c->margin, memory_order_relaxed) + (double)c->extraTarget;
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

    if (!c->running) {
        if (fill >= target && c->underrunPending) {
            // Decide before resuming so a raised target takes effect now.
            classify_underrun(c);
            target = current_target(c);
        }
        if (fill >= target) {
            c->running = 1;
            c->avgFill = fill;
        }
    } else if (fill > target + RESYNC_SLACK_FRAMES) {
        // Backlog after a stall or wake: jump back to the target instead of
        // slowly draining it at +1000 ppm.
        atomic_store_explicit(&c->rpos, w - (int64_t)target, memory_order_release);
        c->avgFill = target;
        c->integ = 0;
        atomic_fetch_add_explicit(&c->resyncs, 1, memory_order_relaxed);
    }

    double a = dt / FILL_EMA_SECONDS;
    if (a > 1.0) a = 1.0;
    c->avgFill += (virtual_fill(c, atomic_load_explicit(&c->rpos, memory_order_relaxed)) - c->avgFill) * a;
    double e = c->avgFill - target;
    if (c->running) {
        c->integ += e * dt;
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
    if (volume != 1.0f) {
        for (UInt32 i = 0; i < io->mNumberBuffers; i++) {
            float *p = io->mBuffers[i].mData;
            UInt32 count = io->mBuffers[i].mDataByteSize / sizeof(float);
            for (UInt32 k = 0; k < count; k++) p[k] *= volume;
        }
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
    UInt32 quality = kRenderQuality_Medium;   // 0.33 ms of SRC latency (Max adds 1 ms)
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
}
