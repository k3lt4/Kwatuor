// Prints generated patterns as text and checks invariants.
// Build: c++ -std=c++17 -O2 -I../src dump.cpp -o dump     Run: ./dump  (checks)   ./dump show [style] [bars = 4]  (grids)
#include "engine.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
using namespace kwatuor;

// The engine must not allocate: every allocation made while it runs is counted.
static long allocs = 0;
void* operator new(std::size_t n) { ++allocs; if (void* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

static const char* kRoleNames[] = {"Drums", "Bass", "Chords", "Melody"};
static int failures = 0;
static void fail(const char* format, ...)
{
    if (++failures > 60) return;       // the count goes on, the printing stops
    va_list args; va_start(args, format);
    std::printf("FAIL "); std::vprintf(format, args); std::printf("\n");
    va_end(args);
}

static bool sameStep(const Step& a, const Step& b)
{
    return a.count == b.count && !std::memcmp(a.note, b.note, sizeof a.note) && !std::memcmp(a.vel, b.vel, sizeof a.vel)
        && a.velocity == b.velocity && a.flags == b.flags && a.span == b.span && a.gate == b.gate;
}
static bool samePattern(const Pattern& a, const Pattern& b)
{
    if (a.length != b.length) return false;
    for (int i = 0; i < a.length; ++i) if (!sameStep(a.steps[i], b.steps[i])) return false;
    return true;
}
static bool hasNote(const Step& st, int note) { for (int v = 0; v < st.count; ++v) if (st.note[v] == note) return true; return false; }
static int onsetDiff(const Pattern& a, const Pattern& b) { int n = 0; for (int i = 0; i < a.length; ++i) n += (a.steps[i].count > 0) != (b.steps[i].count > 0); return n; }
// The steps a drum fill may use: the end of a bar where the plan lets Drums mark the cadence.
static bool fillZone(const Plan& plan, int i) { const int bar = i / 16, pos = i % 16; return (plan.form.act[bar] & kActFill) && pos >= 16 - kFillCap[plan.form.cad[bar] & 3]; }

// A grid a musician can read. Drums: one line per pad, X loud, x medium, - ghost. Pitched: note names, = held.
static void showPattern(const Pattern& p, const Settings& s, const TrackSettings& t, uint32_t seed)
{
    static const char* kPadNames[kDrumPads] = {"kick", "snare", "hat", "open", "perc1", "perc2", "perc3", "perc4"};
    static const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    std::printf("%s seed %u %s (%s)\n", kStyles[s.style].name, seed, kRoleNames[t.role], kScales[s.scale].name);
    if (t.role == kDrums)
    {
        for (int pad = 0; pad < kDrumPads; ++pad)
        {
            std::printf("  %-6s", kPadNames[pad]);
            for (int i = 0; i < p.length; ++i)
            {
                if (i % 16 == 0) std::printf("|");
                char ch = i % 4 == 0 ? '.' : ' ';
                const Step& st = p.steps[i];
                for (int v = 0; v < st.count; ++v)
                    if (st.note[v] == kDrumBaseNote + pad) ch = st.vel[v] >= 100 ? 'X' : (st.vel[v] >= 62 ? 'x' : '-');
                std::printf("%c", ch);
            }
            std::printf("|\n");
        }
        return;
    }
    if (t.role == kBass)               // the plan, once: the form and the chords the pitched tracks share
    {
        Plan plan; makePlan(s, true, plan);
        std::printf("  plan   kick %d", plan.form.kickVar);
        for (int b = 0; b < s.bars; ++b)
            std::printf(" | bar %d: %s/%s%s cad %d from %d act %c%c%c breath %d", b + 1, kNoteNames[plan.chord.tone[2 * b][0]], kNoteNames[plan.chord.tone[2 * b + 1][0]], plan.chord.push[b] ? " push" : "",
                        plan.form.cad[b], plan.form.from[b], plan.form.act[b] & kActFill ? 'F' : '-', plan.form.act[b] & kActBass ? 'B' : '-', plan.form.act[b] & kActChords ? 'C' : '-', plan.form.breath[b]);
        std::printf("\n");
    }
    for (int v = t.voices - 1; v >= 0; --v)
    {
        std::printf("  %-6s", v == 0 ? "note" : "");
        int held = 0;
        for (int i = 0; i < p.length; ++i)
        {
            if (i % 16 == 0) std::printf("|");
            const Step& st = p.steps[i];
            if (st.count) held = (int)(st.gate + 0.5f);
            if (st.count > v) std::printf("%-2s%d", kNoteNames[st.note[v] % 12], st.note[v] / 12 - 1);
            else std::printf(held > 0 && v == 0 ? " = " : " . ");
            --held;
        }
        std::printf("|\n");
    }
}

static Pattern p, p2, ref, scratch;

// Everything a single pattern must satisfy, whatever the knobs.
static void checkPattern(const Pattern& pat, const Settings& s, const TrackSettings& t, const Plan& plan, const char* what, uint32_t seed)
{
    const int role = t.role;
    int hits = 0;
    for (int i = 0; i < pat.length; ++i)
    {
        const Step& st = pat.steps[i];
        if (role == kDrums && !fillZone(plan, i) && kStyles[s.style].d.kick[plan.form.kickVar][i % 16] == '9' && !hasNote(st, kDrumBaseNote))
            fail("%s: a kick of the plan is missing: style %d seed %u step %d", what, s.style, seed, i);
        if (!st.count) continue;
        ++hits;
        if (st.count > (role == kDrums ? kMaxVoices : t.voices)) fail("%s: voices seed %u role %d", what, seed, role);
        if (!(st.gate > 0.0f) || st.gate > (float)pat.length) fail("%s: gate seed %u role %d step %d gate %f", what, seed, role, i, st.gate);
        int top = 0, mask = 0;
        for (int v = 0; v < st.count; ++v)
        {
            if (st.vel[v] < 1 || st.vel[v] > 127) fail("%s: note velocity seed %u role %d step %d", what, seed, role, i);
            if (st.vel[v] > top) top = st.vel[v];
            const int n = st.note[v];
            if (role == kDrums)
            {
                if (n < kDrumBaseNote || n >= kDrumBaseNote + t.voices) fail("%s: pad %d", what, n);
                else mask |= 1 << (n - kDrumBaseNote);
                continue;
            }
            const int pc = ((n - s.tonic) % 12 + 12) % 12;
            if (!inScale(kScales[s.scale], pc)) fail("%s: out of scale: style %d seed %u role %d step %d note %d", what, s.style, seed, role, i, n);
            if (role == kChords)       // a chord is made of the plan's chord tones
            {
                const uint8_t* tone = plan.chord.tone[chordAt(plan, i, s.bars)];
                if (pc != tone[0] && pc != tone[1] && pc != tone[2] && pc != tone[3]) fail("%s: chord note outside the plan's chord: style %d seed %u step %d note %d", what, s.style, seed, i, n);
            }
        }
        if (top != st.velocity) fail("%s: velocity is not the loudest note: seed %u role %d step %d", what, seed, role, i);
        if ((mask & 12) == 12) fail("%s: closed and open hat together: style %d seed %u step %d", what, s.style, seed, i);
        if (role == kDrums && (st.flags & kFFill) && (s.bars == 1 || !fillZone(plan, i))) fail("%s: fill note outside a fill zone: style %d seed %u step %d", what, s.style, seed, i);
        if (role != kDrums && (st.flags & kFLong))
            for (int j = i + 1; j < i + st.span && j < pat.length; ++j)
                if (pat.steps[j].count) fail("%s: a note inside a long note: style %d seed %u role %d step %d", what, s.style, seed, role, j);
    }
    if (hits == 0) fail("%s: empty: style %d seed %u role %d type %d dens %.1f", what, s.style, seed, role, s.type, s.density);
}

int main(int argc, char** argv)
{
    const bool show = argc > 1;
    const int onlyStyle = argc > 2 ? std::atoi(argv[2]) : -1, showBars = argc > 3 ? std::atoi(argv[3]) : 4;
    int patterns = 0;
    long engineAllocs = 0;

    for (int s9 = 0; s9 < kNumStyles; ++s9)
    {
        {                                   // every rhythm row is 16 digits
            const StyleDef& sd = kStyles[s9];
            const char* rows[32]; int n = 0;
            for (int k = 0; k < 3; ++k) { rows[n++] = sd.d.kick[k]; rows[n++] = sd.d.snare[k]; rows[n++] = sd.b.row[k]; }
            rows[n++] = sd.d.hat; rows[n++] = sd.d.openHat; rows[n++] = sd.chords; rows[n++] = sd.lead; rows[n++] = sd.accent;
            for (const PercDef& pd : sd.d.perc) if (pd.row) rows[n++] = pd.row;
            for (int k = 0; k < n; ++k)
            {
                bool ok = std::strlen(rows[k]) == 16;
                for (const char* c = rows[k]; *c; ++c) ok = ok && *c >= '0' && *c <= '9';
                if (!ok) fail("style %s row %d \"%s\" is not 16 digits", sd.name, k, rows[k]);
            }
        }
        if (onlyStyle >= 0 && s9 != onlyStyle) continue;

        if (show)                           // grids, with the plugin's default knobs
        {
            static const float kDens[kTracks] = {0.60f, 0.50f, 0.40f, 0.55f}, kGate[kTracks] = {0.50f, 0.60f, 1.00f, 0.50f}, kLong[kTracks] = {0.0f, 0.15f, 0.0f, 0.25f};
            static const int kVoices[kTracks] = {kDrumPads, 1, 3, 1};
            for (uint32_t seed = 1; seed <= 2; ++seed)
                for (int role = 0; role < kNumRoles; ++role)
                {
                    Settings s; s.style = s9; s.scale = (int)(seed % 2); s.bars = showBars; s.density = kDens[role]; s.gate = kGate[role]; s.longNotes = kLong[role];
                    s.harmonySeed = seed * 31 + (uint32_t)s9;
                    TrackSettings t; t.role = role; t.voices = kVoices[role];
                    Idea idea; resetIdea(idea, seed * 977 + role);
                    generateIdea(p, scratch, idea, s, t);
                    showPattern(p, s, t, seed);
                }
            continue;
        }

        // ---- 1. every scale x type x role, plain and spliced with up to 11 variations ----
        for (uint32_t seed = 1; seed <= 300u; ++seed)
            for (int scale = 0; scale < 20; ++scale)
                for (int type = 0; type < kNumTypes; ++type)
                    for (int role = 0; role < kNumRoles; ++role)
                    {
                        Settings s; s.style = s9; s.scale = scale; s.type = type; s.bars = 1 << (seed % 4); s.tonic = seed % 12;
                        s.density = (float)(seed % 11) / 10.0f; s.complexity = (float)((seed * 7) % 11) / 10.0f; s.gate = (float)(seed % 5) / 4.0f;
                        s.harmonySeed = seed * 31; s.harmonyComplexity = s.complexity; s.longNotes = (float)(seed % 4) / 3.0f;
                        TrackSettings t; t.role = role; t.voices = 1 + (seed + role) % (role == kDrums ? 8 : 4); t.octave = (int)(seed % 7) - 3;
                        Idea idea; resetIdea(idea, seed * 977 + role);
                        const uint32_t anchor = idea.anchor;
                        for (uint32_t v = 0; v < seed % 12; ++v) varyIdea(idea, seed * 131 + v, v % 2 ? 0.12f : 0.4f);   // up to 11 variations: more than there are slots
                        if (idea.anchor != anchor) fail("a variation changed the anchor: seed %u", seed);
                        const long before = allocs;
                        generateIdea(p, scratch, idea, s, t);
                        generateIdea(p2, scratch, idea, s, t);
                        Plan plan; makePlan(s, true, plan);
                        engineAllocs += allocs - before;
                        ++patterns;
                        if (!samePattern(p, p2)) fail("two runs differ: style %d seed %u role %d", s9, seed, role);
                        checkPattern(p, s, t, plan, "sweep", seed);
                        if (s.bars == 1 && (plan.form.breath[0] > 2 || plan.form.act[0])) fail("one bar has a cadence: seed %u", seed);
                    }

        // ---- 2. what the knobs promise ----
        for (uint32_t seed = 1; seed <= 24u; ++seed)
            for (int role = 0; role < kNumRoles; ++role)
                for (int type = 0; type < (role == kDrums ? 1 : (int)kNumTypes); ++type)
                {
                    Settings s; s.style = s9; s.scale = (int)((seed * 3 + (uint32_t)s9) % 20); s.type = type; s.bars = 1 << (seed % 4); s.tonic = 0;
                    s.density = 0.3f + 0.05f * (float)(seed % 9); s.complexity = (float)((seed * 7) % 11) / 10.0f; s.gate = 0.6f;
                    s.harmonySeed = seed * 53 + 7; s.harmonyComplexity = 0.1f * (float)(seed % 8); s.longNotes = 0.0f;
                    TrackSettings t; t.role = role; t.voices = role == kDrums ? 4 + (int)(seed % 5) : 1 + (int)(seed % 4); t.octave = 0;
                    Idea idea; resetIdea(idea, seed * 613 + role);
                    const Idea plain = idea;
                    for (uint32_t v = 0; v < seed % 4; ++v) varyIdea(idea, seed * 17 + v, v % 2 ? 0.12f : 0.4f);
                    Plan plan; makePlan(s, true, plan);
                    const long before = allocs;

                    // Density: more of it only ever adds hits (Long off: a long note swallows its neighbours by design).
                    {
                        Settings k = s;
                        for (int step = 0; step <= 10; ++step)
                        {
                            k.density = 0.1f * (float)step;
                            generateIdea(p, scratch, idea, k, t);
                            ++patterns;
                            checkPattern(p, k, t, plan, "density", seed);
                            if (step > 0)
                                for (int i = 0; i < p.length; ++i)
                                    if (ref.steps[i].count && !p.steps[i].count) { fail("Density %d0 %% loses a hit: style %d seed %u role %d type %d step %d", step, s9, seed, role, type, i); break; }
                            ref = p;
                        }
                    }
                    // Complexity 0: every bar the same, apart from the phrase endings (the second half of a bar that
                    // ends a phrase) and from the kicks the shared plan itself varies (those of weight 1..8).
                    if (type == kLoop)
                    {
                        Settings k = s; k.complexity = 0.0f; k.harmonyComplexity = 0.0f;
                        makePlan(k, true, plan);
                        generateIdea(ref, scratch, plain, k, t);
                        const char* kickRow = kStyles[s9].d.kick[plan.form.kickVar];
                        const bool kickBound = role == kDrums || (role == kBass && kStyles[s9].b.kickRel == kLockKick);
                        for (int i = 16; i < ref.length; ++i)
                        {
                            const int bar = i / 16, pos = i % 16;
                            if (plan.form.cad[bar] >= 1 && pos >= 8) continue;
                            const bool varied = kickBound && kickRow[pos] > '0' && kickRow[pos] < '9';
                            const Step& a = ref.steps[pos];
                            const Step& b = ref.steps[i];
                            bool same = a.count == b.count && !std::memcmp(a.note, b.note, sizeof a.note);
                            if (varied && role == kBass) same = true;
                            if (varied && role == kDrums)
                            {
                                int ma = 0, mb = 0;
                                for (int v = 0; v < a.count; ++v) ma |= 1 << (a.note[v] - kDrumBaseNote);
                                for (int v = 0; v < b.count; ++v) mb |= 1 << (b.note[v] - kDrumBaseNote);
                                same = (ma | 1) == (mb | 1);
                            }
                            if (!same) { fail("Complexity 0: bar %d differs from bar 1: style %d seed %u role %d step %d", bar + 1, s9, seed, role, pos); break; }
                        }
                        // Raising Complexity moves away from that pattern and does not come back, and no small turn is a cliff.
                        // A bar is built from a half-bar figure, so one die that flips can show on two steps of every bar
                        // (12.5 % of the steps): a cliff is more than a fifth of them.
                        int last = 0;
                        const int cliff = ref.length / 5 > 3 ? ref.length / 5 : 3;
                        p2 = ref;
                        for (int step = 1; step <= 100; ++step)
                        {
                            k.complexity = 0.01f * (float)step;
                            generateIdea(p, scratch, plain, k, t);
                            ++patterns;
                            if (onsetDiff(p, p2) > cliff) { fail("Complexity %d %%: one turn changes %d of %d steps: style %d seed %u role %d", step, onsetDiff(p, p2), p.length, s9, seed, role); break; }
                            p2 = p;
                            if (step % 5) continue;
                            // Drums: by the sounds on a step (a lost hat and a new soft kick are two changes, not none),
                            // outside the fills (a fill replaces the end of its bar whatever was there).
                            int dist = 0;
                            for (int i = 0; i < p.length; ++i)
                            {
                                if (role != kDrums) { dist += (p.steps[i].count > 0) != (ref.steps[i].count > 0); continue; }
                                int ma = 0, mb = 0;
                                for (int v = 0; v < p.steps[i].count; ++v) ma |= 1 << (p.steps[i].note[v] - kDrumBaseNote);
                                for (int v = 0; v < ref.steps[i].count; ++v) mb |= 1 << (ref.steps[i].note[v] - kDrumBaseNote);
                                dist += !fillZone(plan, i) && ma != mb;
                            }
                            // (one die that moves to its Alt and lands where it was shows in every bar: that much is allowed)
                            if (dist < last - 2 - s.bars) { fail("Complexity %d %% comes back towards Complexity 0 (%d after %d): style %d seed %u role %d", step, dist, last, s9, seed, role); break; }
                            last = dist;
                        }
                        makePlan(s, true, plan);
                    }
                    if (role == kDrums)
                    {
                        // Key, Scale and the pitched tracks' Complexity are nothing to the Drums.
                        generateIdea(ref, scratch, idea, s, t);
                        Settings k = s; k.tonic = 7; k.scale = (s.scale + 5) % 20; k.harmonyComplexity = 1.0f - s.harmonyComplexity;
                        generateIdea(p, scratch, idea, k, t);
                        if (!samePattern(p, ref)) fail("Key, Scale or harmony Complexity changed the Drums: style %d seed %u", s9, seed);
                        engineAllocs += allocs - before;
                        continue;
                    }
                    // Gate: no note gets shorter as it rises; at 100 % a note runs into the next one (a long note: to the end of its span).
                    {
                        Settings k = s; k.longNotes = 0.3f;
                        for (int step = 1; step <= 20; ++step)
                        {
                            k.gate = 0.05f * (float)step;
                            generateIdea(p, scratch, idea, k, t);
                            ++patterns;
                            if (step > 1)
                                for (int i = 0; i < p.length; ++i)
                                    if (p.steps[i].count && p.steps[i].gate < ref.steps[i].gate - 1e-4f) { fail("Gate %d %% shortens a note: style %d seed %u role %d step %d", step * 5, s9, seed, role, i); break; }
                            ref = p;
                        }
                        for (int i = 0; i < p.length; ++i)
                        {
                            const Step& st = p.steps[i];
                            if (!st.count || (st.flags & (kFDrone | kFHold))) continue;
                            int dist = 1;
                            while (dist < p.length && !p.steps[(i + dist) % p.length].count) ++dist;
                            if ((st.flags & kFLong) && dist > st.span) dist = st.span;   // a long note stops with its span: the chord may change there
                            if (std::fabs(st.gate - 0.98f * (float)dist) > 1e-3f) { fail("Gate 100 %%: a note does not reach the next: style %d seed %u role %d step %d", s9, seed, role, i); break; }
                        }
                    }
                    // Notes: one more voice only adds a note (the Retro arpeggio plays one note at a time: not checked).
                    if (!(role == kChords && kStyles[s9].h.voicing == kVoArp))
                    {
                        TrackSettings tv = t; tv.voices = 1;
                        generateIdea(ref, scratch, idea, s, tv);
                        for (int n = 2; n <= kMaxVoices; ++n)
                        {
                            tv.voices = n;
                            generateIdea(p, scratch, idea, s, tv);
                            checkPattern(p, s, tv, plan, "notes", seed);
                            for (int i = 0; i < p.length; ++i)
                                for (int v = 0; v < ref.steps[i].count; ++v)
                                    if (!hasNote(p.steps[i], ref.steps[i].note[v])) { fail("Notes %d drops a note of Notes %d: style %d seed %u role %d step %d", n, n - 1, s9, seed, role, i); i = p.length; break; }
                            ref = p;
                        }
                    }
                    // Key and Oct are plain transpositions. Key k moves everything by k, or by k - 12 above F sharp.
                    generateIdea(ref, scratch, idea, s, t);
                    static const int kShifts[15] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, -24, -12, 12, 24};
                    for (int semis : kShifts)
                    {
                        Settings k = s; TrackSettings tk = t;
                        int shift = semis;
                        if (semis > 0 && semis < 12) { k.tonic = semis; if (semis > 6) shift = semis - 12; } else tk.octave = semis / 12;
                        generateIdea(p, scratch, idea, k, tk);
                        bool ok = true;
                        for (int i = 0; i < p.length && ok; ++i)
                        {
                            ok = p.steps[i].count == ref.steps[i].count;
                            for (int v = 0; v < ref.steps[i].count && ok; ++v) ok = p.steps[i].note[v] == ref.steps[i].note[v] + shift;
                        }
                        if (!ok) { fail("a shift of %d semitones is not a transposition: style %d seed %u role %d", semis, s9, seed, role); break; }
                    }
                    engineAllocs += allocs - before;
                }
    }
    if (engineAllocs) fail("the engine allocated %ld times", engineAllocs);
    std::printf("%d patterns, %d failures\n", patterns, failures);
    return failures ? 1 : 0;
}
