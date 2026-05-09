// Stub definitions for `luban::store::fetch` so blueprint_apply.cpp links
// into the unit-test binary without dragging in store_fetch.cpp's heavy
// dependency tree (WinHTTP/libcurl, miniz, archive extraction, etc.).
//
// Apply tests that exercise dry-run / file-only blueprints never reach
// the fetch path — but the linker still needs a symbol. If a future test
// accidentally exercises it, fetch returns unexpected so the test fails
// loudly instead of segfaulting.

#include <string>
#include <string_view>

#include "store.hpp"

namespace luban::store {

std::expected<FetchResult, std::string> fetch(
    std::string_view /*artifact_id*/, std::string_view /*url*/,
    std::string_view /*sha256*/, std::string_view /*bin*/,
    const FetchOptions& /*opts*/) {
    return std::unexpected<std::string>(
        "luban::store::fetch is stubbed in unit tests — apply tests must use "
        "dry_run or file-only blueprints");
}

}  // namespace luban::store
