#include "mirage_display_vulkan.h"

#include "common/util.hpp"
#include "vulkan_util.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <memory>
#include <new>

#include <fcntl.h>

/*
 * Vulkan external-memory DMA-BUF importer (include/mirage_display_vulkan.h).
 *
 * Imports each plane as VkDeviceMemory through VK_KHR_external_memory_fd,
 * creates images/views and the optional YCbCr conversion, and exposes the
 * GENERAL-layout queue-family ownership barriers required by protocol v1.
 */

struct md_vk_importer {
    md_vk_context_t context;
    /* Resolved once from the device; NULL means the driver lacks the extension. */
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties;
    PFN_vkImportSemaphoreFdKHR import_semaphore_fd;
    md_vk_imported_pool_t pool;
    bool pool_active;
};

namespace {

constexpr uint32_t make_drm_format(const char code0, const char code1, const char code2,
                                   const char code3) {
    return static_cast<uint32_t>(static_cast<unsigned char>(code0)) |
           (static_cast<uint32_t>(static_cast<unsigned char>(code1)) << 8U) |
           (static_cast<uint32_t>(static_cast<unsigned char>(code2)) << 16U) |
           (static_cast<uint32_t>(static_cast<unsigned char>(code3)) << 24U);
}

constexpr uint32_t kDrmFormatXrgb8888 = make_drm_format('X', 'R', '2', '4');
constexpr uint32_t kDrmFormatArgb8888 = make_drm_format('A', 'R', '2', '4');
constexpr uint32_t kDrmFormatXbgr8888 = make_drm_format('X', 'B', '2', '4');
constexpr uint32_t kDrmFormatAbgr8888 = make_drm_format('A', 'B', '2', '4');
constexpr uint32_t kDrmFormatNv12 = make_drm_format('N', 'V', '1', '2');

void clear_pool(md_vk_imported_pool_t* const pool) {
    md_vk_imported_pool_t cleared{};
    cleared.format = VK_FORMAT_UNDEFINED;
    for (uint32_t buffer_index = 0U; buffer_index < MIRAGE_DISPLAY_MAX_BUFFERS;
         ++buffer_index) {
        cleared.images[buffer_index] = VK_NULL_HANDLE;
        cleared.memories[buffer_index] = VK_NULL_HANDLE;
        cleared.views[buffer_index] = VK_NULL_HANDLE;
        cleared.acquire_semaphores[buffer_index] = VK_NULL_HANDLE;
        cleared.release_semaphores[buffer_index] = VK_NULL_HANDLE;
        for (uint32_t plane_index = 0U; plane_index < MIRAGE_DISPLAY_MAX_PLANES;
             ++plane_index) {
            cleared.plane_memories[buffer_index][plane_index] = VK_NULL_HANDLE;
        }
    }
    cleared.ycbcr_conversion = VK_NULL_HANDLE;
    *pool = cleared;
}

bool format_is_disjoint(const uint32_t fourcc) { return fourcc == kDrmFormatNv12; }

VkImageAspectFlagBits image_plane_aspect(const uint32_t plane_index) {
    switch (plane_index) {
    case 0U:
        return VK_IMAGE_ASPECT_PLANE_0_BIT;
    case 1U:
        return VK_IMAGE_ASPECT_PLANE_1_BIT;
    case 2U:
        return VK_IMAGE_ASPECT_PLANE_2_BIT;
    default:
        return VK_IMAGE_ASPECT_NONE;
    }
}

/*
 * The caller is replacing or abandoning the complete pool, so all Vulkan
 * handles owned by that pool can be released in dependency order here.
 */
void destroy_pool_objects(md_vk_importer_t* const importer) {
    const VkDevice device = importer->context.device;
    for (uint32_t buffer_index = 0U; buffer_index < importer->pool.buffer_count;
         ++buffer_index) {
        if (importer->pool.acquire_semaphores[buffer_index] != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, importer->pool.acquire_semaphores[buffer_index], nullptr);
        }
        if (importer->pool.release_semaphores[buffer_index] != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, importer->pool.release_semaphores[buffer_index], nullptr);
        }
        if (importer->pool.views[buffer_index] != VK_NULL_HANDLE) {
            vkDestroyImageView(device, importer->pool.views[buffer_index], nullptr);
        }
        if (importer->pool.images[buffer_index] != VK_NULL_HANDLE) {
            vkDestroyImage(device, importer->pool.images[buffer_index], nullptr);
        }
        for (uint32_t plane_index = 0U; plane_index < importer->pool.plane_count;
             ++plane_index) {
            if (importer->pool.plane_memories[buffer_index][plane_index] != VK_NULL_HANDLE) {
                vkFreeMemory(device,
                             importer->pool.plane_memories[buffer_index][plane_index], nullptr);
            }
        }
    }
    if (importer->pool.ycbcr_conversion != VK_NULL_HANDLE) {
        vkDestroySamplerYcbcrConversion(device, importer->pool.ycbcr_conversion, nullptr);
    }
    clear_pool(&importer->pool);
}


/*
 * Imports one DMA-BUF plane as VkDeviceMemory and binds it to the image.
 * Disjoint formats (NV12) allocate one memory object per plane; other formats
 * bind the single non-disjoint allocation to plane zero.
 */
md_result_t import_plane_memory(md_vk_importer_t* const importer,
                                const md_buffer_pool_t* const source,
                                const uint32_t image_index,
                                const uint32_t plane_index,
                                const bool disjoint) {
    const VkDevice device = importer->context.device;
    const md_plane_t& plane = source->planes[image_index][plane_index];

    VkMemoryFdPropertiesKHR fd_properties{};
    fd_properties.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    if (importer->get_memory_fd_properties == nullptr) {
        return MD_ERR_UNSUPPORTED;
    }
    const VkResult fd_result = importer->get_memory_fd_properties(
        device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, plane.fd, &fd_properties);
    if (fd_result != VK_SUCCESS) {
        return fd_result == VK_ERROR_EXTENSION_NOT_PRESENT ? MD_ERR_UNSUPPORTED : MD_ERR_IO;
    }

    const VkImageAspectFlagBits aspect =
        disjoint ? image_plane_aspect(plane_index) : VK_IMAGE_ASPECT_NONE;
    if (disjoint && aspect == VK_IMAGE_ASPECT_NONE) {
        return MD_ERR_UNSUPPORTED;
    }

    VkImagePlaneMemoryRequirementsInfo plane_requirements{};
    plane_requirements.sType = VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO;
    plane_requirements.planeAspect = aspect;

    VkImageMemoryRequirementsInfo2 requirements_info{};
    requirements_info.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
    requirements_info.pNext = disjoint ? &plane_requirements : nullptr;
    requirements_info.image = importer->pool.images[image_index];

    VkMemoryDedicatedRequirements dedicated_requirements{};
    dedicated_requirements.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
    VkMemoryRequirements2 requirements{};
    requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    requirements.pNext = &dedicated_requirements;
    vkGetImageMemoryRequirements2(device, &requirements_info, &requirements);

    const uint32_t compatible_type_bits =
        requirements.memoryRequirements.memoryTypeBits & fd_properties.memoryTypeBits;

    /* 枚举全部兼容内存类型：优先 device-local，同时保留非 device-local
     * 候选。PRIME 混合显卡与部分专有驱动只通过非 local 的导入类型暴露
     * DMA-BUF，因此必须逐个尝试分配而不能只挑一个类型。 */
    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(importer->context.physical_device, &memory_properties);
    std::array<uint32_t, VK_MAX_MEMORY_TYPES> candidates{};
    uint32_t candidate_count = 0U;
    for (uint32_t pass = 0U; pass < 2U; ++pass) {
        for (uint32_t index = 0U; index < memory_properties.memoryTypeCount; ++index) {
            if ((compatible_type_bits & (UINT32_C(1) << index)) == 0U) {
                continue;
            }
            const bool device_local =
                (memory_properties.memoryTypes[index].propertyFlags &
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U;
            if ((pass == 0U && !device_local) || (pass == 1U && device_local)) {
                continue;
            }
            candidates[candidate_count++] = index;
        }
    }
    if (candidate_count == 0U) {
        return MD_ERR_UNSUPPORTED;
    }

    VkMemoryDedicatedAllocateInfo dedicated_info{};
    dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated_info.image = importer->pool.images[image_index];
    dedicated_info.buffer = VK_NULL_HANDLE;

    VkDeviceMemory& memory = importer->pool.plane_memories[image_index][plane_index];
    VkResult last_result = VK_ERROR_UNKNOWN;
    bool allocated = false;
    for (uint32_t candidate = 0U; candidate < candidate_count; ++candidate) {
        const int duplicated_descriptor = fcntl(plane.fd, F_DUPFD_CLOEXEC, 0);
        if (duplicated_descriptor < 0) {
            return MD_ERR_IO;
        }
        mirage::UniqueFd imported_fd{static_cast<int32_t>(duplicated_descriptor)};

        VkImportMemoryFdInfoKHR import_info{};
        import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
        import_info.pNext = &dedicated_info;
        import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        const int32_t transferred_descriptor = imported_fd.release();
        import_info.fd = transferred_descriptor;

        VkMemoryAllocateInfo allocation_info{};
        allocation_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation_info.pNext = &import_info;
        allocation_info.allocationSize = requirements.memoryRequirements.size;
        allocation_info.memoryTypeIndex = candidates[candidate];

        last_result = vkAllocateMemory(device, &allocation_info, nullptr, &memory);
        if (last_result == VK_SUCCESS) {
            allocated = true;
            break;
        }
        /* 该内存类型不接受此 DMA-BUF：关闭本次重复出的 fd，尝试下一候选。 */
        mirage::UniqueFd failed_transfer{transferred_descriptor};
    }
    if (!allocated) {
        return last_result == VK_ERROR_INVALID_EXTERNAL_HANDLE ? MD_ERR_UNSUPPORTED
                                                                : MD_ERR_IO;
    }
    /* Vulkan owns the duplicate only after successful external-memory import. */
    if (plane_index == 0U) {
        importer->pool.memories[image_index] = memory;
    }

    VkBindImagePlaneMemoryInfo bind_plane_info{};
    bind_plane_info.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO;
    bind_plane_info.planeAspect = aspect;

    VkBindImageMemoryInfo bind_info{};
    bind_info.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;
    bind_info.pNext = disjoint ? &bind_plane_info : nullptr;
    bind_info.image = importer->pool.images[image_index];
    bind_info.memory = memory;
    const VkResult bind_result = vkBindImageMemory2(device, 1U, &bind_info);
    return bind_result == VK_SUCCESS ? MD_OK : MD_ERR_IO;
}


/*
 * Creates one VkImage through the DRM-format-modifier explicit layout path,
 * binds its plane memories, and creates the image view (with a YCbCr conversion
 * for disjoint formats) plus the per-buffer acquire/release semaphores.
 */
md_result_t import_one_image(md_vk_importer_t* const importer,
                             const md_buffer_pool_t* const source,
                             const uint32_t image_index, const VkFormat format,
                             const VkComponentMapping mapping) {
    const VkDevice device = importer->context.device;
    const bool disjoint = format_is_disjoint(source->fourcc);

    std::array<VkSubresourceLayout, MIRAGE_DISPLAY_MAX_PLANES> layouts{};
    for (uint32_t plane_index = 0U; plane_index < source->plane_count; ++plane_index) {
        const md_plane_t& plane = source->planes[image_index][plane_index];
        /* The modifier extension consumes offset and rowPitch.  Its size is a
         * driver-derived subresource property, not producer bookkeeping. */
        layouts[plane_index].offset = plane.offset;
        layouts[plane_index].size = 0U;
        layouts[plane_index].rowPitch = plane.stride;
    }

    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info{};
    modifier_info.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    modifier_info.drmFormatModifier = source->modifier;
    modifier_info.drmFormatModifierPlaneCount = source->plane_count;
    modifier_info.pPlaneLayouts = layouts.data();

    VkExternalMemoryImageCreateInfo external_info{};
    external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external_info.pNext = &modifier_info;
    external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext = &external_info;
    image_info.flags = disjoint ? static_cast<VkImageCreateFlags>(VK_IMAGE_CREATE_DISJOINT_BIT)
                                : static_cast<VkImageCreateFlags>(0U);
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = VkExtent3D{source->width, source->height, 1U};
    image_info.mipLevels = 1U;
    image_info.arrayLayers = 1U;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    image_info.usage = importer->context.image_usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    const VkResult image_result =
        vkCreateImage(device, &image_info, nullptr, &importer->pool.images[image_index]);
    if (image_result != VK_SUCCESS) {
        return image_result == VK_ERROR_EXTENSION_NOT_PRESENT ? MD_ERR_UNSUPPORTED
                                                              : MD_ERR_IO;
    }

    const uint32_t allocation_count = disjoint ? source->plane_count : 1U;
    for (uint32_t plane_index = 0U; plane_index < allocation_count; ++plane_index) {
        const md_result_t import_result =
            import_plane_memory(importer, source, image_index, plane_index, disjoint);
        if (import_result != MD_OK) {
            return import_result;
        }
    }

    VkSamplerYcbcrConversionInfo conversion_info{};
    conversion_info.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    conversion_info.conversion = importer->pool.ycbcr_conversion;

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.pNext = disjoint ? &conversion_info : nullptr;
    view_info.image = importer->pool.images[image_index];
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.components = mapping;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0U;
    view_info.subresourceRange.levelCount = 1U;
    view_info.subresourceRange.baseArrayLayer = 0U;
    view_info.subresourceRange.layerCount = 1U;
    const VkResult view_result =
        vkCreateImageView(device, &view_info, nullptr, &importer->pool.views[image_index]);
    if (view_result != VK_SUCCESS) {
        return MD_ERR_IO;
    }

    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    const VkResult acquire_result = vkCreateSemaphore(
        device, &semaphore_info, nullptr, &importer->pool.acquire_semaphores[image_index]);
    if (acquire_result != VK_SUCCESS) {
        return MD_ERR_IO;
    }
    const VkResult release_result = vkCreateSemaphore(
        device, &semaphore_info, nullptr, &importer->pool.release_semaphores[image_index]);
    return release_result == VK_SUCCESS ? MD_OK : MD_ERR_IO;
}

md_result_t import_semaphore(md_vk_importer_t* const importer, const uint32_t buffer_index,
                             const int32_t descriptor,
                             const VkExternalSemaphoreHandleTypeFlagBits handle_type,
                             const VkSemaphoreImportFlags flags, VkSemaphore* const out) {
    if (descriptor < 0) {
        return MD_ERR_INVALID;
    }
    mirage::UniqueFd owned_descriptor{descriptor};
    if (importer == nullptr || out == nullptr || !importer->pool_active ||
        buffer_index >= importer->pool.buffer_count) {
        return MD_ERR_INVALID;
    }

    const VkSemaphore semaphore =
        handle_type == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
            ? importer->pool.acquire_semaphores[buffer_index]
            : importer->pool.release_semaphores[buffer_index];
    VkImportSemaphoreFdInfoKHR import_info{};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
    import_info.semaphore = semaphore;
    import_info.flags = flags;
    import_info.handleType = handle_type;
    if (importer->import_semaphore_fd == nullptr) {
        return MD_ERR_UNSUPPORTED;
    }
    const int32_t transferred_descriptor = owned_descriptor.release();
    import_info.fd = transferred_descriptor;
    const VkResult import_result =
        importer->import_semaphore_fd(importer->context.device, &import_info);
    if (import_result != VK_SUCCESS) {
        mirage::UniqueFd failed_transfer{transferred_descriptor};
        return import_result == VK_ERROR_EXTENSION_NOT_PRESENT ? MD_ERR_UNSUPPORTED : MD_ERR_IO;
    }
    /* Vulkan has accepted the FD and now owns its lifetime. */
    *out = semaphore;
    return MD_OK;
}

md_result_t make_barrier(const md_vk_importer_t* const importer,
                         const uint32_t buffer_index, const VkAccessFlags source_access,
                         const VkAccessFlags destination_access,
                         const uint32_t source_family, const uint32_t destination_family,
                         VkImageMemoryBarrier* const out_barrier) {
    if (importer == nullptr || out_barrier == nullptr || !importer->pool_active ||
        buffer_index >= importer->pool.buffer_count) {
        return MD_ERR_INVALID;
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = source_access;
    barrier.dstAccessMask = destination_access;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = source_family;
    barrier.dstQueueFamilyIndex = destination_family;
    barrier.image = importer->pool.images[buffer_index];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0U;
    barrier.subresourceRange.levelCount = 1U;
    barrier.subresourceRange.baseArrayLayer = 0U;
    barrier.subresourceRange.layerCount = 1U;
    *out_barrier = barrier;
    return MD_OK;
}

}  // namespace


/*
 * Maps the DRM fourcc codes supported by the first Vulkan backend revision
 * onto VkFormat plus a component swizzle that preserves the wire colors.
 */
extern "C" md_result_t md_vk_fourcc_to_format(const uint32_t fourcc, VkFormat* const format,
                                                VkComponentMapping* const mapping) {
    if (format == nullptr || mapping == nullptr) {
        return MD_ERR_INVALID;
    }
    mapping->r = VK_COMPONENT_SWIZZLE_IDENTITY;
    mapping->g = VK_COMPONENT_SWIZZLE_IDENTITY;
    mapping->b = VK_COMPONENT_SWIZZLE_IDENTITY;
    mapping->a = VK_COMPONENT_SWIZZLE_IDENTITY;

    switch (fourcc) {
    case kDrmFormatXrgb8888:
        *format = VK_FORMAT_B8G8R8A8_UNORM;
        mapping->a = VK_COMPONENT_SWIZZLE_ONE;
        return MD_OK;
    case kDrmFormatArgb8888:
        *format = VK_FORMAT_B8G8R8A8_UNORM;
        return MD_OK;
    case kDrmFormatXbgr8888:
        *format = VK_FORMAT_R8G8B8A8_UNORM;
        mapping->a = VK_COMPONENT_SWIZZLE_ONE;
        return MD_OK;
    case kDrmFormatAbgr8888:
        *format = VK_FORMAT_R8G8B8A8_UNORM;
        return MD_OK;
    case kDrmFormatNv12:
        *format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        return MD_OK;
    default:
        return MD_ERR_UNSUPPORTED;
    }
}


/*
 * Enumerates DRM modifiers on the physical device that support every required
 * tiling feature, filling md_format_cap_t entries for the producer's capability
 * advertisement.  Passing caps=NULL and capacity=0 queries the count.
 */
extern "C" md_result_t md_vk_query_format_caps(
    const VkPhysicalDevice physical_device, const uint32_t fourcc,
    const VkFormatFeatureFlags required_features, md_format_cap_t* const caps,
    const uint32_t capacity, uint32_t* const out_count) {
    if (physical_device == VK_NULL_HANDLE || out_count == nullptr ||
        (capacity > 0U && caps == nullptr)) {
        return MD_ERR_INVALID;
    }

    VkFormat format{};
    VkComponentMapping mapping{};
    const md_result_t format_result = md_vk_fourcc_to_format(fourcc, &format, &mapping);
    if (format_result != MD_OK) {
        return format_result;
    }

    VkDrmFormatModifierPropertiesListEXT modifier_list{};
    modifier_list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    VkFormatProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    properties.pNext = &modifier_list;
    vkGetPhysicalDeviceFormatProperties2(physical_device, format, &properties);
    if (modifier_list.drmFormatModifierCount == 0U) {
        *out_count = 0U;
        return MD_OK;
    }

    std::unique_ptr<VkDrmFormatModifierPropertiesEXT[]> modifiers{
        new (std::nothrow)
            VkDrmFormatModifierPropertiesEXT[modifier_list.drmFormatModifierCount]{}};
    if (!modifiers) {
        return MD_ERR_NOMEM;
    }
    modifier_list.pDrmFormatModifierProperties = modifiers.get();
    vkGetPhysicalDeviceFormatProperties2(physical_device, format, &properties);

    uint32_t written = 0U;
    uint32_t available = 0U;
    for (uint32_t modifier_index = 0U;
         modifier_index < modifier_list.drmFormatModifierCount; ++modifier_index) {
        const VkDrmFormatModifierPropertiesEXT& modifier = modifiers[modifier_index];
        if ((modifier.drmFormatModifierTilingFeatures & required_features) != required_features ||
            modifier.drmFormatModifierPlaneCount == 0U ||
            modifier.drmFormatModifierPlaneCount > MIRAGE_DISPLAY_MAX_PLANES) {
            continue;
        }
        if (format_is_disjoint(fourcc) && modifier.drmFormatModifierPlaneCount != 2U) {
            /* NV12 binds one allocation per image plane.  Modifiers with
             * auxiliary planes need a different protocol and are excluded. */
            continue;
        }
        if (caps != nullptr && written < capacity) {
            caps[written].fourcc = fourcc;
            caps[written].plane_count = modifier.drmFormatModifierPlaneCount;
            caps[written].modifier = modifier.drmFormatModifier;
            ++written;
        }
        ++available;
    }
    *out_count = available;
    return caps != nullptr && capacity < available ? MD_ERR_NOMEM : MD_OK;
}

extern "C" const char* md_vk_result_string(const VkResult result) {
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    case VK_EVENT_SET:
        return "VK_EVENT_SET";
    case VK_EVENT_RESET:
        return "VK_EVENT_RESET";
    case VK_INCOMPLETE:
        return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:
        return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:
        return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL:
        return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_UNKNOWN:
        return "VK_ERROR_UNKNOWN";
    default:
        return "VK_ERROR_UNRECOGNIZED";
    }
}

extern "C" md_vk_importer_t* md_vk_importer_new(const md_vk_context_t* const context) {
    if (context == nullptr || context->physical_device == VK_NULL_HANDLE ||
        context->device == VK_NULL_HANDLE || context->image_usage == 0U) {
        return nullptr;
    }

    std::unique_ptr<md_vk_importer_t> importer{new (std::nothrow) md_vk_importer_t{}};
    if (!importer) {
        return nullptr;
    }
    importer->context = *context;
    importer->get_memory_fd_properties = std::bit_cast<PFN_vkGetMemoryFdPropertiesKHR>(
        vkGetDeviceProcAddr(importer->context.device, "vkGetMemoryFdPropertiesKHR"));
    importer->import_semaphore_fd = std::bit_cast<PFN_vkImportSemaphoreFdKHR>(
        vkGetDeviceProcAddr(importer->context.device, "vkImportSemaphoreFdKHR"));
    clear_pool(&importer->pool);
    importer->pool_active = false;
    return importer.release();
}

extern "C" void md_vk_importer_release_pool(md_vk_importer_t* const importer) {
    if (importer == nullptr || !importer->pool_active) {
        return;
    }
    destroy_pool_objects(importer);
    importer->pool_active = false;
}

extern "C" void md_vk_importer_free(md_vk_importer_t* const importer) {
    if (importer == nullptr) {
        return;
    }
    md_vk_importer_release_pool(importer);
    delete importer;
}

extern "C" const md_vk_imported_pool_t* md_vk_importer_pool(
    const md_vk_importer_t* const importer) {
    if (importer == nullptr || !importer->pool_active) {
        return nullptr;
    }
    return &importer->pool;
}

extern "C" md_result_t md_vk_importer_import_pool(md_vk_importer_t* const importer,
                                                    const md_buffer_pool_t* const pool) {
    if (importer == nullptr || pool == nullptr || importer->pool_active) {
        return MD_ERR_STATE;
    }
    if (pool->buffer_count < 2U || pool->buffer_count > MIRAGE_DISPLAY_MAX_BUFFERS ||
        pool->plane_count == 0U || pool->plane_count > MIRAGE_DISPLAY_MAX_PLANES ||
        pool->width == 0U || pool->height == 0U || pool->generation == 0U) {
        return MD_ERR_INVALID;
    }
    if (format_is_disjoint(pool->fourcc) && pool->plane_count != 2U) {
        return MD_ERR_UNSUPPORTED;
    }
    for (uint32_t buffer_index = 0U; buffer_index < pool->buffer_count; ++buffer_index) {
        for (uint32_t plane_index = 0U; plane_index < pool->plane_count; ++plane_index) {
            const md_plane_t& plane = pool->planes[buffer_index][plane_index];
            if (plane.fd < 0 || plane.stride == 0U || plane.size == 0U) {
                return MD_ERR_INVALID;
            }
        }
    }

    VkFormat format{};
    VkComponentMapping mapping{};
    const md_result_t format_result = md_vk_fourcc_to_format(pool->fourcc, &format, &mapping);
    if (format_result != MD_OK) {
        return format_result;
    }

    clear_pool(&importer->pool);
    importer->pool.generation = pool->generation;
    importer->pool.buffer_count = pool->buffer_count;
    importer->pool.width = pool->width;
    importer->pool.height = pool->height;
    importer->pool.fourcc = pool->fourcc;
    importer->pool.plane_count = pool->plane_count;
    importer->pool.modifier = pool->modifier;
    importer->pool.format = format;

    if (format_is_disjoint(pool->fourcc)) {
        VkSamplerYcbcrConversionCreateInfo conversion_info{};
        conversion_info.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
        conversion_info.format = format;
        conversion_info.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
        conversion_info.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
        conversion_info.components = mapping;
        conversion_info.xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
        conversion_info.yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
        conversion_info.chromaFilter = VK_FILTER_LINEAR;
        conversion_info.forceExplicitReconstruction = VK_FALSE;
        const VkResult conversion_result = vkCreateSamplerYcbcrConversion(
            importer->context.device, &conversion_info, nullptr,
            &importer->pool.ycbcr_conversion);
        if (conversion_result != VK_SUCCESS) {
            clear_pool(&importer->pool);
            return conversion_result == VK_ERROR_FORMAT_NOT_SUPPORTED ? MD_ERR_UNSUPPORTED
                                                                       : MD_ERR_IO;
        }
    }

    for (uint32_t buffer_index = 0U; buffer_index < pool->buffer_count; ++buffer_index) {
        const md_result_t image_result =
            import_one_image(importer, pool, buffer_index, format, mapping);
        if (image_result != MD_OK) {
            destroy_pool_objects(importer);
            return image_result;
        }
    }
    importer->pool_active = true;
    return MD_OK;
}


/*
 * Imports one frame acquire sync_file as a temporary binary semaphore that
 * the consumer waits in its first read submission.  Consumes the descriptor on
 * every path.
 */
extern "C" md_result_t md_vk_import_acquire_sync(md_vk_importer_t* const importer,
                                                   const uint32_t buffer_index,
                                                   const int32_t acquire_sync_fd,
                                                   VkSemaphore* const out_semaphore) {
    return import_semaphore(importer, buffer_index, acquire_sync_fd,
                            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
                            VK_SEMAPHORE_IMPORT_TEMPORARY_BIT, out_semaphore);
}

extern "C" md_result_t md_vk_import_release_syncobj(
    md_vk_importer_t* const importer, const uint32_t buffer_index,
    const int32_t release_syncobj_fd, VkSemaphore* const out_semaphore) {
    return import_semaphore(importer, buffer_index, release_syncobj_fd,
                            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, 0U,
                            out_semaphore);
}


/*
 * Builds the protocol-v1 GENERAL-layout queue-family ownership barrier the
 * consumer must record before the first read of a frame.
 */
extern "C" md_result_t md_vk_importer_acquire_barrier(
    const md_vk_importer_t* const importer, const uint32_t buffer_index,
    const VkAccessFlags destination_access, VkImageMemoryBarrier* const out_barrier) {
    if (importer == nullptr) {
        return MD_ERR_INVALID;
    }
    return make_barrier(importer, buffer_index, 0U, destination_access,
                        VK_QUEUE_FAMILY_FOREIGN_EXT, importer->context.queue_family_index,
                        out_barrier);
}

extern "C" md_result_t md_vk_importer_release_barrier(
    const md_vk_importer_t* const importer, const uint32_t buffer_index,
    const VkAccessFlags source_access, VkImageMemoryBarrier* const out_barrier) {
    if (importer == nullptr) {
        return MD_ERR_INVALID;
    }
    return make_barrier(importer, buffer_index, source_access, 0U,
                        importer->context.queue_family_index,
                        VK_QUEUE_FAMILY_FOREIGN_EXT, out_barrier);
}
