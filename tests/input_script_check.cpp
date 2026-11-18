// Host check of the scripted input (source/switch/source/lsw_input_script.cpp). Built and run by tests/input_script_check.py.
// Simulates pad polls every 33 ms against a made-up game: title (light) until 2 s, loading the hub (light) to 5 s, the hub in play,
// a level load when the script presses START in the hub, then the level in play.
#include "lsw_input_script.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Game {
    uint64_t t = 0, level_load_at = 0;
    std::string level;
    bool light() const {
        if (t < 5000) return true;
        if (level_load_at && t >= level_load_at && t < level_load_at + 3000) return true;
        return false;
    }
    void update(uint16_t buttons) {
        if (t >= 2000 && level.empty()) level = "Map\\Map";
        if (!level_load_at && t >= 5000 && (buttons & 0x0010)) { level_load_at = t; level = "Episode1\\Gungan_B"; }
    }
};

struct Run {
    std::vector<std::string> log;
    std::vector<uint16_t> buttons;
    std::vector<int16_t> lx;
    int marks = 0, capture = 0;
    LswInputScript s;
    Game g;
    bool go(const std::string& text, uint64_t max_ms = 400000, uint64_t poll_ms = 33) {
        if (!s.load(text)) { log.push_back("load error: " + s.error()); return false; }
        for (g.t = 1000; g.t < max_ms && !s.finished(); g.t += poll_ms) {
            LswScriptWorld w;
            w.now_ms = g.t;
            w.light = g.light();
            w.level = g.level.c_str();
            LswPadOut pad;
            pad.buttons = 0x8000;   // the real pad holds Y: the script must replace it while running
            LswScriptActions act;
            s.poll(w, &pad, &act, &log);
            marks += act.marks;
            if (act.capture) capture = act.capture;
            buttons.push_back(pad.buttons);
            lx.push_back(pad.lx);
            g.update(pad.buttons);
        }
        return true;
    }
    bool logged(const char* what) const {
        for (auto& l : log) if (l.find(what) != std::string::npos) return true;
        return false;
    }
    void dump() const { for (auto& l : log) printf("    %s\n", l.c_str()); }
};

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--parse") {   // tests/scripts/*.txt: parse only
        for (int i = 2; i < argc; ++i) {
            std::string text;
            if (FILE* f = fopen(argv[i], "rb")) {
                char buf[4096];
                for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
                fclose(f);
            }
            LswInputScript s;
            const bool ok = s.load(text);
            if (!ok) ++g_fail;
            printf("%s script %s: %s\n", ok ? "ok  " : "FAIL", argv[i], ok ? (std::to_string(s.steps()) + " steps").c_str() : s.error().c_str());
        }
        return g_fail ? 1 : 0;
    }
    {   // A full route: title, hub, a level, with marks, a capture, a repeat and a stick.
        Run r;
        const char* script =
            "# route\n"
            "waitlevel map 30\n"
            "waitlight 500\n"
            "press START\n"
            "waitplay 1000 30     # the hub\n"
            "stick L 0.5 -1\n"
            "repeat 3\n"
            "  press A 100\n"
            "end\n"
            "stick L 0 0\n"
            "mark\n"
            "hold START 200\n"
            "waitlevel gungan_b\n"
            "waitplay 2000\n"
            "capturefull\n"
            "log heavy window\n"
            "wait 5000\n"
            "done\n";
        CHECK(r.go(script), "load: %s", r.s.error().c_str());
        CHECK(r.s.finished() && !r.s.failed(), "route did not finish cleanly");
        CHECK(r.logged("[SCRIPT] done"), "no done line");
        CHECK(r.logged("log heavy window"), "no log step");
        CHECK(r.marks == 1, "marks %d", r.marks);
        CHECK(r.capture == 2, "capture %d", r.capture);
        CHECK(r.g.level == "Episode1\\Gungan_B", "level %s", r.g.level.c_str());
        int a_presses = 0, y_while_running = 0;
        bool stick_seen = false;
        for (size_t i = 0; i < r.buttons.size(); ++i) {
            if ((r.buttons[i] & 0x1000) && (i == 0 || !(r.buttons[i - 1] & 0x1000))) ++a_presses;
            if (r.buttons[i] & 0x8000) ++y_while_running;
            if (r.lx[i] == 16383) stick_seen = true;
        }
        CHECK(a_presses == 3, "A pressed %d times, want 3 (repeat)", a_presses);
        CHECK(y_while_running <= 1, "the real pad's Y leaked %d times", y_while_running);   // the last poll, after done
        CHECK(stick_seen, "stick not applied");
        if (g_fail) r.dump();
    }
    {   // A press lasts at least two polls even when polls are slower than the press.
        Run r;
        CHECK(r.go("press A 10\ndone\n", 400000, 200), "load");
        int down = 0;
        for (uint16_t b : r.buttons) if (b & 0x1000) ++down;
        CHECK(down >= 2, "press seen by %d polls", down);
    }
    {   // A wait that never comes true fails at its timeout, and the pad goes back to the player.
        Run r;
        CHECK(r.go("waitlevel Kamino 5\ndone\n"), "load");
        CHECK(r.s.failed(), "should have failed");
        CHECK(r.logged("no level 'Kamino'"), "no timeout reason");
        CHECK(r.buttons.back() == 0x8000, "pad not handed back: 0x%X", r.buttons.back());
    }
    {   // No 'done': failed at the end of the file.
        Run r;
        CHECK(r.go("wait 100\n"), "load");
        CHECK(r.s.failed() && r.logged("without 'done'"), "missing done not reported");
    }
    {   // Parse errors name the line.
        LswInputScript s;
        CHECK(!s.load("wait 10\npress Q\n") && s.error().rfind("line 2:", 0) == 0, "bad button: %s", s.error().c_str());
        CHECK(!s.load("repeat 2\npress A\n") && s.error().find("repeat without end") != std::string::npos, "%s", s.error().c_str());
        CHECK(!s.load("end\n") && s.error().find("end without repeat") != std::string::npos, "%s", s.error().c_str());
        CHECK(!s.load("stick L 2 0\n"), "stick range");
        CHECK(!s.load("# only a comment\n"), "empty script accepted");
        CHECK(!s.load("frobnicate\n") && s.error().find("unknown step") != std::string::npos, "%s", s.error().c_str());
        CHECK(s.load("press A+B+LT 50\nrepeat 0\npress X\nend\ndone\n"), "valid script refused: %s", s.error().c_str());
    }
    {   // waitloads: the count of ended load episodes, with a timeout.
        LswInputScript s;
        CHECK(s.load("waitloads 2 5\ndone\n"), "load");
        std::vector<std::string> log;
        for (uint64_t t = 1000, loads = 0; t < 4000 && !s.finished(); t += 33) {
            if (t > 2000) loads = 2;
            LswScriptWorld w; w.now_ms = t; w.loads = loads;
            LswPadOut pad; LswScriptActions act;
            s.poll(w, &pad, &act, &log);
        }
        CHECK(s.finished() && !s.failed(), "waitloads did not pass");
        LswInputScript s2;
        CHECK(s2.load("waitloads 3 2\ndone\n"), "load");
        for (uint64_t t = 1000; t < 6000 && !s2.finished(); t += 33) {
            LswScriptWorld w; w.now_ms = t; w.loads = 1;
            LswPadOut pad; LswScriptActions act;
            s2.poll(w, &pad, &act, &log);
        }
        CHECK(s2.failed(), "waitloads did not time out");
    }
    {   // repeat 0 skips its body; combined buttons and triggers.
        Run r;
        CHECK(r.go("press A+B+LT 50\nrepeat 0\npress X\nend\ndone\n"), "load");
        bool ab = false, x = false;
        for (uint16_t b : r.buttons) { if ((b & 0x3000) == 0x3000) ab = true; if (b & 0x4000) x = true; }
        CHECK(ab && !x && r.s.finished() && !r.s.failed(), "combo %d, X %d", ab, x);
    }
    printf(g_fail ? "input script check: %d FAILED\n" : "input script check: PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
