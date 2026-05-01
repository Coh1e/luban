# 发布流程

## 触发

`git push origin vX.Y.Z`（带 tag）触发 `.github/workflows/build.yml`：

1. Win runner 构建 release preset
2. 产出 `luban.exe` + `luban-shim.exe`
3. 计算 `SHA256SUMS`
4. `gh release create` 自动发版

## 步骤（手动操作只在本地这两步）

```bat
:: 1. 改版本号
sed -i "s|kVersion = \"...\"|kVersion = \"X.Y.Z\"|" src/cli.cpp

:: 2. 本地烟测
cmake --build --preset release
build\release\luban.exe --version

:: 3. 提交并打 tag
git commit -am "Bump X.Y.Z"
git push
git tag -a vX.Y.Z -m "luban X.Y.Z — <一句话亮点>"
git push --tags
```

## SemVer 约定

- **Major（X）**：破坏性 schema 变更（如 `installed.json` schema 升到 2）、删 verb、改 `luban.cmake` v1 不兼容
- **Minor（Y）**：新 verb、新字段、新组件、新 target type
- **Patch（Z）**：bug fix、文档、内部实现

## Release notes

按以下结构（参考最近的 release）：

```
## Highlights
- ...

## Breaking
- ...（若 major）

## Added
- ...

## Fixed
- ...

## Internal
- ...
```

## 文档发布

`docs.yml` workflow 在 `main` push 时自动发 `gh-pages`。手动触发可在 GitHub UI 的
Actions 标签。**永远不依赖 gh-pages 的提交历史**。
