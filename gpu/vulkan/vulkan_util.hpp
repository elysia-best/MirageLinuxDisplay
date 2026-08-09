#ifndef MIRAGE_DISPLAY_VULKAN_UTIL_HPP
#define MIRAGE_DISPLAY_VULKAN_UTIL_HPP

#include <cstdint>
#include <optional>

#include <vulkan/vulkan.h>

namespace mirage::vulkan {

/*
 * Selects the first compatible memory type that satisfies every requested
 * property.  An empty result is deliberate: accepting a different memory
 * class would change the negotiated DMA-BUF contract.
 */
[[nodiscard]] std::optional<uint32_t> choose_memory_type(
    VkPhysicalDevice physical_device, uint32_t type_bits,
    VkMemoryPropertyFlags required_properties);

/* Maps a negotiated memory plane to the Vulkan external-memory aspect bit. */
[[nodiscard]] VkImageAspectFlagBits memory_plane_aspect(uint32_t plane_index);

}  // namespace mirage::vulkan

#endif
