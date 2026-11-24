// ALSA sequencer output for Kwatuor. The MPC discards a plugin's VST MIDI output, so the plugin
// opens its own ALSA client with a readable "MIDI Out" port; MPC OS lists it as a MIDI input device.
// libasound is loaded with dlopen, so the SDK's stock build needs no ALSA headers or libraries.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>

#if defined(__linux__)
#include <dlfcn.h>
#endif

namespace kwatuor {

// ---------------------------------------------------------------------------
// Minimal ALSA sequencer binding, resolved at run time. The event struct is the
// kernel/alsa-lib ABI (28 bytes, stable since ALSA 0.9).
// ---------------------------------------------------------------------------
struct AlsaEvent
{
    uint8_t  type, flags, tag, queue;
    uint32_t time[2];
    uint8_t  srcClient, srcPort;
    uint8_t  dstClient, dstPort;
    union
    {
        struct { uint8_t channel, note, velocity, offVelocity; uint32_t duration; } note;
        struct { uint8_t channel, unused[3]; uint32_t param; int32_t value; } control;
        uint32_t raw[3];
    } data;
};
static_assert(sizeof(AlsaEvent) == 28, "snd_seq_event_t layout");

constexpr uint8_t kEvNoteOn = 6, kEvNoteOff = 7, kEvController = 10;
constexpr uint8_t kQueueDirect = 253, kAddrSubscribers = 254, kAddrUnknown = 253;

enum AlsaStatus { kNoLib = 0, kOpenFailed, kPortFailed, kPortOpen };

inline std::atomic<int> g_instances{0};

class AlsaOut
{
public:
    AlsaOut() = default;
    AlsaOut(const AlsaOut&) = delete;
    AlsaOut& operator=(const AlsaOut&) = delete;

    void open(const char* baseName)
    {
#if defined(__linux__)
        lib_ = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
        if (!lib_) { status_ = kNoLib; return; }
        open_   = (OpenFn)   dlsym(lib_, "snd_seq_open");
        close_  = (CloseFn)  dlsym(lib_, "snd_seq_close");
        name_   = (NameFn)   dlsym(lib_, "snd_seq_set_client_name");
        pool_   = (PoolFn)   dlsym(lib_, "snd_seq_set_client_pool_output");
        port_   = (PortFn)   dlsym(lib_, "snd_seq_create_simple_port");
        output_ = (EventFn)  dlsym(lib_, "snd_seq_event_output");
        direct_ = (EventFn)  dlsym(lib_, "snd_seq_event_output_direct");
        drain_  = (DrainFn)  dlsym(lib_, "snd_seq_drain_output");
        if (!open_ || !close_ || !name_ || !port_ || !output_ || !direct_ || !drain_) { status_ = kNoLib; return; }

        // 1 = SND_SEQ_OPEN_OUTPUT, 1 = SND_SEQ_NONBLOCK: a full buffer drops events instead of blocking audio.
        if (open_(&seq_, "default", 1, 1) < 0 || !seq_) { seq_ = nullptr; status_ = kOpenFailed; return; }
        if (pool_) pool_(seq_, 512);

        const int n = g_instances.fetch_add(1);
        counted_ = true;
        char name[48];
        if (n == 0) std::snprintf(name, sizeof name, "%s", baseName);
        else        std::snprintf(name, sizeof name, "%s %d", baseName, n + 1);
        name_(seq_, name);

        // caps: READ (1<<0) | SUBS_READ (1<<5); type: MIDI_GENERIC (1<<1) | APPLICATION (1<<20)
        portId_ = port_(seq_, "MIDI Out", (1u << 0) | (1u << 5), (1u << 1) | (1u << 20));
        if (portId_ < 0) { close_(seq_); seq_ = nullptr; status_ = kPortFailed; return; }
        status_ = kPortOpen;
#else
        (void)baseName;
        status_ = kNoLib;
#endif
    }

    void close()
    {
        if (seq_)
        {
            for (int ch = 0; ch < 16; ++ch)   // all notes off, so nothing hangs on the receiving tracks
            {
                AlsaEvent ev = make(kEvController);
                ev.data.control.channel = (uint8_t)ch;
                ev.data.control.param = 123;
                direct_(seq_, &ev);
            }
            close_(seq_);
            seq_ = nullptr;
        }
        if (counted_) { g_instances.fetch_sub(1); counted_ = false; }
        status_ = kNoLib;
    }

    ~AlsaOut() { close(); }

    int status() const { return status_; }
    // True when the port should be there and is not. A build for another system has no ALSA at all: not a failure.
    bool failed() const
    {
#if defined(__linux__)
        return status_ != kPortOpen;
#else
        return false;
#endif
    }

    // Audio thread. Buffered and non-blocking; call flush() once at the end of the block.
    void note(bool on, int channel, int note, int velocity)
    {
#ifdef KWATUOR_TRACE
        std::fprintf(stderr, "TRACE %s ch%d note%d vel%d\n", on ? "on" : "off", channel + 1, note, velocity);
#endif
        if (!seq_) return;
        AlsaEvent ev = make(on ? kEvNoteOn : kEvNoteOff);
        ev.data.note.channel = (uint8_t)channel;
        ev.data.note.note = (uint8_t)note;
        ev.data.note.velocity = (uint8_t)(on ? velocity : 0);
        output_(seq_, &ev);   // non-blocking: a full buffer drops the event rather than hold up the audio thread
        pending_ = true;
    }
    void flush()
    {
        if (seq_ && pending_) drain_(seq_);
        pending_ = false;
    }

private:
    AlsaEvent make(uint8_t type) const
    {
        AlsaEvent ev{};
        ev.type = type;
        ev.queue = kQueueDirect;
        ev.srcPort = (uint8_t)portId_;
        ev.dstClient = kAddrSubscribers;
        ev.dstPort = kAddrUnknown;
        return ev;
    }

    typedef int (*OpenFn)(void**, const char*, int, int);
    typedef int (*CloseFn)(void*);
    typedef int (*NameFn)(void*, const char*);
    typedef int (*PoolFn)(void*, size_t);
    typedef int (*PortFn)(void*, const char*, unsigned, unsigned);
    typedef int (*EventFn)(void*, AlsaEvent*);
    typedef int (*DrainFn)(void*);

    void*   lib_ = nullptr;
    void*   seq_ = nullptr;
    OpenFn  open_ = nullptr;
    CloseFn close_ = nullptr;
    NameFn  name_ = nullptr;
    PoolFn  pool_ = nullptr;
    PortFn  port_ = nullptr;
    EventFn output_ = nullptr;
    EventFn direct_ = nullptr;
    DrainFn drain_ = nullptr;
    int     portId_ = -1;
    int     status_ = kNoLib;
    bool    pending_ = false;
    bool    counted_ = false;
};

} // namespace kwatuor
