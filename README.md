# Luban (鲁班)

[![docs](https://img.shields.io/badge/docs-luban.cho1e.com-blue)](https://luban.cho1e.com/)
[![api](https://img.shields.io/badge/api-luban.cho1e.com%2Fapi-green)](https://luban.cho1e.com/api/)
[![license](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

Windows-first C++ toolchain manager + cmake/vcpkg auxiliary frontend.
**Single static-linked binary, zero UAC, XDG-first directories.**

```bat
luban setup && luban env --user
luban new app hello && cd hello
luban add fmt && luban build
```

## Documentation

- **User manual** → [luban.cho1e.com](https://luban.cho1e.com/) (mdBook, narrative)
- **Contributor reference** → [luban.cho1e.com/api](https://luban.cho1e.com/api/) (Doxygen)
- **Source** → this repo

## License

Luban itself: [MIT](LICENSE). Vendored third-party:
- `third_party/json.hpp` — nlohmann/json, MIT
- `third_party/miniz.{h,c}` — Rich Geldreich, BSD-3-Clause (`third_party/miniz.LICENSE`)
- `third_party/toml.hpp` — Mark Gillard's toml++, MIT (`third_party/toml.LICENSE`)
