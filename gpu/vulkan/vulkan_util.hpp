#ifndef MIRAGE_DISPLAY_VULKAN_UTIL_HPP
#define MIRAGE_DISPLAY_VULKAN_UTIL_HPP

#include <cstdint>
#include <optional>

#include <vulkan/vulkan.h>

/*
 * Internal Vulkan helpers shared by the importer, blitter, and exporter.
 */

namespace mirage::vulkan {

/*
 * 优先返回满足全部必需属性的内存类型；若不存在则回退到任意兼容类型
 * type_bits 与任何类型都不匹配时才返回空结果。
 */
[[nodiscard]] std::optional<uint32_t> choose_memory_type(
    VkPhysicalDevice physical_device, uint32_t type_bits,
    VkMemoryPropertyFlags required_properties);

/* Maps a negotiated memory plane to the Vulkan external-memory aspect bit. */
[[nodiscard]] VkImageAspectFlagBits memory_plane_aspect(uint32_t plane_index);

}  // namespace mirage::vulkan

#endif
