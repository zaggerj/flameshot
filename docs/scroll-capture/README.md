# Flameshot zagger fork — 二次开发文档

本目录是本 fork 全部二次开发的设计与运维文档快照（权威源在 Obsidian 库
`zagger_obsidian_gc/zagger/002 - Project/Flameshot 二次开发/`，两侧同步维护）。

## 维护入口

**改任何功能前先读 `总体设计.md`**——它是设计总纲：六条设计铁律、系统架构、
滚动截图五阶段流水线、关键设计决策表、行业对标（参考了 ShareX/deepin/wayscrollshot/
CaptureOCR/Cap 等哪些开源项目、各吸收了什么）、维护指南（改动路由表 + 问题排查链）。

## 文档导航

| 文档 | 内容 |
|---|---|
| `总体设计.md` | 设计入口：原则 / 架构 / 行业对标 / 维护指南 / 演进规划 |
| `SPEC.md` | 原始需求冻结与验收总纲（不可协商项） |
| `滚动截屏调研.md` | 行业方案调研、选型依据、三维对标核对（§10） |
| `滚动截屏算法设计.md` | 拼接内核算法细节与全部参数依据 |
| `功能清单与交接.md` | 44 项功能总账、代码地图、打包前环境准备、构建命令链、环境坑 |
| `实施记录.md` | 逐轮证据流水（v1 → 21 轮迭代） |

## 本 fork 相对上游的增量

- `src/utils/scrollcapture.{h,cpp}` + `src/utils/scrolluia.{h,cpp}` — 滚动截图核心（~3400 行）
- `src/utils/crashhandler.{h,cpp}` — 崩溃取证（minidump）
- `src/tools/scroll/` — 工具栏滚动截图按钮
- `installer/` — 自制纯 Win32 安装器/卸载器（静默安装、优雅退出旧实例、HKCU 自启动）
- `tools/` — 9 件测试运维工装（端到端验证 / 托盘诊断 / 打包 payload）
- 本目录 — 全套设计文档

## 快速上手（详见 `功能清单与交接.md` §4）

```bash
# 构建（MSYS2 MinGW-w64 g++ 16.2 + Qt 6.12 + CMake/Ninja）
ninja -C build flameshot
# 自检
ninja -C build-selftest && ./build-selftest/src/flameshot-cli.exe selftest
# 打包前环境准备与验收流程：功能清单与交接.md §4.0–§4.2（冒烟不过禁止打包）
```
