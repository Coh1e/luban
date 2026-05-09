// See `progress.hpp` for design rationale.

#include "progress.hpp"

#include <cstdio>
#include <cstdlib>
#include <format>

#include <io.h>

namespace luban::progress {

namespace {

bool detect_tty_enabled() {
    bool tty = _isatty(_fileno(stderr));
    bool force_on  = std::getenv("LUBAN_PROGRESS") != nullptr;
    bool force_off = std::getenv("LUBAN_NO_PROGRESS") != nullptr;
    return (tty || force_on) && !force_off;
}

// Width of the glyph + space + verb column. Keep < 10 so the bar after
// it lands at column ~12. Verb is padded to `kVerbWidth - 2` (because the
// column also holds the glyph + 1 space).
constexpr int kVerbWidth = 9;   // "extract  " — verb 7 chars + 2 trailing
constexpr int kBarWidth  = 12;  // matches what the eye reads as "a bar",
                                // not so wide it crowds rate / label.

// Pad `s` on the right with ASCII spaces to total visual width `w`.
// std::format's "{:<W}" doesn't quite do what we want when s holds
// multibyte UTF-8 (the width formatter counts bytes, not display cells).
// Our verbs are ASCII so byte count == display cell count, but be
// explicit anyway in case someone ever uses a non-ASCII verb.
std::string pad_right(std::string_view s, int w) {
    std::string out(s);
    while (static_cast<int>(out.size()) < w) out.push_back(' ');
    return out;
}

std::string format_count(Unit u, std::int64_t n) {
    switch (u) {
        case Unit::Bytes: return format_bytes(n);
        case Unit::Items: return std::format("{}", n);
    }
    return std::format("{}", n);
}

std::string format_rate(Unit u, double per_sec) {
    switch (u) {
        case Unit::Bytes:
            return format_bytes(static_cast<std::int64_t>(per_sec)) + "/s";
        case Unit::Items:
            return std::format("{:.0f}/s", per_sec);
    }
    return std::format("{:.0f}/s", per_sec);
}

std::string format_progress(Unit u, std::int64_t done, std::int64_t total) {
    // "1.4/4.0 MiB" — share the unit suffix when both are bytes;
    // "4/22 files" — only emit the unit word once.
    if (u == Unit::Bytes) {
        // Pick the formatter that fits `total` so done + total share scale
        // and don't render as "1024.0/1.0 MiB" or similar mixed ladders.
        // Reuse format_bytes for total and then re-format done in the same
        // unit class. Simpler: just call format_bytes on both — the slight
        // unit mismatch (e.g. "850 KiB/4.0 MiB") is informative not noisy.
        return format_bytes(done) + "/" + format_bytes(total);
    }
    return std::format("{}/{} files", done, total);
}

}  // namespace

// ---- Action verbs ----------------------------------------------------

const Action& Action::fetch() {
    static const Action a{"\xe2\x86\x93", "fetch"};   // ↓
    return a;
}
const Action& Action::extract() {
    static const Action a{"\xe2\x86\xbb", "extract"}; // ↻
    return a;
}
const Action& Action::render() {
    static const Action a{"\xe2\x86\xbb", "render"};  // ↻
    return a;
}
const Action& Action::shim() {
    static const Action a{"\xe2\x86\x92", "shim"};    // →
    return a;
}
const Action& Action::reg() {
    static const Action a{"\xe2\x86\x92", "register"};
    return a;
}
const Action& Action::done() {
    static const Action a{"\xe2\x9c\x93", ""};        // ✓
    return a;
}

// ---- format_bytes / format_duration_seconds -------------------------

std::string format_bytes(std::int64_t n) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double f = static_cast<double>(n);
    int u = 0;
    while (f >= 1024.0 && u < 4) { f /= 1024.0; ++u; }
    if (u == 0) return std::format("{} B", n);
    return std::format("{:.1f} {}", f, units[u]);
}

std::string format_duration_seconds(double secs) {
    if (secs < 1.0)   return std::format("{:.0f}ms", secs * 1000.0);
    if (secs < 60.0)  return std::format("{:.1f}s", secs);
    int m = static_cast<int>(secs / 60.0);
    int s = static_cast<int>(secs - m * 60.0);
    return std::format("{}m {}s", m, s);
}

// ---- Bar ------------------------------------------------------------

Bar::Bar(const Action& action, std::int64_t total, Unit unit, std::string label)
    : action_(action), unit_(unit), total_(total),
      label_(std::move(label)),
      t0_(std::chrono::steady_clock::now()), last_(t0_),
      enabled_(detect_tty_enabled()) {}

double Bar::elapsed_seconds() const noexcept {
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - t0_).count();
    return (dt > 1e-3) ? dt : 1e-3;
}

double Bar::rate_per_second() const noexcept {
    return static_cast<double>(done_) / elapsed_seconds();
}

void Bar::update(std::size_t n) {
    if (!enabled_) {
        // Still track counters — abandoned/disabled bars need accurate
        // bytes_done() / elapsed_seconds() for the caller's own log
        // lines.
        std::lock_guard<std::mutex> g(mu_);
        if (finalized_) return;
        done_ += static_cast<std::int64_t>(n);
        return;
    }
    std::lock_guard<std::mutex> g(mu_);
    if (finalized_) return;
    done_ += static_cast<std::int64_t>(n);
    auto now = std::chrono::steady_clock::now();
    bool finished = (total_ > 0 && done_ >= total_);
    if (!finished) {
        double dt = std::chrono::duration<double>(now - last_).count();
        if (dt < 0.1) return;
    }
    last_ = now;
    render_locked();
}

void Bar::render_locked() {
    double secs = elapsed_seconds();
    double rate = static_cast<double>(done_) / secs;

    std::string verb_col = pad_right(
        std::string(action_.glyph) + " " + std::string(action_.verb),
        kVerbWidth + 2);  // glyph(1 cell, multibyte) + space + verb-pad

    std::string line;
    if (total_ > 0) {
        double pct = 100.0 * static_cast<double>(done_) / static_cast<double>(total_);
        int filled = static_cast<int>(kBarWidth * static_cast<double>(done_) / total_);
        if (filled > kBarWidth) filled = kBarWidth;
        std::string bar;
        for (int i = 0; i < kBarWidth; ++i) {
            // U+2593 dark shade ▓ filled, U+2591 light shade ░ empty —
            // crisper than "#" / "." in non-monospace contexts and Win10+
            // conhost renders both via VT. Both are 3-byte UTF-8.
            bar += (i < filled ? "\xe2\x96\x93" : "\xe2\x96\x91");
        }
        line = std::format("  {}[{}] {:3.0f}%  {}  @ {}",
                           verb_col, bar, pct,
                           format_progress(unit_, done_, total_),
                           format_rate(unit_, rate));
    } else {
        // Unknown total — drop bar + pct, show running count + rate.
        line = std::format("  {}{}  @ {}",
                           verb_col,
                           format_count(unit_, done_),
                           format_rate(unit_, rate));
    }
    if (!label_.empty()) {
        line += "  ";
        line += label_;
    }
    std::fprintf(stderr, "\r\x1b[2K%s", line.c_str());
    std::fflush(stderr);
}

void Bar::finish_with(std::string_view summary) {
    std::lock_guard<std::mutex> g(mu_);
    if (finalized_) return;
    finalized_ = true;
    if (enabled_) {
        // Clear the live bar then print the completion line on a fresh
        // line so it lands in scrollback. Reuse the verb column for
        // visual continuity — the `done` glyph (✓) replaces the action
        // glyph but verb stays.
        std::string verb_col = pad_right(
            std::string("\xe2\x9c\x93 ") + std::string(action_.verb),
            kVerbWidth + 2);
        std::fprintf(stderr, "\r\x1b[2K  %s%.*s\n",
                     verb_col.c_str(),
                     static_cast<int>(summary.size()), summary.data());
        std::fflush(stderr);
    }
    // When disabled (CI, redirected logs) we still want a record of what
    // happened — print a plain log-style line, no ANSI.
    if (!enabled_) {
        std::fprintf(stderr, "  %s %s  %.*s\n",
                     std::string(action_.glyph).c_str(),
                     std::string(action_.verb).c_str(),
                     static_cast<int>(summary.size()), summary.data());
        std::fflush(stderr);
    }
}

void Bar::finish_done() {
    double secs = elapsed_seconds();
    double rate = static_cast<double>(done_) / secs;
    std::string summary = std::format("{} in {} @ {}",
        format_count(unit_, done_),
        format_duration_seconds(secs),
        format_rate(unit_, rate));
    finish_with(summary);
}

void Bar::abandon() {
    std::lock_guard<std::mutex> g(mu_);
    if (finalized_) return;
    finalized_ = true;
    if (enabled_) {
        std::fprintf(stderr, "\r\x1b[2K");
        std::fflush(stderr);
    }
}

}  // namespace luban::progress
