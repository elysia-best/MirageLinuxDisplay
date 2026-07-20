#define _GNU_SOURCE

#include "mirage_display_vulkan.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MD_DRM_FORMAT(code0, code1, code2, code3) \
    ((uint32_t)(code0) | ((uint32_t)(code1) << 8) | ((uint32_t)(code2) << 16) | \
     ((uint32_t)(code3) << 24))

#define MD_DRM_FORMAT_XRGB8888 MD_DRM_FORMAT('X', 'R', '2', '4')
#define MD_DRM_FORMAT_ARGB8888 MD_DRM_FORMAT('A', 'R', '2', '4')
#define MD_DRM_FORMAT_XBGR8888 MD_DRM_FORMAT('X', 'B', '2', '4')
#define MD_DRM_FORMAT_ABGR8888 MD_DRM_FORMAT('A', 'B', '2', '4')

struct md_vk_importer {
    md_vk_context_t context;
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties;
    PFN_vkImportSemaphoreFdKHR import_semaphore_fd;
    md_vk_imported_pool_t pool;
    bool pool_active;
};

static void clear_pool(md_vk_imported_pool_t* pool) {
    memset(pool, 0, sizeof(*pool));
    for (size_t i = 0; i < MIRAGE_DISPLAY_MAX_BUFFERS; ++i) {
        pool->images[i] = VK_NULL_HANDLE;
        pool->memories[i] = VK_NULL_HANDLE;
        pool->views[i] = VK_NULL_HANDLE;
        pool->acquire_semaphores[i] = VK_NULL_HANDLE;
        pool->release_semaphores[i] = VK_NULL_HANDLE;
    }
}

int md_vk_fourcc_to_format(uint32_t fourcc, VkFormat* format, VkComponentMapping* mapping) {
    if (format == NULL || mapping == NULL) return MD_ERR_INVALID;
    mapping->r = VK_COMPONENT_SWIZZLE_IDENTITY;
    mapping->g = VK_COMPONENT_SWIZZLE_IDENTITY;
    mapping->b = VK_COMPONENT_SWIZZLE_IDENTITY;
    mapping->a = VK_COMPONENT_SWIZZLE_IDENTITY;
    switch (fourcc) {
    case MD_DRM_FORMAT_XRGB8888:
        *format = VK_FORMAT_B8G8R8A8_UNORM;
        mapping->a = VK_COMPONENT_SWIZZLE_ONE;
        return MD_OK;
    case MD_DRM_FORMAT_ARGB8888:
        *format = VK_FORMAT_B8G8R8A8_UNORM;
        return MD_OK;
    case MD_DRM_FORMAT_XBGR8888:
        *format = VK_FORMAT_R8G8B8A8_UNORM;
        mapping->a = VK_COMPONENT_SWIZZLE_ONE;
        return MD_OK;
    case MD_DRM_FORMAT_ABGR8888:
        *format = VK_FORMAT_R8G8B8A8_UNORM;
        return MD_OK;
    default:
        return MD_ERR_UNSUPPORTED;
    }
}

const char* md_vk_result_string(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    default: return "VK_ERROR_UNRECOGNIZED";
    }
}

md_vk_importer_t* md_vk_importer_new(const md_vk_context_t* context) {
    if (context == NULL || context->physical_device == VK_NULL_HANDLE ||
        context->device == VK_NULL_HANDLE) return NULL;
    md_vk_importer_t* importer = calloc(1, sizeof(*importer));
    if (importer == NULL) return NULL;
    importer->context = *context;
    if (importer->context.image_usage == 0) importer->context.image_usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    importer->get_memory_fd_properties =
        (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
            importer->context.device, "vkGetMemoryFdPropertiesKHR");
    importer->import_semaphore_fd =
        (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(
            importer->context.device, "vkImportSemaphoreFdKHR");
    clear_pool(&importer->pool);
    return importer;
}

static void destroy_pool_objects(md_vk_importer_t* importer) {
    VkDevice device = importer->context.device;
    for (uint32_t i = 0; i < importer->pool.buffer_count; ++i) {
        if (importer->pool.acquire_semaphores[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, importer->pool.acquire_semaphores[i], NULL);
        }
        if (importer->pool.release_semaphores[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, importer->pool.release_semaphores[i], NULL);
        }
        if (importer->pool.views[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(device, importer->pool.views[i], NULL);
        }
        if (importer->pool.images[i] != VK_NULL_HANDLE) {
            vkDestroyImage(device, importer->pool.images[i], NULL);
        }
        if (importer->pool.memories[i] != VK_NULL_HANDLE) {
            vkFreeMemory(device, importer->pool.memories[i], NULL);
        }
    }
    clear_pool(&importer->pool);
}

void md_vk_importer_release_pool(md_vk_importer_t* importer) {
    if (importer == NULL || !importer->pool_active) return;
    destroy_pool_objects(importer);
    importer->pool_active = false;
}

void md_vk_importer_free(md_vk_importer_t* importer) {
    if (importer == NULL) return;
    md_vk_importer_release_pool(importer);
    free(importer);
}

const md_vk_imported_pool_t* md_vk_importer_pool(const md_vk_importer_t* importer) {
    return importer != NULL && importer->pool_active ? &importer->pool : NULL;
}

static uint32_t choose_memory_type(VkPhysicalDevice physical_device, uint32_t type_bits,
                                   VkMemoryPropertyFlags preferred) {
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
    uint32_t fallback = UINT32_MAX;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((type_bits & (UINT32_C(1) << i)) == 0) continue;
        if (fallback == UINT32_MAX) fallback = i;
        if ((properties.memoryTypes[i].propertyFlags & preferred) == preferred) return i;
    }
    return fallback;
}

static int import_one_image(md_vk_importer_t* importer, const md_buffer_pool_t* source,
                            uint32_t index, VkFormat format, VkComponentMapping mapping) {
    VkDevice device = importer->context.device;
    const md_plane_t* plane = &source->planes[index][0];
    int query_fd = plane->fd;
    VkMemoryFdPropertiesKHR fd_properties = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
        .pNext = NULL,
        .memoryTypeBits = 0,
    };
    if (importer->get_memory_fd_properties == NULL) return MD_ERR_UNSUPPORTED;
    VkResult vk_result = importer->get_memory_fd_properties(
        device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, query_fd, &fd_properties);
    if (vk_result != VK_SUCCESS) return MD_ERR_IO;

    VkSubresourceLayout layout = {
        .offset = plane->offset,
        .size = plane->size,
        .rowPitch = plane->stride,
        .arrayPitch = 0,
        .depthPitch = 0,
    };
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .pNext = NULL,
        .drmFormatModifier = source->modifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts = &layout,
    };
    VkExternalMemoryImageCreateInfo external_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modifier_info,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_info,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {source->width, source->height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = importer->context.image_usage | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = NULL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    vk_result = vkCreateImage(device, &image_info, NULL, &importer->pool.images[index]);
    if (vk_result != VK_SUCCESS) return MD_ERR_UNSUPPORTED;

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, importer->pool.images[index], &requirements);
    uint32_t type_bits = requirements.memoryTypeBits & fd_properties.memoryTypeBits;
    uint32_t memory_type = choose_memory_type(importer->context.physical_device, type_bits,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memory_type == UINT32_MAX) return MD_ERR_UNSUPPORTED;

    int imported_fd = fcntl(plane->fd, F_DUPFD_CLOEXEC, 0);
    if (imported_fd < 0) return MD_ERR_IO;
    VkImportMemoryFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = NULL,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = imported_fd,
    };
    VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = NULL,
        .image = importer->pool.images[index],
        .buffer = VK_NULL_HANDLE,
    };
    import_info.pNext = &dedicated_info;
    VkMemoryAllocateInfo allocation_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_info,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type,
    };
    vk_result = vkAllocateMemory(device, &allocation_info, NULL, &importer->pool.memories[index]);
    if (vk_result != VK_SUCCESS) {
        close(imported_fd);
        return MD_ERR_IO;
    }
    vk_result = vkBindImageMemory(device, importer->pool.images[index],
                                  importer->pool.memories[index], 0);
    if (vk_result != VK_SUCCESS) return MD_ERR_IO;

    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .image = importer->pool.images[index],
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components = mapping,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    vk_result = vkCreateImageView(device, &view_info, NULL, &importer->pool.views[index]);
    if (vk_result != VK_SUCCESS) return MD_ERR_IO;

    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
    };
    vk_result = vkCreateSemaphore(device, &semaphore_info, NULL,
                                  &importer->pool.acquire_semaphores[index]);
    if (vk_result != VK_SUCCESS) return MD_ERR_IO;
    vk_result = vkCreateSemaphore(device, &semaphore_info, NULL,
                                  &importer->pool.release_semaphores[index]);
    if (vk_result != VK_SUCCESS) return MD_ERR_IO;
    return MD_OK;
}

int md_vk_importer_import_pool(md_vk_importer_t* importer, const md_buffer_pool_t* pool) {
    if (importer == NULL || pool == NULL || importer->pool_active) return MD_ERR_STATE;
    if (pool->buffer_count < 2 || pool->buffer_count > MIRAGE_DISPLAY_MAX_BUFFERS ||
        pool->plane_count != 1 || pool->width == 0 || pool->height == 0 ||
        pool->generation == 0) return MD_ERR_UNSUPPORTED;
    for (uint32_t i = 0; i < pool->buffer_count; ++i) {
        if (pool->planes[i][0].fd < 0 || pool->planes[i][0].stride == 0 ||
            pool->planes[i][0].size == 0) return MD_ERR_INVALID;
    }
    VkFormat format;
    VkComponentMapping mapping;
    int rc = md_vk_fourcc_to_format(pool->fourcc, &format, &mapping);
    if (rc != MD_OK) return rc;
    clear_pool(&importer->pool);
    importer->pool.generation = pool->generation;
    importer->pool.buffer_count = pool->buffer_count;
    importer->pool.width = pool->width;
    importer->pool.height = pool->height;
    importer->pool.fourcc = pool->fourcc;
    importer->pool.modifier = pool->modifier;
    importer->pool.format = format;
    for (uint32_t i = 0; i < pool->buffer_count; ++i) {
        rc = import_one_image(importer, pool, i, format, mapping);
        if (rc != MD_OK) {
            destroy_pool_objects(importer);
            return rc;
        }
    }
    importer->pool_active = true;
    return MD_OK;
}

static int import_semaphore(md_vk_importer_t* importer, uint32_t buffer_index, int fd,
                            VkExternalSemaphoreHandleTypeFlagBits handle_type,
                            VkSemaphoreImportFlags flags, VkSemaphore* out) {
    if (importer == NULL || out == NULL || fd < 0 || !importer->pool_active ||
        buffer_index >= importer->pool.buffer_count || importer->import_semaphore_fd == NULL) {
        if (fd >= 0) close(fd);
        return MD_ERR_INVALID;
    }
    VkSemaphore semaphore = handle_type == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
                                ? importer->pool.acquire_semaphores[buffer_index]
                                : importer->pool.release_semaphores[buffer_index];
    VkImportSemaphoreFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .pNext = NULL,
        .semaphore = semaphore,
        .flags = flags,
        .handleType = handle_type,
        .fd = fd,
    };
    VkResult result = importer->import_semaphore_fd(importer->context.device, &import_info);
    if (result != VK_SUCCESS) {
        close(fd);
        return result == VK_ERROR_EXTENSION_NOT_PRESENT ? MD_ERR_UNSUPPORTED : MD_ERR_IO;
    }
    *out = semaphore;
    return MD_OK;
}

int md_vk_import_acquire_sync(md_vk_importer_t* importer, uint32_t buffer_index,
                              int acquire_sync_fd, VkSemaphore* out_semaphore) {
    return import_semaphore(importer, buffer_index, acquire_sync_fd,
                             VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
                             VK_SEMAPHORE_IMPORT_TEMPORARY_BIT, out_semaphore);
}

int md_vk_import_release_syncobj(md_vk_importer_t* importer, uint32_t buffer_index,
                                 int release_syncobj_fd, VkSemaphore* out_semaphore) {
    return import_semaphore(importer, buffer_index, release_syncobj_fd,
                             VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, 0, out_semaphore);
}

static int make_barrier(const md_vk_importer_t* importer, uint32_t buffer_index,
                        VkAccessFlags source_access, VkAccessFlags destination_access,
                        uint32_t source_family, uint32_t destination_family,
                        VkImageMemoryBarrier* out_barrier) {
    if (importer == NULL || out_barrier == NULL || !importer->pool_active ||
        buffer_index >= importer->pool.buffer_count) return MD_ERR_INVALID;
    *out_barrier = (VkImageMemoryBarrier) {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = NULL,
        .srcAccessMask = source_access,
        .dstAccessMask = destination_access,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = source_family,
        .dstQueueFamilyIndex = destination_family,
        .image = importer->pool.images[buffer_index],
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    return MD_OK;
}

int md_vk_importer_acquire_barrier(const md_vk_importer_t* importer,
                                   uint32_t buffer_index, VkAccessFlags destination_access,
                                   VkImageMemoryBarrier* out_barrier) {
    return make_barrier(importer, buffer_index, 0, destination_access,
                        VK_QUEUE_FAMILY_FOREIGN_EXT, importer != NULL
                            ? importer->context.queue_family_index : 0, out_barrier);
}

int md_vk_importer_release_barrier(const md_vk_importer_t* importer,
                                   uint32_t buffer_index, VkAccessFlags source_access,
                                   VkImageMemoryBarrier* out_barrier) {
    return make_barrier(importer, buffer_index, source_access, 0,
                        importer != NULL ? importer->context.queue_family_index : 0,
                        VK_QUEUE_FAMILY_FOREIGN_EXT, out_barrier);
}
