// `progress` — unified live-progress + completion-line UI for `bp apply`'s
// long-running phases (download, extract, future: pwsh-module install,
// font registration, etc).
//
// Why one class: previously download.cpp had a Progress class for byte
// counters and store_fetch.cpp had a parallel ExtractProgress for item
// counters. Same throttling logic, same TTY/env detection, same `\r\x1b[2K`
// redraw — but slightly divergent format (label trailing vs leading,
// "files" suffix vs no suffix on the rate, finish lines worded
// asymmetrically). Result: download and extract felt like two different
// systems doing similar things.
//
// One ladder, one rhythm. Action verb + bar + pct + count + rate, all
// in fixed columns so multiple consecutive phases line up vertically:
//
//   ↓ fetch    [▓▓▓▓▓▓▓░░░] 73%  2.9/4.0 MiB  @ 3.0 MiB/s
//   ↻ extract  [▓▓░░░░░░░░] 18%  4/22 files   @ 6/s
//   → shim     zoxide → ~/.local/bin/
//
// On phase completion, replace the live bar with a structurally-similar
// summary that shares the same action column and `@ rate` tail:
//
//   ✓ fetch    4.0 MiB in 1.3s @ 3.0 MiB/s
//   ✓ extract  22 files in 0.8s @ 28/s
//   ✓ shim     zoxide → ~/.local/bin/
//
// TTY detection + `LUBAN_PROGRESS` / `LUBAN_NO_PROGRESS` env overrides
// match the prior contract — no behavior change for users; just the
// frame format unifies.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace luban::progress {

/// What the `done` / `total` counters represent. The formatter dispatches
/// on this to render `1.4/4.0 MiB` (Bytes) vs `4/22 files` (Items).
enum class Unit {
    Bytes,
    Items,
};

/// Glyph + verb that anchors each progress line. The struct lets builtin
/// actions share their conventional glyph without callers having to
/// remember which arrow goes with which verb.
struct Action {
    std::string_view glyph;  ///< 1-char visual marker: ↓ ↻ → ✓ etc.
    std::string_view verb;   ///< Padded to fixed column (8 chars) by render.

    /// Built-in actions used across the codebase. Define new ones inline
    /// at the call site; the columns auto-align as long as verb stays ≤8
    /// chars.
    static const Action& fetch();    // ↓ fetch
    static const Action& extract();  // ↻ extract
    static const Action& render();   // ↻ render
    static const Action& shim();     // → shim
    static const Action& reg();      // → register (font / pwsh-module)
    static const Action& done();     // ✓ — used implicitly by finish_with(...)
};

/// One phase's live progress. Construct with action + total + unit;
/// drive with `update(done)`; finalize with `finish_with(...)`. Thread-
/// safe — workers may call update concurrently (the mutex serializes
/// the throttle check + render).
class Bar {
public:
    /// `total = -1` means unknown size (Content-Length absent during
    /// download). The bar still works — just no percentage or full-width
    /// progress bar; instead "done so far + rate".
    Bar(const Action& action, std::int64_t total, Unit unit,
        std::string label = {});

    /// Add `n` to the running counter. Throttled: at most one render to
    /// stderr per ~100ms wall-clock interval. Renders synchronously
    /// inside the mutex; OK because the render is fixed-width and fast.
    /// `n` is the increment, not the absolute counter — call sites
    /// reporting absolute progress should pass the delta.
    void update(std::size_t n);

    /// Replace the live bar with the completion line. The summary text
    /// is rendered as: `<glyph> <verb-padded>  <summary>`. For phases
    /// that have natural numeric closure, prefer `finish_done()`; for
    /// phases that produced a custom one-line description (shim, render
    /// → path), call `finish_with("zoxide → ~/.local/bin/zoxide.cmd")`.
    void finish_with(std::string_view summary);

    /// Conventional "done" finish: prints "<total> in <elapsed>s @ <rate>"
    /// in the unit-appropriate format. Equivalent to manually composing
    /// the string but doesn't lose the elapsed / rate the bar already
    /// tracks.
    void finish_done();

    /// Quietly drop the live bar without writing a completion line. Use
    /// when the caller wants to print its own structured result that
    /// would conflict with the standard summary (e.g. an error).
    void abandon();

    /// Read-only state for callers that want to compose their own log
    /// line (e.g. log::infof("downloaded {} from {}")). Counters are
    /// the live values, not the last-rendered ones.
    [[nodiscard]] std::int64_t bytes_done()  const noexcept { return done_; }
    [[nodiscard]] std::int64_t bytes_total() const noexcept { return total_; }
    [[nodiscard]] double elapsed_seconds() const noexcept;
    [[nodiscard]] double rate_per_second() const noexcept;

private:
    void render_locked();

    const Action& action_;
    Unit unit_;
    std::int64_t total_;
    std::int64_t done_ = 0;
    std::string label_;
    std::chrono::steady_clock::time_point t0_;
    std::chrono::steady_clock::time_point last_;
    std::mutex mu_;
    bool enabled_ = false;
    bool finalized_ = false;
};

/// Format a byte count in 1024-base units with one decimal: `4.2 MiB`.
/// Exposed so callers can construct their own descriptions (`"resumed
/// from offset {}", format_bytes(offset)`).
[[nodiscard]] std::string format_bytes(std::int64_t n);

/// Format a duration as `1.3s` / `45ms` / `2m 15s`. Exposed for the
/// same reasons.
[[nodiscard]] std::string format_duration_seconds(double secs);

}  // namespace luban::progress
