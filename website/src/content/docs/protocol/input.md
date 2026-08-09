---
title: 指针输入与窗口状态
description: 指针 enter/leave/motion/button/axis 与 WINDOW_STATE 的报文格式与坐标约定。
---

坐标为输出物理像素、左上角原点；时间戳使用单调微秒时钟；修饰符在已知时使用 Linux 输入修饰符位，否则为零。

## 指针移动

```text
f32 x
f32 y
u64 timestamp_us
u32 modifiers
```

## 指针按键

```text
f32 x
f32 y
u32 button
u32 state
u64 timestamp_us
u32 modifiers
```

按键取 Linux `BTN_LEFT`、`BTN_RIGHT`、`BTN_MIDDLE`、`BTN_SIDE`、`BTN_EXTRA` 码；`state` 为 0 释放、1 按下。

## 指针滚轮

```text
f32 x
f32 y
f32 delta_x
f32 delta_y
u32 source
u64 timestamp_us
u32 modifiers
```

增量是逻辑滚轮刻度；来源为 wheel=0、finger=1、continuous=2。拖拽由移动事件加按键状态重建。

## 进入与离开

`POINTER_ENTER` 携带 `x`、`y` 与 `timestamp_us`；`POINTER_LEAVE` 只携带 `timestamp_us`。适配器应保证 enter/leave 与桌面环境的指针焦点语义一致。

## 窗口状态

`WINDOW_STATE { u32 flags }` 携带位标志，由适配器从 DE 的工作区/任务模型计算，绝不来自裸 X11 查询。KDE 适配器从 `org.kde.taskmanager`（Plasma/KWin 工作区数据）读取遮盖、活跃、最大化与全屏事实。
