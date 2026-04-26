# `luban search`

Search vcpkg ports.

```text
luban search <pattern>
```

Wraps `vcpkg search <pattern>`. Requires `luban setup --only vcpkg` first.

## Examples

```bat
luban search fmt
:: fmt              12.1.0   {fmt} is an open-source formatting library...
:: log4cxx[fmt]              Include log4cxx::FMTLayout class...
:: spdlog[fmt]               Use fmt library
:: ...

luban search json
:: nlohmann-json    3.12.0   JSON for Modern C++
:: rapidjson        1.1.0    A fast JSON parser/generator for C++...
:: simdjson         3.10.0   Fast JSON parser

luban search boost-asio
:: port name match for Boost.Asio
```

The first column is the port name — pass that to `luban add`.

## Notes

- Search matches port name + description substring (vcpkg's behavior, not fuzzy)
- For features (e.g., `boost[asio]`), the bracket syntax is informational
  only here; you actually `luban add boost-asio` (separate port name)
- Output may include a "results may be outdated" warning from vcpkg — that's
  vcpkg's voice. To refresh, `cd $VCPKG_ROOT && git pull` (or run
  `luban setup --force --only vcpkg` to fully re-sync)
