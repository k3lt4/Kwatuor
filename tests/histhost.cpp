// Tiny VST2 host that checks randomise and Fav with the transport stopped, so every change lands at once:
// randomise, keeping a Fav, recalling it, removing it, and save / load.
//   histhost plugin.dylib        prints one line per check; exit code 1 if any failed
#include "mpc_vst2_abi.h"
#include <dlfcn.h>
#include <unistd.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
static VstTimeInfo ti;
static VstIntPtr host(AEffect*, VstInt32 op, VstInt32, VstIntPtr, void*, float)
{
    if (op == audioMasterVersion) return 2400;
    if (op == audioMasterGetTime) return (VstIntPtr)&ti;
    return 0;
}
static AEffect* e;
static std::map<std::string, int> ix;
static float* outs[2];
static int failures = 0;
static void run(int ms) { for (int i = 0; i < ms / 5; ++i) { e->processReplacing(e, nullptr, outs, 128); usleep(5000); } }
static float norm(const char* n) { return e->getParameter(e, ix.at(n)); }
static int tile(const char* n) { return (int)std::lround(norm(n) * 24.0f); }
static void press(const char* n) { e->setParameter(e, ix.at(n), 1.0f); run(250); }
static void check(bool ok, const char* what) { std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++failures; }
int main(int, char** argv)
{
    void* lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { std::fprintf(stderr, "%s\n", dlerror()); return 1; }
    typedef AEffect* (*Main)(audioMasterCallback);
    e = ((Main)dlsym(lib, "VSTPluginMain"))(host);
    e->dispatcher(e, effOpen, 0, 0, nullptr, 0);
    e->dispatcher(e, effSetSampleRate, 0, 0, nullptr, 44100.0f);
    e->dispatcher(e, effSetBlockSize, 0, 128, nullptr, 0);
    e->dispatcher(e, effMainsChanged, 0, 1, nullptr, 0);
    std::vector<float> l(128), r(128); outs[0] = l.data(); outs[1] = r.data();
    ti.sampleRate = 44100; ti.tempo = 120; ti.flags = kVstTempoValid | kVstPpqPosValid;   // stopped
    for (int i = 0; i < e->numParams; ++i) { char name[64] = {}; e->dispatcher(e, effGetParamName, i, 0, name, 0); ix[name] = i; }
    run(200);
    const auto state = [] { return std::vector<float>{norm("Drums Tile"), norm("Bass Tile"), norm("Chords Tile"), norm("Melody Tile"), norm("Drums Density"), norm("Bass Gate"), norm("Style")}; };

    const auto text = [](const char* n) { char t[64] = {}; e->dispatcher(e, effGetParamDisplay, ix.at(n), 0, t, 0); return std::string(t); };
    const auto shows = [](const char* n, int max) { return (int)std::lround(norm(n) * (float)max); };   // a display parameter's value
    const auto setp = [](const char* n, float v) { e->setParameter(e, ix.at(n), v); run(120); };
    check(tile("F1 Drums") == 0, "Fav starts empty");
    check(shows("Undo Look", 2) == 0 && shows("Fav Look", 2) == 0, "nothing to undo yet, and the star is plain");
    const float dens0 = norm("Drums Density"), gate0 = norm("Bass Gate");
    press("Drums Rand"); press("Bass Rand");
    check(norm("Drums Rand") < 0.5f, "randomise key goes out again");
    check(norm("Drums Density") != dens0 || norm("Bass Gate") != gate0, "randomise moved the Play knobs");
    check(norm("Drums Density") >= 0.25f && norm("Drums Density") <= 0.85f, "Dens inside 25..85");
    check(tile("F1 Drums") == 0, "randomise keeps nothing by itself");

    const auto a = state();
    press("Fav Add");
    check(tile("F1 Drums") == 1 + (int)std::lround(a[0] * 23.0f), "Fav shows the current pattern");
    check(shows("Fav Look", 2) == 1, "the star is blue once the state is kept");
    press("Generate");
    const auto b = state();
    check(b != a, "Generate changed the idea");
    check(shows("Fav Look", 2) == 0, "and plain again after a re-roll");
    // ---- Undo ----
    check(shows("Undo Look", 2) == 1, "after a re-roll the key reads Undo");
    press("Undo");
    check(state() == a, "Undo brings back the idea from before the re-roll");
    check(shows("Undo Look", 2) == 2 && norm("Undo") < 0.5f, "the key then reads Redo, and has gone out");
    press("Undo");
    check(state() == b && shows("Undo Look", 2) == 1, "Redo comes forward again, and the key reads Undo");
    press("Undo");
    setp("Swing", 0.6f);
    check(shows("Undo Look", 2) == 1, "a setting changed by hand turns Redo back into Undo");
    press("Undo");
    check(state() == b, "which still swaps with the other state");
    {
        const float d0 = norm("Bass Density"), g0 = norm("Bass Gate");
        press("Bass Rand");
        const bool moved = norm("Bass Density") != d0 || norm("Bass Gate") != g0;
        press("Undo");
        check(moved && norm("Bass Density") == d0 && norm("Bass Gate") == g0, "Undo also takes back a randomise key");
    }
    check(tile("F2 Drums") == 0, "Generate keeps nothing by itself");
    press("Fav Add");
    check(tile("F1 Drums") == 1 + (int)std::lround(b[0] * 23.0f) && tile("F2 Drums") == 1 + (int)std::lround(a[0] * 23.0f), "a new Fav comes in at slot 1");
    press("F2 Recall");
    check(state() == a, "recalling a Fav brings its state back");
    check(norm("F2 Recall") < 0.5f, "the recall key goes out once it has landed");
    check(tile("F2 Drums") != 0 && tile("F3 Drums") == 0, "the Fav stays and nothing else is kept");

    void* chunk = nullptr;
    const VstIntPtr size = e->dispatcher(e, effGetChunk, 0, 0, &chunk, 0);
    std::vector<char> saved((char*)chunk, (char*)chunk + size);
    // ---- Edit ----
    check(norm("F1 Cross") < 0.5f, "without Edit no cross shows");
    press("F1 Remove");
    check(tile("F2 Drums") != 0, "and a press where the cross would be removes nothing");
    setp("Fav Edit", 1.0f);
    check(norm("F1 Cross") > 0.5f && norm("F2 Cross") > 0.5f && norm("F3 Cross") < 0.5f, "with Edit lit the filled slots show a cross");
    press("F1 Recall");
    check(state() == a && norm("F1 Recall") < 0.5f, "and a tile no longer recalls");
    press("F1 Remove");
    check(tile("F1 Drums") == 1 + (int)std::lround(a[0] * 23.0f) && tile("F2 Drums") == 0, "the cross removes a Fav and the next one moves up");
    press("Generate");
    e->dispatcher(e, effSetChunk, 0, (VstIntPtr)saved.size(), saved.data(), 0);
    run(300);
    check(tile("F2 Drums") != 0 && tile("F3 Drums") == 0 && state() == a, "a saved project brings the idea and the Favs back");

    const std::string n1 = text("F1 Name"), n2 = text("F2 Name");
    check(n1.find(' ') != std::string::npos && n1.size() >= 5 && n1.size() <= 10 && text("F3 Name") == " ", ("a Fav gets a two-word name: " + n1 + ", " + n2).c_str());
    check(n1 != n2, "two Favs have different names");
    const VstIntPtr size2 = e->dispatcher(e, effGetChunk, 0, 0, &chunk, 0);
    std::vector<char> saved2((char*)chunk, (char*)chunk + size2);
    setp("Fav Edit", 1.0f);
    press("F1 Remove");
    check(text("F1 Name") == n2, "the name goes with its Fav");
    e->dispatcher(e, effSetChunk, 0, (VstIntPtr)saved2.size(), saved2.data(), 0);
    run(300);
    check(text("F1 Name") == n1 && text("F2 Name") == n2, "names are saved with the project");
    char typed[64] = {};

    for (int i = 0; i < 12; ++i) press("Fav Add");
    check(tile("F10 Drums") != 0, "Fav fills up and stops at ten");
    check(shows("Fav Look", 2) == 2, "with ten Favs the star is greyed");
    {
        const std::string first = text("F1 Name"), last = text("F10 Name");
        press("Fav Add");
        check(text("F1 Name") == first && text("F10 Name") == last, "and keeps nothing more");
    }
    // ---- the drum kit ----
    const auto note = [](const char* n) { return (int)std::lround(norm(n) * 127.0f); };
    check(note("Kit Kick") == 36 && note("Kit Perc 4") == 43 && norm("Cell Kick") > 0.5f, "the kit starts in a row from 36 with Kick selected");
    press("Cell Snare");
    check(norm("Cell Snare") > 0.5f && norm("Cell Kick") < 0.5f, "tapping a cell selects it");
    e->setParameter(e, ix.at("Cell Snare"), 0.0f); run(250);          // a tap on the selected cell: plays it, stays selected
    check(norm("Cell Snare") > 0.5f, "tapping the selected cell keeps it selected");
    press("Kit +"); press("Kit +");
    check(note("Kit Snare") == 39 && note("Kit Kick") == 36, "+ moves the selected sound only");
    press("Kit -");
    check(note("Kit Snare") == 38, "- moves it back");
    press("Kit MPC");
    check(note("Kit Kick") == 37 && note("Kit Snare") == 36 && note("Kit Open") == 82, "MPC fills all eight");
    check(norm("Kit MPC") > 0.5f && norm("Kit CHR") < 0.5f && norm("Kit GM") < 0.5f, "the MPC key stays lit");
    press("Kit +");
    check(norm("Kit MPC") < 0.5f && norm("Kit CHR") < 0.5f, "it goes out when a note is moved");
    press("Kit -");
    check(norm("Kit MPC") > 0.5f, "and is lit again when the notes are the preset's again");
    // ---- the Kit is part of a Fav ----
    setp("Fav Edit", 1.0f);
    press("F1 Remove");
    setp("Fav Edit", 0.0f);
    press("Fav Add");                                                  // the kit is the MPC preset here
    press("Kit GM");
    check(note("Kit Kick") == 36 && note("Kit Snare") == 38, "GM fills all eight");
    press("F1 Recall");
    check(note("Kit Kick") == 37 && note("Kit Snare") == 36 && note("Kit Open") == 82, "recalling a Fav brings its Kit back");
    press("Undo");
    check(note("Kit Snare") == 38, "and Undo takes the recall back, Kit included");
    // ---- Variations ----
    {
        const auto knobs = [] { return std::vector<float>{norm("Drums Density"), norm("Bass Density"), norm("Chords Density"), norm("Melody Density"), norm("Drums Fills"), norm("Bass Motion")}; };
        const auto before = knobs();
        const auto tiles = [] { return std::vector<float>{norm("Drums Tile"), norm("Bass Tile"), norm("Chords Tile"), norm("Melody Tile")}; };
        setp("Variations", 2.0f / 3.0f);                               // the third key: new patterns, chords and knobs kept
        const auto t0 = tiles();
        press("Generate");
        check(knobs() == before && tiles() != t0, "the third Variations key gives new patterns and leaves the Shape knobs alone");
        setp("Variations", 0.0f);                                      // the first: a variation keeps each tile's colour family
        press("Generate");
        check(knobs() == before, "a variation leaves the Shape knobs alone");
        check(text("Variations") == "I", "Variations has four keys, I to IV");
        setp("Variations", 1.0f);
        check(text("Variations") == "IV", "and the last one is IV");
    }
    check(text("Swing").back() == '%' && text("Bass Gate").back() == '%' && text("Velocity").back() == '%', ("percentages carry their sign: " + text("Swing")).c_str());
    // ---- Pads ----
    const auto look = [](const char* n) { return (int)std::lround(norm(n) * 3.0f); };
    e->setParameter(e, ix.at("Drums Pads"), 0.0f); run(100);          // the lowest setting
    check(e->dispatcher(e, effGetParamDisplay, ix.at("Drums Pads"), 0, typed, 0) == 0 && std::string(typed) == "4", "Pads goes down to 4");
    check(look("Look Open") == 0 && look("Look Perc 1") == 2 && look("Look Perc 4") == 2, "with 4 pads the four percussion cells are greyed");
    press("Drums Pads -");
    check(look("Look Open") == 0, "and - stops at 4");
    e->setParameter(e, ix.at("Drums Pads"), 0.5f); run(100);
    check(look("Look Perc 2") == 0 && look("Look Perc 3") == 2, "with 6 pads only Perc 3 and 4 are greyed");
    press("Cell Perc 4");
    check(look("Look Perc 4") == 3 && look("Look Snare") == 0, "a greyed cell can still be selected");
    e->setParameter(e, ix.at("Drums Pads"), 1.0f); run(100);
    check(look("Look Perc 3") == 0 && look("Look Perc 4") == 1, "with 8 pads none is greyed");

    // ---- Key in ----
    const auto play = [](int channel, int n)
    {
        VstMidiEvent m = {}; m.type = kVstMidiType; m.byteSize = sizeof(m); m.midiData[0] = (char)(0x90 | (channel - 1)); m.midiData[1] = (char)n; m.midiData[2] = 100;
        VstEvents ev = {}; ev.numEvents = 1; ev.events[0] = (VstEvent*)&m;
        e->dispatcher(e, effProcessEvents, 0, 0, &ev, 0);
        run(100);
    };
    const auto key = [] { return (int)std::lround(norm("Key") * 11.0f); };
    e->setParameter(e, ix.at("Key"), 0.0f); run(50);
    play(1, 62);
    check(key() == 0 && norm("Key In Lamp") < 0.25f, "with Key in off a played note changes nothing");
    e->setParameter(e, ix.at("Key In"), 1.0f); run(100);
    check(std::lround(norm("Key In Lamp") * 2.0f) == 1, "the lens shows Key in is on");
    e->setParameter(e, ix.at("Cell Kick"), norm("Cell Kick") > 0.5f ? 0.0f : 1.0f); run(60);   // plays the Kick's note on the Drums channel
    play(2, note("Kit Kick"));
    check(key() == 0, "our own note coming straight back is not taken for a key");
    run(600);
    play(9, 64);
    check(key() == 4, "any channel is listened to");
    e->setParameter(e, ix.at("Key"), 0.0f); run(50);
    play(1, 62);
    check(key() == 2 && std::lround(norm("Key In Lamp") * 2.0f) == 2, "a D on channel 1 sets Key to D and flashes the lens");
    run(400);
    check(std::lround(norm("Key In Lamp") * 2.0f) == 1, "the flash ends");
    play(1, 93);
    check(key() == 9, "the octave does not matter: a high A sets Key to A");
    e->setParameter(e, ix.at("Key In"), 0.0f); run(100);

    // ---- On bar ----
    double ppq = 1.0;                                                  // a real start is on a 1/16 line
    const auto playBlocks = [&ppq](int blocks)
    {
        ti.flags = kVstTempoValid | kVstPpqPosValid | kVstTransportPlaying;
        for (int i = 0; i < blocks; ++i) { ti.ppqPos = ppq; e->processReplacing(e, nullptr, outs, 128); ppq += 128 * 120.0 / (60.0 * 44100.0); }
        usleep(120000);                                                // let the worker show it
    };
    check(norm("On Bar") > 0.5f, "On bar is lit by default");
    playBlocks(20);
    e->setParameter(e, ix.at("Bass Density"), 0.11f);
    playBlocks(20);
    check(norm("Bass Wait") > 0.5f && norm("Drums Wait") < 0.5f, "playing with On bar: a turned knob waits, and only its track");
    playBlocks(520);                                                   // past the next bar line
    check(norm("Bass Wait") < 0.5f, "it lands on the bar line");
    check(std::lround(norm("Bass Strip") * 48.0f) >= 1 && std::lround(norm("Bass Strip") * 48.0f) <= 16, "playing, a track's strip shows the step");
    {
        int flashes = 0, wrong = 0;
        for (int i = 0; i < 700; ++i)                                  // a bar, looked at every 30 blocks or so
        {
            ti.ppqPos = ppq; e->processReplacing(e, nullptr, outs, 128); ppq += 128 * 120.0 / (60.0 * 44100.0);
            if (i % 12 == 0) { usleep(60000); const long f = std::lround(norm("Drums Flash") * 16.0f), st = std::lround(norm("Drums Strip") * 48.0f); if (f) { ++flashes; if (f != st) ++wrong; } }
        }
        check(flashes > 3 && wrong == 0, "the Drums strip flashes on steps with a hit, on the step that plays");
    }
    e->setParameter(e, ix.at("On Bar"), 0.0f);
    e->setParameter(e, ix.at("Bass Density"), 0.77f);
    playBlocks(5);
    check(norm("Bass Wait") < 0.5f, "with On bar out a turned knob acts at once");
    e->setParameter(e, ix.at("On Bar"), 1.0f);
    e->setParameter(e, ix.at("Key"), 0.5f);
    playBlocks(20);
    check(norm("Bass Wait") > 0.5f && norm("Melody Wait") > 0.5f && norm("Drums Wait") < 0.5f, "a new Key waits on the pitched tracks");
    ti.flags = kVstTempoValid | kVstPpqPosValid; run(100);
    check(norm("Bass Wait") < 0.5f, "stopped, everything lands at once");

    // ---- not 4/4 ----
    ti.flags = kVstTempoValid | kVstPpqPosValid | kVstTimeSigValid; ti.timeSigNumerator = 3; ti.timeSigDenominator = 4; run(150);
    check(text("Song Pos") == "3/4 !", ("in 3/4 the position display warns: " + text("Song Pos")).c_str());
    ti.timeSigNumerator = 4; run(150);
    check(text("Song Pos").find(':') != std::string::npos, ("back in 4/4 it shows the position again: " + text("Song Pos")).c_str());
    ti.flags = kVstTempoValid | kVstPpqPosValid;

    // ---- MIDI Send ----
    check(std::lround(norm("MIDI Out") * 2.0f) == 1, "the lens shows sending");
    e->setParameter(e, ix.at("MIDI Send"), 0.0f);
    playBlocks(40); run(20);
    check(std::lround(norm("MIDI Out") * 2.0f) == 2, "MIDI Send off: the lens goes grey");
    check(text("Song Pos") == "OFF", ("and the position display reads OFF: " + text("Song Pos")).c_str());
    playBlocks(40);
    check(norm("Clock") == 0.0f && norm("Drums Strip") == 0.0f, "and the bar clock and the step strips stay dark while playing");
    e->setParameter(e, ix.at("MIDI Send"), 1.0f);
    playBlocks(40);
    check(std::lround(norm("MIDI Out") * 2.0f) == 1 && norm("Clock") > 0.0f, "MIDI Send on again: the lens and the clock are back");
    check(text("Song Pos").find(':') != std::string::npos, ("and the position: " + text("Song Pos")).c_str());
    ti.flags = kVstTempoValid | kVstPpqPosValid; run(50);

    press("Kit GM");
    // Play two bars and see which notes the Drums track (channel 2) sends: build with KWATUOR_TRACE and grep "ch2 ".
    ti.flags = kVstTempoValid | kVstPpqPosValid | kVstTransportPlaying;
    for (int i = 0; i < 1400; ++i) { ti.ppqPos = i * 128 * 120.0 / (60.0 * 44100.0); e->processReplacing(e, nullptr, outs, 128); }
    ti.flags = kVstTempoValid | kVstPpqPosValid; run(50);

    // ---- damaged projects ----
    // A project file is outside input: whatever is in it, loading it and recalling from it must not crash or leave
    // a parameter that is not a number. 400 damaged copies of a real state: random bytes, NaNs, cut short.
    {
        void* good = nullptr;
        const VstIntPtr goodSize = e->dispatcher(e, effGetChunk, 0, 0, &good, 0);
        const std::vector<unsigned char> base((unsigned char*)good, (unsigned char*)good + goodSize);
        uint32_t r = 12345;
        const auto rnd = [&r] { r ^= r << 13; r ^= r >> 17; r ^= r << 5; return r; };
        bool finite = true;
        for (int i = 0; i < 400; ++i)
        {
            std::vector<unsigned char> bad = base;
            for (int k = 0, n = 1 + (int)(rnd() % 24); k < n; ++k)
            {
                const size_t at = rnd() % bad.size();
                if (rnd() % 3 == 0 && at + 4 <= bad.size()) { bad[at] = 0; bad[at + 1] = 0; bad[at + 2] = 0xC0; bad[at + 3] = 0x7F; }   // a NaN
                else bad[at] = (unsigned char)rnd();
            }
            if (i % 5 == 0) bad.resize(1 + rnd() % bad.size());
            e->dispatcher(e, effSetChunk, 0, (VstIntPtr)bad.size(), bad.data(), 0);
            for (int b = 0; b < 4; ++b) e->processReplacing(e, nullptr, outs, 128);
            if (i % 20 == 0) { e->setParameter(e, ix.at(i % 40 ? "F1 Recall" : "F10 Recall"), 1.0f); run(120); }
            for (int q = 0; q < e->numParams; ++q) finite = finite && std::isfinite(e->getParameter(e, q));
        }
        check(finite, "400 damaged projects load and recall without a crash or a parameter that is not a number");
        e->dispatcher(e, effSetChunk, 0, (VstIntPtr)base.size(), (void*)base.data(), 0);
        run(200);
    }

    // A style chosen by hand brings its Gate and Long Notes; a recall or an Undo brings back its own.
    {
        const auto pct = [&text](const char* n) { return std::atoi(text(n).c_str()); };
        const float dens = norm("Bass Density"), drumGateFree = norm("Drums Density");
        setp("Fav Edit", 1.0f); press("F1 Remove"); setp("Fav Edit", 0.0f);   // room for one more Fav
        setp("Style", 0.0f);
        setp("Style", 1.0f / 9.0f);   // House
        check(pct("Bass Gate") == 60 && pct("Chords Gate") == 100 && pct("Melody Long Notes") == 25, "House brings its Gate and Long Notes");
        setp("Bass Gate", 0.5f);
        press("Fav Add");
        const float kept = norm("Bass Gate");
        setp("Style", 0.0f);          // Techno
        check(pct("Bass Gate") == 35 && pct("Chords Gate") == 30 && pct("Chords Long Notes") == 0 && pct("Melody Long Notes") == 5, "Techno brings its Gate and Long Notes");
        check(norm("Bass Density") == dens && norm("Drums Density") == drumGateFree, "a style change leaves Density alone");
        setp("Style", 6.0f / 9.0f);   // Reggae/Dub
        check(pct("Bass Gate") == 85 && pct("Chords Gate") == 25, "and so does the next style");
        press("F1 Recall"); run(400);
        check(std::fabs(norm("Style") - 1.0f / 9.0f) < 0.01f && norm("Bass Gate") == kept, "a recalled Fav keeps its own Gate");
        press("Undo"); run(400);
        check(pct("Bass Gate") == 85 && pct("Chords Gate") == 25, "Undo of the recall brings back the knobs as they were");
    }

    e->dispatcher(e, effMainsChanged, 0, 0, nullptr, 0);
    e->dispatcher(e, effClose, 0, 0, nullptr, 0);
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
