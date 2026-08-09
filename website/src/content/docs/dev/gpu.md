---
title: GPU 助手
description: EGL 与 Vulkan 的 DMA-BUF 导入、relay/blit 回退、帧同步导入与导出。
---

GPU 助手把协议帧（DMA-BUF + 显式同步）接入宿主的 EGL 或 Vulkan 场景，让 DE 适配器在自有壁纸表面上采样。两套助手都提供稳定的 C ABI，DTO 显式打包。

## EGL（`mirage_display_egl.h`）

使用 `EGL_EXT_image_dma_buf_import` 导入，配合原生 fence 同步：

```c
md_egl_importer_t* imp = md_egl_importer_new(&ctx);   /* ctx 只需 EGLDisplay */
md_egl_importer_import_pool(imp, &pool);              /* 生成 EGLImage */
md_egl_wait_acquire_sync(imp, acquire_sync_fd);       /* 消费 acquire FD，插入原生 fence 等待 */
md_egl_release_after_current_context(imp, release_syncobj_fd); /* 消费 release FD */
md_egl_importer_release_pool(imp);                    /* 释放全部 EGLImage */
```

`md_egl_imported_pool_t` 保存每个缓冲的 `EGLImageKHR`；`release_after_current_context` 从当前 GL 上下文把 fence 附加到 release syncobj。

## Vulkan（`mirage_display_vulkan.h`）

external memory FD / DRM 修饰符导入：

- `md_vk_importer_new(&ctx)`：使用已创建的 instance/device，`image_usage` 由调用方指定。
- `md_vk_importer_import_pool`：为每个平面绑定 `VkDeviceMemory`，创建 `VkImage`/`VkImageView`；NV12 等格式还会创建 `VkSamplerYcbcrConversion`。
- `md_vk_import_acquire_sync` / `md_vk_import_release_syncobj`：导入并消费帧同步 FD，返回可提交的信号量（acquire 是临时导入，release 是二进制信号量，必须在最终读提交中信号）。
- `md_vk_importer_acquire_barrier` / `release_barrier`：协议 v1 要求的 `VK_IMAGE_LAYOUT_GENERAL` 队列族所有权屏障。
- `md_vk_fourcc_to_format` / `md_vk_query_format_caps`：fourcc 映射与 DRM 修饰符能力枚举（`caps=NULL, capacity=0` 时只查询数量）。
- `md_vk_result_string`：把 `VkResult` 转成可读字符串。

### relay/blit 回退（`mirage_display_vulkan_blit.h`）

对无法直接采样的修饰符，使用同设备 blit 把导入图像复制到宿主可采样的图像：

```c
md_vk_blitter_t* bl = md_vk_blitter_new(&blit_ctx);
md_vk_blitter_blit(bl, pool, buffer_index, acquire_semaphore, release_semaphore);
/* 之后用 md_vk_blitter_image/layout/format/width/height 采样宿主图像 */
```

blit 会等待 acquire 信号量，在导入图像释放回 `VK_QUEUE_FAMILY_FOREIGN_EXT` 后信号 release 信号量，并在返回前等待完成；信号量仍归 importer 所有。

### 导出（`mirage_display_vulkan_export.h`）

渲染端把 Vulkan 图像导出为 DMA-BUF 帧：

```c
md_vk_exporter_t* ex = md_vk_exporter_new(&export_ctx);  /* 可带 drm_render_fd */
md_vk_exporter_create_pool(ex, &info);                   /* 替换当前池 */
md_vk_exporter_acquire(ex, &index);                      /* 轮询 release syncobj，返回空闲槽 */
md_vk_exporter_export_frame(ex, index, semaphore, &acquire_fd, &release_fd);
md_vk_exporter_copy_frame(ex, index, src_img, layout, w, h, &acquire_fd, &release_fd);
md_vk_exporter_cancel_frame(ex, index);                  /* 提交失败后回滚槽位 */
```

- `md_vk_exporter_acquire` 返回 `MD_ERR_WOULD_BLOCK` 表示所有槽位仍被消费者持有；槽位在 release 之前绝不复用。
- `export_frame` 把已信号的二进制信号量导出为 sync_file，并新建未信号的二进制 DRM syncobj；两个 FD 归调用方所有，用于 `md_producer_submit_frame()`。
- 借用的描述符与 image view 在池替换之前一直有效。

## 相关

- [消费者库](/dev/consumer/)
- [生产者库](/dev/producer/)
- [KDE Plasma 适配器](/adapters/kde/)（EGL 与 Vulkan 双后端）
