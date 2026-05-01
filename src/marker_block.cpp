#include "marker_block.hpp"

#include <regex>

#include "log.hpp"

namespace luban::marker_block {

const std::vector<std::string>& managed_section_order() {
    static const std::vector<std::string> v = {
        "project-context",
        "cpp-modernization",
        "ub-perf-guidance",
    };
    return v;
}

std::map<std::string, std::string> extract_template_blocks(const std::string& rendered) {
    std::map<std::string, std::string> out;
    // Section names are slug-style: lowercase letters, digits, hyphens. The
    // body is greedy-stopped at the first END marker (.*? non-greedy); we
    // rely on the [\s\S] class because std::regex's `.` doesn't cross
    // newlines by default.
    std::regex re(R"(<!-- BEGIN luban-managed: ([a-z0-9-]+) -->\s*\n([\s\S]*?)<!-- END luban-managed -->)",
                  std::regex::ECMAScript);
    auto begin = std::sregex_iterator(rendered.begin(), rendered.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::string section = (*it)[1].str();
        std::string body    = (*it)[2].str();
        out[section] = std::move(body);
    }
    return out;
}

std::string sync_managed_blocks(const std::string& existing,
                                const std::map<std::string, std::string>& tpl_blocks) {
    std::string out = existing;

    for (auto& section : managed_section_order()) {
        std::string begin_marker = "<!-- BEGIN luban-managed: " + section + " -->";
        std::string end_marker   = "<!-- END luban-managed -->";

        // Find the next BEGIN for this specific section. We don't iterate
        // multiple instances — duplicates would be a user authoring bug
        // and silently rewriting all of them is more confusing than the
        // single-instance contract.
        size_t b = out.find(begin_marker);
        if (b == std::string::npos) continue;  // user removed → skip

        // END marker must come AFTER the BEGIN. We pick the nearest END
        // (so nested marker blocks don't break the algorithm).
        size_t e = out.find(end_marker, b + begin_marker.size());
        if (e == std::string::npos) {
            log::warnf("AGENTS.md: '{}' has no matching END marker; skipping sync", section);
            continue;
        }

        // Replace from end of BEGIN line through end of END marker exclusive
        // — i.e., the body, preserving the markers themselves.
        size_t body_start = b + begin_marker.size();
        // Skip immediate trailing newline after BEGIN if present (so we keep
        // one-line separation between marker and content). Handles both \n
        // and \r\n (CRLF written by Windows editors).
        if (body_start < out.size() && out[body_start] == '\n') {
            body_start += 1;
        } else if (body_start + 1 < out.size()
                   && out[body_start] == '\r' && out[body_start + 1] == '\n') {
            body_start += 2;
        }

        auto it = tpl_blocks.find(section);
        if (it == tpl_blocks.end()) continue;  // template didn't have this section

        // Replace [body_start, e) with the new content + a trailing \n
        // before the END marker (so the END marker stays on its own line).
        std::string new_body = it->second;
        if (new_body.empty() || new_body.back() != '\n') new_body.push_back('\n');
        out.replace(body_start, e - body_start, new_body);
    }

    return out;
}

}  // namespace luban::marker_block
