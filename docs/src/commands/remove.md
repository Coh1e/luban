# `luban remove`

Reverse of [`luban add`](./add.md). Removes a vcpkg library from `vcpkg.json` and regenerates `luban.cmake`.

```text
luban remove <pkg>
```

`vcpkg_installed/` is **not** cleaned up — vcpkg's binary cache may still hold the lib for other projects. To force a clean rebuild without the lib: `rm -rf build/ vcpkg_installed/`.
