// SDL3 audio transport for MusyX, adapted from Dusklight's DuskAudioSystem (CC0-1.0,
// https://github.com/TwilitRealm/dusklight).

#include "port/audio.h"

#if defined(PORT_USE_AURORA)

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

// port_monotonic_ns, for the hand-over timestamps the stream's drain rate is measured against.
#include "port/host.h"

// salPortNextBuffer() returns the ring slot the DAC would be playing and advances MusyX by one 5 ms
// tick.
extern "C" {
void* salPortNextBuffer(void);
unsigned int salPortBufferBytes(void);
}

namespace {

// 0x280-byte buffers: 160 stereo s16 frames, 5 ms at 32 kHz, the rate salInitAi picks.
constexpr int kSampleRate = 32000;
constexpr int kChannels = 2;

// How far ahead to keep the device fed: 30 ms covers a dropped frame at 60 Hz.
constexpr int kTargetBuffers = 6;

// Ceiling on catch-up, so a long stall does not run the sequencer forward at speed; past this the
// gap is lost.
constexpr int kMaxBuffersPerUpdate = 24;

SDL_AudioStream* s_stream = nullptr;
// Bytes the device pulls at once, queued in addition to kTargetBuffers.
int s_pullBytes = 0;
bool s_ownsSubsystem = false;
bool s_failed = false;
int s_logging = -1;

unsigned long s_buffers = 0;      // ticks handed to the device
unsigned long s_underruns = 0;    // updates that found the queue already empty
bool s_everNonSilent = false;
bool s_reported = false;

bool logging() {
    if (s_logging < 0) {
        const char* e = getenv("STRIKERS_LOG_AUDIO");
        s_logging = (e != nullptr && *e != '\0') ? 1 : 0;
    }
    return s_logging != 0;
}

static unsigned long s_updateCalls;
static double s_updateTotalMs;
static double s_updateMaxMs;
static unsigned long s_updateOver2ms;

// The transport's own delay, which is the half of the operator's "the sounds seem disconnected from
// the models speaking them" that a mixer peak can never answer: how much audio the game has already
// produced but the device has not played. Measured as the queue's own length in milliseconds at the
// moment the newest buffer is handed over, so it is the delay that buffer will actually serve.
// Everything is at the stream's rate; the device resamples, which changes how the bytes are spelled
// and not how long they last.
double s_leadMs = 0.0;
double s_leadMinMs = 0.0;
double s_leadMaxMs = 0.0;
double s_leadSumMs = 0.0;
unsigned long s_handovers = 0;         // game frames that queued at least one buffer
double s_deviceMs = 0.0;               // what the device holds in front of the stream
unsigned long long s_firstPushNs = 0;
unsigned long long s_lastPushNs = 0;
unsigned long long s_bytesPushed = 0;

// The stream's own byte rate: 32000 frames a second of interleaved s16. The drain check divides by
// this, and a measured rate far from it is how "the queue is a length in time" can fail.
unsigned int streamByteRate(void) {
    return (unsigned int)(kSampleRate * kChannels * (int)sizeof(int16_t));
}

void report() {
    if (logging() && s_updateCalls != 0)
        std::fprintf(stderr,
                     "[port] audio: update cost over %lu frames: mean %.3f ms, max %.2f ms, "
                     "%lu frames over 2 ms\n",
                     s_updateCalls, s_updateTotalMs / (double)s_updateCalls, s_updateMaxMs,
                     s_updateOver2ms);
    if (s_reported || !logging() || s_buffers == 0)
        return;
    s_reported = true;
    const unsigned int frames = salPortBufferBytes() / (kChannels * sizeof(int16_t));
    std::fprintf(stderr,
                 "[port] audio: %lu ticks (%.1f s of output), %lu underruns, %s\n",
                 s_buffers, (double)s_buffers * (double)frames / (double)kSampleRate,
                 s_underruns,
                 s_everNonSilent ? "output was non-silent" : "output was silent throughout");
}

} // namespace

int PortAudioStart(void) {
    if (s_stream != nullptr)
        return 1;
    if (s_failed)
        return 0;

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "[port] audio: SDL_InitSubSystem failed: %s\n", SDL_GetError());
        s_failed = true;
        return 0;
    }
    s_ownsSubsystem = true;

    SDL_AudioSpec spec;
    std::memset(&spec, 0, sizeof(spec));
    // Host-endian s16: nothing on this path carries console byte order.
    spec.format = SDL_AUDIO_S16;
    spec.channels = kChannels;
    spec.freq = kSampleRate;

    s_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (s_stream == nullptr) {
        std::fprintf(stderr, "[port] audio: no output device (%s); running silent\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        s_ownsSubsystem = false;
        s_failed = true;
        return 0;
    }

#if defined(__SWITCH__)
    // Switch's SDL2 takes a whole device buffer per pull and pads any shortfall with silence.
    {
        SDL_AudioSpec dev;
        int frames = 0;
        std::memset(&dev, 0, sizeof(dev));
        if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(s_stream), &dev, &frames) && frames > 0
            && dev.freq > 0)
            s_pullBytes = static_cast<int>(static_cast<long long>(frames) * kSampleRate / dev.freq)
                          * kChannels * static_cast<int>(sizeof(int16_t));
    }
#endif

    SDL_ResumeAudioStreamDevice(s_stream);
    // atexit, not PortAudioStop: salExitAi is reached only if the game shuts MusyX down, and it
    // does not.
    std::atexit(report);

    // What the device holds in front of the stream, taken whether or not anything is being logged:
    // the onset reading is the stream's queue plus this, and a row that published the queue alone
    // would understate the delay the speaker is behind the frame by.
    {
        SDL_AudioSpec deviceSpec;
        int deviceFrames = 0;
        std::memset(&deviceSpec, 0, sizeof(deviceSpec));
        if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(s_stream), &deviceSpec, &deviceFrames)
            && deviceSpec.freq > 0)
            s_deviceMs = (double)deviceFrames * 1000.0 / (double)deviceSpec.freq;
    }

    if (logging()) {
        SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(s_stream);
        SDL_AudioSpec got;
        int frames = 0;
        std::memset(&got, 0, sizeof(got));
        if (SDL_GetAudioDeviceFormat(dev, &got, &frames)) {
            std::fprintf(stderr,
                         "[port] audio: device \"%s\" %d Hz %d ch fmt 0x%x, %d-frame buffer; "
                         "feeding %d Hz s16 stereo in %u-byte ticks, kept %d deep\n",
                         SDL_GetAudioDeviceName(dev), got.freq, got.channels,
                         (unsigned)got.format, frames, kSampleRate, salPortBufferBytes(),
                         kTargetBuffers);
        }
    }
    return 1;
}

int PortAudioDeviceOpen(void) { return s_stream != nullptr ? 1 : 0; }

void PortAudioUpdateCost(unsigned long* calls, double* meanMs, double* maxMs,
                         unsigned long* over2ms) {
    if (calls) *calls = s_updateCalls;
    if (meanMs) *meanMs = s_updateCalls ? s_updateTotalMs / (double)s_updateCalls : 0.0;
    if (maxMs) *maxMs = s_updateMaxMs;
    if (over2ms) *over2ms = s_updateOver2ms;
}

void PortAudioStop(void) {
    if (s_stream != nullptr) {
        report();
        SDL_DestroyAudioStream(s_stream);
        s_stream = nullptr;
    }
    if (s_ownsSubsystem) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        s_ownsSubsystem = false;
    }
}

void PortAudioUpdate(void) {
    if (s_stream == nullptr)
        return;
    struct Timer {
        Uint64 t0;
        unsigned long buffersBefore;
        ~Timer() {
            const double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0
                              / (double)SDL_GetPerformanceFrequency();
            ++s_updateCalls;
            s_updateTotalMs += ms;
            if (ms > s_updateMaxMs) s_updateMaxMs = ms;
            if (ms > 2.0) {
                ++s_updateOver2ms;
                if (logging() && s_updateOver2ms <= 40)
                    std::fprintf(stderr,
                                 "[port] audio: slow update %.1f ms for %lu ticks at tick %lu\n",
                                 ms, s_buffers - buffersBefore, s_buffers);
            }
        }
    } timer{SDL_GetPerformanceCounter(), s_buffers};

    const unsigned int bufBytes = salPortBufferBytes();
    if (bufBytes == 0)
        return;

    const int queued = SDL_GetAudioStreamQueued(s_stream);
    if (queued < 0)
        return;

    if (queued == 0 && s_buffers != 0)
        ++s_underruns;

    const int target = static_cast<int>(bufBytes) * kTargetBuffers + s_pullBytes;
    int want = (target - queued + static_cast<int>(bufBytes) - 1) / static_cast<int>(bufBytes);
    if (want <= 0)
        return;
    if (want > kMaxBuffersPerUpdate)
        want = kMaxBuffersPerUpdate;

    const unsigned long long pushBeginNs = port_monotonic_ns();
    unsigned long pushed = 0;

    for (int i = 0; i < want; ++i) {
        void* pcm = salPortNextBuffer();
        if (pcm == nullptr)
            break;

        if (!s_everNonSilent) {
            const int16_t* p = static_cast<const int16_t*>(pcm);
            const unsigned int n = bufBytes / sizeof(int16_t);
            for (unsigned int j = 0; j < n; ++j) {
                if (p[j] != 0) {
                    s_everNonSilent = true;
                    if (logging())
                        std::fprintf(stderr, "[port] audio: first non-silent buffer at tick %lu\n",
                                     s_buffers);
                    break;
                }
            }
        }

        // What the device is given, which while a movie is playing is not what the mixer rendered.
        PortAudioDumpWrite(pcm, bufBytes / (kChannels * sizeof(int16_t)));

        if (!SDL_PutAudioStreamData(s_stream, pcm, static_cast<int>(bufBytes)))
            break;
        ++s_buffers;
        ++pushed;
        s_bytesPushed += bufBytes;
    }

    // The reading itself: what is queued once this frame's audio is on the end of it is the delay
    // the newest of it will serve, which is what an onset offset is. Sampled only for frames that
    // actually queued something, so a frame that wanted nothing neither moves the minimum towards a
    // queue that is short because nothing is being fed.
    if (pushed != 0) {
        const int after = SDL_GetAudioStreamQueued(s_stream);
        if (after >= 0) {
            s_lastPushNs = port_monotonic_ns();
            if (s_firstPushNs == 0)
                s_firstPushNs = pushBeginNs;
            s_leadMs = (double)after * 1000.0 / (double)streamByteRate();
            if (s_handovers == 0 || s_leadMs < s_leadMinMs)
                s_leadMinMs = s_leadMs;
            if (s_handovers == 0 || s_leadMs > s_leadMaxMs)
                s_leadMaxMs = s_leadMs;
            s_leadSumMs += s_leadMs;
            ++s_handovers;
        }
    }
}

int PortAudioLatencyStats(PortAudioLatencyInfo* out) {
    if (out == nullptr)
        return 0;
    *out = PortAudioLatencyInfo();
    if (s_stream == nullptr)
        return 0;
    out->leadMs = s_leadMs;
    out->leadMinMs = s_leadMinMs;
    out->leadMaxMs = s_leadMaxMs;
    out->leadMeanMs = s_handovers != 0 ? s_leadSumMs / (double)s_handovers : 0.0;
    out->handovers = s_handovers;
    out->deviceMs = s_deviceMs;
    out->streamByteRate = streamByteRate();
    const unsigned long long span =
        s_lastPushNs > s_firstPushNs ? s_lastPushNs - s_firstPushNs : 0;
    out->drainBytesPerSec = span != 0 ? (double)s_bytesPushed * 1e9 / (double)span : 0.0;
    return 1;
}

void PortAudioStats(unsigned long* outBuffers, unsigned long* outUnderruns, int* outEverNonSilent) {
    if (outBuffers != nullptr)
        *outBuffers = s_buffers;
    if (outUnderruns != nullptr)
        *outUnderruns = s_underruns;
    if (outEverNonSilent != nullptr)
        *outEverNonSilent = s_everNonSilent ? 1 : 0;
}

#else // !PORT_USE_AURORA

int PortAudioStart(void) { return 0; }
void PortAudioStop(void) {}
void PortAudioUpdate(void) {}
void PortAudioStats(unsigned long* outBuffers, unsigned long* outUnderruns, int* outEverNonSilent) {
    if (outBuffers != nullptr)
        *outBuffers = 0;
    if (outUnderruns != nullptr)
        *outUnderruns = 0;
    if (outEverNonSilent != nullptr)
        *outEverNonSilent = 0;
}
int PortAudioLatencyStats(PortAudioLatencyInfo* out) {
    if (out != nullptr)
        *out = PortAudioLatencyInfo();
    return 0;
}

#endif
