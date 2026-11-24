// Kwatuor - an MPC instrument plugin by Kelta
//
// A four-track generative MIDI sequencer. It makes no sound: each track sends notes on its own
// MIDI channel through the plugin's ALSA port ("Kwatuor"), and other MPC tracks set their MIDI
// input to that port. The tracks are fixed: Drums, Bass, Chords, Melody (channels 2 to 5). Press a track's
// quadrant to re-roll that track, or the centre to re-roll all four. Variations (kAmount) says how different
// the new idea is; Style says what kind of music it is.
//
// Signal flow:
//   one mosaic of seeds per track + a shared harmony seed + the knobs
//     -> engine.h generateIdea()        four patterns of 1/16 steps (regenerated whenever a knob moves)
//   host transport (tempo, song position, playing)
//     -> step clock with swing          which step of each pattern starts in this audio block
//         -> alsa_out.h                 note-on / note-off on the track's MIDI channel
//
// A pattern is a pure function of its seeds and the settings, so a project only stores the seeds.
// A new idea requested while the transport runs is swapped in on the next bar line; the button
// stays lit until then.
//
// Variations: four keys. The first two keep the idea and change about one step in four, or half of them. The last
// two replace it: new patterns; new patterns and new shared chords (centre button only).
//
// Undo: one level. A copy of the state (what a Fav holds) is taken before a re-roll lands, before a randomise key
// and before a Fav is recalled. The Undo key swaps the current state with that copy, on the bar line like a recall,
// and then reads Redo until a setting is changed by hand.
//
// Fav: the star key keeps the current state (the idea and the settings that shape it) in one of ten slots.
// A tile on the FAV page recalls it on the next bar line; with Edit lit the tiles show a cross that removes one
// (and a tile no longer recalls). The star is blue while the current state is a Fav and grey when all ten slots are full. Under each tile is its
// name: two random music words ("Deep Riff"), given when it is kept, so that it can be told from the others.
//
// Kit: each of the eight drum sounds goes out on its own note, set on the DRUMS page. A cell there selects
// a sound and plays it; + and - move it by a note; three keys fill all eight (in a row from 36, the MPC's default
// pad notes, General MIDI).
//
// Kwatuor is 4/4 only: a bar is 16 steps whatever the host says. In another time signature it keeps playing its
// own 4/4, and the position display shows the host's signature with a "!" instead.
//
// On bar: the five re-roll buttons always land on the next bar line. With On bar lit, so does everything
// else that changes what is played (the selectors, the Play and Setup knobs, randomise, Key in, recalling a Fav);
// with it out, those act at once. Swing, Velocity, Chan, Send and the Kit notes always act at once.
//
// Key in: while it is on, a note played into this track sets Key to that note's name, whatever its octave
// and channel. The MPC XL feeds our own port back into this track, and a plugin is not told which port a note came
// from, so a note that matches one we sent a moment ago (same channel, same note) is taken for our own echo and
// skipped. Other incoming MIDI is ignored.
// The ALSA calls in process() are non-blocking; measured on an MPC XL at about 20 microseconds.

#include "mpc_plugin.h"

#include "alsa_out.h"
#include "engine.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace {

using namespace kwatuor;

constexpr int kMinDrumPads = 4;   // kick, snare and the two hats always play; Pads adds percussion up to kDrumPads
constexpr int kFavSlots = 10;   // kept states

// The skin generator finds parameters by name. Projects depend on the order: new parameters go at the end.
enum ParamIndex
{
    kLevel = 0,     // % applied to every velocity. Parameter 0 is nudged by the host on load: harmless here.
    kGenAll,        // push button: re-roll all four tracks
    kAmount,        // choice I..IV: how different a re-roll is (two variations, two kinds of new idea)
    kKey,           // choice: C..B
    kScale,         // choice: 20 scales
    kStyle,         // choice: 10 genres
    kLength,        // choice: 1, 2, 4, 8 bars
    kSwing,         // 50..75 %, added to the style's own swing
    kGen,                               // x4 push button: re-roll this track
    kTrackSend = kGen + kTracks,        // x4 bool "On": off = this track sends nothing
    kChannel = kTrackSend + kTracks,    // x4 choice: 1..16 (index 0 = channel 1)
    kDensity = kChannel + kTracks,      // x4 %
    kComplexity = kDensity + kTracks,   // x4 %
    kGate = kComplexity + kTracks,      // x4 %: note length as a share of the gap to the next note (unused by Drums)
    kLong = kGate + kTracks,            // x4 %: chance a note is a long one (unused by Drums)
    kVoices = kLong + kTracks,          // x4 int: drum sounds in play 4..8 (Drums), stacked notes 1..4 (the others)
    kOctave = kVoices + kTracks,        // x4 int -3..3 (unused by Drums)
    kType = kOctave + kTracks,          // x4 choice: Loop, Drone (unused by Drums)
    kStatus = kType + kTracks,          // display, the MIDI lens: 0 the ALSA port did not open, 1 sending, 2 switched off (kSend)
    kPos,                               // display: host song position in 1/16 steps, shown as bar.beat.step
    kClock,                             // display: the bar clock. 0 stopped; 1..16 step in the bar; 17..32 the same while a new idea waits; 33..48 count-in
    kArt,                               // x4 display: which pattern the track's pad shows
    kWait = kArt + kTracks,             // x4 display: 1 while the track's new idea waits for the bar line
    // Stepper keys: push buttons that move another parameter by one. The worker thread handles them.
    kChanUp = kWait + kTracks,          // x4
    kChanDown = kChanUp + kTracks,      // x4
    kVoicesUp = kChanDown + kTracks,    // x4
    kVoicesDown = kVoicesUp + kTracks,  // x4
    kOctUp = kVoicesDown + kTracks,     // x4 (unused by Drums)
    kOctDown = kOctUp + kTracks,        // x4 (unused by Drums)
    // All push buttons below are handled by the worker thread.
    kRand = kOctDown + kTracks,         // x4 push button: new random Play settings for this track (no key on screen since 0.7.0)
    kFavAdd = kRand + kTracks,          // push button: keep the current state in Fav
    kFavRecall,                        // x10 push button: recall this slot; stays lit until it lands on the bar line
    kFavDel = kFavRecall + kFavSlots, // x10 push button: remove this Fav
    kFavTile = kFavDel + kFavSlots,    // x40 display, slot * 4 + track: 0 empty, else 1 + that track's pad pattern
    kFavName = kFavTile + kFavSlots * kTracks,   // x10 display: its text is the Fav's name; the number only counts changes
    kKitNote = kFavName + kFavSlots,    // x8 int 0..127: the note each drum sound is sent on (Kick, Snare, Hat, Open, Perc 1..4)
    kKitCell = kKitNote + kDrumPads,    // x8 push button, lit on the selected sound; a press selects it and plays it
    kKitUp = kKitCell + kDrumPads,      // push button: the selected sound one note up
    kKitDown,                           // push button: one note down
    kKitPreset,                         // x3 push button: fill all eight (see kKitPresets); lit while the notes are that preset's
    kKeyIn = kKitPreset + 3,            // switch: a note played in sets Key
    kKeyInLamp,                         // display: 0 off, 1 on, 2 a note has just come in
    kKitLook,                           // x8 display, how a Kit cell looks: +1 selected, +2 beyond the Pads count (not played)
    kOnBar = kKitLook + kDrumPads,      // switch: changes other than the re-rolls wait for the bar line too
    // The step strip on each track page
    kStrip,                             // x4 display, as kClock but for one track: its own waiting, not any track's
    kStripFlash = kStrip + kTracks,     // x4 display: 0, or 1..16 the step on which this track has just started a note
    kHarmony = kStripFlash + kTracks,   // %: how much the shared chord progression moves (Bass, Chords and Melody read it)
    kUndo,                              // push button: swap the current state with the undo copy
    kUndoLook,                          // display: 0 nothing to undo, 1 Undo, 2 Redo
    kFavLook,                           // display, the star: 0 plain, 1 the current state is a Fav, 2 all slots full
    kFavEdit,                           // switch: the FAV page shows the crosses, and a tile no longer recalls
    kFavCross,                          // x10 display: 1 while this slot shows its cross
    kSend = kFavCross + kFavSlots,      // switch, the MIDI readout: off = nothing is sent and the clocks stay dark. Saved, not in a Fav
    kNumParams
};

// Pad art: kArtHues patterns x kArtShapes sizes (the track's colour is fixed). A variation keeps the
// pattern and changes its size; a new idea changes the pattern. The skin generator draws the frames.
constexpr int kArtHues = 16, kArtShapes = 3;
constexpr int kArtFrames = kArtHues * kArtShapes;
constexpr int kNumSteppers = 3 * kTracks;
constexpr int64_t kNoStep = INT64_MIN;
constexpr double kNever = 1e300;
constexpr uint32_t kAllTracks = (1u << kTracks) - 1u;
constexpr uint32_t kAllButton = 1u << kTracks;

struct Held
{
    int note = -1, channel = 0;
    int left = 0;   // samples until the note-off
};

// Everything a track's pattern depends on. Compared every block to know when to regenerate.
struct TrackKey
{
    Settings settings;
    TrackSettings track;
    uint32_t version = 0;
    bool valid = false;

    bool same(const TrackKey& o) const
    {
        const Settings& a = settings; const Settings& b = o.settings;
        return valid && o.valid && version == o.version
            && a.tonic == b.tonic && a.scale == b.scale && a.style == b.style && a.type == b.type && a.bars == b.bars
            && a.density == b.density && a.complexity == b.complexity && a.gate == b.gate && a.longNotes == b.longNotes
            && a.harmonySeed == b.harmonySeed
            && a.harmonyComplexity == b.harmonyComplexity
            && track.role == o.track.role && track.voices == o.track.voices && track.octave == o.track.octave;
    }
};

// What a project stores.
struct IdeaState
{
    Idea     ideas[kTracks];
    uint32_t harmonySeed = 1;
    uint8_t  art[kTracks] = {};
};

// A state that can be recalled: the idea, the settings that shape it and the Kit. Channel and On are routing, not music.
constexpr int kNameSize = 16;
constexpr int kSnapShared = 7, kSnapPerTrack = 7, kSnapMusic = kSnapShared + kSnapPerTrack * kTracks, kSnapValues = kSnapMusic + kDrumPads;
constexpr int kUndoSlot = kFavSlots;   // pendingRecall_: the undo copy instead of a Fav
inline int snapParam(int i)
{
    static const int kShared[kSnapShared] = {kLevel, kKey, kScale, kStyle, kLength, kSwing, kHarmony};
    static const int kPerTrack[kSnapPerTrack] = {kDensity, kComplexity, kGate, kLong, kVoices, kOctave, kType};
    if (i >= kSnapMusic) return kKitNote + (i - kSnapMusic);
    return i < kSnapShared ? kShared[i] : kPerTrack[(i - kSnapShared) / kTracks] + (i - kSnapShared) % kTracks;
}
struct Snapshot
{
    IdeaState idea;
    float     values[kSnapValues] = {};
    char      name[kNameSize] = {};   // two random words; empty (a damaged project): show key and style
};
struct Favs
{
    Snapshot fav[kFavSlots];   // newest first
    int      favCount = 0;
};
inline void pushFront(Snapshot* list, int& count, int capacity, const Snapshot& s)
{
    const int n = count < capacity ? count : capacity - 1;
    for (int i = n; i > 0; --i) list[i] = list[i - 1];
    list[0] = s;
    count = n + 1;
}
inline void removeAt(Snapshot* list, int& count, int at)
{
    for (int i = at; i + 1 < count; ++i) list[i] = list[i + 1];
    --count;
}
// What the three Kit keys fill in. The MPC row is a standard drum program's notes for pads A01..A08.
constexpr int kKitPresets[3][kDrumPads] = {
    {36, 37, 38, 39, 40, 41, 42, 43},      // in a row
    {37, 36, 42, 82, 40, 38, 46, 44},      // MPC default pad notes, A01..A08
    {36, 38, 42, 46, 39, 45, 56, 49}};     // General MIDI: kick, snare, closed hat, open hat, clap, low tom, cowbell, crash
// A Fav's name is one of each: 32 x 32 = 1024 names, all to do with music. Short words (4 and 5 letters at most), so
// that the name fits its window.
// The Gate and Long Notes a style brings when it is chosen, for Bass, Chords and Melody (%). First settings, by ear.
struct StyleShape { float gate[kTracks - 1], longNotes[kTracks - 1]; };
constexpr StyleShape kStyleShape[kNumStyles] = {
    {{ 35,  30, 40}, { 0,  0,  5}},   // Techno: a short rolling bass, chord stabs
    {{ 60, 100, 50}, {15,  0, 25}},   // House: held chords (these are the defaults)
    {{ 70,  85, 55}, {25, 10, 20}},   // Hip-hop
    {{ 90, 100, 50}, {40, 20, 15}},   // Drum+Bass: a long bass under pads
    {{ 95,  90, 40}, {50, 10, 10}},   // Trap: a sustained bass, a clipped tune
    {{ 55,  90, 60}, {10,  5, 25}},   // Dance/Pop
    {{ 85,  25, 60}, {30,  0, 30}},   // Reggae/Dub: a round bass, short chops
    {{ 40,  30, 45}, { 5,  0, 10}},   // Funk: everything tight
    {{ 50,  45, 50}, { 0,  0, 10}},   // Retro Game
    {{ 80, 100, 75}, {30, 20, 40}},   // Modern Game: long and flowing
};

constexpr const char* kNameFirst[32] = {"Slow", "Fast", "Loud", "Soft", "Deep", "High", "Low", "Dark", "Warm", "Cool", "Wild", "Lazy", "Dry", "Wet", "Raw", "Fat",
                                        "Thin", "Big", "Tiny", "Odd", "Late", "Lush", "Dub", "Acid", "Jazz", "Funk", "Soul", "Punk", "Folk", "Trap", "Blue", "Neon"};
constexpr const char* kNameSecond[32] = {"Riff", "Loop", "Beat", "Drum", "Bass", "Horn", "Bell", "Echo", "Tape", "Tune", "Note", "Chord", "Hook", "Vamp", "Jam", "Mix",
                                         "Fill", "Lick", "Break", "Drop", "Verse", "Coda", "Tone", "Hum", "Clap", "Kick", "Snare", "Synth", "Vibe", "Swing", "Pulse", "Wave"};
// Requests from the worker thread (which watches the keys) to the audio thread (which owns the state).
constexpr uint64_t kReqRand = 1ull, kReqFav = 1ull << 4, kReqCancel = 1ull << 5, kReqRecall = 1ull << 6, kReqDel = 1ull << 16,
                   kReqPlay = 1ull << 26, kReqKitUp = 1ull << 34, kReqKitDown = 1ull << 35, kReqPreset = 1ull << 36, kReqUndo = 1ull << 39;

} // namespace

class Kwatuor : public mpc::Plugin
{
public:
    Kwatuor() : mpc::Plugin(makeInfo())
    {
        static const char* kTrackNames[kTracks] = {"Drums", "Bass", "Chords", "Melody"};
        static const int   kDefOctave[kTracks]  = {0, -2, 0, 1};
        static const float kDefDensity[kTracks] = {60.0f, 50.0f, 40.0f, 55.0f};
        static const float kDefGate[kTracks]    = {50.0f, kStyleShape[kHouse].gate[0], kStyleShape[kHouse].gate[1], kStyleShape[kHouse].gate[2]};
        static const float kDefLong[kTracks]    = {0.0f, kStyleShape[kHouse].longNotes[0], kStyleShape[kHouse].longNotes[1], kStyleShape[kHouse].longNotes[2]};
        static const int   kDefVoices[kTracks]  = {kDrumPads, 1, 3, 1};

        addParam(mpc::floatParam ("level",   "Velocity", 25.0f, 125.0f, 100.0f, "", 0));   // percentages carry their sign in the value text (formatValue)
        addParam(mpc::boolParam  ("gen_all", "Generate", false));
        addParam(mpc::choiceParam("amount",  "Variations",  {"I", "II", "III", "IV"}, 3));   // not plain digits: the tools would take it for an int
        addParam(mpc::choiceParam("key",     "Key",     {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 0));
        {
            std::vector<std::string> scales;
            for (const Scale& sc : kScales) scales.push_back(sc.name);
            addParam(mpc::choiceParam("scale", "Scale", scales, 0));   // Major
            std::vector<std::string> styles;
            for (const StyleDef& st : kStyles) styles.push_back(st.name);
            addParam(mpc::choiceParam("style", "Style", styles, kHouse));
        }
        addParam(mpc::choiceParam("length",  "Length",  {"1 bar", "2 bars", "4 bars", "8 bars"}, 2));
        addParam(mpc::floatParam ("swing",   "Swing",   50.0f, 75.0f, 50.0f, "", 0));

        std::vector<std::string> channels;
        for (int c = 1; c <= 16; ++c) channels.push_back(std::to_string(c));
        // Per-track parameters are named "<Track> <Setting>", e.g. "Bass Gate".
        auto perTrack = [this](const char* id, const char* name, const std::function<mpc::Param(const char*, const char*, int)>& make)
        {
            for (int t = 0; t < kTracks; ++t)
            {
                const std::string pid = std::string("t") + std::to_string(t + 1) + "_" + id;
                // Drums: its Notes setting is the Pads count, and its Motion knob drives the fills.
                std::string setting = name;
                if (t == kDrums && std::string(id).rfind("voices", 0) == 0) setting = std::string("Pads") + (name + 5);
                if (t == kDrums && std::string(id) == "complexity") setting = "Fills";
                const std::string pname = std::string(kTrackNames[t]) + " " + setting;
                addParam(make(pid.c_str(), pname.c_str(), t));
            }
        };
        perTrack("gen",        "Gen",    [](const char* i, const char* n, int)   { return mpc::boolParam(i, n, false); });
        perTrack("send",       "On",     [](const char* i, const char* n, int)   { return mpc::boolParam(i, n, true); });
        perTrack("channel",    "Channel", [channels](const char* i, const char* n, int t)
        {
            return mpc::choiceParam(i, n, channels, t + 1);   // Drums 2, Bass 3, Chords 4, Melody 5: 1 is left to a keyboard
        });
        perTrack("density",    "Density", [](const char* i, const char* n, int t) { return mpc::floatParam(i, n, 0.0f, 100.0f, kDefDensity[t], "", 0); });
        perTrack("complexity", "Motion", [](const char* i, const char* n, int)   { return mpc::floatParam(i, n, 0.0f, 100.0f, 35.0f, "", 0); });
        perTrack("gate",       "Gate",   [](const char* i, const char* n, int t) { return mpc::floatParam(i, n, 5.0f, 100.0f, kDefGate[t], "", 0); });
        perTrack("long",       "Long Notes", [](const char* i, const char* n, int t) { return mpc::floatParam(i, n, 0.0f, 100.0f, kDefLong[t], "", 0); });
        perTrack("voices",     "Notes",  [](const char* i, const char* n, int t) { return mpc::intParam(i, n, t == kDrums ? kMinDrumPads : 1, t == kDrums ? kDrumPads : kMaxVoices, kDefVoices[t], ""); });
        perTrack("octave",     "Octave", [](const char* i, const char* n, int t) { return mpc::intParam(i, n, -3, 3, kDefOctave[t], ""); });
        perTrack("type",       "Mode",   [](const char* i, const char* n, int)   { return mpc::choiceParam(i, n, {"Loop", "Drone"}, 0); });

        addParam(mpc::intParam("status", "MIDI Out", 0, 2, 1, ""));
        addParam(mpc::floatParam("pos",  "Song Pos", -64.0f, 160000.0f, 0.0f, "", 0));
        addParam(mpc::intParam("clock",  "Clock",    0, 48, 0, ""));
        perTrack("art",  "Tile", [](const char* i, const char* n, int) { return mpc::intParam(i, n, 0, kArtFrames - 1, 0, ""); });
        perTrack("wait", "Wait", [](const char* i, const char* n, int) { return mpc::intParam(i, n, 0, 1, 0, ""); });
        auto key = [](const char* i, const char* n, int) { return mpc::boolParam(i, n, false); };
        perTrack("chan_up",   "Channel +", key);
        perTrack("chan_dn",   "Channel -", key);
        perTrack("voices_up", "Notes +", key);
        perTrack("voices_dn", "Notes -", key);
        perTrack("oct_up",    "Octave +", key);
        perTrack("oct_dn",    "Octave -", key);
        perTrack("rand",      "Rand",    key);
        addParam(mpc::boolParam("fav_add", "Fav Add", false));
        const auto slotName = [](int s) { return "F" + std::to_string(s + 1); };
        for (int s = 0; s < kFavSlots; ++s) addParam(mpc::boolParam(("fav" + std::to_string(s + 1) + "_recall").c_str(), (slotName(s) + " Recall").c_str(), false));
        for (int s = 0; s < kFavSlots; ++s) addParam(mpc::boolParam(("fav" + std::to_string(s + 1) + "_del").c_str(), (slotName(s) + " Remove").c_str(), false));
        for (int s = 0; s < kFavSlots; ++s)
            for (int t = 0; t < kTracks; ++t)
                addParam(mpc::intParam(("fav" + std::to_string(s + 1) + "_t" + std::to_string(t + 1)).c_str(), (slotName(s) + " " + kTrackNames[t]).c_str(), 0, kArtFrames, 0, ""));
        for (int s = 0; s < kFavSlots; ++s) addParam(mpc::intParam(("fav" + std::to_string(s + 1) + "_name").c_str(), (slotName(s) + " Name").c_str(), 0, 999, 0, ""));
        static const char* kSounds[kDrumPads] = {"Kick", "Snare", "Hat", "Open", "Perc 1", "Perc 2", "Perc 3", "Perc 4"};
        for (int n = 0; n < kDrumPads; ++n) addParam(mpc::intParam(("kit_note" + std::to_string(n + 1)).c_str(), (std::string("Kit ") + kSounds[n]).c_str(), 0, 127, kKitPresets[0][n], ""));
        for (int n = 0; n < kDrumPads; ++n) addParam(mpc::boolParam(("kit_cell" + std::to_string(n + 1)).c_str(), (std::string("Cell ") + kSounds[n]).c_str(), n == 0));
        addParam(mpc::boolParam("kit_up", "Kit +", false));
        addParam(mpc::boolParam("kit_down", "Kit -", false));
        addParam(mpc::boolParam("kit_chr", "Kit CHR", true));
        addParam(mpc::boolParam("kit_mpc", "Kit MPC", false));
        addParam(mpc::boolParam("kit_gm", "Kit GM", false));
        addParam(mpc::boolParam("key_in", "Key In", false));
        addParam(mpc::intParam("key_in_lamp", "Key In Lamp", 0, 2, 0, ""));
        for (int n = 0; n < kDrumPads; ++n) addParam(mpc::intParam(("kit_look" + std::to_string(n + 1)).c_str(), (std::string("Look ") + kSounds[n]).c_str(), 0, 3, n == 0 ? 1 : 0, ""));
        addParam(mpc::boolParam("on_bar", "On Bar", true));
        perTrack("strip", "Strip", [](const char* i, const char* n, int) { return mpc::intParam(i, n, 0, 48, 0, ""); });
        perTrack("flash", "Flash", [](const char* i, const char* n, int) { return mpc::intParam(i, n, 0, 16, 0, ""); });
        addParam(mpc::floatParam("harmony", "Harmony", 0.0f, 100.0f, 35.0f, "", 0));
        addParam(mpc::boolParam("undo", "Undo", false));
        addParam(mpc::intParam("undo_look", "Undo Look", 0, 2, 0, ""));
        addParam(mpc::intParam("fav_look", "Fav Look", 0, 2, 0, ""));
        addParam(mpc::boolParam("fav_edit", "Fav Edit", false));
        for (int s = 0; s < kFavSlots; ++s) addParam(mpc::intParam(("fav" + std::to_string(s + 1) + "_cross").c_str(), (slotName(s) + " Cross").c_str(), 0, 1, 0, ""));
        addParam(mpc::boolParam("midi_send", "MIDI Send", true));
        for (int t = 0; t < kTracks; ++t)
        {
            steppers_[3 * t]     = {kChannel + t, kChanUp + t,   kChanDown + t,   0, 15};
            steppers_[3 * t + 1] = {kVoices + t,  kVoicesUp + t, kVoicesDown + t, t == kDrums ? kMinDrumPads : 1, t == kDrums ? kDrumPads : kMaxVoices};
            steppers_[3 * t + 2] = {kOctave + t,  kOctUp + t,    kOctDown + t,    -3, 3};
        }

        // The one program: the defaults.
        auto program = []()
        {
            std::vector<float> v((size_t)kNumParams, 0.0f);
            v[kLevel] = 100.0f; v[kAmount] = 3.0f; v[kStyle] = (float)kHouse; v[kLength] = 2.0f; v[kSwing] = 50.0f;
            for (int t = 0; t < kTracks; ++t)
            {
                v[(size_t)(kTrackSend + t)] = 1.0f;
                v[(size_t)(kChannel + t)] = (float)(t + 1);
                v[(size_t)(kDensity + t)] = kDefDensity[t];
                v[(size_t)(kComplexity + t)] = 35.0f;
                v[(size_t)(kGate + t)] = kDefGate[t];
                v[(size_t)(kLong + t)] = kDefLong[t];
                v[(size_t)(kVoices + t)] = (float)kDefVoices[t];
                v[(size_t)(kOctave + t)] = (float)kDefOctave[t];
            }
            for (int n = 0; n < kDrumPads; ++n) v[(size_t)(kKitNote + n)] = (float)kKitPresets[0][n];
            v[kKitCell] = 1.0f;
            v[kKitPreset] = 1.0f;
            v[kOnBar] = 1.0f;
            v[kHarmony] = 35.0f;
            v[kStatus] = 1.0f;
            v[kSend] = 1.0f;
            return v;
        };
        addProgram("Init", program());

        // A different idea for every new instance; a loaded project overwrites it.
        const uint64_t now = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
        rngState_ = mix((uint32_t)now ^ (uint32_t)(now >> 32) ^ (uint32_t)(uintptr_t)this) | 1u;
        for (int t = 0; t < kTracks; ++t) { resetIdea(state_.ideas[t], nextRandom()); state_.art[t] = (uint8_t)(ideaHash(state_.ideas[t]) % kArtFrames); }
        state_.harmonySeed = nextRandom();
        shadow_ = state_;
        publishDisplays();
        publishFavs();
        for (auto& f : flashShown_) f.store(0);

        alsa_.open("Kwatuor");   // not in process(): this allocates and talks to the kernel
    }

    ~Kwatuor() override
    {
        stop_.store(true);
        if (worker_.joinable()) worker_.join();
        releaseAll();    // the audio thread has stopped by now: do not leave notes hanging on other tracks
        alsa_.flush();
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        sampleRate_ = sampleRate > 1000.0 ? sampleRate : 44100.0;
        echoWindow_ = (int64_t)(0.3 * sampleRate_);
        reset();
        // The worker talks to the host, and the framework only knows the host once the constructor has returned: it
        // starts here, the first time the host prepares the plugin (found with the thread sanitizer).
        if (!worker_.joinable()) worker_ = std::thread([this] { publishLoop(); });
    }

    // Every write to a parameter passes here, whoever makes it (the host, a loaded project, this plugin). The
    // framework's range check lets a value that is not a number through, and such a value would then pass every
    // later range check too: put the parameter back to its default instead. Nothing else happens here.
    void parameterChanged(int index, float plainValue) override
    {
        // The stored value is tested, not plainValue: for a switch or a list the plain value is already rounded to
        // some whole number and looks fine.
        (void)plainValue;
        if (!std::isfinite(getNormalised(index))) set(index, param(index).def);
    }

    // Runs on resume and after a state load, possibly off the audio thread: only raise a flag.
    void reset() override { resync_.store(true, std::memory_order_relaxed); }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        for (int ch = 0; ch < ctx.numOutputs; ++ch) std::memset(ctx.outputs[ch], 0, sizeof(float) * (size_t)n);

        syncShadow();

        // After a load the buttons may hold any value: adopt it without rolling, and switch them off.
        if (resync_.exchange(false, std::memory_order_relaxed))
        {
            for (int b = 0; b <= kTracks; ++b) lastButton_[b] = getBool(buttonParam(b));
            clearButtons_.fetch_or(kAllTracks | kAllButton, std::memory_order_relaxed);
            releaseAll();
            lastStep_ = kNoStep;
            pendingMask_ = pendingNew_ = 0;
            pendingHarmony_ = false;
            pendingRecall_ = -1;
            requests_.store(0, std::memory_order_relaxed);
            stepResync_.store(true, std::memory_order_relaxed);
        }
        // The buttons are toggles on screen, so every flip by the user is a press. A flip to off that we
        // made ourselves (switching the light off once the idea landed) is not.
        for (int b = 0; b <= kTracks; ++b)
        {
            const bool on = getBool(buttonParam(b));
            if (on == lastButton_[b]) continue;
            lastButton_[b] = on;
            const bool ours = (selfCleared_.fetch_and(~(1u << b), std::memory_order_relaxed) & (1u << b)) != 0;
            if (on || !ours) request(b);
        }

        const mpc::Transport& t = ctx.transport;
        const bool running = t.valid && t.playing && t.ppqValid;
        if (t.valid) timeSig_.store(mpc::clamp(t.timeSigNumerator, 1, 99) * 100 + mpc::clamp(t.timeSigDenominator, 1, 99), std::memory_order_relaxed);
        if (t.valid && t.ppqValid) posStep_.store((int)std::floor(t.ppqPos * 4.0 + 1e-6), std::memory_order_relaxed);

        // A setting moved and it was not us (our own changes are absorbed where they are made): the state is no longer
        // the Fav it was, and there is nothing to redo any more.
        followStyle();
        if (snapMoved()) { redo_ = false; saved_ = false; }
        handleRequests();
        if (getBool(kKeyIn))
            for (int i = 0; i < ctx.numMidiEvents; ++i)
            {
                if (!ctx.midi[i].isNoteOn()) continue;
                bool echo = false;   // one of ours, come back
                for (const Sent& sent : sent_) echo = echo || (sent.note == ctx.midi[i].note() && sent.channel == ctx.midi[i].channel() && sampleClock_ - sent.at < echoWindow_);
                if (echo) continue;
                set(kKey, (float)(ctx.midi[i].note() % 12));   // the note's name; its octave does not matter
                keyHit_.store(true, std::memory_order_relaxed);
            }
        sampleClock_ += n;
        const bool atOnce = !running || !getBool(kOnBar);   // changes other than the re-rolls need not wait
        if (pendingRecall_ >= 0 && atOnce) recall();
        else if (pendingMask_ && !running) commit();
        staleMask_ = refreshPatterns(atOnce || !current_[0].valid);

        // Note-offs that fall inside this block, and tracks that were switched off.
        for (int tr = 0; tr < kTracks; ++tr)
        {
            const bool off = !getBool(kSend) || (!getBool(kTrackSend + tr) && !(tr == kDrums && auditioned_));
            for (Held& h : held_[tr])
            {
                if (h.note < 0) continue;
                if (off || h.left <= n) noteOff(h);
                else h.left -= n;
            }
        }

        if (auditioned_ && std::all_of(std::begin(held_[kDrums]), std::end(held_[kDrums]), [](const Held& h) { return h.note < 0; })) auditioned_ = false;
        if (!running)
        {
            if (wasRunning_) releaseAll();
            wasRunning_ = false;
            alsa_.flush();
            publishClock(t);
            return;
        }
        const bool started = !wasRunning_;
        if (started) { lastStep_ = kNoStep; havePrevEnd_ = false; playedPrev_ = false; }
        wasRunning_ = true;

        const double bpm = mpc::clamp(t.tempoValid ? t.bpm : 120.0, 20.0, 999.0);
        const double ppqPerSample = bpm / (60.0 * sampleRate_);
        const double start = t.ppqPos;
        const double end = start + n * ppqPerSample;
        const double stepSamples = 0.25 / ppqPerSample;
        // Swing delays every second 1/16: 50 % = straight, 75 % = dotted. The knob adds to the style's own swing.
        const double swing = mpc::clamp((double)get(kSwing) + kStyles[mpc::clamp(getInt(kStyle), 0, kNumStyles - 1)].swing, 50.0, 75.0);
        const double swingPpq = (swing - 50.0) / 100.0 * 2.0 * 0.25;

        // The song position jumped (loop, locate): forget which step was last played.
        // Only a real jump counts: the MPC's position does not continue exactly where the tempo says the last block
        // ended, and a tight test here reset the sequencer on almost every block (the clock stuck on step 1).
        // Small overlaps are harmless, a step is never played twice (k <= lastStep_).
        const bool back = havePrevEnd_ && start < prevEnd_ - 0.125;
        const bool jumped = back || (havePrevEnd_ && start > prevEnd_ + 0.25);
        // A step already sent in the block the jump happened in is the one the position has just landed on.
        const bool landedOnPlayed = back && playedPrev_;
        const double oneBlock = n * ppqPerSample + 0.01;
        // The loop usually wraps in the middle of a block. The step on the loop point belongs after the wrap: sent
        // before it, the MPC records it in front of the loop end and the note at 1.1.1 is lost when overdubbing.
        // The MPC reports its loop points; a host that does not has the end learned from the first wrap.
        const double hostEnd = (t.cycling && t.cycleEndPpq > t.cycleStartPpq + 0.25) ? t.cycleEndPpq : kNever;
        const bool wrapped = back && hostEnd < kNever && std::fabs(prevEnd_ - hostEnd) <= 2.0 * oneBlock;
        if (back && !countIn_ && hostEnd == kNever)
        {
            // Where the loop ends: the 1/16 boundary inside the block the position jumped back from.
            const double boundary = std::floor(prevEnd_ * 4.0 + 1e-6) * 0.25;
            learnedEnd_ = boundary >= prevStart_ - 1e-6 ? boundary : kNever;
        }
        const double loopEnd = hostEnd < kNever ? hostEnd : learnedEnd_;

        // Count-in. Seen on the MPC XL (transport logs): during a count-in, in Play or Record,
        // the host says "playing" and the position is a free-running clock that kept going while stopped: it starts
        // anywhere and wraps at the loop end. When the count-in ends the position jumps (back, forward, or by next
        // to nothing) to where play really starts. Every real start is exactly on a 1/16 line, whether from the top,
        // from a located bar or continuing after a stop; the free clock practically never is.
        // So: a start off the 1/16 grid is a count-in, and it ends when the position lands on the grid with a break
        // in it. The free clock wrapping at the loop end is not that. Two bars is the longest count-in.
        const double barPpq = 4.0 * mpc::clamp(t.timeSigNumerator, 1, 32) / (double)mpc::clamp(t.timeSigDenominator, 1, 32);
        const auto onGrid = [](double ppq) { return std::fabs(ppq * 4.0 - std::round(ppq * 4.0)) < 0.002; };
        if (started) { countIn_ = !onGrid(start); countInSamples_ = 0.0; }
        else if (countIn_)
        {
            const bool broke = havePrevEnd_ && std::fabs(start - prevEnd_) > 0.003;
            const bool timedOut = (countInSamples_ += n) > (2.0 * barPpq + 0.5) / ppqPerSample;
            if ((broke && onGrid(start) && !wrapped) || timedOut) { countIn_ = false; lastStep_ = kNoStep; releaseAll(); }
        }
        if (jumped) { lastStep_ = kNoStep; releaseAll(); }
        bool played = false;
        prevStart_ = start; prevEnd_ = end; havePrevEnd_ = true;

        for (int64_t k = (int64_t)std::floor(start * 4.0) - 1; ; ++k)
        {
            const double when = (double)k * 0.25 + ((k & 1) ? swingPpq : 0.0);
            if (when >= end) break;
            if (k <= lastStep_) continue;
            // A step just before this block: with a known last step it fell in a gap between two blocks and is
            // played now, a few samples late, instead of being lost. After a start or a jump it is played only if
            // the position landed within a block of it (a loop wrapping mid-block lands just past its first step).
            if (lastStep_ == kNoStep && when < start - 1e-9 && start - when > oneBlock) continue;
            if (landedOnPlayed && lastStep_ == kNoStep && std::fabs(when - start) <= oneBlock) { lastStep_ = k; continue; }   // it went out just before the wrap
            if (when >= loopEnd - 1e-9 && start < loopEnd - 1e-9) break;   // on the loop point: after the wrap
            if (k < 0) continue;   // the position is before the song start
            lastStep_ = k;
            if (countIn_) continue;
            const int offset = mpc::clamp((int)((when - start) / ppqPerSample), 0, n - 1);
            playStep(k, offset, n, stepSamples);
            played = true;
        }
        playedPrev_ = played;

        alsa_.flush();
        publishClock(t);
    }

    bool formatValue(int index, float plainValue, std::string& out) override
    {
        if (index == kStatus)
        {
            switch (alsa_.status())
            {
                case kPortOpen:   out = "Open"; break;
                case kOpenFailed: out = "Seq fail"; break;
                case kPortFailed: out = "Port fail"; break;
                default:          out = "No ALSA"; break;
            }
            return true;
        }
        if (index >= kFavName && index < kFavName + kFavSlots)   // a Fav's name; UI thread
        {
            static const char* kKeys[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
            const int slot = index - kFavName;
            std::lock_guard<std::mutex> lock(shadowMutex_);
            if (slot >= shadowFavs_.favCount) { out = " "; return true; }
            const Snapshot& sn = shadowFavs_.fav[slot];
            if (sn.name[0]) out = sn.name;
            else out = std::string(kKeys[mpc::clamp((int)std::lround(sn.values[1]), 0, 11)]) + " " + kStyles[mpc::clamp((int)std::lround(sn.values[3]), 0, kNumStyles - 1)].name;
            return true;
        }
        if (index == kPos)   // bar:beat:step, assuming 4/4; bars before the song start (a count-in) are negative
        {
            if (alsa_.failed()) { out = "ERR"; return true; }
            if (!getBool(kSend)) { out = "OFF"; return true; }
            const int sig = timeSig_.load(std::memory_order_relaxed);
            if (sig != 404) { out = std::to_string(sig / 100) + "/" + std::to_string(sig % 100) + " !"; return true; }   // not 4/4: say so
            const int step = (int)std::lround(plainValue);
            const int bar = (int)std::floor((double)step / kStepsPerBar);
            const int in = step - bar * kStepsPerBar;
            out = std::to_string(bar >= 0 ? bar + 1 : bar) + ":" + std::to_string(in / 4 + 1) + ":" + std::to_string(in % 4 + 1);
            return true;
        }
        if (index == kLevel || index == kSwing || index == kHarmony || (index >= kDensity && index < kLong + kTracks))   // a percentage
        {
            out = std::to_string((int)std::lround(plainValue)) + "%";
            return true;
        }
        return false;
    }

    // "TSR6": the current idea (the shared harmony seed, and per track eight seeds, the anchor, a step map and a tile number),
    // then the Favs: how many values a snapshot has, how many Favs, and per Fav its idea, its values and its name.
    // Anything else, earlier formats included, is ignored.
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        // Too big for the stack of a host thread we know nothing about.
        std::unique_ptr<Favs> h(new Favs);
        IdeaState s;
        { std::lock_guard<std::mutex> lock(shadowMutex_); s = shadow_; *h = shadowFavs_; }
        out.assign({'T', 'S', 'R', '6'});
        auto put = [&out](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(v >> (8 * i))); };
        auto putIdea = [&](const IdeaState& st)
        {
            put(st.harmonySeed);
            for (int t = 0; t < kTracks; ++t)
            {
                for (int q = 0; q < kSlots; ++q) put(st.ideas[t].seeds[q]);
                put(st.ideas[t].anchor);
                out.insert(out.end(), st.ideas[t].owner, st.ideas[t].owner + kMaxSteps);
                out.push_back(st.art[t]);
            }
        };
        auto putSnap = [&](const Snapshot& sn)
        {
            putIdea(sn.idea);
            for (float v : sn.values) { uint32_t bits; std::memcpy(&bits, &v, 4); put(bits); }
            out.insert(out.end(), sn.name, sn.name + kNameSize);
        };
        putIdea(s);
        out.push_back((uint8_t)kSnapValues); out.push_back((uint8_t)h->favCount);
        for (int i = 0; i < h->favCount; ++i) putSnap(h->fav[i]);
    }
    void loadExtraState(const uint8_t* data, size_t size) override
    {
        const size_t ideaSize = 4 + kTracks * (4 * kSlots + 4 + kMaxSteps + 1), snapSize = ideaSize + 4 * kSnapValues + kNameSize;
        if (size < 4 + ideaSize || std::memcmp(data, "TSR6", 4) != 0) return;
        auto get32 = [](const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); };
        auto getIdea = [&](const uint8_t* p, IdeaState& st)
        {
            st.harmonySeed = get32(p);
            p += 4;
            for (int t = 0; t < kTracks; ++t)
            {
                for (int q = 0; q < kSlots; ++q, p += 4) st.ideas[t].seeds[q] = get32(p);
                st.ideas[t].anchor = get32(p);
                p += 4;
                for (int i = 0; i < kMaxSteps; ++i) st.ideas[t].owner[i] = (uint8_t)(*p++ % kSlots);
                st.art[t] = (uint8_t)(*p++ % kArtFrames);
            }
            return p;
        };
        IdeaState s;
        const uint8_t* p = getIdea(data + 4, s);
        std::unique_ptr<Favs> h(new Favs);
        if (size >= 4 + ideaSize + 2 && p[0] == kSnapValues && p[1] <= kFavSlots
            && size >= 4 + ideaSize + 2 + (size_t)p[1] * snapSize)
        {
            h->favCount = p[1];
            p += 2;
            auto getSnap = [&](Snapshot& sn)
            {
                p = getIdea(p, sn.idea);
                // A project file is outside input. A value that is not a number would pass every range check on its
                // way to the engine, so it is dropped here; set() clamps the rest to the parameter's range on recall.
                for (float& v : sn.values) { const uint32_t bits = get32(p); std::memcpy(&v, &bits, 4); if (!std::isfinite(v)) v = 0.0f; p += 4; }
                for (int c = 0; c < kNameSize - 1 && p[c] >= 32 && p[c] < 127; ++c) sn.name[c] = (char)p[c];   // printable ASCII, cut at anything else
                p += kNameSize;
            };
            for (int i = 0; i < h->favCount; ++i) getSnap(h->fav[i]);
        }
        std::lock_guard<std::mutex> lock(shadowMutex_);
        shadow_ = s;
        shadowFavs_ = *h;
        loaded_.store(true, std::memory_order_relaxed);
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "Kwatuor";
        info.vendor       = "Kelta";   // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version      = 700;    // 0.7.0.0
        info.isInstrument = true;
        info.numInputs    = 0;
        info.numOutputs   = 2;
        info.tailSamples  = 1;      // silent
        return info;
    }

    static constexpr char kUid[5] = "KBK6";

    static int buttonParam(int b) { return b < kTracks ? kGen + b : kGenAll; }

    uint32_t nextRandom()   // audio thread (and the constructor)
    {
        rngState_ ^= rngState_ << 13; rngState_ ^= rngState_ >> 17; rngState_ ^= rngState_ << 5;
        return rngState_;
    }

    // ---- ideas -------------------------------------------------------------------------------
    // The audio thread owns state_. shadow_ is the copy that save and load use; the audio thread
    // only ever try-locks it.
    void syncShadow()
    {
        if (!loaded_.load(std::memory_order_relaxed) && !shadowDirty_) return;
        std::unique_lock<std::mutex> lock(shadowMutex_, std::try_to_lock);
        if (!lock.owns_lock()) return;   // next block
        if (loaded_.exchange(false, std::memory_order_relaxed))
        {
            state_ = shadow_;
            favs_ = shadowFavs_;
            ++version_;
            pendingMask_ = pendingNew_ = 0;
            pendingHarmony_ = false;
            pendingRecall_ = -1;
            haveUndo_ = redo_ = saved_ = haveLast_ = false;   // another project: nothing of the old one to go back to
            lastStyle_ = -1;                                  // and its Style comes with its own knob values
            publishDisplays();
            publishFavs();
        }
        else { shadow_ = state_; shadowFavs_ = favs_; }
        shadowGen_.fetch_add(1, std::memory_order_relaxed);   // the names on screen are read from the shadow
        shadowDirty_ = false;
    }

    // Button b (0..3 = a track, 4 = all) was pressed.
    void request(int b)
    {
        // 1 and 2 vary the idea, 3 and 4 replace it; 4 also renews what the tracks share.
        static const float kVary[2] = {0.25f, 0.5f};
        const int amount = mpc::clamp(getInt(kAmount), 0, 3) + 1;
        const uint32_t mask = b < kTracks ? (1u << b) : kAllTracks;
        for (int t = 0; t < kTracks; ++t)
        {
            if (!(mask & (1u << t))) continue;
            if (!(pendingMask_ & (1u << t))) pendingIdeas_[t] = state_.ideas[t];   // a second press builds on the first
            if (amount >= 3) { resetIdea(pendingIdeas_[t], nextRandom()); pendingNew_ |= 1u << t; }
            else varyIdea(pendingIdeas_[t], nextRandom(), kVary[amount - 1]);
        }
        pendingMask_ |= mask;
        if (b == kTracks)   // only the centre button touches what the tracks share
        {
            pendingHarmony_ = pendingHarmony_ || amount == 4;
        }
        litButtons_ |= 1u << b;
    }

    // ---- Fav ---------------------------------------------------------------------------------------------------
    Snapshot snapshotNow() const
    {
        Snapshot s;
        s.idea = state_;
        for (int i = 0; i < kSnapValues; ++i) s.values[i] = get(snapParam(i));
        return s;
    }
    void favsChanged() { shadowDirty_ = true; publishFavs(); }

    // ---- Style -------------------------------------------------------------------------------------------------
    // A style changed by hand brings its own Gate and Long Notes: those two are absolute, so unlike Density and
    // Motion they do not adapt to the style by themselves. A recall or a load sets Style too and must not do this.
    void followStyle()
    {
        const int style = mpc::clamp(getInt(kStyle), 0, kNumStyles - 1);
        if (style == lastStyle_) return;
        const bool had = lastStyle_ >= 0;
        lastStyle_ = style;
        if (!had) return;
        for (int t = kDrums + 1; t < kTracks; ++t)
        {
            set(kGate + t, kStyleShape[style].gate[t - 1]);
            set(kLong + t, kStyleShape[style].longNotes[t - 1]);
        }
        notifySnap_.store(true, std::memory_order_relaxed);
    }

    // ---- Undo ---------------------------------------------------------------------------------------------------
    void markUndo() { undo_ = snapshotNow(); haveUndo_ = true; redo_ = false; }
    // Takes the settings as they are now for the baseline that snapMoved() compares with.
    void absorb() { for (int i = 0; i < kSnapValues; ++i) lastSnap_[i] = get(snapParam(i)); haveLast_ = true; }
    bool snapMoved()
    {
        bool moved = false;
        for (int i = 0; i < kSnapValues; ++i) moved = moved || lastSnap_[i] != get(snapParam(i));
        const bool had = haveLast_;
        if (moved || !had) absorb();
        return moved && had;
    }
    void publishFavs()   // audio thread (and the constructor): what the FAV page shows
    {
        for (int s = 0; s < kFavSlots; ++s)
        {
            const bool filled = s < favs_.favCount;
            const Snapshot& sn = favs_.fav[s];
            for (int t = 0; t < kTracks; ++t) favShown_[s][t].store(filled ? (uint8_t)(1 + sn.idea.art[t]) : (uint8_t)0, std::memory_order_relaxed);
        }
    }

    // New random Play settings for a track, inside a window that never empties it or turns it to mush.
    void randomise(int t)
    {
        const auto pick = [this](int lo, int hi) { return (float)(lo + (int)(nextRandom() % (uint32_t)(hi - lo + 1))); };
        set(kDensity + t, pick(25, 85));
        set(kComplexity + t, pick(10, 90));
        if (t != kDrums) { set(kGate + t, pick(20, 100)); set(kLong + t, pick(0, 50)); }
    }

    // What the keys asked for since the last block.
    void handleRequests()
    {
        const uint64_t req = requests_.exchange(0, std::memory_order_relaxed);
        if (!req) return;
        if (req & (kReqRand * kAllTracks))
        {
            markUndo();
            for (int t = 0; t < kTracks; ++t) if (req & (kReqRand << t)) randomise(t);
            notifySnap_.store(true, std::memory_order_relaxed);
            saved_ = false;
            absorb();
        }
        if ((req & kReqFav) && favs_.favCount < kFavSlots)   // full: the star is greyed and keeps nothing
        {
            Snapshot sn = snapshotNow();
            for (int attempt = 0; attempt < 8; ++attempt)   // a name no other Fav has, if eight tries find one
            {
                const uint32_t r = nextRandom();
                std::snprintf(sn.name, kNameSize, "%s %s", kNameFirst[r & 31u], kNameSecond[(r >> 5) & 31u]);
                bool taken = false;
                for (int i = 0; i < favs_.favCount; ++i) taken = taken || std::strcmp(favs_.fav[i].name, sn.name) == 0;
                if (!taken) break;
            }
            pushFront(favs_.fav, favs_.favCount, kFavSlots, sn);
            saved_ = true;
            favsChanged();
        }
        const bool edit = getBool(kFavEdit);
        for (int i = kFavSlots - 1; i >= 0; --i)
            if ((req & (kReqDel << i)) && i < favs_.favCount && edit)   // the crosses only show, and only work, with Edit lit
            {
                removeAt(favs_.fav, favs_.favCount, i);
                saved_ = false;        // it may have been the one the star stood for
                pendingRecall_ = -1;   // the slots moved under it
                favsChanged();
            }
        if (req & kReqCancel) pendingRecall_ = -1;
        for (int s = 0; s < kFavSlots; ++s)
            if ((req & (kReqRecall << s)) && s < favs_.favCount && !edit) pendingRecall_ = s;
        if ((req & kReqUndo) && haveUndo_) pendingRecall_ = kUndoSlot;
        recallShown_.store(pendingRecall_, std::memory_order_relaxed);

        // Kit.
        for (int p = 0; p < 3; ++p)
            if (req & (kReqPreset << p))
            {
                for (int n = 0; n < kDrumPads; ++n) set(kKitNote + n, (float)kKitPresets[p][n]);
                notifyKit_.store(true, std::memory_order_relaxed);
            }
        const int cell = mpc::clamp(kitSel_.load(std::memory_order_relaxed), 0, kDrumPads - 1);
        if (req & (kReqKitUp | kReqKitDown))
        {
            set(kKitNote + cell, (float)mpc::clamp(getInt(kKitNote + cell) + ((req & kReqKitUp) ? 1 : 0) - ((req & kReqKitDown) ? 1 : 0), 0, 127));
            notifyKit_.store(true, std::memory_order_relaxed);
            audition(cell);
        }
        for (int n = 0; n < kDrumPads; ++n) if (req & (kReqPlay << n)) audition(n);
    }

    // Plays a drum sound once so that it can be found by ear.
    void audition(int sound)
    {
        if (!getBool(kSend)) return;
        const int note = mpc::clamp(getInt(kKitNote + sound), 0, 127), channel = mpc::clamp(getInt(kChannel + kDrums), 0, 15);
        Held* slot = nullptr;
        for (Held& h : held_[kDrums]) if (h.note == note) { noteOff(h); slot = &h; break; }
        if (!slot) for (Held& h : held_[kDrums]) if (h.note < 0) { slot = &h; break; }
        if (!slot) { slot = &held_[kDrums][0]; noteOff(*slot); }
        alsa_.note(true, channel, note, 100);
        remember(channel, note);
        slot->note = note; slot->channel = channel; slot->left = (int)(0.15 * sampleRate_);
        auditioned_ = true;
    }

    void land() { if (pendingRecall_ >= 0) recall(); else if (pendingMask_) commit(); }

    // Brings a Fav (or the undo copy) back. What it replaces becomes the undo copy.
    void recall()
    {
        const int s = pendingRecall_;
        pendingRecall_ = -1;
        recallShown_.store(-1, std::memory_order_relaxed);
        if (s == kUndoSlot ? !haveUndo_ : s >= favs_.favCount) return;
        const Snapshot before = snapshotNow();
        const Snapshot target = s == kUndoSlot ? undo_ : favs_.fav[s];   // a copy: the undo copy is replaced below
        state_ = target.idea;
        for (int i = 0; i < kSnapValues; ++i) set(snapParam(i), target.values[i]);
        notifySnap_.store(true, std::memory_order_relaxed);
        // What was playing becomes the undo copy: Undo after a recall goes back to it, and Undo after Undo comes forward.
        undo_ = before; haveUndo_ = true;
        redo_ = s == kUndoSlot && !redo_;
        saved_ = s != kUndoSlot;
        lastStyle_ = mpc::clamp(getInt(kStyle), 0, kNumStyles - 1);   // the recalled knob values stand
        absorb();
        pendingMask_ = pendingNew_ = 0;      // a re-roll that was waiting too is dropped
        pendingHarmony_ = false;
        clearButtons_.fetch_or(litButtons_, std::memory_order_relaxed);
        litButtons_ = 0;
        ++version_;
        shadowDirty_ = true;
        publishDisplays();
    }

    void commit()
    {
        markUndo();
        saved_ = false;
        for (int t = 0; t < kTracks; ++t)
        {
            if (!(pendingMask_ & (1u << t))) continue;
            state_.ideas[t] = pendingIdeas_[t];
            int hue = state_.art[t] / kArtShapes, shape = state_.art[t] % kArtShapes;
            if (pendingNew_ & (1u << t)) { hue = (hue + 1 + (int)(nextRandom() % (uint32_t)(kArtHues - 1))) % kArtHues; shape = (int)(nextRandom() % (uint32_t)kArtShapes); }
            else shape = (shape + 1 + (int)(nextRandom() % (uint32_t)(kArtShapes - 1))) % kArtShapes;
            state_.art[t] = (uint8_t)(hue * kArtShapes + shape);
        }
        if (pendingHarmony_) state_.harmonySeed = nextRandom();
        pendingMask_ = pendingNew_ = 0;
        pendingHarmony_ = false;
        ++version_;
        shadowDirty_ = true;
        clearButtons_.fetch_or(litButtons_, std::memory_order_relaxed);
        litButtons_ = 0;
        publishDisplays();
    }

    void publishDisplays()   // audio thread (and the constructor): what the worker shows
    {
        for (int t = 0; t < kTracks; ++t) artShown_[t].store(state_.art[t], std::memory_order_relaxed);
    }

    // Audio thread, end of every block: where the bar clock stands and which tracks wait for the bar line.
    // The step comes straight from the host position, not from the sequencer's own bookkeeping (lastStep_, running),
    // which is reset on position jumps and standstills: on the device that made the clock flicker.
    void publishClock(const mpc::Transport& t)
    {
        const bool playing = t.valid && t.playing && t.ppqValid;
        int step = !playing ? -1 : (int)wrap((int64_t)std::floor(t.ppqPos * 4.0 + 1e-6), kStepsPerBar);
        // During a count-in the host position means nothing: count the bar from when it began, in its own colour.
        if (playing && countIn_)
        {
            const double bpm = mpc::clamp(t.tempoValid ? t.bpm : 120.0, 20.0, 999.0);
            step = 2 * kStepsPerBar + (int)wrap((int64_t)std::floor(countInSamples_ * bpm / (60.0 * sampleRate_) * 4.0), kStepsPerBar);
        }
        clockStep_.store(step, std::memory_order_relaxed);
        const int64_t now = (int64_t)std::floor(t.ppqPos * 4.0 + 1e-6);
        for (int tr = 0; tr < kTracks; ++tr) flashShown_[tr].store(playing && !countIn_ && hitStep_[tr] == now ? 1 : 0, std::memory_order_relaxed);
        waitShown_.store((pendingRecall_ >= 0 ? kAllTracks : pendingMask_) | staleMask_, std::memory_order_relaxed);
        undoShown_.store(!haveUndo_ ? 0 : (redo_ ? 2 : 1), std::memory_order_relaxed);
        starShown_.store(favs_.favCount >= kFavSlots ? 2 : (saved_ ? 1 : 0), std::memory_order_relaxed);
    }

    // Reads the knobs and regenerates the patterns whose inputs changed.
    // Returns the tracks whose pattern no longer matches the knobs (none when `apply`).
    uint32_t refreshPatterns(bool apply = true)
    {
        Settings shared;
        shared.tonic = mpc::clamp(getInt(kKey), 0, 11);
        shared.style = mpc::clamp(getInt(kStyle), 0, kNumStyles - 1);
        shared.scale = mpc::clamp(getInt(kScale), 0, 19);
        shared.bars = 1 << mpc::clamp(getInt(kLength), 0, 3);
        shared.harmonySeed = state_.harmonySeed;
        // How much the shared chords move has its own knob; a track's Complexity is that track's business only.
        shared.harmonyComplexity = mpc::clamp(get(kHarmony), 0.0f, 100.0f) * 0.01f;

        uint32_t stale = 0;
        for (int t = 0; t < kTracks; ++t)
        {
            TrackKey now;
            now.settings = shared;
            now.settings.type = t == kDrums ? (int)kLoop : mpc::clamp(getInt(kType + t), 0, kNumTypes - 1);
            now.settings.density = mpc::clamp(get(kDensity + t), 0.0f, 100.0f) * 0.01f;
            now.settings.complexity = mpc::clamp(get(kComplexity + t), 0.0f, 100.0f) * 0.01f;
            now.settings.gate = mpc::clamp(get(kGate + t), 5.0f, 100.0f) * 0.01f;
            now.settings.longNotes = mpc::clamp(get(kLong + t), 0.0f, 100.0f) * 0.01f;
            now.track.role = t;   // the tracks are fixed: Drums, Bass, Chords, Melody
            if (t == kDrums)      // drums have no pitch: a new key, scale or chord movement is no change for them
            { now.settings.tonic = 0; now.settings.scale = 0; now.settings.harmonyComplexity = 0.0f; }   // they keep the harmony seed: it holds the shared form and the kick
            now.track.voices = mpc::clamp(getInt(kVoices + t), t == kDrums ? kMinDrumPads : 1, t == kDrums ? kDrumPads : kMaxVoices);
            now.track.octave = mpc::clamp(getInt(kOctave + t), -3, 3);
            now.version = version_;
            now.valid = true;
            if (now.same(current_[t])) continue;
            if (!apply) { stale |= 1u << t; continue; }
            generateIdea(patterns_[t], scratch_, state_.ideas[t], now.settings, now.track);
            current_[t] = now;
        }
        return stale;
    }

    // ---- playback ----------------------------------------------------------------------------
    static int64_t wrap(int64_t k, int64_t length) { const int64_t m = k % length; return m < 0 ? m + length : m; }

    void playStep(int64_t k, int offset, int blockFrames, double stepSamples)
    {
        if (wrap(k, kStepsPerBar) == 0) { land(); staleMask_ = refreshPatterns(); }   // the bar line: what was waiting lands

        const float level = mpc::clamp(get(kLevel), 25.0f, 125.0f) * 0.01f;
        for (int tr = 0; tr < kTracks; ++tr)
        {
            const int channel = mpc::clamp(getInt(kChannel + tr), 0, 15);
            if (!getBool(kTrackSend + tr) || !getBool(kSend)) continue;
            const Pattern& p = patterns_[tr];
            const Step& st = p.steps[wrap(k, p.length)];
            if (!st.count) continue;
            hitStep_[tr] = k;   // a note starts here: the track's step strip flashes

            if (tr != kDrums) for (Held& h : held_[tr]) noteOff(h);   // a new hit replaces what the track was holding
            uint8_t notes[kMaxVoices];
            for (int v = 0; v < st.count; ++v)   // a drum step names its sounds; the kit says which notes they are
                notes[v] = tr == kDrums ? (uint8_t)mpc::clamp(getInt(kKitNote + mpc::clamp(st.note[v] - kDrumBaseNote, 0, kDrumPads - 1)), 0, 127) : st.note[v];
            int left = (int)((double)st.gate * stepSamples) - (blockFrames - offset);   // counted from the next block on
            if (left < 1) left = 1;
            for (int v = 0; v < st.count; ++v)
            {
                Held* slot = nullptr;
                for (Held& h : held_[tr]) if (h.note == notes[v]) { noteOff(h); slot = &h; break; }   // retrigger
                if (!slot) for (Held& h : held_[tr]) if (h.note < 0) { slot = &h; break; }
                if (!slot) { slot = &held_[tr][0]; for (Held& h : held_[tr]) if (h.left < slot->left) slot = &h; noteOff(*slot); }
                alsa_.note(true, channel, notes[v], mpc::clamp((int)std::lround((float)st.vel[v] * level), 1, 127));   // every note has its own loudness
                remember(channel, notes[v]);
                slot->note = notes[v]; slot->channel = channel; slot->left = left;
            }
        }
    }

    void remember(int channel, int note) { sent_[sentNext_] = {note, channel, sampleClock_}; sentNext_ = (sentNext_ + 1) % kSentSize; }

    void noteOff(Held& h)
    {
        if (h.note < 0) return;
        alsa_.note(false, h.channel, h.note, 0);
        h.note = -1;
    }
    void releaseAll() { for (auto& track : held_) for (Held& h : track) noteOff(h); }

    // Worker thread: switches the buttons back off once their idea has landed, and updates the displays.
    void publishLoop()
    {
        while (!stop_.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            const uint32_t clear = clearButtons_.exchange(0, std::memory_order_relaxed);
            for (int b = 0; b <= kTracks; ++b)
                if ((clear & (1u << b)) && getBool(buttonParam(b)))
                {
                    selfCleared_.fetch_or(1u << b, std::memory_order_relaxed);   // tell the audio thread this flip is ours
                    set(buttonParam(b), 0.0f);
                    notifyHostParameterChanged(buttonParam(b));
                }
            // Stepper keys: a lit key is a press. Move the target by one and put the key out again.
            const bool skip = stepResync_.exchange(false, std::memory_order_relaxed);   // after a load: only clear them
            for (const Stepper& sp : steppers_)
                for (int dir = 1; dir >= -1; dir -= 2)
                {
                    const int keyParam = dir > 0 ? sp.up : sp.down;
                    if (!getBool(keyParam)) continue;
                    const int now = getInt(sp.target), next = mpc::clamp(now + dir, sp.lo, sp.hi);
                    if (!skip && next != now) { set(sp.target, (float)next); notifyHostParameterChanged(sp.target); }
                    set(keyParam, 0.0f);
                    notifyHostParameterChanged(keyParam);
                }

            // The other keys. A lit key is a press: pass it to the audio thread and put the key out again.
            const auto pressed = [&](int keyParam)
            {
                if (!getBool(keyParam)) return false;
                set(keyParam, 0.0f);
                notifyHostParameterChanged(keyParam);
                return !skip;
            };
            uint64_t req = 0;
            for (int t = 0; t < kTracks; ++t) if (pressed(kRand + t)) req |= kReqRand << t;
            if (pressed(kFavAdd)) req |= kReqFav;
            if (pressed(kUndo)) req |= kReqUndo;
            show(kUndoLook, (float)undoShown_.load(std::memory_order_relaxed), 0.5f);
            show(kFavLook, (float)starShown_.load(std::memory_order_relaxed), 0.5f);
            // Edit: a loaded project never opens with the crosses showing.
            if (skip && getBool(kFavEdit)) { set(kFavEdit, 0.0f); notifyHostParameterChanged(kFavEdit); }
            const bool edit = getBool(kFavEdit);
            for (int i = 0; i < kFavSlots; ++i)
            {
                if (pressed(kFavDel + i) && edit) req |= kReqDel << i;
                show(kFavCross + i, edit && favShown_[i][0].load(std::memory_order_relaxed) ? 1.0f : 0.0f, 0.5f);
            }
            // A recall key stays lit while its state waits for the bar line. Pressing it again cancels.
            const int waitingSlot = recallShown_.load(std::memory_order_relaxed);
            for (int s = 0; s < kFavSlots; ++s)
            {
                const uint32_t bit = 1u << s;
                const bool on = getBool(kFavRecall + s);
                const auto putOut = [&] { set(kFavRecall + s, 0.0f); notifyHostParameterChanged(kFavRecall + s); recallLit_ &= ~bit; };
                if (skip) { if (on) putOut(); recallLit_ &= ~bit; }
                else if (on && edit) putOut();                                       // Edit is lit: a tile does nothing
                else if (on && !(recallLit_ & bit)) { req |= kReqRecall << s; recallLit_ |= bit; recallAge_[s] = 0; }
                else if (on && waitingSlot != s && ++recallAge_[s] > 4) putOut();     // it landed, was refused, or another took over
                else if (!on && (recallLit_ & bit)) { recallLit_ &= ~bit; if (waitingSlot == s) req |= kReqCancel; }
            }
            // Kit. A cell's key is lit while its sound is selected, so a press shows as a change either way.
            int sel = mpc::clamp(kitSel_.load(std::memory_order_relaxed), 0, kDrumPads - 1);
            if (skip) { sel = 0; for (int n = kDrumPads - 1; n >= 0; --n) if (getBool(kKitCell + n)) sel = n; }   // a loaded project: adopt its selection
            else
                for (int n = 0; n < kDrumPads; ++n)
                    if (getBool(kKitCell + n) != (n == sel)) { sel = n; req |= kReqPlay << n; break; }
            kitSel_.store(sel, std::memory_order_relaxed);
            for (int n = 0; n < kDrumPads; ++n)
                if (getBool(kKitCell + n) != (n == sel)) { set(kKitCell + n, n == sel ? 1.0f : 0.0f); notifyHostParameterChanged(kKitCell + n); }
            const int padsInPlay = mpc::clamp(getInt(kVoices + kDrums), kMinDrumPads, kDrumPads);
            for (int n = 0; n < kDrumPads; ++n) show(kKitLook + n, (float)((n == sel ? 1 : 0) + (n >= padsInPlay ? 2 : 0)), 0.5f);
            if (pressed(kKitUp)) req |= kReqKitUp;
            if (pressed(kKitDown)) req |= kReqKitDown;
            // A preset's key is lit while the eight notes are that preset's, so it goes out when a note is moved.
            // Like a cell's key, a press shows as a change either way. After a press the key is held lit for a few
            // ticks, until the audio thread has written the notes.
            int preset = -1;
            for (int p = 2; p >= 0; --p)
            {
                bool same = true;
                for (int n = 0; n < kDrumPads; ++n) same = same && getInt(kKitNote + n) == kKitPresets[p][n];
                if (same) preset = p;
            }
            if (presetHold_ > 0 && --presetHold_ > 0) preset = presetHeld_;
            if (!skip)
                for (int p = 0; p < 3; ++p)
                    if (getBool(kKitPreset + p) != (p == presetLit_)) { req |= kReqPreset << p; preset = presetHeld_ = p; presetHold_ = 6; break; }
            presetLit_ = preset;
            for (int p = 0; p < 3; ++p)
                if (getBool(kKitPreset + p) != (p == preset)) { set(kKitPreset + p, p == preset ? 1.0f : 0.0f); notifyHostParameterChanged(kKitPreset + p); }
            if (notifyKit_.exchange(false, std::memory_order_relaxed))
                for (int n = 0; n < kDrumPads; ++n) notifyHostParameterChanged(kKitNote + n);
            // Key in: tell the host that Key moved, and flash the lens for a fifth of a second.
            if (keyHit_.exchange(false, std::memory_order_relaxed)) { notifyHostParameterChanged(kKey); keyFlash_ = 8; }
            else if (keyFlash_ > 0) --keyFlash_;
            show(kKeyInLamp, !getBool(kKeyIn) ? 0.0f : (keyFlash_ > 0 ? 2.0f : 1.0f), 0.5f);
            if (req) requests_.fetch_or(req, std::memory_order_relaxed);
            // A name is the text of its parameter: move the number so that the host asks for the text again.
            const uint32_t gen = shadowGen_.load(std::memory_order_relaxed);
            if (gen != shownGen_)
            {
                shownGen_ = gen;
                for (int i = 0; i < kFavSlots; ++i) { set(kFavName + i, (float)(gen % 1000u)); notifyHostParameterChanged(kFavName + i); }
            }
            if (notifySnap_.exchange(false, std::memory_order_relaxed))   // the audio thread changed settings: tell the host
                for (int i = 0; i < kSnapValues; ++i) notifyHostParameterChanged(snapParam(i));
            for (int s = 0; s < kFavSlots; ++s)
                for (int t = 0; t < kTracks; ++t) show(kFavTile + s * kTracks + t, (float)favShown_[s][t].load(std::memory_order_relaxed), 0.5f);

            const uint32_t waiting = waitShown_.load(std::memory_order_relaxed);
            for (int t = 0; t < kTracks; ++t)
            {
                show(kArt + t, (float)artShown_[t].load(std::memory_order_relaxed), 0.5f);
                show(kWait + t, (waiting & (1u << t)) ? 1.0f : 0.0f, 0.5f);
            }
            int step = clockStep_.load(std::memory_order_relaxed);
            // A single "stopped" reading does not blank the clock: it has to last four ticks (100 ms).
            if (step >= 0) { clockHeld_ = step; clockIdle_ = 0; }
            else if (++clockIdle_ < 4) step = clockHeld_;
            else clockHeld_ = -1;
            // The MIDI lens: red when the port did not open, grey when sending is switched off; then the clocks stay dark.
            const int lens = alsa_.failed() ? 0 : (getBool(kSend) ? 1 : 2);
            show(kStatus, (float)lens, 0.5f);
            if (lens != 1) step = -1;
            show(kClock, step < 0 ? 0.0f : (float)(1 + step + (waiting && step < 2 * kStepsPerBar ? kStepsPerBar : 0)), 0.5f);
            for (int t = 0; t < kTracks; ++t)
            {
                const bool waits = (waiting & (1u << t)) != 0;
                show(kStrip + t, step < 0 ? 0.0f : (float)(1 + step + (waits && step < 2 * kStepsPerBar ? kStepsPerBar : 0)), 0.5f);
                show(kStripFlash + t, step >= 0 && step < kStepsPerBar && flashShown_[t].load(std::memory_order_relaxed) ? (float)(1 + step) : 0.0f, 0.5f);
            }
            // The position's text also carries the time-signature warning: when the signature changes, move the
            // number so that the host asks for the text again, even while stopped.
            const int sig = timeSig_.load(std::memory_order_relaxed);
            if (sig != sigShown_ || lens != lensShown_) { sigShown_ = sig; lensShown_ = lens; set(kPos, -64.0f); notifyHostParameterChanged(kPos); }
            else show(kPos, (float)mpc::clamp(posStep_.load(std::memory_order_relaxed), -63, 160000), 0.5f);
        }
    }
    void show(int param, float value, float tolerance)
    {
        if (std::fabs(get(param) - value) > tolerance) { set(param, value); notifyHostParameterChanged(param); }
    }

    AlsaOut  alsa_;
    double   sampleRate_ = 44100.0;
    std::atomic<int> posStep_{0};       // host song position in 1/16 steps
    std::atomic<int> timeSig_{404};     // the host's time signature, numerator * 100 + denominator
    int sigShown_ = 404;                // worker thread only
    int lensShown_ = 1;                 // worker thread only
    int clockHeld_ = -1, clockIdle_ = 0;   // worker thread only
    std::atomic<int> clockStep_{-1};    // step in the bar that last played; -1 when stopped
    std::atomic<uint32_t> waitShown_{0};
    int64_t  hitStep_[kTracks] = {kNoStep, kNoStep, kNoStep, kNoStep};   // audio thread: the step each track last started a note on
    std::atomic<int> flashShown_[kTracks];

    struct Stepper { int target, up, down, lo, hi; };
    Stepper  steppers_[kNumSteppers];
    std::atomic<bool> stepResync_{true};

    IdeaState state_;                    // audio thread
    IdeaState shadow_;                   // guarded by shadowMutex_
    Favs      favs_;                     // audio thread
    Favs      shadowFavs_;               // guarded by shadowMutex_
    int       pendingRecall_ = -1;       // audio thread: the Fav (or kUndoSlot) that lands on the next bar line
    Snapshot  undo_;                     // audio thread: the state to go back to
    bool      haveUndo_ = false, redo_ = false;   // redo_: the last thing that landed was an Undo
    bool      saved_ = false;            // the current state is a Fav (the star is blue)
    float     lastSnap_[kSnapValues] = {};   // the settings as last seen, to notice a change made by hand
    bool      haveLast_ = false;
    int       lastStyle_ = -1;           // audio thread: the Style last seen, -1 = adopt whatever is set
    std::atomic<int> undoShown_{0}, starShown_{0};
    std::atomic<uint64_t> requests_{0};  // worker -> audio thread: kReq... bits
    std::atomic<int>      recallShown_{-1};
    std::atomic<bool>     notifySnap_{false};
    std::atomic<uint8_t>  favShown_[kFavSlots][kTracks];
    std::atomic<uint32_t> shadowGen_{0};  // counts shadow updates
    uint32_t shownGen_ = 0;              // worker thread only
    std::atomic<int>  kitSel_{0};        // the selected drum sound
    int presetLit_ = -1, presetHeld_ = -1, presetHold_ = 0;   // worker thread: the lit Kit preset key
    std::atomic<bool> keyHit_{false};    // audio -> worker: Key in took a note
    // The notes sent last, so that Key in can tell our own echo. 64 covers well over the window at any density.
    struct Sent { int note = -1, channel = -1; int64_t at = 0; };
    static constexpr int kSentSize = 64;
    Sent     sent_[kSentSize];
    int      sentNext_ = 0;
    int64_t  sampleClock_ = 0;           // samples processed since the plugin started
    int64_t  echoWindow_ = 13230;        // 0.3 s at 44.1 kHz; set in prepare()
    int      keyFlash_ = 0;              // worker thread only: ticks the lens still flashes
    std::atomic<bool> notifyKit_{false};
    bool     auditioned_ = false;        // audio thread: a sound played from the Kit panel is still sounding
    uint32_t recallLit_ = 0;             // worker thread only: recall keys it has passed on
    int      recallAge_[kFavSlots] = {};
    std::mutex shadowMutex_;
    std::atomic<bool> loaded_{false};    // shadow_ holds a loaded project the audio thread has not taken yet
    bool     shadowDirty_ = false;
    uint32_t version_ = 1;               // bumped whenever state_ changes

    Idea     pendingIdeas_[kTracks];
    uint32_t pendingMask_ = 0;
    uint32_t staleMask_ = 0;             // tracks whose changed settings wait for the bar line (On bar)
    uint32_t pendingNew_ = 0;            // pending tracks that get a whole new idea, not a variation
    bool     pendingHarmony_ = false;
    uint32_t litButtons_ = 0;            // buttons to switch off at the next commit
    bool     lastButton_[kTracks + 1] = {};
    uint32_t rngState_ = 1;

    Pattern  patterns_[kTracks];
    Pattern  scratch_;
    TrackKey current_[kTracks];

    Held     held_[kTracks][kMaxVoices];
    int64_t  lastStep_ = kNoStep;
    bool     wasRunning_ = false;
    bool     countIn_ = false;           // muted: the host is counting in
    double   countInSamples_ = 0.0;
    bool     havePrevEnd_ = false;
    double   prevEnd_ = 0.0;
    double   prevStart_ = 0.0;
    double   learnedEnd_ = kNever;       // where the host's loop was last seen to wrap
    bool     playedPrev_ = false;        // a step went out in the previous block

    std::atomic<uint32_t> clearButtons_{0};
    std::atomic<uint32_t> selfCleared_{0};
    std::atomic<int>      artShown_[kTracks];
    std::atomic<bool>     resync_{true};
    std::atomic<bool>     stop_{false};
    std::thread           worker_;
};

// This macro creates the exported VSTPluginMain entry point. Keep it last.
MPC_PLUGIN_ENTRY(Kwatuor)
