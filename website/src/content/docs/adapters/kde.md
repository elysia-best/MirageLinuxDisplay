---
title: KDE Plasma 适配器
description: Plasma 6 壁纸包的构建、安装、配置与 EGL/Vulkan 双后端实现。
---

KDE 适配器是 Plasma 6 的 `Plasma/Wallpaper` 包，后端为 Qt Quick 模块；同一个包在 Plasma X11 与 Plasma Wayland 的 `plasmashell` 中运行，不分化出裸 X11 宿主。

## 职责划分

| 层 | 职责 |
|---|---|
| Plasma `WallpaperItem` | 拥有每屏壁纸表面及其生命周期 |
| Qt Quick 显示项 | 导入 DMA-BUF 帧、绘制，并观察指针事件 |
| Plasma/KWin 工作区桥 | 上报遮盖、活跃、最大化与全屏窗口事实 |
| `libmirage-display` | 协议握手、缓冲池、帧、同步 FD 与输入消息 |

输出标识由 Plasma/Qt 暴露的 `QScreen` 名称、厂商、型号与序列号派生；几何与设备像素比来自壁纸项的 `Screen` 对象；窗口与工作区状态来自 Plasma 任务模型或 KWin 工作区接口（如 `KWinWorkspaceWrapper`），绝不直接查询 X11 窗口。

## 双后端

Qt Quick 显示项同时支持 **OpenGL/EGL** 与 **Vulkan** 两条导入路径：

- EGL 路径使用 `EGL_EXT_image_dma_buf_import` 与原生 fence 同步；
- Vulkan 路径使用 external memory FD / DRM 修饰符导入，并以同设备 relay/blit 回退到宿主可采样图像。

指针观察必须让 Qt 事件过滤器返回 `false`，使 Plasma 继续接收桌面点击、右键菜单、拖放与滚轮事件；渲染器由移动事件加按键状态重建拖拽。

## 构建与安装

构建需要 CMake 3.20、Ninja、Qt 6.5 或更新版本（Gui、Qml、Quick）、pkg-config 以及 EGL/GLESv2 开发包。`kpackagetool6` 用于安装壁纸包。从仓库根目录执行：

```sh
cmake -S . -B build-kde-package -G Ninja \
  -DMIRAGE_DISPLAY_PLUGIN_QML=ON \
  -DMIRAGE_DISPLAY_WITH_EGL=ON \
  -DMIRAGE_DISPLAY_QML_URI=Mirage.DisplayEmbed
cmake --build build-kde-package --target mirage-wallpaper-package
```

构建目录只用于生成包，不会把核心库或 QML 模块安装到系统路径。

安装到当前用户：

```sh
kpackagetool6 -t Plasma/Wallpaper \
  -i build-kde-package/adapters/kde/mirage-wallpaper-0.2.0.zip
```

卸载：

```sh
kpackagetool6 -t Plasma/Wallpaper -r org.mirage.wallpaper
```

:::note[自包含壁纸包]
`mirage-wallpaper-package` 生成的 ZIP 是标准 Plasma/Wallpaper kpackage：`metadata.json` 位于压缩包根目录，`contents/ui/main.qml` 为主脚本，原生 QML 模块随包内置在 `contents/ui/MirageDisplayEmbed`，无需另行安装 QML 模块或系统库。根目录 CPack 生成的 `mirage-linux-display-<版本>-Linux.zip` 是核心库发行包，不能交给 `kpackagetool6`。
:::

## 配置项

安装后在"桌面壁纸"中选择 **MirageWallpaper**，按需配置：

- **Display name**：输出名称（留空自动）。
- **Broker socket**：`mirage-display-v1` broker 的 Unix 域套接字路径（留空使用默认路径）。
- **Forward pointer events**：把指针事件回传给渲染器。
- **Show diagnostics**：显示后端、连接状态、输出与帧计数等信息。

## 相关

- [适配器边界](/adapters/boundary/)
- [规划中的适配器](/adapters/planned/)
- [GPU 助手](/dev/gpu/)
