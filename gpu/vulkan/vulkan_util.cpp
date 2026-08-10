#include "vulkan_util.hpp"

/*
 * Implementation of the shared Vulkan instance/device helpers.
 */

namespace mirage::vulkan {

std::optional<uint32_t> choose_memory_type(
    const VkPhysicalDevice physical_device, const uint32_t type_bits,
    const VkMemoryPropertyFlags required_properties) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);

    /* 第一轮：优先选择满足全部必需属性的类型。 */
    for (uint32_t index = 0U; index < properties.memoryTypeCount; ++index) {
        const uint32_t type_mask = UINT32_C(1) << index;
        const VkMemoryPropertyFlags properties_for_type =
            properties.memoryTypes[index].propertyFlags;
        if ((type_bits & type_mask) != 0U &&
            (properties_for_type & required_properties) == required_properties) {
            return index;
        }
    }
    /* 第二轮：没有任何类型满足必需属性时回退到任意兼容类型。PRIME 混合
     * 显卡与部分专有驱动只通过非 DEVICE_LOCAL 的导入类型暴露 DMA-BUF，
     * 没有该回退时外部内存导入必然失败。 */
    for (uint32_t index = 0U; index < properties.memoryTypeCount; ++index) {
        const uint32_t type_mask = UINT32_C(1) << index;
        if ((type_bits & type_mask) != 0U) {
            return index;
        }
    }
    return std::nullopt;
}

VkImageAspectFlagBits memory_plane_aspect(const uint32_t plane_index) {
    switch (plane_index) {
    case 0U:
        return VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
    case 1U:
        return VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT;
    case 2U:
        return VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT;
    case 3U:
        return VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT;
    default:
        return VK_IMAGE_ASPECT_NONE;
    }
}

}  // namespace mirage::vulkan
