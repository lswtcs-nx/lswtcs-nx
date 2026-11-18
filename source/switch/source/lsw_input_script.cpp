// Scripted input (LSWTCS_INPUTSCRIPT): see include/lsw_input_script.h for the format.
#include "lsw_input_script.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace {
constexpr uint64_t kDefaultTimeoutMs = 180000;
constexpr uint64_t kDefaultPressMs = 150;
constexpr int kMinPolls = 2;   // a press is seen down, and then up, by at least this many polls

struct Btn { const char* name; uint16_t bit; };
// XINPUT_GAMEPAD_* bits.
const Btn kButtons[] = {{"UP", 0x0001},    {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008}, {"START", 0x0010},
                        {"BACK", 0x0020},  {"LS", 0x0040},   {"RS", 0x0080},   {"LB", 0x0100},    {"RB", 0x0200},
                        {"A", 0x1000},     {"B", 0x2000},    {"X", 0x4000},    {"Y", 0x8000}};

std::string upper(std::string s) {
    for (char& c : s) c = char(toupper(static_cast<unsigned char>(c)));
    return s;
}

bool parse_buttons(const std::string& word, LswInputScript::Step* st) {
    std::string w = upper(word);
    size_t p = 0;
    while (p <= w.size()) {
        const size_t q = w.find('+', p);
        const std::string name = w.substr(p, q == std::string::npos ? std::string::npos : q - p);
        bool ok = false;
        if (name == "LT") { st->lt = ok = true; }
        else if (name == "RT") { st->rt = ok = true; }
        else for (const Btn& b : kButtons) if (name == b.name) { st->buttons |= b.bit; ok = true; break; }
        if (!ok) return false;
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return true;
}

bool parse_u64(const std::string& s, uint64_t* out) {
    if (s.empty()) return false;
    for (char c : s) if (!isdigit(static_cast<unsigned char>(c))) return false;
    *out = strtoull(s.c_str(), nullptr, 10);
    return true;
}

bool parse_axis(const std::string& s, int16_t* out) {
    char* end = nullptr;
    const double v = strtod(s.c_str(), &end);
    if (end == s.c_str() || *end || v < -1.0 || v > 1.0) return false;
    *out = int16_t(v * 32767.0);
    return true;
}

bool contains_nocase(const char* hay, const std::string& needle) {
    const std::string h = upper(hay ? hay : ""), n = upper(needle);
    return h.find(n) != std::string::npos;
}

const char* op_name(LswInputScript::Op op) {
    using O = LswInputScript::Op;
    switch (op) {
    case O::Wait: return "wait"; case O::WaitLevel: return "waitlevel"; case O::WaitPlay: return "waitplay";
    case O::WaitLight: return "waitlight"; case O::WaitLoads: return "waitloads"; case O::Press: return "press"; case O::Hold: return "hold"; case O::Stick: return "stick";
    case O::Repeat: return "repeat"; case O::End: return "end"; case O::Mark: return "mark"; case O::Capture: return "capture";
    case O::CaptureFull: return "capturefull"; case O::Log: return "log"; case O::Done: return "done";
    }
    return "?";
}
}  // namespace

bool LswInputScript::load(const std::string& text) {
    steps_.clear();
    error_.clear();
    state_ = State::Empty;
    std::vector<size_t> open;
    std::istringstream in(text);
    std::string raw;
    int line = 0;
    auto err = [&](const std::string& what) {
        error_ = "line " + std::to_string(line) + ": " + what;
        steps_.clear();
        return false;
    };
    while (std::getline(in, raw)) {
        ++line;
        if (const size_t hash = raw.find('#'); hash != std::string::npos) raw.resize(hash);
        std::istringstream ls(raw);
        std::vector<std::string> w;
        for (std::string t; ls >> t;) w.push_back(t);
        if (w.empty()) continue;
        Step st{};
        st.line = line;
        st.timeout_ms = kDefaultTimeoutMs;
        const std::string cmd = upper(w[0]);
        auto arg_timeout = [&](size_t i) {
            if (w.size() <= i) return true;
            uint64_t s = 0;
            if (!parse_u64(w[i], &s) || s == 0) return false;
            st.timeout_ms = s * 1000;
            return w.size() == i + 1;
        };
        if (cmd == "WAIT") {
            st.op = Op::Wait;
            if (w.size() != 2 || !parse_u64(w[1], &st.ms)) return err("wait MS");
        } else if (cmd == "WAITLEVEL") {
            st.op = Op::WaitLevel;
            if (w.size() < 2 || !arg_timeout(2)) return err("waitlevel TEXT [TIMEOUT_S]");
            st.text = w[1];
        } else if (cmd == "WAITPLAY" || cmd == "WAITLIGHT") {
            st.op = cmd == "WAITPLAY" ? Op::WaitPlay : Op::WaitLight;
            if (w.size() < 2 || !parse_u64(w[1], &st.ms) || !arg_timeout(2)) return err(upper(w[0]) + " MS [TIMEOUT_S]");
        } else if (cmd == "WAITLOADS") {
            st.op = Op::WaitLoads;
            if (w.size() < 2 || !parse_u64(w[1], &st.ms) || !arg_timeout(2)) return err("waitloads N [TIMEOUT_S]");
        } else if (cmd == "PRESS") {
            st.op = Op::Press;
            st.ms = kDefaultPressMs;
            if (w.size() < 2 || w.size() > 3 || !parse_buttons(w[1], &st) || (w.size() == 3 && !parse_u64(w[2], &st.ms)))
                return err("press BUTTONS [MS]");
        } else if (cmd == "HOLD") {
            st.op = Op::Hold;
            if (w.size() != 3 || !parse_buttons(w[1], &st) || !parse_u64(w[2], &st.ms)) return err("hold BUTTONS MS");
        } else if (cmd == "STICK") {
            st.op = Op::Stick;
            const std::string side = w.size() > 1 ? upper(w[1]) : "";
            if (w.size() != 4 || (side != "L" && side != "R") || !parse_axis(w[2], &st.x) || !parse_axis(w[3], &st.y))
                return err("stick L|R X Y (X and Y in -1..1)");
            st.right_stick = side == "R";
        } else if (cmd == "REPEAT") {
            st.op = Op::Repeat;
            uint64_t n = 0;
            if (w.size() != 2 || !parse_u64(w[1], &n) || n > 100000) return err("repeat N");
            st.count = int(n);
            open.push_back(steps_.size());
        } else if (cmd == "END") {
            st.op = Op::End;
            if (w.size() != 1) return err("end takes nothing");
            if (open.empty()) return err("end without repeat");
            st.match = int(open.back());
            steps_[open.back()].match = int(steps_.size());
            open.pop_back();
        } else if (cmd == "MARK" || cmd == "CAPTURE" || cmd == "CAPTUREFULL" || cmd == "DONE") {
            st.op = cmd == "MARK" ? Op::Mark : cmd == "CAPTURE" ? Op::Capture : cmd == "CAPTUREFULL" ? Op::CaptureFull : Op::Done;
            if (w.size() != 1) return err(w[0] + " takes nothing");
        } else if (cmd == "LOG") {
            st.op = Op::Log;
            const size_t at = raw.find_first_not_of(" \t", raw.find_first_of(" \t", raw.find_first_not_of(" \t")));
            st.text = at == std::string::npos ? "" : raw.substr(at);
            while (!st.text.empty() && isspace(static_cast<unsigned char>(st.text.back()))) st.text.pop_back();
        } else {
            return err("unknown step '" + w[0] + "'");
        }
        steps_.push_back(st);
    }
    if (!open.empty()) { line = steps_[open.back()].line; return err("repeat without end"); }
    if (steps_.empty()) { line = 0; return err("no steps"); }
    state_ = State::Running;
    pc_ = 0;
    phase_ = Phase::Start;
    t_begin_ = 0;
    repeat_left_.clear();
    held_ = LswPadOut{};
    return true;
}

void LswInputScript::fail(const LswScriptWorld& w, const std::string& why, std::vector<std::string>* log) {
    state_ = State::Failed;
    char b[512];
    snprintf(b, sizeof b, "[SCRIPT] failed at %.1f s: %s (level %s)", (w.now_ms - t_begin_) / 1000.0, why.c_str(),
             w.level && *w.level ? w.level : "?");
    log->push_back(b);
}

void LswInputScript::begin_step(const LswScriptWorld& w, std::vector<std::string>* log) {
    const Step& st = steps_[pc_];
    t_step_ = t_phase_ = w.now_ms;
    cond_seen_ = false;
    polls_in_phase_ = 0;
    if (st.op == Op::Repeat || st.op == Op::End) return;   // silent: the steps inside are logged
    char b[512];
    snprintf(b, sizeof b, "[SCRIPT] line %d %s%s%s at %.1f s (level %s, %s frames, %llu loads)", st.line, op_name(st.op),
             st.text.empty() ? "" : " ", st.text.c_str(), (w.now_ms - t_begin_) / 1000.0, w.level && *w.level ? w.level : "?",
             w.light ? "light" : "play", (unsigned long long)w.loads);
    log->push_back(b);
}

void LswInputScript::poll(const LswScriptWorld& w, LswPadOut* pad, LswScriptActions* act, std::vector<std::string>* log) {
    if (state_ != State::Running) return;
    if (!t_begin_) {
        t_begin_ = w.now_ms ? w.now_ms : 1;
        char b[128];
        snprintf(b, sizeof b, "[SCRIPT] started: %zu steps", steps_.size());
        log->push_back(b);
    }
    // Instant steps run back to back in one poll; a step that takes time returns until it is over. The guard bounds a
    // repeat of instant steps only.
    for (int guard = 0; guard < 4096 && state_ == State::Running; ++guard) {
        if (pc_ >= steps_.size()) { fail(w, "the script ended without 'done'", log); break; }
        const Step& st = steps_[pc_];
        if (phase_ == Phase::Start) { begin_step(w, log); phase_ = Phase::Down; }
        bool wait = false;
        switch (st.op) {
        case Op::Wait:
            wait = w.now_ms - t_step_ < st.ms;
            break;
        case Op::WaitLevel:
            if (contains_nocase(w.level, st.text)) break;
            if (w.now_ms - t_step_ >= st.timeout_ms) { fail(w, "line " + std::to_string(st.line) + ": no level '" + st.text + "'", log); return; }
            wait = true;
            break;
        case Op::WaitLoads:   // st.ms holds N
            if (w.loads >= st.ms) break;
            if (w.now_ms - t_step_ >= st.timeout_ms) {
                fail(w, "line " + std::to_string(st.line) + ": " + std::to_string(w.loads) + " of " + std::to_string(st.ms) +
                            " load episodes", log);
                return;
            }
            wait = true;
            break;
        case Op::WaitPlay:
        case Op::WaitLight: {
            const bool want_light = st.op == Op::WaitLight;
            if (w.light == want_light) {
                if (!cond_seen_) { cond_seen_ = true; t_cond_ = w.now_ms; }
                if (w.now_ms - t_cond_ >= st.ms) break;
            } else {
                cond_seen_ = false;
            }
            if (w.now_ms - t_step_ >= st.timeout_ms) {
                fail(w, "line " + std::to_string(st.line) + ": no " + (want_light ? "light" : "play") + " frames for " +
                            std::to_string(st.ms) + " ms", log);
                return;
            }
            wait = true;
            break;
        }
        case Op::Press:
        case Op::Hold:
            if (phase_ == Phase::Down) {
                ++polls_in_phase_;
                if (w.now_ms - t_phase_ < st.ms || polls_in_phase_ <= kMinPolls) {
                    held_.buttons = st.buttons;
                    held_.lt = st.lt ? 255 : 0;
                    held_.rt = st.rt ? 255 : 0;
                    wait = true;
                    break;
                }
                held_.buttons = 0; held_.lt = held_.rt = 0;
                if (st.op == Op::Hold) break;
                phase_ = Phase::Up; t_phase_ = w.now_ms; polls_in_phase_ = 0;
            }
            if (++polls_in_phase_ <= kMinPolls || w.now_ms - t_phase_ < st.ms) wait = true;
            break;
        case Op::Stick:
            if (st.right_stick) { held_.rx = st.x; held_.ry = st.y; } else { held_.lx = st.x; held_.ly = st.y; }
            break;
        case Op::Repeat:
            if (st.count == 0) { pc_ = size_t(st.match) + 1; phase_ = Phase::Start; continue; }
            repeat_left_.push_back(st.count);
            break;
        case Op::End:
            if (--repeat_left_.back() > 0) { pc_ = size_t(st.match) + 1; phase_ = Phase::Start; continue; }
            repeat_left_.pop_back();
            break;
        case Op::Mark: ++act->marks; break;
        case Op::Capture: act->capture = 1; break;
        case Op::CaptureFull: act->capture = 2; break;
        case Op::Log: break;
        case Op::Done: {
            state_ = State::Done;
            held_ = LswPadOut{};
            char b[96];
            snprintf(b, sizeof b, "[SCRIPT] done in %.1f s", (w.now_ms - t_begin_) / 1000.0);
            log->push_back(b);
            return;
        }
        }
        if (wait) break;
        next();
    }
    if (state_ == State::Running) *pad = held_;
}
