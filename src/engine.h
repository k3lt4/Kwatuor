// Kwatuor pattern generator. Pure functions: a pattern is fully determined by
// (seeds, harmony seed, settings), so turning a knob reshapes the current idea
// instead of replacing it, and a project only has to store the seeds.
//
// Four fixed tracks: Drums, Bass, Chords, Melody. A Style decides where each of them
// tends to play (a weight per 1/16 step) and how; Density turns the weights into hits.
//
// Three tiers decide everything:
//   the plan    shared by the four tracks (harmony seed, style, length; chords also scale and the
//               shared Complexity): phrase form, cadences and who marks them, the kick line, the chords
//   the anchor  one per track, set by a new idea and kept through its variations: snare variant,
//               fill gesture, percussion lines, bass rhythm, the tune's pitch set
//   the seeds   one per mosaic slot, new with every variation: the dice of each step
// Tracks never read each other's pattern: what they agree on, they each read from the plan.
//
// No allocation and no I/O: safe to call from the audio thread.
#pragma once

#define KWATUOR_ENGINE_VERSION 2             // 2: per-note velocity, shared plan, anchor (tests that build against both engines)

#include <cmath>
#include <cstdint>

namespace kwatuor {

constexpr int kTracks = 4;
constexpr int kStepsPerBar = 16;                      // 1/16 notes
constexpr int kMaxBars = 8;
constexpr int kMaxSteps = kStepsPerBar * kMaxBars;
constexpr int kMaxVoices = 4;
constexpr int kDrumBaseNote = 36;                     // pad A1 on the MPC's chromatic pad map
constexpr int kDrumPads = 8;                          // A1 kick, A2 snare, A3 closed hat, A4 open hat, A5..A8 percussion

enum Role { kDrums = 0, kBass, kChords, kMelody, kNumRoles };   // also the track index
enum Type { kLoop = 0, kDrone, kNumTypes };
enum Style { kTechno = 0, kHouse, kHipHop, kDnB, kTrap, kPop, kReggae, kFunk, kRetroGame, kModernGame, kNumStyles };

// What a step asks of the resolve pass that runs once the mosaic is put together (generateIdea).
enum StepFlag : uint8_t
{
    kFLong = 1,                        // a long note: it swallows the steps of its span and is no longer than it
    kFStyleLen = 2,                    // gate holds the length the style wants, in steps at Gate 60 %
    kFDrone = 4,                       // held until the next note
    kFHold = 8,                        // a phrase ending: held, but not for ever
    kFFill = 16,                       // Drums: part of a fill (tests)
    kFKeep = 32,                       // the note its bar keeps at the lowest Density: the mosaic never drops it (generateIdea)
};

struct Step
{
    uint8_t count = 0;                 // notes on this step; 0 = rest
    uint8_t note[kMaxVoices] = {};
    uint8_t vel[kMaxVoices] = {};      // per note: a ghost snare and a loud kick can share a step
    uint8_t velocity = 0;              // the loudest of vel[] (step strips, tests)
    uint8_t flags = 0;                 // StepFlag
    uint8_t span = 0;                  // kFLong: steps this note swallows (2..8); kFHold: the most it may ring
    float   gate = 0.0f;               // note length in steps (inside generate(): what resolve is asked for)
};

struct Pattern
{
    int  length = kStepsPerBar;        // steps
    Step steps[kMaxSteps];
};

struct TrackSettings
{
    int role = kBass;
    int voices = 1;                    // notes per hit; for Drums, how many pads are in play (1..kDrumPads)
    int octave = 0;                    // 0 = the octave of middle C
};

struct Settings
{
    int   tonic = 0;                   // 0 = C
    int   scale = 0;                   // index into kScales
    int   style = kHouse;
    int   type = kLoop;
    int   bars = 2;
    float density = 0.6f;              // 0..1
    float complexity = 0.35f;          // 0..1
    float gate = 0.6f;                 // 0..1, note length as a share of the gap to the next note
    float longNotes = 0.0f;            // 0..1, chance of a long note that swallows the notes after it (where the role allows one)
    uint32_t harmonySeed = 1;          // shared by all tracks: the plan (form, kick, chords)
    float harmonyComplexity = 0.35f;   // shared by all tracks, so they agree on the chord changes
};

// What a track plays is a mosaic: every step belongs to one of a few seeds. A new idea gives all
// steps to one seed; a variation hands a fraction of the steps to a fresh seed.
constexpr int kSlots = 8;
struct Idea
{
    uint32_t seeds[kSlots] = {};
    uint32_t anchor = 0;               // what makes this idea itself and survives its variations: set by a new idea only
    uint8_t  owner[kMaxSteps] = {};    // slot index per step
};

struct Scale { const char* name; int size; int semis[8]; };

// RND synth's twenty scales, in its order.
inline const Scale kScales[20] = {
    {"Major",         7, {0, 2, 4, 5, 7, 9, 11}},
    {"Minor",         7, {0, 2, 3, 5, 7, 8, 10}},
    {"Harm Minor",    7, {0, 2, 3, 5, 7, 8, 11}},
    {"Blues",         6, {0, 3, 5, 6, 7, 10}},
    {"Maj Penta",     5, {0, 2, 4, 7, 9}},
    {"Min Penta",     5, {0, 3, 5, 7, 10}},
    {"Dorian",        7, {0, 2, 3, 5, 7, 9, 10}},
    {"Phrygian",      7, {0, 1, 3, 5, 7, 8, 10}},
    {"Lydian",        7, {0, 2, 4, 6, 7, 9, 11}},
    {"Mixolydian",    7, {0, 2, 4, 5, 7, 9, 10}},
    {"Locrian",       7, {0, 1, 3, 5, 6, 8, 10}},
    {"Whole Tone",    6, {0, 2, 4, 6, 8, 10}},
    {"Dbl Harmonic",  7, {0, 1, 4, 5, 7, 8, 11}},
    {"Hungarian Min", 7, {0, 2, 3, 6, 7, 8, 11}},
    {"Phrygian Dom",  7, {0, 1, 4, 5, 7, 8, 10}},
    {"Hirajoshi",     5, {0, 2, 3, 7, 8}},
    {"In Sen",        5, {0, 1, 5, 7, 10}},
    {"Prometheus",    6, {0, 2, 4, 6, 9, 10}},
    {"Octatonic",     8, {0, 2, 3, 5, 6, 8, 9, 11}},
    {"Persian",       7, {0, 1, 4, 5, 6, 8, 11}},
};

// A style. Each rhythm row is 16 digits, one per 1/16 step of a bar: 9 = always plays,
// 1..8 = more or less likely (Density decides), 0 = never. All rows and numbers below are
// first settings written from general genre practice, to be tuned by ear.

// Percussion pad: either a weight row, or a Euclidean line (eK hits over eN steps) of which
// lo..hi candidates play as Density goes from 0 to 1.
struct PercDef { const char* row; uint8_t eK, eN, lo, hi; };
constexpr PercDef euc(int k, int n, int lo, int hi) { return {nullptr, (uint8_t)k, (uint8_t)n, (uint8_t)lo, (uint8_t)hi}; }
constexpr PercDef row(const char* digits) { return {digits, 0, 0, 0, 0}; }

struct DrumDef
{
    const char* kick[3];               // kick families: the shared plan picks one
    const char* snare[3];              // snare variants: the track's anchor picks one
    const char* hat; const char* openHat;
    uint8_t hatAccent;                 // row of kHatVel
    float   kickExtra, burst, fillAmt; // chance of an extra kick, of a hat burst, fill length per Complexity
    uint8_t fillW[5];                  // fill gesture weights: build, toms, stutter, drop, hats
    PercDef perc[4];                   // pads A5..A8
    bool    pair;                      // pads A7 and A8 share perc[2]'s line (even / odd candidate)
};

// How the bass treats the kick: lands with it, plays less where it surely plays (Hard: even its own "always" steps,
// all but the downbeat), never plays where it may play, or does not care.
enum KickRel : uint8_t { kLockKick, kDodgeKick, kDodgeHard, kBanKick, kFreeKick };
struct BassDef
{
    const char* row[3];                // rhythm variants: the track's anchor picks one
    uint8_t pal[6];                    // root, octave, fifth, third, step below, neighbour (% at Complexity 35)
    float   ant, oddRoot;              // chance to anticipate the next root; how much odd 1/16s insist on the root
    uint8_t kickRel;                   // KickRel
    float   len[3];                    // note length in steps at Gate 60 %: anchor, body, approach
    bool    lowFifth;                  // the fifth is played below the root
};

enum Colour : uint8_t { kColOct, kCol7, kCol9 };
enum Voicing : uint8_t { kVoWindow, kVoPlane, kVoSpread, kVoArp };
struct HarmDef
{
    float   move;                      // 0..1: how readily the chord changes from bar to bar
    uint8_t tplW[12];                  // weights of kTemplates
    bool    darkTurn;                  // the Turn role prefers the flat side (kDarkTurn)
    uint8_t colour;                    // Colour: what the fourth chord tone is
    float   half, pedal;               // how readily a half-bar passing chord appears (high Complexity); chance the bass stays on the tonic
    uint8_t voicing;                   // Voicing
    uint8_t voiceOrd[4];               // which chord tone voice 1, 2, 3, 4 is (0 root, 1 third, 2 fifth, 3 colour)
};

struct StyleDef
{
    const char* name;
    DrumDef d; BassDef b; HarmDef h;
    const char* chords; const char* lead;
    uint8_t setSize;                   // how many scale notes the lead uses at low Complexity
    bool    sequenced;                 // the lead moves with the chord root
    const char* accent;                // pitched velocity per step: (digit - 5) * 4
    uint8_t formW[4];                  // phrase form weights: AA'AB, AA'BA', ABAB, AAAA'
    uint8_t actW[3];                   // who marks a cadence: drum fill, bass pickup, chord push
    float   vary, cellBreak;           // how much later bars differ; how often beats 3-4 leave the rhythm of beats 1-2
    int     swing;                     // % of swing the style brings by itself
};

inline const StyleDef kStyles[kNumStyles] = {
// Techno is the hard kind: kick on every beat, hats off the beat, a rolling root-note bass between the kicks,
// one chord stab, a repeating 1/16 hook, no chord changes to speak of.
{"Techno",
 {{"9000900090009000", "9000900090009020", "9000900090029000"},
  {"0000600000006000", "0000000000006000", "0000600000006020"},
  "6404640464046404", "0080008000800080", 1, 0.0f, 0.0f, 1.3f, {1, 0, 0, 4, 3},
  {row("0000700000007000"), euc(3, 16, 1, 3), euc(3, 8, 2, 6), euc(3, 8, 2, 6)}, true},
 {{"0888088808880888", "0092009200920092", "0078007800780078"}, {80, 8, 6, 0, 4, 2}, 0.0f, 0.9f, kBanKick, {0.7f, 0.7f, 0.7f}, true},
 {0.1f, {0, 0, 0, 0, 2, 2, 0, 3, 2, 0, 0, 0}, true, kColOct, 0.0f, 0.8f, kVoPlane, {0, 2, 1, 3}},
 "0000003000000600", "7070770770707707", 7, false, "7545654575456545", {1, 0, 1, 4}, {3, 1, 0}, 0.5f, 0.15f, 0},
{"House",
 {{"9000900090009000", "9000900090009030", "9000900090039000"},
  {"0000900000009000", "0000900000009020", "0000900100009001"},
  "5304530453045304", "0070007000700070", 1, 0.0f, 0.0f, 0.9f, {2, 1, 0, 3, 2},
  {row("0000800000008001"), euc(5, 16, 2, 5), euc(3, 8, 3, 6), euc(3, 8, 3, 6)}, true},
 {{"2073207320732073", "0090009000900093", "9003006090030060"}, {45, 22, 15, 8, 6, 4}, 0.3f, 0.3f, kDodgeKick, {1.2f, 1.0f, 0.8f}, false},
 {0.6f, {2, 3, 0, 0, 2, 0, 2, 0, 2, 1, 0, 0}, false, kCol7, 0.3f, 0.0f, kVoPlane, {0, 1, 3, 2}},
 "9002090020900200", "7003007070030700", 8, false, "6384638463846384", {2, 1, 2, 3}, {2, 2, 1}, 0.7f, 0.3f, 4},
{"Hip-hop",
 {{"9000000600900300", "9002000090600000", "9000006000960000"},
  {"0000900000009000", "0000900200009002", "0000900000009010"},
  "8040804080408050", "0000000000000020", 0, 0.10f, 0.15f, 0.9f, {1, 1, 3, 2, 1},
  {row("0000600000006002"), euc(5, 16, 1, 4), euc(2, 16, 1, 2), euc(3, 16, 0, 3)}, false},
 {{"9000000600900300", "9000002070030000", "9002000090600030"}, {55, 8, 15, 8, 10, 4}, 0.25f, 0.2f, kLockKick, {4.0f, 3.0f, 1.5f}, true},
 {0.7f, {0, 0, 0, 1, 3, 2, 2, 0, 3, 0, 0, 0}, true, kCol7, 0.0f, 0.0f, kVoWindow, {1, 3, 2, 0}},
 "9000000040000000", "7000070000700303", 7, false, "9353735383537353", {2, 1, 3, 2}, {2, 2, 1}, 0.8f, 0.6f, 10},
{"Drum+Bass",
 {{"9000000000900000", "9060000000960000", "9000000600900000"},
  {"0000900201009002", "0000900202009002", "0000900000009020"},
  "8050805080508050", "0000000000200000", 0, 0.08f, 0.3f, 1.3f, {4, 0, 3, 1, 0},
  {row("0000000200000020"), euc(7, 16, 2, 6), euc(3, 16, 1, 3), euc(2, 16, 0, 2)}, false},
 {{"9000000300900020", "9003006000900300", "9000000000907060"}, {60, 14, 14, 8, 4, 0}, 0.2f, 0.3f, kLockKick, {8.0f, 6.0f, 0.8f}, true},
 {0.9f, {0, 0, 0, 0, 3, 2, 0, 2, 3, 0, 0, 0}, true, kColOct, 0.0f, 0.3f, kVoWindow, {0, 2, 1, 3}},
 "9000000000000000", "7000207000702003", 8, false, "9353635383536353", {2, 1, 3, 2}, {4, 1, 0}, 0.9f, 0.6f, 0},
{"Trap",
 {{"9000000300600200", "9000000000090060", "9003000000900300"},
  {"0000000090000000", "0000000090000020", "0000000090000300"},
  "8281828182818283", "0000000000000030", 2, 0.12f, 1.0f, 0.9f, {1, 0, 1, 3, 4},
  {row("0000000080000030"), euc(3, 16, 1, 3), euc(3, 8, 2, 6), euc(3, 8, 2, 6)}, true},
 {{"9000000300600200", "9000000000603020", "9002000300600230"}, {70, 12, 12, 6, 0, 0}, 0.1f, 0.5f, kLockKick, {12.0f, 8.0f, 2.0f}, true},
 {0.85f, {0, 0, 0, 0, 3, 3, 0, 2, 2, 0, 0, 0}, true, kColOct, 0.0f, 0.3f, kVoWindow, {0, 2, 1, 3}},
 "9000000300000000", "7007007000700703", 6, false, "9454645484546454", {1, 1, 2, 4}, {3, 2, 0}, 0.7f, 0.5f, 0},
{"Dance/Pop",
 {{"9000900090009000", "9000000090900000", "9000009000900000"},
  {"0000900000009000", "0000900000009020", "0000900000029000"},
  "6181618161816181", "0030003000300030", 0, 0.0f, 0.0f, 1.1f, {3, 2, 1, 1, 1},
  {row("0000800000008000"), euc(7, 16, 2, 6), euc(3, 8, 2, 6), euc(3, 8, 2, 6)}, true},
 {{"9070707090707070", "0090009000900090", "9007007090070070"}, {65, 15, 10, 5, 3, 2}, 0.2f, 0.6f, kDodgeHard, {0.8f, 0.8f, 0.8f}, false},
 {0.7f, {2, 3, 3, 2, 0, 0, 0, 0, 0, 1, 0, 2}, false, kCol9, 0.5f, 0.0f, kVoWindow, {0, 1, 2, 3}},
 "9000009000900000", "7070700770707000", 6, true, "8454745484547454", {4, 2, 2, 0}, {3, 1, 2}, 0.8f, 0.35f, 0},
{"Reggae/Dub",
 {{"0000000090000000", "9000000090000000", "9000900090009000"},
  {"0000000090000000", "0000000090000020", "0003000090000000"},
  "6050805060508050", "0000000000000030", 1, 0.0f, 0.0f, 1.0f, {1, 4, 1, 1, 0},
  {row("0000000070000020"), euc(7, 16, 3, 7), euc(3, 8, 2, 5), euc(3, 8, 2, 5)}, true},
 {{"7090700090907000", "0090709090007000", "9060000090600070"}, {40, 5, 25, 20, 5, 5}, 0.0f, 0.4f, kFreeKick, {2.5f, 1.8f, 1.8f}, true},
 {0.7f, {1, 0, 0, 0, 0, 3, 4, 0, 0, 0, 0, 0}, false, kCol7, 0.2f, 0.0f, kVoWindow, {1, 2, 3, 0}},
 "0000904000009040", "0070070000700700", 7, true, "6374637463746374", {2, 1, 3, 2}, {3, 2, 0}, 0.7f, 0.4f, 8},
{"Funk",
 {{"9002000090200300", "9090000000900300", "9000002090020000"},
  {"0000900100009010", "0000900201029002", "0000900100009020"},
  "8474847484748474", "0000000300000030", 3, 0.10f, 0.0f, 1.1f, {1, 1, 4, 0, 1},
  {row("0000700100007010"), euc(5, 16, 2, 5), euc(3, 16, 1, 3), euc(2, 16, 0, 2)}, false},
 {{"9003070290030720", "9002006090270060", "9060020090600272"}, {40, 20, 10, 5, 15, 10}, 0.5f, 0.3f, kLockKick, {2.0f, 0.5f, 0.5f}, false},
 {0.65f, {0, 0, 0, 1, 0, 0, 4, 1, 0, 0, 0, 0}, false, kCol7, 0.2f, 0.0f, kVoWindow, {1, 2, 3, 0}},
 "0090020900900209", "7007070070070700", 7, false, "9363547383635473", {2, 1, 2, 3}, {2, 3, 2}, 1.0f, 0.5f, 4},
// Retro game (8-bit console platformer): a light bouncing beat, a bass hopping between root and fifth on the
// 1/8s, an arpeggio where the chords would be, a busy catchy hook, chords changing every bar.
{"Retro Game",
 {{"9000003090000030", "9000000090090000", "9009000090090000"},
  {"0000900000009000", "0000900000009020", "0000900000909000"},
  "8280828082808280", "0000000000000030", 2, 0.05f, 0.0f, 1.1f, {4, 1, 1, 0, 1},
  {row("0000600000006000"), euc(5, 16, 1, 5), euc(3, 16, 1, 3), euc(2, 16, 0, 2)}, false},
 {{"9000907090009070", "9080908090809080", "9007009090070090"}, {45, 20, 25, 8, 0, 2}, 0.0f, 0.2f, kLockKick, {0.9f, 0.9f, 0.9f}, false},
 {0.8f, {3, 2, 2, 2, 0, 0, 0, 0, 0, 0, 1, 2}, false, kColOct, 0.6f, 0.0f, kVoArp, {0, 1, 2, 3}},
 "9080908090809080", "9079009790970700", 5, true, "7555755575557555", {4, 3, 1, 0}, {3, 1, 2}, 0.8f, 0.3f, 0},
// Modern game (cinematic, hybrid orchestral): half-time drums with a ticking hat, a pulsing root-note ostinato
// in the bass, one long wide chord a bar, a sparse slow tune, harmony that moves now and then.
{"Modern Game",
 {{"9000000000600020", "9009009009009000", "9000000000090600"},
  {"0000000090000000", "0000000090000020", "0000000090000200"},
  "6252625262526252", "0000000000000000", 0, 0.05f, 0.0f, 1.0f, {1, 4, 0, 2, 0},
  {row("0000000070000000"), euc(5, 16, 2, 5), euc(3, 16, 1, 3), euc(2, 16, 0, 2)}, false},
 {{"9080908090809080", "9686968696869686", "9000000000600020"}, {80, 8, 8, 2, 2, 0}, 0.0f, 0.9f, kLockKick, {0.6f, 0.6f, 0.6f}, true},
 {0.5f, {0, 1, 0, 0, 2, 0, 0, 0, 3, 3, 0, 0}, true, kCol9, 0.0f, 0.6f, kVoSpread, {0, 2, 1, 3}},
 "9000000000200000", "9000006000700300", 8, true, "8454645474546454", {3, 3, 1, 1}, {3, 1, 0}, 0.9f, 0.7f, 0},
};

// Closed hat loudness by position in the beat, one row per accent habit:
// on the beat, off the beat (the "and"), flat, on the beat with a strong "and".
inline const uint8_t kHatVel[4][4] = {{100, 70, 85, 60}, {70, 55, 104, 55}, {88, 88, 88, 88}, {100, 52, 92, 58}};

inline uint32_t mix(uint32_t x)   // murmur3 finaliser: decorrelates nearby seeds
{
    x ^= x >> 16; x *= 0x85EBCA6Bu; x ^= x >> 13; x *= 0xC2B2AE35u; x ^= x >> 16;
    return x;
}

struct Rng
{
    uint32_t s;
    explicit Rng(uint32_t seed) : s(mix(seed ^ 0x9E3779B9u)) { if (s == 0) s = 0x1234567u; }
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float uni() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }          // [0, 1)
    int range(int lo, int hi) { return lo + (int)(uni() * (float)(hi - lo + 1)); }   // inclusive
};

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int iround(float v) { return (int)std::lround(v); }

// Euclidean rhythm: k hits spread as evenly as possible over n steps, rotated by rot.
inline bool euclid(int i, int k, int n, int rot)
{
    if (k <= 0) return false;
    if (k >= n) return true;
    const int j = ((i + rot) % n + n) % n;
    return (j * k) % n < k;
}

// A style weight (digit) and a random number make a hit; more Density, more hits.
inline bool weighted(char digit, float u, float density)
{
    const int w = digit - '0';
    if (w >= 9) return true;
    if (w <= 0) return false;
    return u < (float)w / 8.0f * (0.35f + 1.3f * density);
}

// ---------------------------------------------------------------------------
// The plan: what all four tracks agree on without hearing each other. It is computed from
// shared inputs only, so every track builds the same one. Two sections: the form (from the
// harmony seed, the style and the length: Drums read this one only) and the chords (form
// plus scale and the shared harmony Complexity: pitched tracks).
// ---------------------------------------------------------------------------
enum Actor : uint8_t { kActFill = 1, kActBass = 2, kActChords = 4 };   // who marks the end of a phrase

struct FormPlan
{
    uint8_t cad[kMaxBars];             // 0..3: how strong the cadence at the end of the bar is
    uint8_t from[kMaxBars];            // first step that may leave the first bar's dice (16 = none)
    float   gain[kMaxBars];            // how much it may: 0 (A), 1.0 (A'), 1.6 (B)
    uint8_t act[kMaxBars];             // Actor bits
    uint8_t breath[kMaxBars];          // trailing steps the melody leaves empty
    uint8_t kickVar;                   // 0..2: the style's kick family
    uint8_t window;                    // 0..6 semitones: where the chord voicing window starts
    float   kickA[kStepsPerBar], kickAlt[kStepsPerBar], kickMut[kStepsPerBar];   // the kick's dice: Drums play them, Bass reads them
};

struct ChordPlan
{
    uint8_t root[2 * kMaxBars];        // per half bar: scale degree of the chord root
    uint8_t bass[2 * kMaxBars];        // the degree the bass treats as root (the tonic under a pedal)
    uint8_t tone[2 * kMaxBars][4];     // semitones above the tonic, 0..11: root, third or sus, fifth, colour (== root: octave double)
    bool    push[kMaxBars];            // steps 14 and 15 of the bar already belong to the next bar's chord
    uint8_t deg[8][4];                 // the same four tones for the chord on every scale degree
};

struct Plan { FormPlan form; ChordPlan chord; };

// Phrase forms over four bars: 0 = A, 1 = A' (new ending), 2 = B (changed from beat 2 on).
inline const uint8_t kForm[4][4]  = {{0, 1, 0, 2}, {0, 1, 2, 1}, {0, 2, 0, 2}, {0, 0, 0, 1}};
inline const uint8_t kFormFrom[3] = {16, 8, 4};
inline const float   kFormGain[3] = {0.0f, 1.0f, 1.6f};
inline const uint8_t kFillCap[4]  = {0, 2, 4, 8};       // longest fill in steps, by cadence strength

// Chord progressions as roles per bar: 0 Home, 1 Sub, 2 Turn, 3 Colour. Which scale degree plays a role
// depends on the scale: the first degree found at one of these distances (semitones) from the tonic.
enum ChordRole : uint8_t { kHome = 0, kSub, kTurn, kColr };
inline const uint8_t kTemplates[12][4] = {
    {0, 1, 0, 2}, {0, 3, 1, 2}, {0, 2, 3, 1}, {0, 0, 1, 2}, {0, 3, 0, 3}, {0, 2, 0, 2},
    {0, 1, 0, 1}, {0, 0, 0, 2}, {0, 0, 3, 3}, {0, 3, 1, 1}, {0, 1, 2, 2}, {0, 1, 3, 2}};
inline const int8_t kRoleSemis[4][4] = {{0, 0, 0, 0}, {5, 2, 3, 4}, {7, 10, 1, 11}, {9, 8, 3, 4}};
inline const int8_t kDarkTurn[4]     = {10, 8, 1, 7};

// Index whose cumulative weight first exceeds u * sum (0 when all weights are 0).
inline int pickW(const uint8_t* w, int n, float u)
{
    int sum = 0;
    for (int i = 0; i < n; ++i) sum += w[i];
    if (sum <= 0) return 0;
    const float target = u * (float)sum;
    int acc = 0, last = 0;
    for (int i = 0; i < n; ++i)
    {
        if (!w[i]) continue;
        acc += w[i];
        last = i;
        if (target < (float)acc) return i;
    }
    return last;
}

// Nearest note with pitch class pc (a tie goes down).
inline int pcNear(int note, int pc)
{
    const int up = ((pc - note) % 12 + 12) % 12;
    return up <= 5 ? note + up : note + up - 12;
}

inline int foldNote(int note)   // into the MIDI range by octaves, so it stays in the scale
{
    while (note > 127) note -= 12;
    while (note < 0) note += 12;
    return note;
}

inline bool inScale(const Scale& sc, int semis)
{
    const int pc = (semis % 12 + 12) % 12;
    for (int k = 0; k < sc.size; ++k) if (sc.semis[k] == pc) return true;
    return false;
}

// The chord a scale offers on degree d, found by semitone search so that it works in all twenty scales:
// a third (or a sus when the scale has none), a fifth, and the style's colour tone when the scale has it
// clear of the chord tones. Returns the quality: 2 = perfect fifth, +1 = real third.
inline int buildChord(const Scale& sc, int d, int colour, uint8_t tone[4])
{
    static const int8_t kThird[4] = {4, 3, 5, 2}, kFifth[3] = {7, 8, 6}, kAltFifth[2] = {5, 9}, kSeventh[4] = {10, 11, 9, 2};
    const int rootSemis = sc.semis[d];
    int third = 0, fifth = 0, col = 0;
    for (int x : kThird) if (inScale(sc, rootSemis + x)) { third = x; break; }
    for (int x : kFifth) if (inScale(sc, rootSemis + x)) { fifth = x; break; }
    if (!fifth) for (int x : kAltFifth) if (x != third && inScale(sc, rootSemis + x)) { fifth = x; break; }
    const int nCol = colour == kCol7 ? 4 : (colour == kCol9 ? 1 : 0);
    for (int k = 0; k < nCol; ++k)
    {
        const int x = colour == kCol7 ? kSeventh[k] : 2;
        if (!inScale(sc, rootSemis + x) || x == third || x == fifth) continue;
        if (x - 1 == 0 || x - 1 == third || x - 1 == fifth) continue;      // a semitone above a chord tone grates
        col = x;
        break;
    }
    tone[0] = (uint8_t)(rootSemis % 12);
    tone[1] = (uint8_t)((rootSemis + third) % 12);
    tone[2] = (uint8_t)((rootSemis + fifth) % 12);
    tone[3] = (uint8_t)((rootSemis + col) % 12);
    return 2 * (fifth == 7) + (third == 3 || third == 4);
}

inline void makePlan(const Settings& s, bool pitched, Plan& plan)
{
    const StyleDef& style = kStyles[clampi(s.style, 0, kNumStyles - 1)];
    const int bars = clampi(s.bars, 1, kMaxBars);
    plan = Plan();

    // ---- form ----
    FormPlan& f = plan.form;
    {
        Rng r(mix(s.harmonySeed ^ 0xF0A3C1D5u));
        const float formU = r.uni(), kickVarU = r.uni(), windowU = r.uni();
        float actU[kMaxBars][2], breathU[kMaxBars];
        for (auto& a : actU) { a[0] = r.uni(); a[1] = r.uni(); }
        for (float& v : breathU) v = r.uni();
        for (float& v : f.kickA) v = r.uni();
        for (float& v : f.kickAlt) v = r.uni();
        for (float& v : f.kickMut) v = r.uni();

        int top = 0;                   // the strongest cadence this length has: 0, 1, 2, 3 for 1, 2, 4, 8 bars
        while ((1 << top) < bars) ++top;
        const int form = pickW(style.formW, 4, formU);
        for (int b = 0; b < kMaxBars; ++b)
        {
            f.from[b] = kStepsPerBar;
            if (b >= bars) continue;
            int twos = 0;              // bars 2, 4, 8 end ever larger phrases
            while (((b + 1) >> twos & 1) == 0) ++twos;
            const int cad = b == bars - 1 ? top : (twos < top ? twos : top);
            f.cad[b] = (uint8_t)cad;
            const int code = kForm[form][b % 4];
            f.from[b] = kFormFrom[code];
            f.gain[b] = kFormGain[code];
            if (b >= 4)                // second half of eight bars: every bar ends differently
            {
                if (f.from[b] > 12) f.from[b] = 12;
                if (f.gain[b] < 1.0f) f.gain[b] = 1.0f;
            }
            if (bars == 1) f.from[b] = kStepsPerBar;
            if (cad >= 1)
            {
                const int first = pickW(style.actW, 3, actU[b][0]);
                f.act[b] = (uint8_t)(1 << first);
                if (cad == top && top >= 2)     // the big ending gets a second actor
                {
                    uint8_t rest[3] = {style.actW[0], style.actW[1], style.actW[2]};
                    rest[first] = 0;
                    if (rest[0] + rest[1] + rest[2] > 0) f.act[b] |= (uint8_t)(1 << pickW(rest, 3, actU[b][1]));
                }
                f.breath[b] = (uint8_t)(2 + (int)(breathU[b] * 3.0f));
            }
            else f.breath[b] = (bars == 1 && breathU[0] < 0.5f) ? 2 : 0;
        }
        f.kickVar = (uint8_t)clampi((int)(kickVarU * 3.0f), 0, 2);
        f.window = (uint8_t)clampi((int)(windowU * 7.0f), 0, 6);
    }
    if (!pitched) return;              // Drums never see the chords: Key, Scale and pitched Complexity leave them alone

    // ---- chords ----
    ChordPlan& ch = plan.chord;
    const Scale& sc = kScales[clampi(s.scale, 0, 19)];
    const HarmDef& h = style.h;
    int quality[8] = {}, minQ = 0;
    for (int d = 0; d < sc.size; ++d)
    {
        quality[d] = buildChord(sc, d, h.colour, ch.deg[d]);
        if (d > 0 && quality[d] >= 2) minQ = 2;        // scales without a single perfect fifth take what there is
    }
    uint8_t role[4][2] = {};           // first and second choice of degree per role; Home is the tonic
    bool taken[8] = {true};
    for (int k = kSub; k <= kColr; ++k)
    {
        const int8_t* list = (k == kTurn && h.darkTurn) ? kDarkTurn : kRoleSemis[k];
        int n = 0;
        for (int j = 0; j < 4 && n < 2; ++j)
            for (int d = 1; d < sc.size; ++d)
                if (sc.semis[d] == list[j] && quality[d] >= minQ && !taken[d]) { role[k][n++] = (uint8_t)d; break; }
        if (k == kColr)                // Colour takes any chord that is left
            for (int d = 1; d < sc.size && n < 2; ++d)
                if (quality[d] >= minQ && !taken[d] && !(n == 1 && role[k][0] == d)) role[k][n++] = (uint8_t)d;
        if (n == 1) role[k][1] = role[k][0];
        if (n > 0) taken[role[k][0]] = true;
    }

    Rng r(mix(s.harmonySeed ^ 0xC40D5EEDu));
    const float tplU = r.uni();
    float substU[4], rank[kMaxBars], halfU[kMaxBars];
    for (float& v : substU) v = r.uni();
    for (float& v : rank) v = r.uni();
    for (float& v : halfU) v = r.uni();
    const float pedalU = r.uni();

    const uint8_t* tpl = kTemplates[pickW(h.tplW, 12, tplU)];
    // Complexity reveals the progression bar by bar, in an order of its own: a bar not yet revealed holds the chord before it.
    float reveal = 3.0f * h.move * (s.harmonyComplexity - 0.05f);
    reveal = reveal < 0.0f ? 0.0f : (reveal > 1.0f ? 1.0f : reveal);
    auto degreeOf = [&](int e) { return role[tpl[e]][substU[e] < 0.25f ? 1 : 0]; };
    uint8_t chord[kMaxBars] = {};
    for (int b = 1; b < bars; ++b)
    {
        const int e = (b == bars - 1 && bars < 4) ? 3 : b % 4;
        chord[b] = rank[b] < reveal ? degreeOf(e) : chord[b - 1];
    }
    for (int b = 0; b < bars; ++b) ch.root[2 * b] = ch.root[2 * b + 1] = chord[b];
    if (bars == 8 && rank[7] < reveal)                 // the last bar of eight turns round, in two chords when it can
    {
        ch.root[14] = tpl[3] == kTurn ? role[kSub][0] : role[kTurn][0];
        ch.root[15] = role[kTurn][0];
    }
    if (bars == 1) ch.root[1] = rank[1] < 0.6f * reveal ? degreeOf(3) : 0;
    // High Complexity, in styles that like it: a passing chord on the second half of a bar, a step below the chord
    // it leads to (the Sub when that step makes no good chord).
    float passing = h.half * 1.8f * (s.harmonyComplexity - 0.45f);
    for (int b = 0; b < bars && bars > 1; ++b)
    {
        const int here = ch.root[2 * b], next = ch.root[2 * ((b + 1) % bars)];
        if (halfU[b] >= passing || next == here || ch.root[2 * b + 1] != here) continue;
        const int below = (next + sc.size - 1) % sc.size;
        const int pass = quality[below] >= 2 ? below : role[kSub][0];
        if (pass != here && pass != next) ch.root[2 * b + 1] = (uint8_t)pass;
    }
    // A push: when Chords mark the cadence and the chord changes, the last two 1/16s already belong to the next
    // chord, for all pitched tracks.
    for (int b = 0; b < bars; ++b)
        ch.push[b] = (f.act[b] & kActChords) && ch.root[2 * ((b + 1) % bars)] != ch.root[2 * b + 1];
    const bool pedal = pedalU < h.pedal;
    for (int sl = 0; sl < 2 * bars; ++sl)
    {
        ch.bass[sl] = pedal ? 0 : ch.root[sl];
        for (int k = 0; k < 4; ++k) ch.tone[sl][k] = ch.deg[ch.root[sl]][k];
    }
}

// The kick belongs to the plan, not to the Drums track: Drums play it with their Density, Bass asks with its
// own, and since no other knob enters, what Bass believes is always a subset or a superset of what Drums play.
inline float kickDie(const FormPlan& f, const StyleDef& style, int bar, int pos)
{
    return (pos >= f.from[bar] && f.kickMut[pos] < f.gain[bar] * 0.3f * style.vary) ? f.kickAlt[pos] : f.kickA[pos];
}
inline bool kickHit(const FormPlan& f, const StyleDef& style, int bar, int pos, float density)
{
    return weighted(style.d.kick[f.kickVar % 3][pos], kickDie(f, style, bar, pos), density);
}

// Which of a seed's dice a step reads. Every seed has two sets per bar, A and Alt. Beats 3 and 4 reuse the
// dice of beats 1 and 2 unless the style breaks the cell there, so a bar is built from a half-bar figure;
// later bars move single steps from A to Alt (from where and how readily is the shared phrase form), so a
// B bar is the A bar with changes, and at Complexity 0 every bar is the same.
inline int dieIndex(const FormPlan& f, const StyleDef& style, const float mut[kStepsPerBar], const float cell[kStepsPerBar / 2], int bar, int pos, float complexity)
{
    int q = (pos >= kStepsPerBar / 2 && cell[pos - kStepsPerBar / 2] >= style.cellBreak) ? pos - kStepsPerBar / 2 : pos;
    if (pos >= f.from[bar] && mut[pos] < f.gain[bar] * complexity * style.vary) q = kStepsPerBar + pos;
    return q;
}

// Which half-bar slot of the chord plan step i hears.
inline int chordAt(const Plan& plan, int i, int bars)
{
    const int bar = i / kStepsPerBar, pos = i % kStepsPerBar;
    if (pos >= 14 && plan.chord.push[bar]) return 2 * ((bar + 1) % bars);
    return 2 * bar + (pos >= kStepsPerBar / 2 ? 1 : 0);
}

constexpr int kChordDrop = 4;          // the voicing window starts this far below the tonic, to leave the tune its octave
// The notes of a chord hit, voice 1 first. Every tone keeps its place inside one octave (the window),
// so chords move by small steps, and voice n + 1 only ever adds a note.
inline int voiceChord(const uint8_t tone[4], const HarmDef& h, int window, int base, int voices, int pos, bool invert, int out[kMaxVoices])
{
    // Root position chords count from the tonic; the others fill an octave that starts a third below it, so
    // that their top stays under the tune in every key.
    const int lo = base + window - (h.voicing == kVoPlane ? 0 : kChordDrop);
    int pitch[4];
    if (h.voicing == kVoPlane)         // root position, moved in parallel
    {
        const int rootPitch = lo - ((lo - base - tone[0]) % 12 + 12) % 12;
        for (int k = 0; k < 4; ++k) pitch[k] = rootPitch + ((tone[k] - tone[0]) % 12 + 12) % 12;
    }
    else
        for (int k = 0; k < 4; ++k) pitch[k] = lo + ((base + tone[k] - lo) % 12 + 12) % 12;
    if (tone[3] == tone[0]) pitch[3] = pitch[0] + 12;                  // no colour tone: the root, an octave up
    if (h.voicing == kVoWindow && invert)                              // the bottom of the triad goes on top
    {
        int low = 0;
        for (int k = 1; k < 3; ++k) if (pitch[k] < pitch[low]) low = k;
        pitch[low] += 12;
    }
    if (h.voicing == kVoArp)           // one note per hit, walking through the chord
    {
        out[0] = pitch[h.voiceOrd[(pos / 2) % voices] & 3];
        return 1;
    }
    int n = 0;
    for (int v = 0; v < voices; ++v)
    {
        int note = pitch[h.voiceOrd[v] & 3];
        if (h.voicing == kVoSpread && v == 2) note += 12;
        bool dup = false;
        for (int q = 0; q < n; ++q) dup = dup || out[q] == note;
        if (!dup) out[n++] = note;
    }
    return n;
}

inline void finishStep(Step& st, int velocity, float gate)
{
    st.velocity = (uint8_t)clampi(velocity, 1, 127);
    for (int v = 0; v < st.count; ++v) st.vel[v] = st.velocity;
    st.gate = gate < 0.1f ? 0.1f : gate;
}

// One percussion line for the whole call: which of the 16 steps of a bar the pad plays.
// A row pad is decided per step by the caller; here only the Euclidean pads.
// The rotation is moved on until the line stays off the downbeat and off the backbeat, then
// Density reveals the candidates one by one in a fixed order (prio), so the line is nested.
inline uint16_t percLine(const PercDef& pd, const char* snareRow, int rot, const float prio[kStepsPerBar], float density, int more, uint16_t& candidates)
{
    candidates = 0;
    if (pd.row || pd.eN == 0) return 0;
    int best = rot, bestClash = 99;
    for (int k = 0; k < pd.eN && bestClash > 0; ++k)
    {
        int clash = 0;
        for (int pos = 0; pos < kStepsPerBar; ++pos)
            if (euclid(pos % pd.eN, pd.eK, pd.eN, rot + k) && (pos == 0 || snareRow[pos] == '9')) ++clash;
        if (clash < bestClash) { bestClash = clash; best = rot + k; }
    }
    int n = 0;
    for (int pos = 0; pos < kStepsPerBar; ++pos)
        if (euclid(pos % pd.eN, pd.eK, pd.eN, best)) { candidates |= (uint16_t)(1u << pos); ++n; }
    const int count = clampi(pd.lo + iround(density * (float)(pd.hi - pd.lo)) + more, 0, n);
    uint16_t plays = 0;
    for (int pos = 0; pos < kStepsPerBar; ++pos)
    {
        if (!(candidates >> pos & 1)) continue;
        int rank = 0;                  // candidates that come before this one
        for (int q = 0; q < kStepsPerBar; ++q)
            if ((candidates >> q & 1) && (prio[q] < prio[pos] || (prio[q] == prio[pos] && q < pos))) ++rank;
        if (rank < count) plays |= (uint16_t)(1u << pos);
    }
    return plays;
}

// ---------------------------------------------------------------------------
// Drums. Pads A1 kick, A2 snare/clap, A3 closed hat, A4 open hat follow the style;
// A5..A8 are percussion. t.voices says how many pads play. Every sound has its own
// loudness, the open hat chokes the closed one, and a step holds at most four sounds.
// ---------------------------------------------------------------------------
inline void generateDrums(Pattern& p, uint32_t seed, uint32_t anchor, const Settings& s, const TrackSettings& t, const Plan& plan)
{
    const StyleDef& style = kStyles[clampi(s.style, 0, kNumStyles - 1)];
    const DrumDef& dd = style.d;
    const float d = s.density, c = s.complexity;
    const int pads = clampi(t.voices, 1, kDrumPads);

    Rng r(seed);
    float hat[2 * kStepsPerBar], snr[2 * kStepsPerBar], oh[2 * kStepsPerBar], sh[2 * kStepsPerBar];
    float mut[kStepsPerBar], cell[kStepsPerBar / 2], stepU[kMaxSteps], xk[2 * kStepsPerBar];
    for (float& v : hat) v = r.uni();
    for (float& v : snr) v = r.uni();
    for (float& v : oh) v = r.uni();
    for (float& v : sh) v = r.uni();
    for (float& v : mut) v = r.uni();
    for (float& v : cell) v = r.uni();
    for (float& v : stepU) v = r.uni();
    for (float& v : xk) v = r.uni();

    // The anchor: what stays through every variation of this beat. A new beat gets a new one.
    Rng a(anchor ^ 0xD2u);
    const int snareVar = a.range(0, 2);
    const int fillKind = pickW(dd.fillW, 5, a.uni());
    const float burstU = a.uni(), burstStartU = a.uni(), burstLenU = a.uni();
    // Hat bursts (Trap above all): a short run of 1/16s into beat 3 or into the bar line, by turns.
    int burstLen = burstU < dd.burst ? iround((float)(2 + (int)(burstLenU * 3.0f)) * 3.0f * c) : 0;
    if (burstLen > 4) burstLen = 4;
    enum { kBuild = 0, kToms, kStutter, kDrop, kHats };
    const char* snareRow = dd.snare[snareVar];
    const char* kickRow = dd.kick[plan.form.kickVar % 3];

    uint16_t percPlays[4] = {}, percCand[4] = {};
    for (int j = 0; j < 4; ++j)
    {
        const int rot = a.range(0, kStepsPerBar - 1);
        float prio[kStepsPerBar];
        for (float& v : prio) v = a.uni();
        percPlays[j] = percLine(dd.perc[j], snareRow, rot, prio, d, 0, percCand[j]);
    }

    // Closed hats come in by tiers: Density lowers the bar a weight has to clear, and that alone says how many
    // hats a bar has. Complexity loosens the tiers: it decides which steps get them, so a looser beat is not
    // a thinner one.
    bool hatOn[kMaxSteps];
    for (int bar = 0; bar * kStepsPerBar < p.length; ++bar)
    {
        float score[kStepsPerBar];
        int n = 0;
        for (int pos = 0; pos < kStepsPerBar; ++pos)
        {
            const int hw = dd.hat[pos] - '0';
            n += hw > 0 && (float)hw >= 8.5f - 8.0f * d;
            score[pos] = hw > 0 ? (float)hw + (hat[dieIndex(plan.form, style, mut, cell, bar, pos, c)] - 0.5f) * 4.0f * c : -1.0f;
        }
        for (int pos = 0; pos < kStepsPerBar; ++pos)
        {
            int rank = 0;              // steps that come before this one
            for (int q = 0; q < kStepsPerBar; ++q) rank += score[q] > score[pos] || (score[q] == score[pos] && q < pos);
            hatOn[bar * kStepsPerBar + pos] = score[pos] >= 0.0f && rank < n;
        }
    }

    for (int i = 0; i < p.length; ++i)
    {
        Step& st = p.steps[i];
        st = Step();
        const int bar = i / kStepsPerBar, pos = i % kStepsPerBar;
        const int ui = dieIndex(plan.form, style, mut, cell, bar, pos, c);
        // A fill only where the plan says Drums mark the cadence; its length grows with the cadence and Complexity.
        const int cap = kFillCap[plan.form.cad[bar] & 3];
        int fillLen = (plan.form.act[bar] & kActFill) ? iround(2.0f * c * (float)cap * dd.fillAmt) : 0;
        if (fillLen > cap) fillLen = cap;
        const bool inFill = pos >= kStepsPerBar - fillLen;

        int vel[kDrumPads] = {};       // 0 = the pad is silent on this step
        const int kd = kickRow[pos] - '0';
        if (kickHit(plan.form, style, bar, pos, d)) vel[0] = i == 0 ? 124 : (kd >= 9 ? 116 : 92);
        // The kick line itself is shared and stays, but each seed may add a soft kick of its own in the gaps,
        // so a re-roll of the Drums is heard in the kick too.
        // Only on steps 3, 7, 10, 11, 14, 15, and never next to a kick of the line: that would be a flam.
        else if (kd == 0 && (0xCC88u >> pos & 1) && kickRow[pos - 1] == '0' && kickRow[(pos + 1) % kStepsPerBar] == '0'
                 && xk[ui] < dd.kickExtra * 2.5f * c) vel[0] = 88;
        const int sd = snareRow[pos] - '0';
        if (weighted(snareRow[pos], snr[ui], d)) vel[1] = sd >= 9 ? 110 : (sd >= 5 ? 96 : 34 + (int)(18.0f * stepU[i]));   // low weights are ghost notes
        if (pads >= 4 && weighted(dd.openHat[pos], oh[ui], d)) vel[3] = 100;
        if (hatOn[i] && !vel[3])
            vel[2] = kHatVel[dd.hatAccent & 3][pos % 4] + iround((stepU[i] - 0.5f) * 6.0f);
        if (burstLen > 0 && !vel[3])
        {
            const int end = ((burstStartU < 0.5f) != ((bar & 1) == 1)) ? kStepsPerBar / 2 : kStepsPerBar;
            if (pos >= end - burstLen && pos < end) vel[2] = 58 + 12 * (pos - (end - burstLen));
        }
        for (int j = 0; j < 4; ++j)
        {
            const bool paired = dd.pair && j == 3;                    // pad A8 plays the odd candidates of pad A7's line
            const PercDef& pd = dd.perc[paired ? 2 : j];
            if (pd.row)
            {
                if (weighted(pd.row[pos], sh[ui], d)) vel[4 + j] = pd.row[pos] >= '5' ? 90 : 46;
                continue;
            }
            const uint16_t plays = percPlays[paired ? 2 : j], cand = percCand[paired ? 2 : j];
            if (!(plays >> pos & 1)) continue;
            if (dd.pair && j >= 2)
            {
                int index = 0;
                for (int q = 0; q < pos; ++q) index += cand >> q & 1;
                if ((index & 1) != (j == 3 ? 1 : 0)) continue;
            }
            vel[4 + j] = (plays & ((1u << pos) - 1u)) == 0 ? 84 : 60;  // the line's first hit of the bar leads
        }

        if (inFill)
        {
            // The groove makes room: hats and percussion stop, the backbeat stays (not in a drop), the kick
            // stays unless the gesture is about taking it away.
            for (int pad = 2; pad < kDrumPads; ++pad) vel[pad] = 0;
            const bool drop = fillKind == kDrop && fillLen >= 2;
            if (sd < 9 || drop) vel[1] = 0;
            if (fillKind == kStutter || drop) vel[0] = 0;
            const int idx = pos - (kStepsPerBar - fillLen);
            const int v = 60 + 58 * (idx + 1) / fillLen;               // a fill gets louder towards the bar line
            // The 1/8s always play, Density fills in the 1/16s between (a one-step fill is a pickup and always plays).
            // All by position in the bar, so a fill that grows with Complexity keeps what it already had.
            float odd = 2.5f * d - 0.5f;
            odd = odd < 0.0f ? 0.0f : (odd > 1.0f ? 1.0f : odd);
            const bool plays = !(pos & 1) || fillLen == 1 || stepU[i] < odd;
            int pad = -1, fv = v;
            // A drop needs room to be heard as one: a single step is a pickup, played as the build would.
            switch (fillKind == kDrop && fillLen < 2 ? (int)kBuild : fillKind)
            {
                case kBuild:   if (plays) pad = 1; break;
                case kToms:    if (plays) pad = pads >= 8 ? (2 * idx < fillLen ? 7 : 6) : 1; break;
                case kStutter: if (plays && pos % 4 != 1) pad = pos % 4 == 2 ? 1 : 0; break;
                // All stops, then the snare and the open hat together on the last 1/8: the open hat alone is what
                // many grooves play there anyway.
                case kDrop:    if (pos == kStepsPerBar - 2) { pad = 1; fv = 100; if (pads >= 4) vel[3] = 104; } break;
                default:       if (plays) pad = 2; break;
            }
            if (pad >= 0 && pad < pads)
            {
                if (fv > vel[pad]) vel[pad] = fv;
                st.flags = kFFill;
            }
        }

        int top = 0;
        static const int kOrder[kDrumPads] = {0, 1, 3, 2, 4, 5, 6, 7};  // what stays when a step is full
        for (int k = 0; k < kDrumPads && st.count < kMaxVoices; ++k)
        {
            const int pad = kOrder[k];
            if (pad >= pads || vel[pad] <= 0) continue;
            const int v = clampi(vel[pad], 1, 127);
            st.note[st.count] = (uint8_t)(kDrumBaseNote + pad);
            st.vel[st.count++] = (uint8_t)v;
            if (v > top) top = v;
        }
        st.velocity = (uint8_t)top;
        st.gate = 0.5f;
    }
}

// ---------------------------------------------------------------------------
// Bass, Chords, Melody. All three read the chord plan, so they are in the same chord.
// Random numbers are drawn in a fixed order whatever the settings are, so a knob
// changes how they are used, not which numbers a step gets.
// ---------------------------------------------------------------------------
constexpr float kNoHit = 1.0e9f;
// The bass register, in semitones from the track's tonic: roots more than kBassFold above it go an octave down,
// and no note of the line leaves kBassLo..kBassHi (an octave pop on a high root would: it stays on the root).
constexpr int kBassFold = 7, kBassLo = -4, kBassHi = 16;
constexpr int kTuneLo = -10, kTuneHi = 14;   // the tune's register, in semitones from its tonic

// A weight and a random number as one figure: the step plays when it is below 0.35 + 1.3 * Density
// (the same rule as weighted()). The step with the lowest figure is the first that Density brings in.
inline float hitNeed(int w, float u) { return w >= 9 ? -1.0f : (w <= 0 ? kNoHit : u * 8.0f / (float)w); }

inline void generatePitched(Pattern& p, uint32_t seed, uint32_t anchor, const Settings& s, const TrackSettings& t, const Plan& plan)
{
    const Scale& sc = kScales[clampi(s.scale, 0, 19)];
    const StyleDef& style = kStyles[clampi(s.style, 0, kNumStyles - 1)];
    const FormPlan& form = plan.form;
    const ChordPlan& ch = plan.chord;
    const float d = s.density, c = s.complexity;
    const int role = t.role;
    const int voices = clampi(t.voices, 1, kMaxVoices);
    // Key moves every track by less than an octave: the keys above F sharp sit below C, not above it.
    const int base = 60 + (s.tonic > 6 ? s.tonic - 12 : s.tonic) + 12 * t.octave;
    const int bars = p.length / kStepsPerBar;

    // The anchor: what stays through every variation of this idea.
    Rng a(anchor ^ (uint32_t)role);
    const int rowVar = a.range(0, 2);
    float setPrio[8];
    for (float& v : setPrio) v = a.uni();
    const char* weights = role == kBass ? style.b.row[rowVar] : (role == kChords ? style.chords : style.lead);
    const char* kickRow = style.d.kick[form.kickVar % 3];
    const int kickRel = role == kBass ? style.b.kickRel : (int)kFreeKick;

    Rng r(seed);
    // [0] function / contour  [1] choice  [2] velocity and gate jitter  [3] long  [4] rhythm  [5] root flip / inversion
    float u[6][2 * kStepsPerBar], mut[kStepsPerBar], cell[kStepsPerBar / 2];
    for (auto& m : u) for (float& v : m) v = r.uni();
    for (float& v : mut) v = r.uni();
    for (float& v : cell) v = r.uni();
    const float answerU = r.uni();     // Melody: whether the answering bars sit above or below the first

    auto degNote = [&](int degree)     // scale degree (may be negative or beyond one octave) to a note
    {
        int oct = degree / sc.size, idx = degree % sc.size;
        if (idx < 0) { idx += sc.size; --oct; }
        return base + 12 * oct + sc.semis[idx];
    };

    // How many steps a note that starts on step i may ring before the harmony leaves it (the loop wraps round).
    // note < 0: until the root changes (the bass's own root for Bass). Otherwise: while the note is in the triad.
    auto roomFor = [&](int i, int note)
    {
        const uint8_t* roots = role == kBass ? ch.bass : ch.root;
        const int here = roots[chordAt(plan, i, bars)];
        const int pc = ((note - base) % 12 + 12) % 12;
        int n = 1;
        for (; n < kStepsPerBar; ++n)
        {
            const int sl = chordAt(plan, (i + n) % p.length, bars);
            if (note < 0 ? roots[sl] != here : (pc != ch.tone[sl][0] && pc != ch.tone[sl][1] && pc != ch.tone[sl][2])) break;
        }
        return n;
    };

    // Melody: one contour over the scale, mostly by step. A leap is followed by a step back, as a singer
    // would. The odds do not depend on any knob: Complexity only scales how far the line travels.
    int walk[2 * kStepsPerBar];
    bool member[8] = {true};           // the scale degrees the tune uses: few of them make a hook
    if (role == kMelody)
    {
        int deg = 0, back = 0;
        for (int i = 0; i < 2 * kStepsPerBar; ++i)
        {
            if (i == kStepsPerBar) { deg = walk[kStepsPerBar / 2]; back = 0; }   // the Alt line branches off mid-bar
            const float x = u[0][i];
            const int sign = u[1][i] < 0.5f ? -1 : 1;
            const int size = x < 0.25f ? 0 : (x < 0.80f ? 1 : (x < 0.95f ? 2 : 3));
            int move = sign * size;
            if (back && move) { move = back; back = 0; }
            else if (size >= 2) back = -sign;
            deg += move;
            if (deg > 6) deg = 12 - deg;
            if (deg < -6) deg = -12 - deg;
            walk[i] = deg;
        }
        // The pitch set belongs to the anchor: the tonic, then the degrees in the anchor's order, the tonic
        // chord's third and fifth first more often than not. Complexity adds degrees, never swaps them.
        int n = style.setSize - 1 + iround(3.0f * c);
        if (n > sc.size) n = sc.size;
        float key[8];
        for (int k = 1; k < sc.size; ++k)
            key[k] = setPrio[k] - ((sc.semis[k] == ch.deg[0][1] || sc.semis[k] == ch.deg[0][2]) ? 0.25f : 0.0f);
        for (int k = 1; k < sc.size; ++k)
        {
            int rank = 1;              // the tonic comes first
            for (int q = 1; q < sc.size; ++q) if (key[q] < key[k] || (key[q] == key[k] && q < k)) ++rank;
            member[k] = rank < n;
        }
    }

    bool hit[kMaxSteps], lock[kMaxSteps];
    int keep[kMaxBars];                // per bar: the step that plays at any Density (-1: a drone that holds through)
    const float level = 0.35f + 1.3f * d;
    for (int bar = 0; bar < bars; ++bar)
    {
        // Where the tune ends its phrase: the last beat before its breath, in bars that end one.
        const int breath = role == kMelody ? form.breath[bar] : 0;
        const bool cadence = role == kMelody && form.cad[bar] >= 1;
        int first = -1, firstEnd = -1; // the steps Density would bring in first: in the bar, in the phrase ending
        float firstNeed = kNoHit, firstEndNeed = kNoHit;
        bool any = false, anyEnd = false;
        for (int pos = 0; pos < kStepsPerBar; ++pos)
        {
            const int i = bar * kStepsPerBar + pos;
            lock[i] = false;
            if (s.type == kDrone)      // a drone starts again where the chord changes, and now and then on a bar line
            {
                const uint8_t* roots = role == kBass ? ch.bass : ch.root;
                hit[i] = i == 0 || roots[chordAt(plan, i, bars)] != roots[chordAt(plan, i - 1, bars)]
                      || (pos == 0 && u[3][bar] < 0.35f + 0.5f * d);
                any = true;
                continue;
            }
            const int ui = dieIndex(form, style, mut, cell, bar, pos, c);
            int w = weights[pos] - '0';
            float need;
            if (role == kBass)
            {
                // The bass knows the kick from the plan. It lands with it (lock), leaves the kicks alone
                // (dodge: half as likely there; ban: never there), or goes its own way.
                const int kd = kickRow[pos] - '0';
                if (kickRel == kLockKick && kd > 0)
                {
                    need = hitNeed(kd, kickDie(form, style, bar, pos));
                    lock[i] = need < level;
                }
                else
                {
                    // Dodging halves a weight: a step that was likely is now possible, and none is lost for good.
                    const bool dodge = kd >= 9 && (kickRel == kDodgeKick || kickRel == kDodgeHard);
                    if (dodge && (w < 9 || (kickRel == kDodgeHard && pos != 0))) w = (w + 1) / 2;
                    const bool banned = kickRel == kBanKick && kd > 0;     // not even where the kick only may play
                    if ((form.act[bar] & kActBass) && pos >= 12 && w < 9) w = w + 3 > 8 ? 8 : w + 3;   // a pickup into the next bar
                    need = banned ? kNoHit : hitNeed(w, u[4][ui]);
                }
            }
            else if (role == kChords && (form.act[bar] & kActChords) && pos >= 12)
                need = pos == 14 ? -1.0f : kNoHit;                    // one stab ahead of the bar line, nothing around it
            else if (role == kMelody && pos >= kStepsPerBar - breath)
                need = kNoHit;                                         // the tune breathes
            else
                need = hitNeed(w, u[4][ui]);
            hit[i] = need < level;
            any = any || hit[i];
            if (need < firstNeed) { firstNeed = need; first = pos; }
            if (cadence && pos >= 12 - breath && pos < kStepsPerBar - breath)
            {
                anyEnd = anyEnd || hit[i];
                if (firstEnd < 0 || need < firstEndNeed) { firstEndNeed = need; firstEnd = pos; }
            }
        }
        // A bar the dice left empty gets the step that more Density would have brought first, so the hits stay
        // nested. The same for a phrase ending without a note: the tune has to land somewhere.
        if (cadence && !anyEnd && firstEnd >= 0 && s.type != kDrone) { hit[bar * kStepsPerBar + firstEnd] = true; any = true; }
        if (!any) hit[bar * kStepsPerBar + (first < 0 ? 0 : first)] = true;
        keep[bar] = s.type == kDrone ? (bar == 0 ? 0 : -1) : (first < 0 ? 0 : first);
    }

    for (int i = 0; i < p.length; ++i)
    {
        Step& st = p.steps[i];
        st = Step();
        const int bar = i / kStepsPerBar, pos = i % kStepsPerBar;
        if (!hit[i]) continue;
        const int ui = dieIndex(form, style, mut, cell, bar, pos, c);
        const int slot = chordAt(plan, i, bars);
        const uint8_t* tone = ch.tone[slot];
        // A long note lasts a beat or two and swallows the notes that would have fallen inside it. Which ones
        // is only known once the mosaic is put together, so the step just says so (resolve, in generateIdea).
        // It may only be a note that belongs to the chord for as long as it rings, so the die only asks: the
        // role decides below whether this note can be one, and how far it may reach (never over a chord change).
        // The tune rolls that die once per beat, and for beats 3 and 4 by themselves: rolled per note and with the
        // half-bar figure, a busy tune at Long 25 % was two held notes a bar.
        const int longDie = role == kMelody ? (ui >= kStepsPerBar ? kStepsPerBar : 0) + (pos & ~3) : ui;
        const bool wantLong = s.type != kDrone && u[3][longDie] < s.longNotes;
        bool longNote = wantLong;
        float gate = 1.0f + (u[2][ui] - 0.5f) * 0.6f * c;             // Complexity loosens the lengths a little
        int flags = (s.type == kDrone ? (int)kFDrone : 0) | (pos == keep[bar] ? (int)kFKeep : 0);
        int room = kStepsPerBar;       // steps this note may ring before the harmony leaves it

        int notes[kMaxVoices], count = 0, baseVelocity = 90;
        switch (role)
        {
            case kBass:
            {
                // The bass thinks in functions of its root: the root itself, or one other note a step keeps for good
                // (its octave, fifth, third, the scale step below, the one above). Complexity only decides how often
                // a step shows its other note, so turning it never swaps one colour for another.
                const BassDef& bd = style.b;
                const int R = ch.bass[slot], nextR = ch.bass[2 * ((bar + 1) % bars)];
                const uint8_t* own = ch.deg[R];                        // the chord on the bass's own root
                const int third = (own[1] - own[0] + 12) % 12, fifth = (own[2] - own[0] + 12) % 12;
                // Roots stay within a fifth of the tonic: those far above it are played below it. Their neighbours are
                // counted from there, and whatever the line does, it stays in kBassLo..kBassHi around the tonic.
                auto rootOf = [&](int degree) { return base + sc.semis[degree] - (sc.semis[degree] > kBassFold ? 12 : 0); };
                auto beside = [&](int degree, int dir)                 // the scale note above or below a root
                {
                    const int up = ((sc.semis[(degree + dir + sc.size) % sc.size] - sc.semis[degree]) % 12 + 12) % 12;
                    return rootOf(degree) + (dir > 0 ? up : up - 12);
                };
                const int root = rootOf(R);
                // Anchors are roots: the downbeat, a note with the kick, the first note of a new chord.
                // A note with the kick in the last 1/8 before a new root is no anchor: the old root there would undo
                // the approach notes before it.
                const bool leaving = pos >= 14 && nextR != R && !ch.push[bar];
                const bool anchor = pos == 0 || (lock[i] && !leaving) || (slot > 0 && pos == kStepsPerBar / 2 && ch.bass[slot - 1] != R)
                                 || (pos == 14 && ch.push[bar]);
                const bool approach = !anchor && pos >= 12 && nextR != R && !ch.push[bar];
                int note = root;
                bool passing = false;                                  // a scale step beside the root: kept as short as an approach note
                if (approach)
                {
                    const float x = u[1][ui];
                    const int nextRoot = rootOf(nextR);
                    const uint8_t* next = ch.deg[nextR];
                    float ant = bd.ant * c / 0.35f;
                    if (ant > bd.ant) ant = bd.ant;
                    if (pos >= 14 && u[5][ui] < ant) note = nextRoot;                       // the next root, early
                    else if (x < 0.45f) note = beside(nextR, -1);                            // lead in from below
                    else if (x < 0.75f) note = beside(nextR, 1);                             // or from above
                    else note = nextRoot + (next[2] - next[0] + 12) % 12 - 12;               // or from its fifth, below it
                }
                else if (!anchor)
                {
                    uint8_t w[5] = {bd.pal[1], bd.pal[2], bd.pal[3], bd.pal[4], bd.pal[5]};
                    if (pos % 4 == 2) w[0] = (uint8_t)(w[0] > 127 ? 255 : 2 * w[0]);       // the octave likes the offbeat
                    float amount = (float)(100 - bd.pal[0]) * 0.01f * c / 0.35f;
                    if (amount > 0.95f) amount = 0.95f;
                    if (pos & 1) amount *= 1.0f - bd.oddRoot;
                    if (w[0] + w[1] + w[2] + w[3] + w[4] > 0 && u[5][ui] < amount)
                        switch (pickW(w, 5, u[0][ui]))
                        {
                            case 0: note = root + 12; break;
                            case 1: note = root + fifth - (bd.lowFifth ? 12 : 0); break;
                            case 2: note = root + third; break;
                            case 3: note = beside(R, -1); passing = true; break;
                            default: note = beside(R, 1); passing = true; break;
                        }
                }
                if (note > base + kBassHi) note -= 12;
                if (note < base + kBassLo) note += 12;
                // Extra voices stack on the root: its fifth, the octave, the fifth above that, which stays clear low down.
                const int stack[kMaxVoices] = {note, root + fifth, root + 12, root + 12 + fifth};
                for (int v = 0; v < voices; ++v) notes[count++] = stack[v];
                baseVelocity = 100 + (lock[i] ? 8 : 0);
                flags |= kFStyleLen;   // the style says how long an anchor, a body note and an approach note ring
                gate = bd.len[anchor ? 0 : (approach || passing ? 2 : 1)];
                // A long bass note is a root on beat 1 or 3: anything else that rings for half a bar is a wrong
                // note or a hole in the groove. No bass note is asked to ring past its own root either.
                room = roomFor(i, -1);
                if (pos % (kStepsPerBar / 2) != 0 || note != root) longNote = false;
                if (gate > (float)room) gate = (float)room;
                break;
            }
            case kChords:
                count = voiceChord(tone, style.h, form.window, base, voices, pos, u[5][ui] < 0.4f * c, notes);
                baseVelocity = 80;
                room = roomFor(i, -1);
                break;
            default:   // Melody
            {
                // The contour picks a degree, the pitch set pulls it to the nearest of its notes (a tie goes down).
                // Beats 3 and 4 repeat the rhythm of beats 1 and 2 but go on along the contour: the same pitches
                // again would leave a bar two or three notes to sing.
                int deg = iround((float)walk[ui >= kStepsPerBar ? ui : pos] * (0.72f + 0.8f * c));
                // Question and answer: every second bar restates the figure a scale step away (the strong notes then
                // find their chord tones from there). Where the phrase form lets a bar differ, that part moves too,
                // a B part further. A figure that moves with the chords only answers where the chord has not moved.
                {
                    float lift = 3.0f * c;                             // none at Complexity 0: every bar the same
                    if (lift > 1.3f) lift = 1.3f;
                    float gain = pos >= form.from[bar] ? form.gain[bar] : 0.0f;
                    const bool answers = (bar & 1) && (!style.sequenced || ch.root[2 * bar] == ch.root[2 * bar - 2]);
                    if (answers && gain < 1.0f) gain = 1.0f;
                    deg += (answerU < 0.5f ? -1 : 1) * iround(gain * lift);
                }
                int oct = deg / sc.size, idx = deg % sc.size;
                if (idx < 0) { idx += sc.size; --oct; }
                for (int dist = 0; dist <= sc.size; ++dist)
                {
                    if (idx - dist >= 0 && member[idx - dist]) { idx -= dist; break; }
                    if (idx + dist == sc.size) { idx = 0; ++oct; break; }
                    if (idx + dist < sc.size && member[idx + dist]) { idx += dist; break; }
                }
                deg = oct * sc.size + idx;
                // The figure moves with the chords, by the shorter way: a chord on the fifth or above takes it down.
                if (style.sequenced) deg += ch.root[slot] > sc.size / 2 ? ch.root[slot] - sc.size : ch.root[slot];
                int note = degNote(deg);

                auto nearestOf = [&](const uint8_t* tones, int n, bool tieUp)
                {
                    int best = note, bestDist = 99;
                    for (int k = 0; k < n; ++k)
                    {
                        const int cand = pcNear(note, (base + tones[k]) % 12);
                        const int dd = cand > note ? cand - note : note - cand;
                        if (dd < bestDist || (dd == bestDist && cand != best && tieUp)) { bestDist = dd; best = cand; }
                    }
                    return best;
                };
                const int breath = form.breath[bar];
                if (s.type != kDrone && form.cad[bar] >= 1 && pos >= 12 - breath && pos < kStepsPerBar - breath)
                {
                    // A phrase ending rests on the chord that is sounding, whatever chord that is: the last bar on its
                    // root or third (it settles), the others on its fifth or third (they stay open). A tone that sits a
                    // semitone above another note of the chord would grate when held, so the next one stands in.
                    static const uint8_t kSettle[3] = {0, 1, 2}, kOpen[3] = {2, 1, 0};
                    const uint8_t* order = bar == bars - 1 ? kSettle : kOpen;
                    uint8_t rest[2] = {tone[order[0]], tone[order[0]]};
                    int n = 0;
                    for (int k = 0; k < 3 && n < 2; ++k)
                    {
                        const int cand = tone[order[k]];
                        bool grates = false;
                        for (int q = 0; q < 4; ++q) grates = grates || (cand - tone[q] + 12) % 12 == 1;
                        if (!grates) rest[n++] = (uint8_t)cand;
                    }
                    note = nearestOf(rest, n > 0 ? n : 1, false);
                    if (!wantLong) flags |= kFHold;
                }
                else if (pos % 4 == 0 || wantLong)                     // strong notes are chord tones: the nearest of root, third, fifth
                    note = nearestOf(tone, 3, u[1][ui] >= 0.5f);
                else                                                   // a weak note a semitone above a chord tone gives way to it
                    for (int k = 0; k < 3; ++k)
                        if (((note - 1 - base - tone[k]) % 12 + 12) % 12 == 0) { --note; break; }
                // The tune stays within two octaves around its tonic, whatever the contour, the chords and the
                // answer add up to: a note beyond comes back by an octave.
                if (note > base + kTuneHi) note -= 12;
                if (note < base + kTuneLo) note += 12;
                notes[count++] = note;
                room = roomFor(i, note);   // a common tone may ring through a chord change, any other note may not
                // Extra voices: the next chord tones below the note.
                for (int below = note - 1; count < voices && below > note - 24; --below)
                    for (int k = 0; k < 4; ++k)
                        if (((below - base - tone[k]) % 12 + 12) % 12 == 0) { notes[count++] = below; break; }
                baseVelocity = 96;
                break;
            }
        }
        for (int v = 0; v < count; ++v)
        {
            const int note = foldNote(notes[v]);
            bool dup = false;
            for (int q = 0; q < st.count; ++q) dup = dup || st.note[q] == note;
            if (!dup) st.note[st.count++] = (uint8_t)note;
        }
        int span = 0;
        if (longNote)                  // 4, 6 or 8 steps, as far as the harmony lets it; under two steps it is no long note
        {
            span = 4 + 2 * (int)(u[1][ui] * 3.0f);
            if (span > room) span = room;
            if (span >= 2) flags |= kFLong; else span = 0;
        }
        else if (flags & kFHold) span = room < 6 ? room : 6;
        st.flags = (uint8_t)flags;
        st.span = (uint8_t)span;
        // Loudness: the style's accent pattern over the bar, and a little life.
        finishStep(st, baseVelocity + (style.accent[pos] - '5') * 4 + iround((u[2][ui] - 0.5f) * 8.0f), gate);
        st.gate = gate;                // may be below finishStep's floor: it is a factor, resolve turns it into steps
    }
}

inline void generate(Pattern& p, uint32_t seed, uint32_t anchor, const Settings& s, const TrackSettings& t, const Plan& plan)
{
    p.length = clampi(s.bars, 1, kMaxBars) * kStepsPerBar;
    if (t.role == kDrums) generateDrums(p, seed, anchor, s, t, plan);
    else generatePitched(p, seed, anchor, s, t, plan);
}

inline void resetIdea(Idea& d, uint32_t seed)
{
    d = Idea();
    d.seeds[0] = seed;
    d.anchor = mix(seed ^ 0x51A7C0DEu);
}

// Gives about `fraction` of the steps to a new seed. Reuses the slot that owns the fewest steps.
// The new seed takes positions of the bar, the same ones in every bar, so bars that were alike stay alike and
// the phrase keeps its form. Only the ending (the second half of every fourth bar) is taken step by step.
// The first half of a bar is the part the ear holds on to, so it changes least.
inline void varyIdea(Idea& d, uint32_t seed, float fraction)
{
    int use[kSlots] = {};
    for (int i = 0; i < kMaxSteps; ++i) ++use[d.owner[i] % kSlots];
    int slot = 0;
    for (int q = 1; q < kSlots; ++q) if (use[q] < use[slot]) slot = q;
    Rng r(seed ^ 0xA5C31E77u);
    float posU[kStepsPerBar];
    for (float& v : posU) v = r.uni();
    for (int i = 0; i < kMaxSteps; ++i)
    {
        const int bar = i / kStepsPerBar, pos = i % kStepsPerBar;
        const bool late = pos >= kStepsPerBar / 2;
        const float own = r.uni();
        const bool take = ((late && bar % 4 == 3) ? own : posU[pos]) < fraction * (late ? 1.25f : 0.75f);
        if (take || d.owner[i] == slot) d.owner[i] = (uint8_t)slot;
    }
    d.seeds[slot] = seed;
}

inline uint32_t ideaHash(const Idea& d)
{
    uint32_t h = 0x7E55E7A1u;
    for (int q = 0; q < kSlots; ++q) h = mix(h ^ d.seeds[q]);
    h = mix(h ^ d.anchor);
    for (int i = 0; i < kMaxSteps; ++i) h = mix(h + d.owner[i]);
    return h;
}

// Builds the track's pattern from its mosaic. `scratch` is working memory.
// Steps from different seeds only meet here, so what depends on the neighbours is settled here, on the
// finished mosaic: what a long note swallows, how long every note really is, and that something plays.
inline void generateIdea(Pattern& p, Pattern& scratch, const Idea& d, const Settings& settings, const TrackSettings& track)
{
    // The engine's one entry point: whatever the caller passes, everything below sees values in range. A value
    // that is not a number fails the first test and becomes 0.
    Settings s = settings;
    TrackSettings t = track;
    const auto unit = [](float v) { return !(v >= 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v); };
    s.density = unit(s.density); s.complexity = unit(s.complexity); s.gate = unit(s.gate);
    s.longNotes = unit(s.longNotes); s.harmonyComplexity = unit(s.harmonyComplexity);
    s.tonic = clampi(s.tonic, 0, 11);
    t.octave = clampi(t.octave, -5, 5);
    t.role = clampi(t.role, kDrums, kMelody);
    p.length = clampi(s.bars, 1, kMaxBars) * kStepsPerBar;
    Plan plan;
    makePlan(s, t.role != kDrums, plan);
    bool used[kSlots] = {};
    for (int i = 0; i < p.length; ++i) used[d.owner[i] % kSlots] = true;
    // Every seed has in each bar one note that plays at any Density, but a seed may not own the step it is on:
    // a sparse track could lose whole bars to the mosaic. So the first seed's note stays wherever the owner of
    // its step plays nothing there. It is the same step at every Density, so more Density still only adds notes.
    Step kept[kMaxBars];
    int keptAt[kMaxBars], nKept = 0;
    bool firstSeed = true;
    for (int q = 0; q < kSlots; ++q)
    {
        if (!used[q]) continue;
        generate(scratch, d.seeds[q], d.anchor, s, t, plan);
        for (int i = 0; i < p.length; ++i)
        {
            if (d.owner[i] % kSlots == q) p.steps[i] = scratch.steps[i];
            else if (firstSeed && (scratch.steps[i].flags & kFKeep) && nKept < kMaxBars) { keptAt[nKept] = i; kept[nKept++] = scratch.steps[i]; }
        }
        firstSeed = false;
    }
    if (t.role == kDrums) return;      // a drum step is spliced whole and has no length to settle
    for (int k = 0; k < nKept; ++k) if (!p.steps[keptAt[k]].count) p.steps[keptAt[k]] = kept[k];

    int first = -1;
    for (int i = 0; i < p.length; ++i)
    {
        Step& st = p.steps[i];
        if (!st.count) continue;
        if (first < 0) first = i;
        if (st.flags & kFLong)
            for (int j = i + 1; j < i + st.span && j < p.length; ++j) p.steps[j] = Step();
    }
    if (first < 0) return;

    const float g = s.gate;
    int next = first + p.length;       // the loop wraps round: the last note runs to the first
    for (int i = p.length - 1; i >= 0; --i)
    {
        Step& st = p.steps[i];
        if (!st.count) continue;
        const float dist = (float)(next - i), full = dist * 0.98f;
        next = i;
        float len;
        if (st.flags & kFDrone) len = dist - 0.05f;
        else if (st.flags & (kFLong | kFHold)) len = (dist < (float)st.span ? dist : (float)st.span) * 0.98f;
        else if (st.flags & kFStyleLen)
        {
            // Up to 60 % Gate scales the style's own length; above, it stretches towards the next note.
            const float own = st.gate < full ? st.gate : full;
            if (g <= 0.6f) { len = st.gate * g / 0.6f; if (len > full) len = full; }
            else len = own + (full - own) * (g - 0.6f) / 0.4f;
        }
        else if (g >= 0.99f) len = full;   // at 100 % every note runs into the next one, whatever the Complexity
        else { len = dist * g * st.gate; if (len > full) len = full; }
        st.gate = len >= 0.1f ? len : 0.1f;   // also catches a length that is not a number
    }
}

} // namespace kwatuor
