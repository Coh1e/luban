# Luban (鲁班)

[![docs](https://img.shields.io/badge/docs-luban.coh1e.com-blue)](https://luban.coh1e.com/)
[![api](https://img.shields.io/badge/api-luban.coh1e.com%2Fapi-green)](https://luban.coh1e.com/api/)
[![license](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![中文](https://img.shields.io/badge/中文-README-red)](README.zh-CN.md)

Windows-first C++ toolchain manager + cmake/vcpkg auxiliary frontend.
**Single static-linked binary, zero UAC, XDG-first directories.**

```bat
luban setup && luban env --user
luban new app hello && cd hello
luban add fmt && luban build
```

## Documentation

- **User manual** → [luban.coh1e.com](https://luban.coh1e.com/) (mdBook, narrative)
- **Contributor reference** → [luban.coh1e.com/api](https://luban.coh1e.com/api/) (Doxygen)
- **Source** → this repo

## License

Luban itself: [MIT](LICENSE). Vendored third-party:
- `third_party/json.hpp` — nlohmann/json, MIT
- `third_party/miniz.{h,c}` — Rich Geldreich, BSD-3-Clause (`third_party/miniz.LICENSE`)
- `third_party/toml.hpp` — Mark Gillard's toml++, MIT (`third_party/toml.LICENSE`)
