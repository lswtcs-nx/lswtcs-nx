// Scripted input for automated runs (LSWTCS_INPUTSCRIPT): a text file of steps that drives player 1's pad, waiting on what the
// game shows (the level being loaded, light frames such as menus and loading screens, play) instead of fixed times, so the same
// script runs on the console and in an emulator. Pure C++ (no libnx), so tests/input_script_check.py can run it on a PC.
//
// One step per line; '#' starts a comment. Times are milliseconds unless named _S (seconds).
//   wait MS                          do nothing for MS
//   waitlevel TEXT [TIMEOUT_S]       until the level path contains TEXT (case-insensitive), e.g. "Map", "Gungan_B"
//   waitplay MS [TIMEOUT_S]          until the frames have been play frames (not light) for MS in a row
//   waitlight MS [TIMEOUT_S]         until the frames have been light (title, menus, loading) for MS in a row
//   waitloads N [TIMEOUT_S]          until N load episodes ([LOADEP], counted from boot) have ended; an episode ends 2 s
//                                    after its last file call. Runs 112-117 always had 2 by the title screen
//   press BUTTONS [MS]               hold for MS (default 150), then let go for as long; each at least 2 polls
//   hold BUTTONS MS                  hold for MS, then let go (no pause after)
//   stick L|R X Y                    set a stick, X and Y in -1..1; it stays until set again ("stick L 0 0" centres it)
//   repeat N ... end                 run the steps between N times (nests)
// Frame-exact steps (log113; tools/padrec_to_script.py --frames writes them), counted in guest frames (VdSwap calls):
//   frameset                         the current guest frame becomes frame 0 for waitframe
//   waitframe K [TIMEOUT_S]          until guest frame 0 + K
//   pad BUTTONS | -                  hold these buttons from now on (until the next pad step); "-" lets all go
//   speed max | normal | FPS         the frame limiter: off, back to its target, or FPS; with LSWTCS_FIXEDDT=1 the game
//                                    simulates the same frames faster than real time (tests only; back to normal at the end)
//   mark | capture | capturefull     the tester's mark ([MARK]), a light or full capture, as the pad combos do
//   log TEXT                         a [SCRIPT] line in stdout.log
//   done                             end: "[SCRIPT] done", the pad goes back to the player
// BUTTONS: 360 names joined by '+': A B X Y START BACK LB RB LT RT LS RS UP DOWN LEFT RIGHT. They are the 360 pad's buttons,
// so a script does not depend on the face-button layout (LSWTCS_PADLAYOUT).
// A wait past its timeout (default 180 s) ends the script with "[SCRIPT] failed", as does the end of the file without "done",
// and so does any button really pressed on player 1's pad while it runs (the player takes over).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct LswPadOut {
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

// What the script reads each poll.
struct LswScriptWorld {
    uint64_t now_ms = 0;
    bool light = false;        // the frames are light (gl_backend.cpp's g_frame_light)
    const char* level = "";    // native_crt.cpp's level path, "" before the first level
    uint64_t loads = 0;        // load episodes ended since boot (kernel_stubs.cpp's [LOADEP])
    uint64_t frame = 0;        // guest frames (VdSwap calls) since boot
};

// What a poll asks of the runtime besides the pad.
struct LswScriptActions {
    int marks = 0;
    int capture = 0;           // 1 light, 2 full
    int fps_cap = -2;          // speed step: -2 no change, -1 back to the limiter's target, 0 uncapped, N fps
};

class LswInputScript {
public:
    // Parses the script. On an error returns false and sets error() ("line N: ...").
    bool load(const std::string& text);
    const std::string& error() const { return error_; }
    bool active() const { return state_ == State::Running; }
    bool finished() const { return state_ == State::Done || state_ == State::Failed; }
    bool failed() const { return state_ == State::Failed; }
    // One pad poll: advances the steps and, while running, replaces `pad` with the script's pad. Log lines go to `log`
    // (each without the trailing newline); the caller prints them.
    void poll(const LswScriptWorld& w, LswPadOut* pad, LswScriptActions* act, std::vector<std::string>* log);
    size_t steps() const { return steps_.size(); }

    enum class Op { Wait, WaitLevel, WaitPlay, WaitLight, WaitLoads, Press, Hold, Stick, Repeat, End, Mark, Capture, CaptureFull, Log, Done,
                    FrameSet, WaitFrame, Pad, Speed };
    struct Step {
        Op op;
        int line = 0;
        uint64_t ms = 0, timeout_ms = 0;
        uint16_t buttons = 0;
        bool lt = false, rt = false, right_stick = false;
        int16_t x = 0, y = 0;
        int count = 0, match = -1;   // repeat: count, index of its end; end: index of its repeat
        std::string text;
    };

private:
    enum class State { Empty, Running, Done, Failed };
    enum class Phase { Start, Down, Up };
    void begin_step(const LswScriptWorld& w, std::vector<std::string>* log);
    void fail(const LswScriptWorld& w, const std::string& why, std::vector<std::string>* log);
    void next() { ++pc_; phase_ = Phase::Start; }

    std::vector<Step> steps_;
    std::string error_;
    State state_ = State::Empty;
    size_t pc_ = 0;
    Phase phase_ = Phase::Start;
    uint64_t t_step_ = 0, t_phase_ = 0, t_cond_ = 0, t_begin_ = 0;
    bool cond_seen_ = false;
    int polls_in_phase_ = 0;
    std::vector<int> repeat_left_;   // per open repeat, innermost last
    uint64_t frame_base_ = 0;        // frameset
    LswPadOut held_;                 // sticks persist; buttons only during press/hold
};
