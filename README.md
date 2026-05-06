# Luban (鲁班)

[![license](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![中文](https://img.shields.io/badge/中文-README-red)](README.zh-CN.md)

**Build a C++ workshop from a blueprint.**

luban reads a Lua/TOML blueprint and assembles your machine: toolchain
(LLVM-MinGW + cmake + ninja + mingit + vcpkg), CLI utilities (ripgrep / fd / bat / ...),
and dotfiles (git / bat / fastfetch / ...). Layer blueprints on top of each
other; roll back atomically.

Single static-linked binary (~5 MB), zero UAC, XDG-first directories.
Embeds Lua 5.4. Windows-first; POSIX builds.

```bat
:: 1. Build the workshop (toolchain + CLI utilities)
luban bp apply embedded:cpp-base
luban bp apply embedded:cli-quality
luban env --user

:: 2. Create + build a project
luban new app hello && cd hello
luban add fmt && luban build
```

## Documentation

- **Design** (decided architecture) → [`docs/DESIGN.md`](./docs/DESIGN.md)
- **Future** (open questions, planned but unscheduled) → [`docs/FUTURE.md`](./docs/FUTURE.md)
- **AI agent guide** → [`CLAUDE.md`](./CLAUDE.md) + [`AGENTS.md`](./AGENTS.md)
- **Long-form `--help`** → `luban new --help`, `luban bp --help`, etc.

## License

Luban itself: [MIT](LICENSE). Vendored third-party:
- `third_party/json.hpp` — nlohmann/json, MIT
- `third_party/miniz.{h,c}` — Rich Geldreich, BSD-3-Clause (`third_party/miniz.LICENSE`)
- `third_party/toml.hpp` — Mark Gillard's toml++, MIT (`third_party/toml.LICENSE`)
- `third_party/doctest.h` — Viktor Kirilov, MIT (`third_party/doctest.LICENSE`)
- `third_party/lua54/` — Lua 5.4, MIT (`third_party/lua54/LICENSE`)
