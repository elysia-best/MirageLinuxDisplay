---
title: 握手与注册
description: HELLO/WELCOME、显示端注册、消费能力上报与生产端注册的报文格式。
---

## 公共握手

对端的第一条消息是 `HELLO`；broker 回复 `WELCOME` 或致命 `ERROR`。

`HELLO`：

```text
u32 role
u16 min_minor
u16 max_minor
u64 features
string name
string version
```

`WELCOME`：

```text
u16 selected_minor
u16 reserved
u64 features
string server_name
string server_version
```

`WELCOME` 之后，显示端发送 `REGISTER_OUTPUT`，渲染端发送 `REGISTER_PRODUCER`；注册被接受前不允许任何角色专属流量。

## 显示端注册

`REGISTER_OUTPUT`：

```text
string stable_id
string name
u32 physical_width
u32 physical_height
u32 logical_width
u32 logical_height
u32 scale_120
u32 refresh_mhz
u32 transform
u32 drm_render_major
u32 drm_render_minor
u64 input_caps
```

`scale_120` 以每逻辑缩放系数 120 为单位；`transform` 使用 `wl_output.transform` 的数值 0–7。

broker 回复 `OUTPUT_ACCEPTED { u64 output_id }`。显示端随后发送 `CONSUMER_CAPS`：

```text
u64 sync_caps
u64 color_caps
u32 max_width
u32 max_height
bytes16 device_uuid
bytes16 driver_uuid
array<format_cap> formats
```

格式能力为：

```text
u32 fourcc
u32 plane_count
u64 modifier
```

至多允许 256 个格式能力。

显示端可通过 `UPDATE_OUTPUT` 在会话中更新几何信息（物理/逻辑尺寸、`scale_120`、刷新率、变换），broker 据此决定是否重新协商 `OUTPUT_CONFIG`。

## 生产端注册

渲染端在公共握手后发送 `REGISTER_PRODUCER`，广告稳定输出标识、渲染器种类、DRM 渲染节点、设备与驱动 UUID，以及其支持的 `(fourcc, plane_count, modifier)` 元组。

`PRODUCER_ACCEPTED` 之后，broker 向生产者发送 `OUTPUT_CONFIG`（选定范围与格式）：

```text
u32 physical_width
u32 physical_height
u32 refresh_mhz
u32 transform
u32 fourcc
u32 plane_count
u64 modifier
```

## 协商规则

broker 为同一稳定输出标识建立路由，取消费者与生产者格式能力的**交集**协商：

- fourcc 与 modifier 必须完全匹配；
- plane_count 取双方都支持的值；
- 消费者上报的 `max_width` / `max_height` 约束生产者的输出尺寸。

协商成功后 broker 才进入[缓冲池绑定](/protocol/buffers/)阶段。
