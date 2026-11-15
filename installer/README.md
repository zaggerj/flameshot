# Windows 安装包构建说明

产物：`Flameshot-15.0.0-Setup.exe`（约 38MB，单文件，双击即装）。

## 组成

| 文件 | 作用 |
|---|---|
| `setup.cpp` | 安装器本体，纯 Win32 + 静态 zlib，不依赖 Qt |
| `uninstaller.cpp` | 独立卸载器（约 380KB），编译后作为资源内嵌，安装时释放为 `uninstall.exe` |
| `installer.rc` / `resource.h` / `installer.manifest` | 图标、载荷资源、VERSIONINFO、清单（asInvoker + comctl32 v6 + DPI 感知） |
| `payload.bin` | `stage/` 打成的压缩归档，生成物，不要手改 |
| `../stage/` | 安装载荷：`build/src/flameshot.exe` + windeployqt 的 DLL/插件 + `flameshot.ico` |

## 重建步骤

```bash
# 0) 先重新编译应用，并把结果放进 stage/
ninja -C build flameshot
cp build/src/flameshot.exe stage/flameshot.exe

# 1) 重新生成载荷（stage/ 里有什么就装什么）
python tools/make_payload.py

# 2) 编译资源（会内嵌 payload.bin 和 uninstaller.exe）
cd installer
windres installer.rc -O coff -o installer.res

# 3) 编译卸载器
g++ -O2 -municode -mwindows -static uninstaller.cpp -o uninstaller.exe \
    -lole32 -lshell32 -luuid -s

# 4) 编译安装器（必须 -static，否则缺 libwinpthread-1.dll 会 127 退出）
g++ -O2 -municode -mwindows setup.cpp installer.res -o Flameshot-15.0.0-Setup.exe \
    -I. -lz -lgdi32 -lole32 -luuid -lcomctl32 -lshell32 -lshlwapi -static

# 5) 自检：只剩系统 DLL
objdump -p Flameshot-15.0.0-Setup.exe | grep "DLL Name"
```

工具链：MSYS2 MinGW-w64（`D:/install/Scoop/apps/msys2/current/mingw64/bin`）。

## 静默部署

```
Flameshot-15.0.0-Setup.exe --silent [--dir=<路径>] [--no-desktop] [--no-autostart] [--launch]
uninstall.exe --silent
```

退出码 0 表示成功，非 0 表示失败；过程日志追加到 `%TEMP%\FlameshotSetup.log`。
静默模式下不会弹任何对话框，适合脚本批量安装或验收。

## 安装器做的事

1. 停掉安装目录里正在运行的 `flameshot.exe`（只认该目录的映像，不碰别处的构建）。
2. 解包 91 个文件到目标目录，已存在的 `flameshot.ini` **不覆盖**（保留用户设置）。
3. 若无 ini 则写一份默认值：`Ctrl+Alt+A` 截图 / `Ctrl+Alt+S` 滚动截屏 / `Ctrl+Alt+H` 打开目录，`captureActiveMonitor=true`。
4. 释放 `uninstall.exe`，建开始菜单与桌面快捷方式，写 `HKCU` 的 `App Paths`、`Run`（`"…\flameshot.exe" daemon`）、`Uninstall`。

## 卸载器做的事

把自身复制到 `%TEMP%` 再运行，因此能删除安装目录里的每一个文件（包括 `uninstall.exe` 自己），
不需要 `MOVEFILE_DELAY_UNTIL_REBOOT` 等重启；随后清理快捷方式与 `HKCU` 三个键。
