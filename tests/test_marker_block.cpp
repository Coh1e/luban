// Tests for src/marker_block.cpp — AGENTS.md "luban-managed" block engine.
//
// This module is the AI-contract surface for `luban specs`. A regression
// here either silently destroys user-edited content (worst case) or
// silently fails to refresh project context (annoying). The tests pin
// both behaviors.

#include "doctest.h"

#include "marker_block.hpp"

namespace mb = luban::marker_block;

TEST_CASE("marker_block: managed_section_order matches AGENTS.md template contract") {
    auto& v = mb::managed_section_order();
    REQUIRE(v.size() == 3);
    CHECK(v[0] == "project-context");
    CHECK(v[1] == "cpp-modernization");
    CHECK(v[2] == "ub-perf-guidance");
}

TEST_CASE("marker_block: extract_template_blocks pulls every section") {
    std::string rendered =
        "header\n"
        "<!-- BEGIN luban-managed: project-context -->\n"
        "C++23\nfmt 10\n"
        "<!-- END luban-managed -->\n"
        "middle\n"
        "<!-- BEGIN luban-managed: cpp-modernization -->\n"
        "ranges\n"
        "<!-- END luban-managed -->\n"
        "footer\n";
    auto m = mb::extract_template_blocks(rendered);
    REQUIRE(m.size() == 2);
    CHECK(m["project-context"] == "C++23\nfmt 10\n");
    CHECK(m["cpp-modernization"] == "ranges\n");
}

TEST_CASE("marker_block: sync replaces only managed bodies, leaves outer text alone") {
    std::string existing =
        "# AGENTS.md\n"
        "\n"
        "Hand-written intro.\n"
        "\n"
        "<!-- BEGIN luban-managed: project-context -->\n"
        "OLD: cpp 17\n"
        "<!-- END luban-managed -->\n"
        "\n"
        "## My notes (user-owned)\n"
        "I prefer fmt over std::format because of compile times.\n";

    std::map<std::string, std::string> tpl{
        {"project-context", "NEW: cpp 23\ntriplet x64-mingw-static\n"},
    };

    auto out = mb::sync_managed_blocks(existing, tpl);

    // Header preserved
    CHECK(out.find("# AGENTS.md") != std::string::npos);
    CHECK(out.find("Hand-written intro.") != std::string::npos);
    // Old body gone
    CHECK(out.find("OLD: cpp 17") == std::string::npos);
    // New body present
    CHECK(out.find("NEW: cpp 23") != std::string::npos);
    CHECK(out.find("triplet x64-mingw-static") != std::string::npos);
    // User notes preserved
    CHECK(out.find("## My notes (user-owned)") != std::string::npos);
    CHECK(out.find("I prefer fmt") != std::string::npos);
    // Markers preserved
    CHECK(out.find("<!-- BEGIN luban-managed: project-context -->") != std::string::npos);
    CHECK(out.find("<!-- END luban-managed -->") != std::string::npos);
}

TEST_CASE("marker_block: sync skips sections the user removed (no BEGIN marker)") {
    // User decided they don't want cpp-modernization sync — they removed
    // both markers. Sync must NOT re-insert the section.
    std::string existing =
        "<!-- BEGIN luban-managed: project-context -->\n"
        "old context\n"
        "<!-- END luban-managed -->\n"
        "\n"
        "## My modernization notes\n"
        "I'm sticking to C++17 patterns here.\n";

    std::map<std::string, std::string> tpl{
        {"project-context", "new context\n"},
        {"cpp-modernization", "ranges, format, expected\n"},
    };

    auto out = mb::sync_managed_blocks(existing, tpl);
    CHECK(out.find("new context") != std::string::npos);
    // cpp-modernization should NOT be auto-inserted
    CHECK(out.find("ranges, format, expected") == std::string::npos);
    CHECK(out.find("BEGIN luban-managed: cpp-modernization") == std::string::npos);
    // User text untouched
    CHECK(out.find("## My modernization notes") != std::string::npos);
}

TEST_CASE("marker_block: sync handles CRLF line endings (Windows editors)") {
    // notepad / VS save files with CRLF. The body_start advance must skip
    // \r\n, not just \n.
    std::string existing =
        "<!-- BEGIN luban-managed: project-context -->\r\n"
        "OLD\r\n"
        "<!-- END luban-managed -->\r\n";

    std::map<std::string, std::string> tpl{
        {"project-context", "NEW\n"},
    };

    auto out = mb::sync_managed_blocks(existing, tpl);
    CHECK(out.find("OLD") == std::string::npos);
    CHECK(out.find("NEW") != std::string::npos);
    // BEGIN/END markers preserved including their CRLF
    CHECK(out.find("<!-- BEGIN luban-managed: project-context -->\r\n") != std::string::npos);
}

TEST_CASE("marker_block: sync skips section when END marker is missing") {
    // Authoring bug: user accidentally deleted the END marker. We must
    // not overwrite from BEGIN to EOF — that would clobber whatever
    // followed in the file.
    std::string existing =
        "<!-- BEGIN luban-managed: project-context -->\n"
        "old body — END marker missing!\n"
        "\n"
        "## My very important user notes\n"
        "Some essential prose I do NOT want destroyed.\n";

    std::map<std::string, std::string> tpl{
        {"project-context", "new body\n"},
    };

    auto out = mb::sync_managed_blocks(existing, tpl);
    // Sync was skipped → file unchanged.
    CHECK(out == existing);
    CHECK(out.find("My very important user notes") != std::string::npos);
}

TEST_CASE("marker_block: sync is a no-op for sections not in the template") {
    // Template doesn't render cpp-modernization (e.g. older luban version),
    // but the file has the markers. Body should be left alone, not blanked.
    std::string existing =
        "<!-- BEGIN luban-managed: cpp-modernization -->\n"
        "preserve me\n"
        "<!-- END luban-managed -->\n";

    std::map<std::string, std::string> tpl{};  // intentionally empty

    auto out = mb::sync_managed_blocks(existing, tpl);
    CHECK(out.find("preserve me") != std::string::npos);
}

TEST_CASE("marker_block: sync against text with no markers is identity") {
    // First-time-ever AGENTS.md (or one user fully detached from the
    // managed system) → sync must not corrupt it.
    std::string existing = "# AGENTS.md\n\nJust user prose, nothing managed.\n";
    std::map<std::string, std::string> tpl{
        {"project-context", "ignored\n"},
    };
    auto out = mb::sync_managed_blocks(existing, tpl);
    CHECK(out == existing);
}

TEST_CASE("marker_block: extract handles empty body") {
    // Edge case: user emptied a managed block to opt out of its content
    // but left the markers in. Round-trip should preserve emptiness.
    std::string rendered =
        "<!-- BEGIN luban-managed: project-context -->\n"
        "<!-- END luban-managed -->\n";
    auto m = mb::extract_template_blocks(rendered);
    REQUIRE(m.size() == 1);
    CHECK(m["project-context"].empty());
}

TEST_CASE("marker_block: sync inserts trailing newline if template body lacks it") {
    // sync writes new_body verbatim, but appends '\n' if missing — this
    // keeps the END marker on its own line. Pin the contract.
    std::string existing =
        "<!-- BEGIN luban-managed: project-context -->\n"
        "old\n"
        "<!-- END luban-managed -->\n";
    std::map<std::string, std::string> tpl{
        {"project-context", "no trailing nl"},  // no \n
    };
    auto out = mb::sync_managed_blocks(existing, tpl);
    CHECK(out.find("no trailing nl\n<!-- END luban-managed -->") != std::string::npos);
}
