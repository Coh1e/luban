# Manifest overlay format

Place a JSON file at `<data>/luban/registry/overlay/<name>.json` (or `<repo>/manifests_seed/<name>.json` for ones that ship with luban). It will take **priority over** the corresponding Scoop bucket manifest.

Use overlays when:

- the upstream Scoop manifest uses `installer.script` / `pre_install` / `post_install` / `uninstaller` / `persist` / `psmodule` (luban refuses these for safety)
- you want a different upstream URL
- you want to lock to a specific version

## Schema (subset of Scoop manifest)

```jsonc
{
  "_comment": "Free-form. Documentation/audit only.",
  "version": "2026.03.18",
  "url":  "https://example.com/path/to/release.zip",
  "hash": "sha256:528ff8708702e296b5744d9168c3fb4343c015fa024cd3770ede8ac94d9971b9",

  "extract_dir": "subdir-name-inside-the-archive",
  "extract_to":  "subdir-of-toolchain-dir",

  "bin": [
    ["relative/path/to/exe.exe", "alias"],
    ["relative/path/to/another.exe", "another-alias", "prefix arg1 arg2"]
  ],

  "env_set": {
    "TOOL_HOME": "$dir"
  },
  "env_add_path": ["bin", "lib/runtime"],

  "depends": ["mingit"]
}
```

| Field | Type | Notes |
|---|---|---|
| `version` | string | required |
| `url` | string \| string[1] | required, plain HTTPS download |
| `hash` | string \| string[1] | required; `sha256:<hex>` (or just `<hex>`) |
| `extract_dir` | string | optional; descend into this subdir of the extracted archive |
| `extract_to` | string | optional; place archive contents into this subdir of the toolchain dir |
| `bin` | array | optional; entries are `string` or `[rel,alias]` or `[rel,alias,"prefix args"]` |
| `env_set` | object | optional; future env-injection (M3) |
| `env_add_path` | array<string> | optional; subdirs to add to PATH (auto-shimmed at install time) |
| `depends` | array<string> | optional; install order hint (M3) |

## Refused fields (safety)

These are **always** rejected, even in overlay manifests:

- `installer` (e.g., `installer.script`)
- `pre_install` / `post_install`
- `uninstaller`
- `persist`
- `psmodule`

If a manifest has any of these, luban errors out at parse time and refuses to install. Overlays are the **only** way to get past a Scoop manifest that needs these fields — by stripping them and providing a plain url+hash alternative.

## Special-case: `vcpkg`

The `vcpkg` overlay (`manifests_seed/vcpkg.json`) ships in luban itself. After extract, luban runs `bootstrap-vcpkg.bat` once to fetch the matching `vcpkg.exe` from microsoft/vcpkg-tool releases. This is **luban's** decision (hardcoded for `name == "vcpkg"`), not a manifest-driven script execution — the manifest itself is plain url+hash.

## Example: a simple "just download a zip" overlay

```json
{
  "version": "1.0.0",
  "url": "https://example.com/mytool-1.0.0.zip",
  "hash": "sha256:abc...",
  "bin": [["mytool.exe", "mytool"]]
}
```

Drop at `<data>/luban/registry/overlay/mytool.json`, then:

```bat
luban setup --only mytool
```
