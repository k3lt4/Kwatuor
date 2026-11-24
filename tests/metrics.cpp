// Measures what the generator does musically, per style, and checks it against first-setting ranges.
// Build: c++ -std=c++17 -O2 -I../src metrics.cpp -o metrics     Run: ./metrics [baseline cost in microseconds]
// 500 new ideas per style in Minor and 500 in Maj Penta, 4 bars, Dens 60, Cmplx 35, Gate 60, Long 0.
// The width of the tune is measured apart, the way it is heard first: the plugin's default knobs (Dens 55,
// Cmplx 35, Long 25), 500 ideas in Major and 500 in Minor.
// It also builds against an older engine.h (no KWATUOR_ENGINE_VERSION), to compare the two and to measure the
// cost baseline: run that build first and pass the cost it prints to this one.
#include "engine.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <utility>
using namespace kwatuor;

// What the tracks are supposed to agree on, read from the engine the same way a track reads it.
struct Harmony
{
    int  pcs[kMaxSteps][4];            // pitch classes of the chord a step hears: root, third, fifth, colour (or the root again)
    int  bassRoot[kMaxBars];           // pitch class the bass treats as root at the start of the bar
    int  barRoot[kMaxBars];            // scale degree of the chord at the start of the bar
    bool cadence[kMaxSteps];           // the melody's phrase endings
    bool fill[kMaxSteps];              // steps a drum fill may take over
};

#ifdef KWATUOR_ENGINE_VERSION
static bool lockStyle(int style)   { return kStyles[style].b.kickRel == kLockKick; }
static bool windowStyle(int style) { return kStyles[style].h.voicing == kVoWindow; }
static bool burstStyle(int style)  { return kStyles[style].d.burst > 0.0f; }
static float moveOf(int style)     { return kStyles[style].h.move; }
static void harmony(const Settings& s, Harmony& h)
{
    Plan plan; makePlan(s, true, plan);
    const Scale& sc = kScales[s.scale];
    for (int i = 0; i < s.bars * 16; ++i)
    {
        const int bar = i / 16, pos = i % 16, slot = chordAt(plan, i, s.bars);
        for (int k = 0; k < 4; ++k) h.pcs[i][k] = (plan.chord.tone[slot][k] + s.tonic) % 12;
        h.cadence[i] = plan.form.cad[bar] >= 1 && pos >= 12 - plan.form.breath[bar] && pos < 16 - plan.form.breath[bar];
        h.fill[i] = (plan.form.act[bar] & kActFill) && pos >= 16 - kFillCap[plan.form.cad[bar] & 3];
    }
    for (int b = 0; b < s.bars; ++b) { h.barRoot[b] = plan.chord.root[2 * b]; h.bassRoot[b] = (sc.semis[plan.chord.bass[2 * b]] + s.tonic) % 12; }
}
#else
static bool lockStyle(int style)   { return style == 2 || style == 3 || style == 4 || style == 7 || style == 8 || style == 9; }
static bool windowStyle(int style) { return style >= 2 && style <= 7; }
static bool burstStyle(int)        { return false; }
static float moveOf(int style)     { return kStyles[style].move; }
static void harmony(const Settings& s, Harmony& h)
{
    const Scale& sc = kScales[s.scale];
    int roots[kMaxBars]; barRoots(s, roots);
    for (int i = 0; i < s.bars * 16; ++i)
    {
        for (int k = 0; k < 4; ++k) h.pcs[i][k] = (sc.semis[(roots[i / 16] + 2 * (k % 3)) % sc.size] + s.tonic) % 12;
        h.cadence[i] = false; h.fill[i] = false;
    }
    for (int b = 0; b < s.bars; ++b) { h.barRoot[b] = roots[b]; h.bassRoot[b] = (sc.semis[roots[b] % sc.size] + s.tonic) % 12; }
}
#endif

static const int kVoicesOf[kTracks] = {kDrumPads, 1, 3, 1};
static const char* kRoleNames[kTracks] = {"Drums", "Bass", "Chords", "Melody"};
static Pattern pat[kTracks], spliced, scratch;

static Settings settingsFor(int style, int scale, int bars, float complexity, float longNotes, uint32_t n)
{
    Settings s; s.style = style; s.scale = scale; s.bars = bars; s.tonic = 0;
    s.density = 0.6f; s.complexity = complexity; s.gate = 0.6f; s.longNotes = longNotes;
    s.harmonySeed = mix(7919u * (uint32_t)style + n + 1u); s.harmonyComplexity = complexity;
    return s;
}
static void build(const Settings& s, uint32_t n, int variations = 0, int only = -1)
{
    for (int tr = 0; tr < kTracks; ++tr)
    {
        if (only >= 0 && tr != only) continue;
        TrackSettings t; t.role = tr; t.voices = kVoicesOf[tr];
        Idea idea; resetIdea(idea, mix(s.harmonySeed + 101u * (uint32_t)(tr + 1)));
        for (int v = 0; v < variations; ++v) varyIdea(idea, mix(n * 31u + (uint32_t)v + 5u), v % 2 ? 0.12f : 0.4f);
        generateIdea(variations ? spliced : pat[tr], scratch, idea, s, t);
    }
}
static int hatsOutsideFills(const Pattern& p, const Harmony& h)
{
    int n = 0;
    for (int i = 0; i < p.length; ++i)
        for (int v = 0; v < p.steps[i].count && !h.fill[i]; ++v) n += p.steps[i].note[v] == kDrumBaseNote + 2;
    return n;
}
static bool isPc(const int* pcs, int n, int note) { for (int k = 0; k < n; ++k) if (pcs[k] == note % 12) return true; return false; }
static bool sameBar(const Pattern& p, int a, int b, int steps = 16)
{
    for (int i = 0; i < steps; ++i)
    {
        const Step& x = p.steps[a * 16 + i]; const Step& y = p.steps[b * 16 + i];
        if (x.count != y.count || std::memcmp(x.note, y.note, x.count)) return false;
    }
    return true;
}
static int distinctBars(const Pattern& p, int steps = 16)
{
    int n = 0;
    for (int b = 0; b < p.length / 16; ++b) { bool seen = false; for (int a = 0; a < b; ++a) seen = seen || sameBar(p, a, b, steps); n += !seen; }
    return n;
}
static unsigned barMask(const Pattern& p, int bar, int note = -1)
{
    unsigned m = 0;
    for (int i = 0; i < 16; ++i)
    {
        const Step& st = p.steps[bar * 16 + i];
        bool hit = note < 0 ? st.count > 0 : false;
        for (int v = 0; v < st.count; ++v) hit = hit || st.note[v] == note;
        if (hit) m |= 1u << i;
    }
    return m;
}

static int failures = 0;
static const char* verdict(bool ok) { if (!ok) ++failures; return ok ? "PASS" : "FAIL"; }
static double pct(long a, long b) { return b ? 100.0 * (double)a / (double)b : 100.0; }   // nothing to judge: nothing wrong

int main(int argc, char** argv)
{
    const double baseline = argc > 1 ? std::atof(argv[1]) : 0.0;
    const int kIdeas = 500;
    static const int kScalesUsed[2] = {1, 4};   // Minor, Maj Penta
    static Harmony h;
#ifdef KWATUOR_ENGINE_VERSION
    std::printf("engine version %d\n", KWATUOR_ENGINE_VERSION);
#else
    std::printf("older engine\n");
#endif

    for (int style = 0; style < kNumStyles; ++style)
    {
        long strong = 0, strongTone = 0, strongS = 0, strongToneS = 0, strongOut = 0, strongToneOut = 0, mel = 0, melTone = 0;
        long lastTone = 0, lastRest = 0, lastGrates = 0, hats0 = 0, hats80 = 0, sameBefore = 0, sameAfter = 0, patterns = 0, bars = 0, awayBars = 0, changed = 0, changedRoot = 0, changedL = 0, changedRootL = 0;
        long kicks = 0, kicksBass = 0, distinct[kTracks] = {}, distinct0[kTracks] = {}, ideas8 = 0, differ8 = 0, pcsSum = 0;
        long tunes = 0, narrow = 0, strongD = 0, strongToneD = 0, motifSteps = 0, motifSame = 0;
        long moveHist[64] = {}, moves = 0;
        std::set<unsigned> bassRhythms;
        std::set<std::pair<unsigned, unsigned>> beats;

        for (int n = 0; n < kIdeas; ++n)
            for (int scaleIndex = 0; scaleIndex < 2; ++scaleIndex)
            {
                const uint32_t id = (uint32_t)(n * 2 + scaleIndex);
                Settings s = settingsFor(style, kScalesUsed[scaleIndex], 4, 0.35f, 0.0f, id);
                harmony(s, h);
                build(s, id);
                ++patterns;

                // Melody against the chords.
                const Pattern& m = pat[kMelody];
                int last = -1, lastAt = 0;
                for (int i = 0; i < m.length; ++i)
                {
                    const Step& st = m.steps[i];
                    if (!st.count) continue;
                    const bool tone = isPc(h.pcs[i], 3, st.note[0]);
                    if (!h.cadence[i]) { ++mel; melTone += tone; }   // a phrase ending is a chord tone by rule: it says nothing about the line
                    if (i % 4 == 0) { ++strong; strongTone += tone; if (!h.cadence[i]) { ++strongOut; strongToneOut += tone; } }
                    last = st.note[0]; lastAt = i;
                }
                // The tune's last note: on the chord that sounds there, best on its root or third, and never a
                // semitone above one of the chord's notes.
                lastTone += last >= 0 && isPc(h.pcs[lastAt], 3, last);
                lastRest += last >= 0 && isPc(h.pcs[lastAt], 2, last);
                for (int k = 0; k < 4 && last >= 0; ++k) if ((last - h.pcs[lastAt][k] + 120) % 12 == 1) { ++lastGrates; break; }
                build(s, id, 3, kMelody);
                for (int i = 0; i < spliced.length; i += 4)
                    if (spliced.steps[i].count) { ++strongS; strongToneS += isPc(h.pcs[i], 3, spliced.steps[i].note[0]); }

                // A variation keeps the phrase form: bars that repeated the first bar still do, on every track.
                for (int tr = 0; tr < kTracks; ++tr)
                {
                    for (int b = 1; b < 4; ++b) sameBefore += sameBar(pat[tr], 0, b);
                    build(s, id, 1, tr);
                    for (int b = 1; b < 4; ++b) sameAfter += sameBar(spliced, 0, b);
                }

                // Harmony movement, and the bass under it.
                for (int b = 1; b < 4; ++b) { ++bars; awayBars += h.barRoot[b] != 0; }
                for (int pass = 0; pass < 2; ++pass)
                {
                    if (pass == 1) { Settings sl = s; sl.longNotes = 0.25f; build(sl, id, 0, kBass); }
                    for (int b = 0; b < 4; ++b)
                    {
                        const Step& st = pat[kBass].steps[b * 16];
                        if (h.bassRoot[b] == h.bassRoot[(b + 3) % 4] || !st.count) continue;
                        const bool root = st.note[0] % 12 == h.bassRoot[b];
                        if (pass == 0) { ++changed; changedRoot += root; } else { ++changedL; changedRootL += root; }
                    }
                }
                build(s, id, 0, kBass);

                // Kick and bass.
                for (int i = 0; i < pat[kDrums].length; ++i)
                {
                    bool kick = false;
                    for (int v = 0; v < pat[kDrums].steps[i].count; ++v) kick = kick || pat[kDrums].steps[i].note[v] == kDrumBaseNote;
                    if (kick) { ++kicks; kicksBass += pat[kBass].steps[i].count > 0; }
                }
                for (int tr = 0; tr < kTracks; ++tr) distinct[tr] += distinctBars(pat[tr]);
                if (scaleIndex == 0)
                    for (int b = 0; b < 4; ++b)
                    {
                        bassRhythms.insert(barMask(pat[kBass], b));
                        beats.insert({barMask(pat[kDrums], b, kDrumBaseNote), barMask(pat[kDrums], b, kDrumBaseNote + 1)});
                    }

                // Voice leading: the largest move of any voice from one chord to the next.
                {
                    const Pattern& c = pat[kChords];
                    int prev[kMaxVoices], prevCount = 0, prevRoot = -1;
                    for (int rep = 0; rep < 2; ++rep)
                        for (int i = 0; i < c.length; ++i)
                        {
                            const Step& st = c.steps[i];
                            if (!st.count) continue;
                            int now[kMaxVoices];
                            for (int v = 0; v < st.count; ++v) now[v] = st.note[v];
                            std::sort(now, now + st.count);
                            if (rep == 1 && prevCount == st.count && prevRoot != h.pcs[i][0])
                            {
                                int top = 0;
                                for (int v = 0; v < st.count; ++v) top = std::max(top, std::abs(now[v] - prev[v]));
                                ++moveHist[std::min(top, 63)]; ++moves;
                            }
                            prevCount = st.count; prevRoot = h.pcs[i][0];
                            std::memcpy(prev, now, sizeof now);
                        }
                }

                // Complexity 0: how many different bars are left (the first half of each: phrase endings may differ).
                Settings s0 = settingsFor(style, kScalesUsed[scaleIndex], 4, 0.0f, 0.0f, id);
                build(s0, id);
                for (int tr = 0; tr < kTracks; ++tr) distinct0[tr] += distinctBars(pat[tr], 8);
                // Complexity moves the closed hats around, it does not take them away (outside the fills it also brings).
                hats0 += hatsOutsideFills(pat[kDrums], h);
                Settings s80 = settingsFor(style, kScalesUsed[scaleIndex], 4, 0.8f, 0.0f, id);
                build(s80, id, 0, kDrums);
                hats80 += hatsOutsideFills(pat[kDrums], h);

                // Eight bars: is the second half more than a repeat?
                if (scaleIndex == 0)
                {
                    Settings s8 = settingsFor(style, 1, 8, 0.35f, 0.0f, id);
                    build(s8, id);
                    bool differ = false;
                    for (int tr = 0; tr < kTracks; ++tr)
                        for (int b = 0; b < 4; ++b) differ = differ || !sameBar(pat[tr], b, b + 4);
                    ++ideas8; differ8 += differ;
                }
            }

        // The tune as it first sounds: the plugin's default knobs, Major and Minor.
        for (int n = 0; n < 2 * kIdeas; ++n)
        {
            Settings s = settingsFor(style, n % 2, 4, 0.35f, 0.25f, 100000u + (uint32_t)n);
            s.density = 0.55f; s.gate = 0.5f;
            harmony(s, h);
            build(s, (uint32_t)n, 0, kMelody);
            const Pattern& m = pat[kMelody];
            bool pcSeen[12] = {};
            for (int i = 0; i < m.length; ++i)
            {
                const Step& st = m.steps[i];
                if (!st.count) continue;
                pcSeen[st.note[0] % 12] = true;
                if (i % 4 == 0) { ++strongD; strongToneD += isPc(h.pcs[i], 3, st.note[0]); }
            }
            int pcs = 0; for (bool b : pcSeen) pcs += b;
            ++tunes; pcsSum += pcs; narrow += pcs <= 2;
            // The motif: bar 3 has the rhythm of bar 1, and its notes where the chord is the same.
            for (int i = 0; i < 16; ++i)
            {
                const Step& a = m.steps[i]; const Step& b = m.steps[32 + i];
                const bool sameChord = h.pcs[i][0] == h.pcs[32 + i][0];
                ++motifSteps; motifSame += a.count == b.count && (!a.count || !sameChord || a.note[0] == b.note[0]);
            }
        }

        int median = 0; { long acc = 0; while (median < 63 && (acc += moveHist[median]) * 2 < moves) ++median; }
        const double away = pct(awayBars, bars), kb = pct(kicksBass, kicks);
        const float move = moveOf(style);
        std::printf("\n%s\n", kStyles[style].name);
        std::printf("  melody on the beat is a chord tone        %5.1f %%  (>= 90)        %s\n", pct(strongTone, strong), verdict(pct(strongTone, strong) >= 90.0));
        std::printf("    the same after 3 variations             %5.1f %%  (>= 90)        %s\n", pct(strongToneS, strongS), verdict(pct(strongToneS, strongS) >= 90.0));
        std::printf("    the same outside phrase endings         %5.1f %%  (100)          %s\n", pct(strongToneOut, strongOut), verdict(strongToneOut == strongOut));
        std::printf("  melody notes that are chord tones (not endings) %5.1f %%  (55..85)  %s\n", pct(melTone, mel), verdict(pct(melTone, mel) >= 55.0 && pct(melTone, mel) <= 85.0));
        std::printf("  last melody note is a tone of its chord   %5.1f %%  (>= 95)        %s\n", pct(lastTone, patterns), verdict(pct(lastTone, patterns) >= 95.0));
        std::printf("    its root or third                       %5.1f %%  (>= 80)        %s\n", pct(lastRest, patterns), verdict(pct(lastRest, patterns) >= 80.0));
        std::printf("    a semitone above a note of the chord    %5ld    (0)            %s\n", lastGrates, verdict(lastGrates == 0));
        if (style == kTechno) std::printf("  bars 2..4 away from the tonic chord       %5.1f %%  (<= 20)        %s\n", away, verdict(away <= 20.0));
        else if (move >= 0.4f) std::printf("  bars 2..4 away from the tonic chord       %5.1f %%  (40..75)       %s\n", away, verdict(away >= 40.0 && away <= 75.0));
        else std::printf("  bars 2..4 away from the tonic chord       %5.1f %%\n", away);
        std::printf("  bass on the new root on the downbeat      %5.1f %%  (100)          %s   (%ld bars)\n", pct(changedRoot, changed), verdict(changedRoot == changed), changed);
        std::printf("    the same with Long 25                   %5.1f %%  (>= 95)        %s\n", pct(changedRootL, changedL), verdict(changedL == 0 || pct(changedRootL, changedL) >= 95.0));
        if (style == kTechno) std::printf("  kicks with a bass note                    %5.1f %%  (0)            %s\n", kb, verdict(kicksBass == 0));
        else if (style == kHouse) std::printf("  kicks with a bass note                    %5.1f %%  (<= 35)        %s\n", kb, verdict(kb <= 35.0));
        // Pop: first set at <= 35 like House, but one of its three bass rows is the root on every beat, with the kick by design.
        else if (style == kPop) std::printf("  kicks with a bass note                    %5.1f %%  (<= 60)        %s\n", kb, verdict(kb <= 60.0));
        else if (lockStyle(style)) std::printf("  kicks with a bass note                    %5.1f %%  (>= 70)        %s\n", kb, verdict(kb >= 70.0));
        else std::printf("  kicks with a bass note                    %5.1f %%\n", kb);
        for (int tr = 0; tr < kTracks; ++tr)
        {
            const double d0 = (double)distinct0[tr] / (double)patterns, d35 = (double)distinct[tr] / (double)patterns;
            // Chords and Melody: exactly one. Drums and a kick-locked Bass: the shared kick line varies by itself, whatever the knob.
            const bool kickBound = tr == kDrums || (tr == kBass && lockStyle(style));
            std::printf("  distinct bars of 4, %-6s: Cmplx 0        %5.2f    (%s)       %s\n", kRoleNames[tr], d0, kickBound ? "<= 1.5" : "1     ", verdict(kickBound ? d0 <= 1.5 : distinct0[tr] == patterns));
            // Techno's bass and stab are a machine by design (weights that always play, hardly a chord change): reported only.
            if (style == kTechno && (tr == kBass || tr == kChords)) std::printf("                               Cmplx 35      %5.2f\n", d35);
            else std::printf("                               Cmplx 35      %5.2f    (>= 2)         %s\n", d35, verdict(d35 >= 2.0));
        }
        const int needRhythms = (style == kTechno || style == kModernGame) ? 3 : 50;
        std::printf("  distinct bass bar rhythms in 500 ideas    %5zu    (>= %d)%s       %s\n", bassRhythms.size(), needRhythms, needRhythms < 10 ? " " : "", verdict((int)bassRhythms.size() >= needRhythms));
        std::printf("  distinct kick+snare bars in 500 ideas     %5zu    (>= 6)         %s\n", beats.size(), verdict(beats.size() >= 6));
        std::printf("  8 bars: bars 5..8 differ from 1..4        %5.1f %%  (>= 90)        %s\n", pct(differ8, ideas8), verdict(pct(differ8, ideas8) >= 90.0));
        // First set at <= 2. A fixed window keeps every tone in place, but a tone near its edge still jumps when the
        // chord moves by a third or more, and progressions through several chords do that often: 3 is what it gives.
        if (windowStyle(style)) std::printf("  largest voice move between chords, median %5d    (<= 3)         %s   (%ld changes)\n", median, verdict(median <= 3), moves);
        else std::printf("  largest voice move between chords, median %5d\n", median);
        {
            const double ratio = hats0 ? (double)hats80 / (double)hats0 : 1.0;
            // Styles with hat bursts get more hats with Complexity by design: only the thinning is judged there.
            const bool ok = ratio >= 0.9 && (ratio <= 1.1 || burstStyle(style));
            std::printf("  closed hats a bar, Cmplx 0 and 80         %5.1f %5.1f  (%s)  %s\n", (double)hats0 / (4.0 * patterns), (double)hats80 / (4.0 * patterns), burstStyle(style) ? ">= 90 %    " : "within 10 %", verdict(ok));
        }
        {
            const double before = pct(sameBefore, 12 * patterns), after = pct(sameAfter, 12 * patterns);
            std::printf("  bars equal to bar 1, then after 1 variation %5.1f %5.1f %%  (>= 70 %% kept)  %s\n", before, after, verdict(after >= 0.7 * before));
        }
        const double mean = (double)pcsSum / (double)tunes;
        std::printf("  default knobs: melody pitch classes, mean %5.2f    (4..6)         %s\n", mean, verdict(mean >= 4.0 && mean <= 6.0));
        std::printf("    tunes with 2 or fewer                   %5.1f %%  (< 8)          %s\n", pct(narrow, tunes), verdict(pct(narrow, tunes) < 8.0));
        std::printf("    melody on the beat is a chord tone      %5.1f %%  (>= 90)        %s\n", pct(strongToneD, strongD), verdict(pct(strongToneD, strongD) >= 90.0));
        std::printf("    bar 3 keeps the motif of bar 1          %5.1f %%  (>= 80)        %s\n", pct(motifSame, motifSteps), verdict(pct(motifSame, motifSteps) >= 80.0));
    }

    // Cost: generateIdea on a mosaic that uses all eight slots, mean of the four roles and the ten styles.
    {
        Idea ideas[kTracks];
        for (int tr = 0; tr < kTracks; ++tr)
        {
            resetIdea(ideas[tr], 1234u + (uint32_t)tr);
            for (uint32_t v = 0; v < 12; ++v) varyIdea(ideas[tr], 99u + v * 7u + (uint32_t)tr, 0.4f);
        }
        double best = 1e9;
        unsigned sink = 0;
        for (int round = 0; round < 7; ++round)
        {
            const auto t0 = std::chrono::steady_clock::now();
            int calls = 0;
            for (int rep = 0; rep < 40; ++rep)
                for (int style = 0; style < kNumStyles; ++style)
                    for (int tr = 0; tr < kTracks; ++tr)
                    {
                        Settings s = settingsFor(style, 1, 4, 0.35f, 0.1f, (uint32_t)rep);
                        TrackSettings t; t.role = tr; t.voices = kVoicesOf[tr];
                        generateIdea(pat[tr], scratch, ideas[tr], s, t);
                        sink += pat[tr].steps[rep % 64].count;
                        ++calls;
                    }
            const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / calls;
            best = std::min(best, us);
        }
        std::printf("\ncost: generateIdea with 8 slots, 4 bars: %.1f microseconds a track (%u)", best, sink & 1u);
        if (baseline > 0.0) std::printf("  = %.1f x the baseline of %.1f  (<= 5)  %s", best / baseline, baseline, verdict(best <= 5.0 * baseline));
        std::printf("\n");
    }
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
