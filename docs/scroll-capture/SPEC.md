---
tags: [project, flameshot, windows, cpp, qt6]
status: 已验收（v1 交付）
created: 2026-11-14
updated: 2026-11-14
owner: zagger
---

# Flameshot Windows 二次开发 SPEC

## 0. 目标

在 Windows 上自行打包一份 Flameshot 派生版，实现三项上游不具备的能力：全局快捷键自由配置、滚动截屏、序号标注可缩小到 5×5px。

同步索引：[[总体设计]]（设计总纲/架构/行业对标/维护指南）、[[实施记录]]、[[功能清单与交接]]

---

## 1. 基线

| 项 | 值 |
|---|---|
| 上游 | `https://github.com/flameshot-org/flameshot` |
| 基线 commit | `88a968c85cf5ece9d073bb1232b58f9125482bd2`（2026-11-13） |
| 版本 | CMakeLists.txt 中 `FLAMESHOT_VERSION 15.0.0` |
| 本地克隆 | `C:\Users\Administrator\WorkBuddy\2026-11-14-11-08-20\flameshot` |
| 技术栈 | C++20 / Qt 6 / CMake ≥ 3.22 / QHotkey（Windows、macOS） |
| 许可证 | GPL-3.0-or-later |
| 打包 | CPack，Windows 便携版 ZIP / WIX MSI；`windeployqt` 补 Qt DLL |

**项目定位**：抓一帧屏幕 → 全屏置顶无边框窗口冻结画面 → 就地标注（箭头 / 文字 / 马赛克 / 序号气泡等 30+ 工具）→ 复制到剪贴板 / 保存 / 固定悬浮 / 上传。另有 CLI、托盘、配置文件、Linux D-Bus。

**架构硬约束**：整个编辑链路只围绕**单张 QPixmap**（`CaptureContext.screenshot` / `origScreenshot`），`CaptureWidget` 窗口尺寸等于屏幕尺寸。所有改造都必须兼容这一点，否则标注 / 撤销 / 导出会全线崩。

---

## 2. 需求冻结（用户 2026-11-14 确认）

| # | 需求 | 冻结结论 |
|---|---|---|
| 1 | 全局快捷键 | **全部自由配**。4 个动作：捕获屏幕、第二捕获键（原硬编码 PrtSc）、截图历史（原 Shift+PrtSc）、滚动截屏触发键。任意改、留空 = 禁用、**改完立即生效不重启** |
| 2 | 滚动截屏 | **自动滚 + 拼接即出图**。点一次「开始」，程序自动一帧帧滚 / 拍，滚到底或按 Esc 停；拼好的长图直接进剪贴板 / 存 PNG，**不进标注界面**。长图标注明确不做 v1 |
| 3 | 序号标注 | 嫌当前最小 32–34px 太大、挡内容，要求**尽可能小，最小 5×5px**。气泡直径做成独立像素档位；直径 < 12px 时**自动隐藏数字**，只留圆点 |
| 4 | 打包 | Windows x64 **ZIP 便携版优先**，MSI 可选 |

**被明确否决的做法**：5px 内还要塞数字；滚动截屏 v1 就做长图标注画布；只解放 PrtSc 一条热键。

---

## 3. 验收总纲（不可协商）

1. **不接受「编译通过」代替功能验证**，也不接受 mock / 局部成功冒充完成。
2. 每个票据必须有可复现的**实测证据**（命令输出、像素量测结果、实际按键行为、输出图片尺寸），记录进 [[实施记录]]。
3. 未通过验收的票据保持「进行中 / 阻塞」，不得标记完成。
4. 代码改动与文档状态必须同步；环境与网络造成的阻塞要如实写明，不得略过。

---

## 4. 改造清单（票据）

| 票据 | 内容 | 改动文件 | 难度 |
|---|---|---|---|
| T0 | 工具链核验 + 基线 ZIP 构建 | 无源码改动 | 前置 |
| T1 | 序号气泡最小 5px | `src/tools/circlecount/circlecounttool.cpp` | 低 |
| T2 | 4 个全局键自由配 + 热生效 | `src/core/globalshortcutfilter.{h,cpp}`、`src/core/flameshot.{h,cpp}`、`src/config/shortcutswidget.cpp`、`src/config/setshortcutwidget.cpp`、`src/utils/confighandler.{h,cpp}` | 中 |
| T3 | 自动滚 + 拼接即出图 | 新增 `src/utils/scrollcapture.{h,cpp}`；改 `src/main.cpp`、`src/core/capturerequest.{h,cpp}`、`src/CMakeLists.txt`、`src/utils/CMakeLists.txt` | 高 |
| T4 | 出最终 ZIP + SHA256 | 无源码改动 | 低 |

---

## 5. 详细设计

### T0 工具链

官方 CI 参考 `.github/workflows/Windows-pack.yml`：

- VS 2022 MSVC x64 + CMake ≥ 3.22 + Qt 6.9.3（`win64_msvc2022_64`，含 `qtimageformats`）
- WiX v3（仅 MSI 需要） + vcpkg `openssl-windows`（`-DENABLE_OPENSSL=ON`）
- QHotkey / Qt-Color-Widgets / KDSingleApplication 由 CMake `FetchContent` 拉取，首次配置需能访问 GitHub、GitLab

```
cmake -S flameshot -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=<Qt>/msvc2022_64 -DENABLE_OPENSSL=ON
cmake --build build --config Release
cd build && cpack -G ZIP -B Package
```

**DoD**：`flameshot.exe` 能启动、能截一张图、配置能落盘。基线 ZIP 产出并留档。

### T1 序号气泡尺寸

现状根因（不在配置，在绘制公式）：

- `confighandler.cpp:120` `drawCircleCounterSize` 下限已是 1；`capturewidget.cpp:1075` `qBound(1, size, maxToolSize)`；`sidepanelwidget.h:19` `maxToolSize = 50`
- `SidePanelWidget` **已有** `QSpinBox` + `QSlider`（1–50），滚轮调节也已接通 → **不需要新增 UI、不需要新增配置项**
- `circlecounttool.cpp:13` `THICKNESS_OFFSET 15`，`bubble_size = size() + 15`，而 `drawEllipse(center, rx, ry)` 后两参是**半径** → 最小直径 `2*(1+15) = 32px`，加描边 34px

设计：

- 直径映射 `diameter = 2 * size + 3`（size=1 → **5px**，size=50 → 103px），用 `qreal` 半径版 `drawEllipse(QPointF, qreal, qreal)`
- 描边半径 = `rx + PADDING_VALUE / 2.0`
- 直径 `< 12px` 时跳过 `drawText`，只画实心圆点
- **必须同步 4 处**：`boundingRect()`、`mousePreviewRect()`、`process()`、`paintMousePreview()`；另统一 `onSizeChanged()` 的传参（现 preview 传 `toolSize + PADDING_VALUE` 而实际绘制传 `toolSize`，本身就不一致），否则出现残影 / 预览偏差

**DoD**：滑杆拨到 1，导出 PNG 中气泡**实测 5×5 像素**（量像素，不靠肉眼）；拨到 50 无残影；<12px 无数字；预览圆与实际落点大小一致。

### T2 全局快捷键

现状根因：

- `src/core/globalshortcutfilter.cpp:13,17` 硬编码 `RegisterHotKey(NULL,1,0,VK_SNAPSHOT)` 与 `MOD_SHIFT|VK_SNAPSHOT`，源码自带 TODO 承认「proper global support would need custom shortcuts defined by the user」
- `src/core/flameshot.cpp:117` 的 `QHotkey` 才是可配置的那条（`TAKE_SCREENSHOT`，Windows 默认 `Meta+Shift+X`），**只在构造时读一次配置** → 改完必须重启，提示写死在 `setshortcutwidget.cpp:48`
- `src/config/shortcutswidget.cpp:219-227` 把 "Print Screen" 行设成 identifier 为空 = 不可编辑
- `flameshotdaemon.cpp:393-396` 在 Windows 安装 `GlobalShortcutFilter`

设计：

1. 在 `confighandler.cpp` 的 `recognizedShortcuts` 增加 4 个全局动作键（Windows 默认值保持兼容）：捕获屏幕、第二捕获键、截图历史、滚动截屏
2. `GlobalShortcutFilter` 改为配置驱动：从 `QKeySequence` 映射到 Win32 `vk` + `MOD_*`，用固定 ID 区间注册；`nativeEventFilter` 按 `LOWORD(msg->wParam)` 的 ID 派发到对应动作；注册失败（被占用）通过通知明确报错并保留旧值
3. `Flameshot` 里 QHotkey 与原生 filter 统一由 `ConfigHandler::fileChanged` 触发**反注册 + 重注册**，去掉「需重启」提示
4. `ShortcutsWidget` 放开 Print Screen 行、补齐 4 个可编辑条目；留空 = 禁用
5. 保留「禁用硬编码 PrtSc」能力（留空即可），避免与 Windows 自带截图工具抢键；`ignorePrntScrForcesSnipping` 现有逻辑保留

**DoD**：设置里改 4 个键 → **不重启立即生效**；留空后按键无反应；热键被占用给明确提示；PrtSc 不与 Windows 截图工具冲突。

### T3 滚动截屏（自动滚 + 拼接即出图）

上游态度：issue #4548 被维护者以「out of scope，需要视频采集方案」关闭，#1130 自 2021 年长期未处理 → 必须自行 fork，不等上游。

架构障碍：`CaptureWidget` 是覆盖全屏的置顶窗口，一出现就挡住目标窗口并冻结画面，**滚动必须发生在它创建之前**。因此不改现有编辑链路，新增独立 Stage-0 采集器：

1. 入口：`scroll` CLI 子命令（`src/main.cpp:287-370` 是命令注册处）、工具栏按钮、T2 新增的全局热键
2. 框选滚动区域（复用 `SelectionWidget` 或独立轻量矩形选择窗口）
3. 隐藏自身窗口，`WindowFromPoint` 找目标 `HWND`，`PostMessage(WM_MOUSEWHEEL)` / `SendInput` 逐段滚动
4. 每帧 `QScreen::grabWindow` 裁同一矩形，帧间保留 20–30% 重叠
5. 重叠带逐行灰度差（SAD）搜索求垂直位移后拼接 —— **纯 QImage 实现，不引入 OpenCV**，避免打包依赖爆炸
6. 终止：连续 N 帧位移 ≈ 0，或用户按 Esc；设最大高度上限（建议 16384px）防内存溢出
7. 产出直接写 PNG + 进剪贴板，`CaptureRequest` 的注入字段作为 v2 扩展点保留

必须处理的坑：混合 DPI 物理像素换算（复用 `screengrabber.cpp:781` 的 Windows 逻辑）、固定表头 / 悬浮按钮重复出现（顶部锚定区裁剪）、滚动动画未结束就抓帧（帧间加短延时）。

**DoD**：3 屏长网页点一次开始，输出 PNG 高度 ≈ 3× 视口、**无断层 / 重复 / 漏行**，Ctrl+V 可直接粘贴；Esc 能中止；混合 DPI 不错位。

**v1 明确限制**：懒加载、无限滚动、动画 / 视频内容会漏帧或无法判定终点；长图不能标注。写进功能说明，不假装支持。

### T4 打包

`CMakeLists.txt` / `Windows-pack.yml` **不需要改**，`windeployqt` 自动补 Qt DLL。默认 `USE_PORTABLE_CONFIG=ON` 出 ZIP；需要 MSI 时改 `OFF` 并装 WiX v3。

**DoD**：产出 `flameshot-15.0.0-win64-*.zip`，附 SHA256，解压后双击可运行且三项功能可用。

---

## 6. 实施顺序与依赖

```
T0（前置，阻塞后续验收）
 └─ T1（独立，最先出成果）
     └─ T2（滚动截屏热键依赖它）
         └─ T3（最大，独立分支）
             └─ T4
```

T1 与 T2 互不依赖，可并行开发；**但任何一项的验收都依赖 T0 的可编译基线**。

---

## 7. 风险与限制

| 风险 | 处理 |
|---|---|
| 本机 `cmake` / `qmake` 均不在 PATH，Qt 6.9.3 + VS2022 需从零安装（数 GB） | T0 先核验；装不了就如实标记阻塞，不伪造构建成功 |
| `FetchContent` 需访问 GitHub / GitLab | 首次配置可能卡网络，需代理或预置 `external/` 目录 |
| 自动滚对懒加载 / 动画内容漏帧 | v1 不处理，明确写入限制 |
| Win32 热键被系统或其他软件占用 | 注册失败明确报错，保留旧值 |
| GPL-3.0 派生 | 自用不外发无额外义务；外发须保留 LICENSE 并提供源码 |

---

## 8. 关键代码定位（基于 88a968c）

| 位置 | 作用 |
|---|---|
| `src/core/globalshortcutfilter.cpp:13,17,22-49` | Windows 硬编码 PrtSc 热键与派发 |
| `src/core/flameshot.cpp:117-122` | QHotkey 注册 `TAKE_SCREENSHOT` |
| `src/core/flameshotdaemon.cpp:393-396` | 安装原生事件过滤器 |
| `src/config/shortcutswidget.cpp:219-227` | 不可编辑的 Print Screen 行 |
| `src/config/setshortcutwidget.cpp:48` | 「需重启」提示 |
| `src/utils/confighandler.cpp:120,201,205` | 序号尺寸下限 / 各平台默认全局键 |
| `src/widgets/panel/sidepanelwidget.h:19` | `maxToolSize = 50` |
| `src/tools/circlecount/circlecounttool.cpp:12-15,42,53,113,141-152,191` | 序号气泡尺寸计算四处 |
| `src/widgets/capture/capturewidget.cpp:107-130,497` | 截图注入点 / `pixmap()` 导出 |
| `src/utils/screengrabber.cpp:781` | Windows 混合 DPI 抓屏 |
| `src/main.cpp:287-370` | CLI 子命令注册 |
| `.github/workflows/Windows-pack.yml` | 官方 Windows 打包流程 |

---

## 9. 验收结果（2026-11-14，v1）

> T3 / T4 已由第 10 节的 v2 重写取代，本节保留首次交付记录。

逐条证据见 [[实施记录]]。汇总：

| 票据 | 结论 | 决定性实测 |
|---|---|---|
| T0 | 通过 | `Flameshot v15.0.0 / Qt 6.12.0`；真实截屏 3840×3600 PNG；`flameshot.ini` 落盘 |
| T1 | 通过 | 生产绘制代码出图量测：size=1 → **5×5** 无数字；4 → 11×11 无数字；5 → 13×13 有数字；50 → 103×103 |
| T2 | 通过 | 同进程改 ini 立即重注册（Print/Shift+Print/Ctrl+Alt+S → F8/留空/Ctrl+Alt+R）；留空禁用；占用告警；重复键校验报错 |
| T3 | 通过（v1，已被第 10 节取代） | 51 帧全部 300px 位移；PNG 1302×**16401** = `scrollY` 10200×1.5 + 视口 1101（完全对账）；行间距 **120/120 精确 135px**；剪贴板 `image/png`；Esc 中止仍保存 |
| T4 | 通过（v3 重打包） | `flameshot-15.0.0-win64.zip`（34.9MB / 33 DLL）；PATH 裁到仅 System32 仍可运行可截图；SHA256 `f76096f67077077006b2f00dae8a449e1cf7c262affc402b1bad57292124b97a` |

**实现偏离说明**：工具链改用 MSYS2 MinGW-w64 + Qt 6.12（VS2022 因 CDN 大文件超时不可用），`ENABLE_OPENSSL=OFF`、`USE_PORTABLE_CONFIG=ON`。为此新增三处平台适配——资源脚本版本宏改为数字拼接、`/SUBSYSTEM:CONSOLE` 等 MSVC 专有链接选项加 `if(MSVC)` 守卫、新增 `cmake/copy_windows_runtime.cmake` 补齐 MinGW 版 Qt 的传递依赖闭包。MSVC 官方流程不受影响。

**T3 v1 限制（与第 5 节一致）**：仅主屏；懒加载 / 无限滚动 / 动画内容会漏帧或无法判定终点；长图不进标注界面。

---

## 10. T3 v2：拼接内核重写（2026-11-14）

v1 通过验收后复核发现，其拼接方法的验证环境太宽松（测试页无固定头尾栏、无滚动条、步长恒定），而真实页面三者都不满足，且**"匹配失败"与"滚动到底"共用同一个信号**，会静默产出腰斩长图。

先做行业调研（[[滚动截屏调研]]），再按 six 条行业共识重写（[[滚动截屏算法设计]]）。核心改动：

| 项 | v1 | v2 |
|---|---|---|
| 匹配输入 | 底部一条 420px 横带，全像素 SAD | 全帧 48 列行签名，全重叠区 MAD |
| 静态区域 | 不处理 | 静态列剔除 + 固定头尾栏检测（判一次后冻结） |
| 滚动条 | 参与匹配（噪声） | 右侧 8% 排除 |
| 搜索 | 固定 ±45px 窗口 | 从预测位移向外扩散 |
| 提前退出 | `MAD ≤ 2.5` 就停 | 只在 `MAD == 0` 时停 |
| 失败语义 | 与"到底"合并 → 静默截断 | 三态：MATCHED / NO_CHANGE / FAILED，输出标 `clean` 或 `degraded` |
| 固定尾栏 | 每道接缝重复写入 + 丢内容 | 只写一次 |
| 抓帧 | Qt `grabWindow` 整屏抓再裁，106ms | `BitBlt` + DIB section 精确选区，75ms；次数 9.6→6.4/步 |
| 反馈 | 无 | 置顶进度面板（不进画面、不抢焦点） |

**新增验收页** `tools/sticky.html`（固定顶栏 56px + 固定左侧栏 110px + 固定底栏 88px + 200 行确定性色块）与校验器 `tools/stickycheck.cpp`。

**v2 验收结论**（逐条证据见 [[实施记录]]）：

| 场景 | 结果 |
|---|---|
| 固定栏页面 | 200/200 行、0 重复 0 漏行 0 逆序、顶栏只在图首、底栏只在图尾 → `CONTINUOUS` |
| 反向对照（关掉固定栏排除） | 186/200 行、6 处漏行、尾栏重复、31 步 `degraded` → `PROBLEM DETECTED` |
| 无固定栏长页面回归 | 198/198 间距精确 135px、200/200 行 → `CONTINUOUS` |
| 从页面中途开始 | 探测到已滚动 → 自动回顶 → 与从顶部开始结果一致 |
| Esc | 2 帧后中止仍保存，剪贴板 `image/png` 同尺寸 |
| 性能 | 每帧 2540ms → 922ms（48 帧 122s → 87s） |

**v2 仍存在的边界**：仅主屏；懒加载 / 整页重复卡片会标 `degraded`（但不再静默截断）；固定栏超过帧高 35% 不排除。

---

## 11. 本机安装与热键（2026-11-14）

**安装位置**：`%LOCALAPPDATA%\Programs\Flameshot`（用户态，无需管理员；只装运行时，91 文件 / 88MB）

| 功能 | 热键 | 说明 |
|---|---|---|
| 截图（选区 + 标注） | `Ctrl+Alt+A` | 唤出全屏选区，光标所在屏直接进入，Esc 退出 |
| 滚动截屏 | `Ctrl+Alt+S` | 唤出选区 → 拖框 → 自动滚动拼接 → 剪贴板 |
| 打开截图保存目录 | `Ctrl+Alt+H` | 原 `Shift+Print` 改为组合键，避免与系统截图冲突 |

`Print` / `Shift+Print` 不再占用（默认值已改），因此不会和 Windows 自带截图重叠。

**开机自启**：`HKCU\...\Windows\CurrentVersion\Run` → `"<exe>" daemon`。
`daemon` 是本版新增的子命令：只建托盘图标与全局热键，**不显示任何窗口**（对比 `launcher` 每次启动都会弹启动器）。

**验收结论**（详见 [[实施记录]] 第 T5 节）：

| 项 | 结果 |
|---|---|
| 热键注册 | `RegisterHotKey` 探测返回 1409（已被本进程占用），三个键全部注册成功 |
| `Ctrl+Alt+A` | 全屏选区窗口 `3840×2160` 出现且为**前台窗口**，Esc 关闭 |
| `Ctrl+Alt+S` | 滚动截屏选择器出现且为前台窗口，日志 `waiting for region selection` → Esc → `selection cancelled` |
| 选区复制 | 拖出 600×400 后 `Ctrl+C`，剪贴板 `image/png` 正是 600×400 |
| 纯净环境 | `env -i PATH=System32` 下 `--version` 正常，`EXIT=0` |

**已知边界**：前台是提权（管理员）窗口时，Windows UIPI 会拦截热键投递，本程序和其他非提权截图工具一样收不到；开始菜单快捷方式因本机策略禁止脚本调用 COM 未能自动创建，需手动"发送到 → 桌面快捷方式"。


---

## 12. 稳定性修复、双屏、序号默认值与 Windows 安装包（2026-11-14 晚）

### 12.1 本轮要解决的 6 个用户可见问题

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | 托盘里冒出 3–4 个 flameshot 图标 | 只有 `argc==1` 与 `launcher` 会建 `KDSingleApplication`，`daemon` 路径绕过单实例检查 → 每次开机/每次点击都多一个常驻进程，每个进程各画一个托盘图标 | 统一 `claimSingleInstance(verb)`：`daemon`/`capture`/`launcher`/`config` 全部走同一入口，非主实例把 verb 转发给主实例后**静默退出 0** |
| 2 | 按热键没反应、界面"卡死" | 已有叠加层时 `gui()` 只 `emit captureFailed()` 就返回，不把已有窗口提到前台 | 先 `raiseCaptureWindow()`（`AttachThreadInput` + `SetForegroundWindow`）再返回失败；重复触发热键只是把窗口提到前面 |
| 3 | 截图几次后异常退出（静默消失） | `reinitializeAsQApplication()` 里 `delete QCoreApplication::instance()` 再 `new QApplication`，拆掉 Qt 平台集成仍在引用的对象；退出路径还有 `delete qApp`，静态单例（Flameshot / FlameshotDaemon / QHotkey）访问已析构的 app | 全程只建一个 `QApplication`；删除所有重建与 `delete qApp` |
| 4 | 双屏电脑抓错屏、图像错位 | `windowsScreenshot()` 把整块虚拟桌面抓下来再按逻辑几何反推物理偏移，只在"单行同 DPI"布局成立；本机是上下排列且主屏 1.5× | Windows 直接 `QScreen::grabWindow(0)` 抓目标屏；叠加层按光标所在屏定位；新增 Tab 换屏 + 顶部屏幕提示 |
| 5 | 序号默认只有 5px，几乎看不见 | `drawCircleCounterSize` 默认值 1（直径 `2*1+3 = 5px`） | 默认 15（直径 33px，数字清晰），下限仍 1（5px 圆点） |
| 6 | 没有"放大/缩小工具"快捷键 | `TYPE_SIZEINCREASE/DECREASE` 只在配置里定义，没有任何代码绑定 | 绑定 `]` / `[`，并**始终追加** `Ctrl+=` / `Ctrl+-` 作为不被输入法吞掉的备选 |

附带修掉：Windows 上 `QHotkey` 与 `GlobalShortcutFilter` 重复注册同一 chord（每次都 `RegisterHotKey` 失败 1409）；配置窗口只有 513px 宽导致标签被截断；`AbstractLogger::LogFile` 是 `// TODO`，daemon 完全没有日志。

### 12.2 崩溃取证

新增 `src/utils/crashhandler.{h,cpp}`：滚动日志（`%LOCALAPPDATA%\Flameshot\logs\flameshot.log`，>4MB 轮转）+ 未处理异常报告 + minidump（`logs\crashes\crash-<时间>-<pid>.txt|.dmp`）。异常过滤器只使用 `_open/_write` 与 `CreateFileA`，不碰 Qt 与堆，最后返回 `EXECUTE_EXCEPTION_HANDLER`，因此不会弹 WER，也不会留下幽灵托盘图标。

### 12.3 Windows 安装包

`installer/Flameshot-15.0.0-Setup.exe`：纯 Win32 + 静态 zlib，**不依赖它即将安装的 Qt 运行时**。

- 载荷：`tools/make_payload.py` 把 `stage/` 打成 `FLPAY1` 归档（条目表 + zlib 流），91 文件 87.5MB → 34.7MB，作为 `RCDATA` 内嵌。
- 安装：解压到 `%LOCALAPPDATA%\Programs\Flameshot`（用户态，无 UAC），写默认 ini（`Ctrl+Alt+A/S/H`、`captureActiveMonitor=true`），建开始菜单 + 桌面 `.lnk`（`IShellLinkW`，原生代码不受本机"禁止脚本调 COM"策略限制），写 `App Paths` / `Run` / `Uninstall`。
- 卸载：随包释放 380KB 独立卸载器（**不是**复制 38MB 安装包），先把自己复制到 `%TEMP%` 再运行，因此能把安装目录连同 `uninstall.exe` 一起删干净，不需要重启。
- 静默部署：`Setup.exe --silent [--dir=<路径>] [--no-desktop] [--no-autostart] [--launch]`，非零退出码表示失败，日志追加到 `%TEMP%\FlameshotSetup.log`。

### 12.4 工具栏新增「滚动截屏」按钮

滚动截屏原先只能靠全局热键 `Ctrl+Alt+S` 进入，和工具栏其它功能是两套入口。现在它就是一个普通工具按钮：

| 项 | 值 |
|---|---|
| 枚举 | `CaptureTool::TYPE_SCROLLCAPTURE = 25`（加在末尾，不打乱已有用户配置的编号） |
| 工具类 | `src/tools/scroll/scrollcapturetool.{h,cpp}`（`AbstractActionTool`，不可选中、不画东西，只发请求） |
| 按钮位置 | `buttonTypeOrder = 17`，即 **复制 / 保存 / 滚动截屏 / 确认** 这一组动作按钮中间 |
| 图标 | `data/img/material/{black,white}/scroll-capture.svg`（方框 + 向下箭头），已进 `graphics.qrc` |
| 图标快捷键 | `TYPE_SCROLLCAPTURE` = `Ctrl+Shift+S`（在叠加层内生效，可在配置里改） |
| 请求 | `CaptureTool::REQ_SCROLL_CAPTURE` |

**为什么不是就地滚动**：编辑器里显示的是一张冻结的屏幕位图，在它下面滚动永远看不到。所以按钮做的是**交接**：

1. 把当前选区（`m_context.selection + widgetOffset`，即屏幕全局逻辑坐标）交给 `Flameshot::requestScrollCapture()`；
2. 叠加层立刻关闭，`m_scrollCaptureHandoff` 置位，析构时既不上报 `captureFailed` 也不导出；
3. 150ms 后 `ScrollCapture::start(region)` 在**真实桌面**上接管：复用这个选区（不再让用户框第二次），选区中心所在显示器由 `QGuiApplication::screenAt()` 解析；
4. 没选区时（一进叠加层就点按钮）退回原来的交互框选。

`ScrollCapture::start()` 保留原签名并转发到 `start(QRect())`，所以热键、CLI `flameshot scroll`、按钮三条入口走同一条缝合逻辑。

### 12.5 托盘消失 / Ctrl+Alt+A 失效 / 标号快捷键与滚轮（2026-11-14 晚）

用户反馈三条：打开软件不显示托盘、`Ctrl+Alt+A` 打不开截图、标号要有快捷键且能滚轮缩放。三条都是**入口问题**，不是截图内核问题。

| 现象 | 根因 | 修复 |
|---|---|---|
| 双击图标没有托盘 | 12.3 的安装器把快捷方式参数写成 `gui`（当时为了让点击"有反应"），`gui` 是**截一次就退出**的一次性模式，托盘与热键随进程一起消失 | 快捷方式参数改回 `daemon`（只建托盘 + 热键，不弹任何窗口）；`Run` 自启项本来就是 `daemon`，未受影响 |
| `Ctrl+Alt+A` 无反应 | Windows 上 `TAKE_SCREENSHOT` 只由 macOS 的 `QHotkey` 注册，`GlobalShortcutFilter` 只注册了 `CAPTURE_PRTSC`（默认 `Print`）等三个动作；用户 ini 里的 `CAPTURE_PRTSC=Ctrl+Alt+A` 虽然注册成功，但只要 `daemon` 不在就跑不到 | `TAKE_SCREENSHOT` 在 Windows 默认改为 `Ctrl+Alt+A`，并作为第 4 个动作进 `GlobalShortcutFilter`；`CAPTURE_PRTSC` 默认留空（不再占 `Print`），重复 chord 自动去重，不再报 1409 |
| 标号没有快捷键 | `TYPE_CIRCLECOUNT` 默认值为空 | 默认 `N`（与 P/D/A/S/R/C/M/B/I/T/G 均不冲突），按钮 tooltip 会显示 `(N)` |
| 已放下的标号改不了大小 | 滚轮只走 `setToolSize()`，而它只刷新"活动工具"的预览；侧栏选中的已有对象不受影响 | `setToolSize()` 里额外把新尺寸应用到**侧栏选中的** `TYPE_CIRCLECOUNT` 对象；未选中时仍是"设置下一个标号的尺寸"。`Ctrl+滚轮` 保持原来的"改下一个编号" |

配套改动：

1. `configFilePath` 迁移：旧 ini 里的 `CAPTURE_PRTSC` 若存在而 `TAKE_SCREENSHOT` 不存在，一次性搬到 `TAKE_SCREENSHOT` 并删掉旧键，避免设置界面出现两条同值条目。实测 ini 变为 `TAKE_SCREENSHOT=Ctrl+Alt+A / SCROLL_CAPTURE=Ctrl+Alt+S / HISTORY_PRTSC=Ctrl+Alt+H`。
2. 热键注册日志、滚动截屏的选区/交接日志全部改写到 `%LOCALAPPDATA%\Flameshot\logs\flameshot.log`（原来写 stderr，GUI 进程等于丢掉），日志里能直接看到 `Registered global shortcut TAKE_SCREENSHOT as Ctrl+Alt+A` 与 `Tray icon created (system tray available: yes, visible: yes)`。
3. 托盘构造时记录可用性/可见性，下次"没有托盘"的反馈可以先用一行日志区分"进程没起"和"通知区不可用"。

**验收证据（不依赖抓屏）**：

| 项 | 方法 | 结果 |
|---|---|---|
| 托盘 | daemon 启动日志 | `Tray icon created (system tray available: yes, visible: yes)` |
| `Ctrl+Alt+A` | 另起进程 `RegisterHotKey` 探测（同 chord 必失败 1409） | `TAKEN` |
| `Ctrl+Alt+S` / `Ctrl+Alt+H` | 同上 | `TAKEN` / `TAKEN` |
| 配置迁移 | 读安装目录 `flameshot.ini` | 旧 `CAPTURE_PRTSC` 消失，`TAKE_SCREENSHOT=Ctrl+Alt+A` |
| 快捷方式 | 解析 `.lnk` 参数 | 开始菜单 + 桌面均为 `daemon` |
| 标号尺寸映射 | `flameshot selftest` 渲染真实 `CircleCountTool` 并量像素 | size 1→5×5、4→11×11、5→13×13、15→33×33、50→103×103；<12px 无数字 |
| 滚轮语义 | 同上，调生产 `handleMouseWheelEvent` | 裸滚轮 `consumed=no`（→改尺寸）、`Ctrl+滚轮 consumed=yes`（编号 7→8） |

### 12.6 滚动截屏为什么"完全没滚动"：目标窗口识别重写（2026-11-14 夜）

用户框选后点按钮，程序确实生成了图，但**滚的是桌面壁纸**——图是一张和选区等大的单帧。用户的原话是"你先是有矩形截图的，在框中，如果存在滚动条，有滚动区域，应该识别出这个区域"。

#### 根因（日志逐条对应）

```
20:53:32 [info] Scrolling capture: reusing the editor selection
20:53:32 [info] Scroll selection logical=(1071,474 1542x1155) dpr=1.5
20:53:37 [info] First frame 2313x1733
20:53:37 [info] Scroll target window: Program Manager      <-- 这里
20:53:49 [info] Step 0: weak match (mad=10.36), re-grabbing (1)
20:53:52 [info] Frames=1 dy=[] bars=top0/bottom0 ... stopped=Esc pressed
```

| # | 缺陷 | 后果 |
|---|---|---|
| 1 | `WindowFromPoint(选区几何中心)` 单点取窗口 | 中心落在壁纸/侧栏/边框上就拿到 `Program Manager`，滚轮无人接收 |
| 2 | 没有"这个区域能不能滚"的判断 | 把"不可滚"当成"滚到底"，静默产出单帧图 |
| 3 | 滚轮注入点等于选区中心 | 即使窗口对了，点落在固定标签栏/侧栏上也不动 |
| 4 | 回顶用 `Ctrl+Home` | 会把用户正在编辑的窗口内容跳到开头，且不适用于只看不编的应用 |
| 5 | 失败无反馈 | 编辑器已经关闭，用户只看到"屏幕闪了一下"，判定为功能没做 |
| 6 | 选区可超出屏幕几十像素 | `BitBlt` 越界部分为黑，首帧带黑边 |

#### 修法

**a) 窗口投票取代单点。** 选区内部取 6×6 网格采样，每个点 `WindowFromPoint` → `GetAncestor(GA_ROOT)`，排除本进程窗口与 `Progman/WorkerW/Shell_TrayWnd/tooltips_class32`，按命中票数选窗口（平票取面积更小者）。日志输出 `Scroll target window: <标题> (votes=N, frame=...)`。

**b) UI Automation 找滚动容器（核心）。** 新增 `utils/scrolluia.{h,cpp}`：`ElementFromPoint` 起沿 ControlView 向上最多 14 层，取第一个 `UIA_ScrollPatternId` 且 `VerticalViewSize ∈ (0,100)` 的祖先——这正好回答"这个区域能不能滚、滚到哪了"。拿到 `VerticalScrollPercent` 后：

- 回顶：`SetScrollPercent(NoScroll, 0)`，不再`Ctrl+Home`；
- 步进：`SetScrollPercent(NoScroll, p + view*0.45)`，步长按可视比例算，永远留重叠；
- 到底：`p + view >= 99.4` 精确判定，不靠"连着两帧没动"猜。

**c) 采样点不靠中心。** `attachScrollContainer()` 依次尝试中心、四象限、四角（各内缩 1/5～1/8），第一个命中即为容器；容器自身的 `BoundingRectangle` 与选区求交后作为滚轮注入点（`wheel-point`），所以即便没有 ScrollPattern，退化成滚轮时也落在内容上而不是标签栏。

**d) 等自己的叠加层消失。** 编辑器是全屏半透明窗口，交接后若立刻采样会投给 flameshot 自己。`start()` 里轮询选区中心的 `WindowFromPoint`，直到 pid 不是本进程（上限 1.5s）。

**e) 明确失败。** 三种情况各自在 HUD 上给中文提示并写日志，不再产出假长图：
- 选区里没有窗口 → `没有可滚动的内容`；
- 一帧都没滚动 → `滚动截图未完成 / 选区没有滚动，换一块内容区再试`；
- 成功 → `滚动截图完成 / 已保存并复制到剪贴板`。

**f) 选区裁剪到屏幕。** 越界即 `intersect(screen->geometry())`，日志记 `Selection clamped to the screen`。

#### 新增诊断命令

```
flameshot scroll --probe
```

对每个可见顶层窗口跑**产品代码**（投票 + UIA 查找），只读输出，不滚动、不抢焦点、不抓屏。实测本机：

```
visible top-level windows: 3
WINDOW "WorkBuddy" [Chrome_WidgetWin_1] 2182x1432
    target="WorkBuddy" votes=36 | <no scroll container>
WINDOW "ChatGPT" [Chrome_WidgetWin_1] 1920x1234
    target="ChatGPT" votes=36 | Group "对话" (view 22.3%) probe=(1108,701) view=22.3% pos=100.0% wheel-point=(1821,1061)
WINDOW "Microsoft Text Input Application" [Windows.UI.Core.CoreWindow] 3840x2160
    target="ChatGPT" votes=25 | Group "对话" (view 22.3%) ...
```

`votes=36`（6×6 全中）说明投票不再被壁纸夺走；`Group "对话" view=22.3% pos=100%` 说明识别到真实滚动容器、且它当前在底部（回顶逻辑会先归零）。WorkBuddy 自身窗口没有 ScrollPattern 也如实报告 `<no scroll container>`，不会假装成功。

#### 兼容与降级

- 没有 ScrollPattern（部分自绘框架、Java 老应用）→ 自动退回滚轮注入，注入点用容器/内容矩形中心；
- provider 声称支持 ScrollPattern 但 `SetScrollPercent` 无效 → 回顶时检测到 percent 不变，日志 `UI Automation refuses absolute scrolling`，切回滚轮；
- UIA 滚动后画面没变（假阳性容器）→ 连续两帧无变化即 `falling back to wheel events`；
- MT4/UWP/Chromium/WPF/WinForms 走 UIA；`FLAMESHOT_SCROLL_UIA=0` 可整体关闭。
