# 本地开发

## 一次性准备（dogfood luban 自身）

```bat
:: 用已发布的 luban 安装一套工具链来构建源码版 luban
luban setup
luban env --user
:: 重开 shell 让 PATH 生效
```

或在干净环境里用 luban-managed 激活脚本：

```bat
call %LOCALAPPDATA%\luban\env\activate.cmd
```

## 构建

```bat
cmake --preset default                  :: Debug，~31 MB
cmake --build --preset default

cmake --preset release                  :: Release，~3 MB
cmake --build --preset release
```

产出：

- `build\default\luban.exe`（Debug）
- `build\release\luban.exe`、`build\release\luban-shim.exe`（Release）

## 烟雾测试

```bat
build\release\luban.exe --version
build\release\luban.exe --help
build\release\luban.exe doctor
```

更全面的人工流程：

```bat
:: 在 tmp 目录验证完整生命周期
mkdir tmp && cd tmp
luban new app smoke
cd smoke
luban add fmt
luban build
build\default\smoke.exe
```

## 常见的 "为什么不工作"

- **`D:\dobby` 是 junction**：`D:\dobby\.local\share\luban` 与
  `C:\Users\Rust\.local\share\luban` 实际是同一份磁盘内容。**不要在代码里 canonicalize**——
  会破坏 `relative_to`。详见 `../architecture/module-boundaries.md` 不变量 #2。
- **`gh-pages` 分支被 `force_orphan: true` 重写**：依赖它的提交历史会丢。
- **gh CLI 推 workflow 文件需要 `workflow` scope**：`gh auth refresh -s workflow`。
- **GH Pages 证书** 需要 Cloudflare DNS-only（灰云），否则 Let's Encrypt ACME 验证失败。
- **域名是 `coh1e.com`**（不是 `cho1e.com`，可用 `gh api repos/Coh1e/luban/pages` 的 `cname` 字段验证）。
