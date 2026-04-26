# 命令清单

luban 共 17 个 verb，分 4 组。

## 工具链 / 环境（每台机器一次性）

| 命令 | 作用 |
|---|---|
| [`luban setup` →](https://luban.coh1e.com/commands/setup.html) | 装 LLVM-MinGW + cmake + ninja + mingit + vcpkg |
| [`luban env` →](https://luban.coh1e.com/commands/env.html) | 显示 env 状态；改 activate 脚本；**注册 HKCU PATH（rustup 风格）** |

## 单项目（在项目目录里跑）

| 命令 | 作用 |
|---|---|
| [`luban new app|lib <name>` →](https://luban.coh1e.com/commands/new.html) | 脚手架新项目 |
| [`luban build` →](https://luban.coh1e.com/commands/build.html) | `cmake --preset && cmake --build`；同步 `compile_commands.json` |
| [`luban target add|remove` →](https://luban.coh1e.com/commands/target.html) | 加 / 删 target（lib 或 exe） |

## 依赖管理（vcpkg.json + luban.cmake）

| 命令 | 作用 |
|---|---|
| [`luban add <pkg>[@version]` →](https://luban.coh1e.com/commands/add.html) | 编辑 `vcpkg.json` + 重生成 `luban.cmake`（自动 find_package + link） |
| [`luban remove <pkg>` →](https://luban.coh1e.com/commands/remove.html) | `luban add` 反操作 |
| [`luban sync` →](https://luban.coh1e.com/commands/sync.html) | 重读 `vcpkg.json` + `luban.toml`，重生成 `luban.cmake` |
| [`luban search <pattern>` →](https://luban.coh1e.com/commands/search.html) | 搜 vcpkg ports（包 `vcpkg search`） |

## 诊断 / 自治

| 命令 | 作用 |
|---|---|
| [`luban doctor` →](https://luban.coh1e.com/commands/doctor.html) | 报告目录、已装组件、PATH 上的工具 |
| [`luban run <cmd> [args...]` →](https://luban.coh1e.com/commands/run.html) | uv 风格透传执行；用 luban 工具链 env 跑 cmd |
| [`luban which <alias>` →](https://luban.coh1e.com/commands/which.html) | 打印 alias 解析到的绝对 exe 路径 |
| [`luban describe [--json]` →](https://luban.coh1e.com/commands/describe.html) | dump 系统 + 项目状态（IDE / scripts 用） |
| [`luban shim` →](https://luban.coh1e.com/commands/shim.html) | 重生成 `<data>/bin/` shim（文本 + .exe；修复用） |
| [`luban self {update,uninstall}` →](https://luban.coh1e.com/commands/self.html) | 自更新二进制 / 完全卸载 luban |
| [`luban completion <shell>` →](https://luban.coh1e.com/commands/completion.html) | 生成 shell 补全脚本（目前仅 clink；bash/pwsh/zsh 待支持） |

## 全局 flag

放在子命令前：

| Flag | 作用 |
|---|---|
| `-V`, `--version` | 打印 `luban X.Y.Z` 退出 |
| `-h`, `--help` | 顶层帮助 |
| `-v`, `--verbose` | 详细 log |

## 约定

- **幂等**：每个命令都能安全重跑。`luban setup` 跳过已装组件、`luban add`
  替换已有 dep、`luban target add` 拒绝同名 target。
- **原子文件写**：每次配置 / manifest 写入走 `tmp + rename`，崩了就只剩
  老文件或新文件，绝不会留半个文件。
- **退出码约定**：`0` 成功，`1` 运行失败（下载失败、cmake 出错），`2` 用户错
  （参数错、操作被拒绝）。
- **log 走 stderr**：`✓` `→` `!` `✗` 前缀的行进 stderr。**stdout 留给机读输出**
  （比如 `compile_commands.json` 路径）。pipe stdout 干净。

## 详细单页参考

每个命令的 examples / flags / behavior 详细见
[英文版命令参考](https://luban.coh1e.com/commands/overview.html)。
