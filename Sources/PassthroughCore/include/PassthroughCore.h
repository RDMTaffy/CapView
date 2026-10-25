#pragma once
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <stdbool.h>
#include <stdint.h>

/// Real-time audio passthrough:
///
///     capture device ─► input AUHAL ─► SPSC ring ─► Varispeed (SRC + drift) ─► output AUHAL
///
/// Everything that runs on a Core Audio IO thread lives in C so the compiler
/// can prove it never allocates or locks (-Werror=function-effects).
typedef struct PTCore PTCore;

typedef struct {
    /// Smoothed frames buffered between the two devices (input-rate frames).
    double fillFrames;
    /// Current Varispeed correction in parts per million.
    double ratePPM;
    /// Fill level the controller steers toward (IO buffers + margin + adaptive extra).
    uint32_t targetFrames;
    /// 0 while priming (silence), 1 while audio flows.
    uint32_t running;
    /// The output ran dry while input was flowing (jitter); raises the target.
    uint64_t underruns;
    /// The capture device stopped delivering audio (USB stall, unplug, ...).
    uint64_t inputGaps;
    uint64_t overruns;
    uint64_t resyncs;
    uint64_t inputCallbacks;
    uint64_t outputCallbacks;
} PTStats;

/// `ringFrames` must be a power of two. `maxIOFrames` bounds every render slice.
PTCore *_Nullable pt_create(uint32_t ringFrames, uint32_t maxIOFrames);
void pt_destroy(PTCore *_Nonnull core);

// Control-thread API. Never call from an IO thread.

/// Starts capturing `device` with the given IO buffer size. Uses the device's
/// nominal sample rate, which `pt_input_sample_rate` reports afterwards.
OSStatus pt_start_input(PTCore *_Nonnull core, AudioObjectID device, uint32_t ioFrames);
void pt_stop_input(PTCore *_Nonnull core);
double pt_input_sample_rate(const PTCore *_Nonnull core);

/// Starts playing to `device`, converting from `inputSampleRate` to the
/// device's own rate. Resets the ring so stale audio is never played.
OSStatus pt_start_output(PTCore *_Nonnull core, AudioObjectID device, uint32_t ioFrames, double inputSampleRate);
void pt_stop_output(PTCore *_Nonnull core);

// Thread-safe.

/// Safety margin in input frames. The fill target is one input IO buffer plus
/// one output IO buffer plus this margin; jitter underruns add to it and quiet
/// time with spare headroom removes it again.
void pt_set_margin_frames(PTCore *_Nonnull core, uint32_t frames);
void pt_set_volume(PTCore *_Nonnull core, float volume);
void pt_get_stats(PTCore *_Nonnull core, PTStats *_Nonnull stats);
