// `source_resolver_pwsh` — `pwsh-module:Name` source scheme handler.
//
// Resolves a PowerShell module source from PSGallery (the official
// powershellgallery.com NuGet feed) into a LockedTool. The fetched
// .nupkg is just a zip — luban's existing archive::extract handles it
// without any new format support.
//
// Use case: PSReadLine, PSFzf, posh-git, etc. ship as PowerShell modules
// only (no GitHub release artifacts), and pwsh's own `Install-Module`
// flow doesn't fit luban's content-addressed store + atomic apply +
// rollback model. By making it a luban tool, you get all of the above
// (idempotent re-apply, snapshot generation, store dedup across machines)
// at the cost of one extra `[tool.X]` block per module.
//
// Schema:
//   [tool.psreadline]
//   source = "pwsh-module:PSReadLine"
//   version = "2.4.0"           # required — auto-latest is v0.4.x work
//   no_shim = true              # PowerShell modules go to PSModulePath, not PATH
//   bin = "PSReadLine.psd1"     # for the lock's `bin` field; never shimmed
//   post_install = "bp:scripts/install-pwsh-module.ps1"
//                                # bp ships the script that copies from the
//                                # luban store into ~/Documents/PowerShell/
//                                # Modules/<Name>/<Version>/ — pwsh auto-
//                                # discovers via $PSModulePath.
//
// Why version is required (for now):
//   PSGallery's "latest" lookup needs an OData query (`$filter=IsLatestVersion
//   eq true`) which adds an XML parser dependency we haven't pulled in yet.
//   Pinning version is also better practice — auto-latest masks supply-chain
//   bumps. v0.4.x can add `version = "*"` semantics if anyone asks.
//
// Why .nupkg works:
//   PowerShell `.nupkg` files are zip archives with a NuGet manifest at
//   `<Name>.nuspec` plus the actual module files (PSD1, PSM1, scripts).
//   archive::extract unpacks them just like any other zip; the
//   post_install script picks the module subset out and copies it to
//   PSModulePath (the .nuspec / _rels / package metadata stays in the
//   luban store but doesn't reach pwsh).

#include "source_resolver.hpp"

#include <cctype>
#include <filesystem>
#include <string>

#include "download.hpp"
#include "hash.hpp"
#include "paths.hpp"
#include "platform.hpp"
#include "store.hpp"

namespace luban::source_resolver {

namespace {

namespace bp = luban::blueprint;
namespace bpl = luban::blueprint_lock;
namespace fs = std::filesystem;

constexpr std::string_view kSchemePrefix = "pwsh-module:";

// Validate that `s` is a plausible PSGallery module name. NuGet IDs are
// `[A-Za-z0-9_.-]+` per the v2 spec; we mirror that. Empty is rejected;
// pathological names (slashes, spaces) are rejected. Doesn't try to be
// authoritative — server returns 404 for genuinely unknown packages.
bool valid_module_name(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
              c == '.' || c == '-')) {
            return false;
        }
    }
    return true;
}

std::expected<bpl::LockedTool, std::string> resolve_pwsh_module_impl(
    const bp::ToolSpec& spec) {
    // 1. Parse `pwsh-module:Name` from spec.source.
    if (!spec.source.has_value()) {
        return std::unexpected("tool `" + spec.name + "`: pwsh-module resolver "
                               "called without source field");
    }
    const std::string& src = *spec.source;
    if (!src.starts_with(kSchemePrefix)) {
        return std::unexpected("tool `" + spec.name + "`: source `" + src +
                               "` doesn't start with `pwsh-module:`");
    }
    std::string module_name = src.substr(kSchemePrefix.size());
    if (!valid_module_name(module_name)) {
        return std::unexpected("tool `" + spec.name + "`: invalid module name `" +
                               module_name + "` (expected [A-Za-z0-9_.-]+)");
    }

    // 2. Require explicit version. Auto-latest is v0.4.x followup work.
    if (!spec.version.has_value() || spec.version->empty()) {
        return std::unexpected(
            "tool `" + spec.name + "` (pwsh-module:" + module_name + "): "
            "`version = \"X.Y.Z\"` is required. PSGallery `IsLatestVersion` "
            "lookup needs an OData query that v0.4.0 doesn't ship yet — "
            "pin a version explicitly. Find current versions at "
            "https://www.powershellgallery.com/packages/" + module_name);
    }

    bpl::LockedTool out;
    out.version = *spec.version;
    out.source = src;

    std::string target = std::string(luban::platform::host_triplet());

    // 3. Construct the deterministic .nupkg URL.
    //    https://www.powershellgallery.com/api/v2/package/<Name>/<Version>
    //    302-redirects to the actual blob; download::download follows.
    std::string url = "https://www.powershellgallery.com/api/v2/package/" +
                      module_name + "/" + *spec.version;

    bpl::LockedPlatform lp;
    lp.url = url;
    // bin is the file pwsh expects to find at the module root after deploy.
    // PowerShell's loader looks for `<Name>.psd1` (manifest) or `<Name>.psm1`.
    // Default to the .psd1 since well-formed modules ship one. Caller can
    // override via spec.bin if upstream is irregular.
    lp.bin = spec.bin.value_or(module_name + ".psd1");

    // 4. Download once to compute sha256 — same pattern github resolver uses
    //    on its no-digest path. NuGet doesn't expose sha in the REST shape
    //    we hit, so we always pay this on first resolve / `bp apply --update`.
    fs::path cache = paths::cache_dir() / "downloads";
    std::error_code ec;
    fs::create_directories(cache, ec);
    fs::path tmp = cache / (spec.name + "-resolve-" + *spec.version + ".nupkg");

    luban::download::DownloadOptions dlopts;
    dlopts.label = "resolve " + spec.name + " (pwsh-module)";
    auto dl = luban::download::download(url, tmp, dlopts);
    if (!dl) {
        return std::unexpected("download " + url + ": " + dl.error().message);
    }
    lp.sha256 = "sha256:" + dl->sha256.hex;
    fs::remove(tmp, ec);

    // 5. artifact_id is canonical; computed here so the lock is self-
    //    contained (mirror github resolver — see its comment for the full
    //    rationale on why this can't be deferred to store::fetch).
    lp.artifact_id = luban::store::compute_artifact_id(
        spec.name, out.version, target, lp.sha256);

    out.platforms.emplace(target, std::move(lp));
    return out;
}

struct Registrar {
    Registrar() {
        luban::source_resolver::detail::set_pwsh_module_resolver(
            &resolve_pwsh_module_impl);
    }
};
Registrar g_registrar;

}  // namespace

}  // namespace luban::source_resolver
