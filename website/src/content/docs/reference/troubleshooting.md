---
title: 故障排查
description: 构建、连接、GPU 与桌面环境的常见问题与排查路径。
---

## 构建问题

**Vulkan / EGL 助手没有构建？**

助手在检测到相应开发环境时自动构建。确认安装了 Vulkan 开发包（`find_package(Vulkan)` 可检测）或 EGL/GLESv2 开发包，或用 `-DMIRAGE_DISPLAY_WITH_VULKAN=ON` / `-DMIRAGE_DISPLAY_WITH_EGL=ON` 显式开启；对应依赖缺失时会报错而不是静默跳过。

**KDE 壁纸包构建报错？**

`MIRAGE_DISPLAY_PLUGIN_QML=ON` 要求 `MIRAGE_DISPLAY_WITH_EGL=ON`，且需要 Qt 6.5+（Gui、Qml、Quick）与 EGL/GLESv2 开发包。建议使用独立的 `build-kde-package` 目录，避免复用旧缓存。

## 连接问题

**消费者连不上 broker？**

- 确认 broker 已监听 `$XDG_RUNTIME_DIR/mirage-wallpaper/display-v1.sock`，权限为 `0600`。
- 确认消费者与 broker 属于同一 UID：broker 通过 `SO_PEERCRED` 拒绝不同 UID 的对端。
- 命令行工具可用 `mirage_mock_broker` 做端到端验证。

**握手失败或连接立即断开？**

检查 `md_display_connect` 返回的错误码与 `on_disconnected` 的 `reason`/`message`。协议违规总是致命错误；确认 `mirage-display-v1` 报文头、FD 数量与消息格式符合[协议参考](/protocol/overview/)。

## 帧与同步问题

**没有帧回调？**

- 确认生产端与消费端协商出了兼容的 `(fourcc, plane_count, modifier)`：broker 取交集协商，不匹配则不下发 `BIND_BUFFERS`。
- 确认缓冲池已经 `BIND_BUFFERS` 且不是旧代际：非当前绑定代际的帧会被直接关闭丢弃。

**同步描述符泄露或双关？**

帧的 acquire sync_file 与 release syncobj FD 所有权转移给帧回调，每个路径都必须恰好关闭一次；关闭未信号的 release 描述符属于异常回退，可能让生产者超时该槽位。详见[缓冲池与帧](/protocol/buffers/)。

## 桌面环境问题

**Plasma 桌面点击/右键失效？**

指针观察必须让 Qt 事件过滤器返回 `false`，使 Plasma 继续接收桌面点击、右键菜单、拖放与滚轮事件。检查壁纸包的 "Forward pointer events" 配置。

**窗口状态不更新？**

窗口与工作区状态来自 Plasma 任务模型或 KWin 工作区接口（如 `org.kde.taskmanager`），不查询裸 X11 窗口。确认会话类型下工作区数据源可用。

**如何收集更多信息？**

开启壁纸包的 "Show diagnostics"（后端、连接状态、输出与帧计数），并抓取 broker/适配器日志后提交 [GitHub Issue](https://github.com/elysia-best/MirageLinuxDisplay/issues/new/choose)，说明系统版本、会话类型（X11/Wayland）、复现步骤、预期结果与实际现象。

## 文档网站部署

推送 `website/**` 到 `master` 会触发 `.github/workflows/deploy-docs.yml` 构建并发布到 GitHub Pages。首次使用前需在仓库 **Settings → Pages** 将发布源设为 **GitHub Actions**；部署地址为 `https://elysia-best.github.io/MirageLinuxDisplay/`。
