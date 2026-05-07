# luban — FUTURE

Open questions and longer-horizon work. **DESIGN.md** is the architecture
of decided things; this file is the queue of "we know we want it, not
sure exactly how / when". Items here have not been promised in any
release. Once an item lands, move it to DESIGN.md §24.1 (decisions) and
delete from here.

History (already-shipped milestones, ADRs, S1-S8 step ledger) lives in
`git log`, not here.

---

## v1.x — planned but not scheduled

### Core

- **`embedded:cpp-base-wasm` / `cpp-base-msvc` blueprints**
  `embedded:cpp-base` ships in v1.0 with llvm-mingw / cmake / ninja /
  mingit / vcpkg (vcpkg via the source-zip fallback + `post_install`
  bootstrap). Two cousins still missing:
  - **cpp-base-wasm**: emsdk has its own installer (Python / batch);
    inline platforms with hardcoded URLs work as a stopgap until a custom
    resolver lands.
  - **cpp-base-msvc**: needs MSVC env capture (vswhere + vcvars64.bat) —
    blocked on DESIGN issue D's Phase 2 decision.

- **POSIX symlink shim — done in blueprint engine, v0.x text shim still around**
  `blueprint_apply.cpp::write_cmd_shim` already creates POSIX symlinks
  (with copy-fallback for FAT-style mounts). The remaining v0.x text-shim
  writer at `src/shim.cpp` is plumbing for the internal `luban shim` verb
  + writes under `<data>/bin/` (not `~/.local/bin/`); v1.x cleanup should
  retire it entirely once nothing refers to its alias-table format.

- **`luban tool install <pkg>` (pipx-equivalent)**
  Standalone tool installs without writing a blueprint — sugar over
  `bp apply` with a one-tool ad-hoc blueprint. OQ-1 era candidate.

### Distribution

- **Multi-source registry / mirror**
  `source = "mirror.example.com/registry"` + `<config>/luban/registries.toml`
  in cargo style. Lets orgs pin a mirror without touching every blueprint.

- **Transparency log (sumdb-style)**
  Public-side supply-chain audit before any "official luban registry"
  lands. Out-of-scope until a registry exists to log against.

### Authoring

- **`pwsh-module:` auto-latest version**
  v0.4.1's `pwsh-module:Name` requires explicit `version = "X.Y.Z"`. To
  support `version = "*"` (or auto-latest), the resolver needs to query
  `https://www.powershellgallery.com/api/v2/Packages()?$filter=Id eq 'X'
  and IsLatestVersion eq true` which returns Atom XML. We don't ship an
  XML parser; pulling one in (pugixml? expat?) is the gating decision.
  Until then, pin versions explicitly — better practice anyway (masks
  supply-chain bumps).

- **Project-level blueprints**
  `<project>/.luban/blueprints/` merging into user-level via topology +
  priority. Pin per-project tool versions; share within a team via git.

- **Template variables in blueprints**
  chezmoi-style `{{ .git_email }}`. Needs a values file + escape policy
  inside the Lua sandbox.

### UX

- **Workspace**
  Multiple sibling projects sharing one vcpkg + blueprint set. Two
  candidates: `luban.toml [workspace] members` (luban-native) vs cmake
  superbuild (out — not luban's job).

- **`luban tui` interactive mode**
  FTXUI. Likely after the CLI surface stabilizes; speculative.

### Engineering

- **`lib_targets` table maintenance** (currently 224 hand-curated rows)
  Candidate: one-shot scraper over vcpkg-registry to seed the table,
  retain hand overrides for aliases / multi-target ports.

- **`describe --json` schema formalization**
  Schema:1 stable since v0.2; needs an ADR to lock additions = additive
  only, deletions/renames require a major.

---

## Open questions (unresolved)

These need a decision before they can move to DESIGN. Each has at least
one design path; none is committed.

### OQ-D2. MSVC env Phase 2: HKCU vs per-shell

Phase 1 ✅ (vswhere + vcvars capture to `<state>/msvc-env.json`). Phase 2
would write the ~30 MSVC env vars to HKCU so a fresh shell runs `cl.exe`
without `luban env --msvc`. Tension: that's a lot of pollution under
"Limit PATH pollution" (invariant 5). Wait for Phase 1 user feedback.

### OQ-D3. Workspace shape

See "Workspace" above. Decide between `luban.toml [workspace]` vs
delegating to cmake superbuild. Probably blocked on at least one real
multi-project user surfacing.

### OQ-D4. Linux/macOS Phase C

Phases A (CI gating) ✅ and B (hash via OpenSSL EVP, download via
libcurl) ✅ both shipped. Phase C remaining: `~/.bashrc` PATH integration,
symlink shim, vcpkg triplet auto-detection, `install.sh`. PATH writeout
landed in `1ea6bc4`; symlink shim is the largest chunk left.

### OQ-D6. `lib_targets` long-term ownership

See "Engineering" above. Decide whether to invest in scraping or stay
hand-curated. Either way the on-disk table format doesn't change.

---

## Cross-references

- Decided architecture / invariants / verb table → `docs/DESIGN.md`
- Recipes for Claude Code agents → `CLAUDE.md`
- Active gap inventory between docs and code → ad-hoc; check
  recent commits or open this file's git history for the latest scan.
