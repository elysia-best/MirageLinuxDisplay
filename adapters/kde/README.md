# KDE Plasma 适配器

这是 Plasma 6 的壁纸包：Plasma 在 X11 与 Wayland 上同样拥有表面，插件
从不创建或放置 X11 窗口。

## 构建与安装

```sh
cmake -S . -B build-kde -G Ninja \
  -DMIRAGE_DISPLAY_PLUGIN_QML=ON \
  -DMIRAGE_DISPLAY_WITH_EGL=ON \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-kde
cmake --install build-kde
```

Qt Quick 显示项按场景图后端自动选择导入路径：

- **OpenGL/EGL**：`EGL_EXT_image_dma_buf_import` + 原生 fence 同步。
- **Vulkan**：external memory FD / DRM 修饰符导入；对无法直接采样的
  修饰符使用同设备 relay/blit 回退。

两种路径下，表面归属与输入都由 Plasma 负责。

窗口状态取自 `org.kde.taskmanager`（由 Plasma/KWin 工作区数据支撑，在
两种会话类型下行为一致），不查询 X11 窗口。`MirageSurfaceEmbed.qml`
变体用于把显示项嵌入其他 Plasma 界面（以 `MIRAGE_DISPLAY_QML_URI`
含 `Embed` 时启用打包）。

## 可安装壁纸包（kpackage）

构建时会在构建目录生成 `mirage-wallpaper-<版本>.zip`（Embed 变体为
`mirage-wallpaper-<版本>-embed.zip`），这是标准的 Plasma/Wallpaper
kpackage：`metadata.json` 位于压缩包根目录，`contents/ui/main.qml` 为主脚本。
安装到当前用户：

```sh
kpackagetool6 -t Plasma/Wallpaper -i mirage-wallpaper-0.1.0.zip
```

卸载：

```sh
kpackagetool6 -t Plasma/Wallpaper -r org.mirage.wallpaper
```

说明：根目录 CPack 生成的 `mirage-linux-display-<版本>-Linux.zip` 是核心库
发行包，布局与壁纸包不同，不能直接交给 `kpackagetool6`。壁纸包由
`mirage-wallpaper-package` 目标生成（随 `cmake --build` 自动构建）。
