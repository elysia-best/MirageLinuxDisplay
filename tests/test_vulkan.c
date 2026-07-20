#include "mirage_display_vulkan.h"

#include <assert.h>
#include <stdint.h>

#define DRM_FORMAT(code0, code1, code2, code3) \
    ((uint32_t)(code0) | ((uint32_t)(code1) << 8) | ((uint32_t)(code2) << 16) | \
     ((uint32_t)(code3) << 24))

static void test_fourcc_mapping(void) {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkComponentMapping mapping;
    assert(md_vk_fourcc_to_format(DRM_FORMAT('X', 'R', '2', '4'), &format, &mapping) == MD_OK);
    assert(format == VK_FORMAT_B8G8R8A8_UNORM);
    assert(mapping.a == VK_COMPONENT_SWIZZLE_ONE);

    assert(md_vk_fourcc_to_format(DRM_FORMAT('A', 'B', '2', '4'), &format, &mapping) == MD_OK);
    assert(format == VK_FORMAT_R8G8B8A8_UNORM);
    assert(mapping.a == VK_COMPONENT_SWIZZLE_IDENTITY);

    assert(md_vk_fourcc_to_format(DRM_FORMAT('N', 'V', '1', '2'), &format, &mapping) ==
           MD_ERR_UNSUPPORTED);
    assert(md_vk_fourcc_to_format(0, NULL, &mapping) == MD_ERR_INVALID);
}

static void test_invalid_context(void) {
    md_vk_context_t context = {0};
    assert(md_vk_importer_new(NULL) == NULL);
    assert(md_vk_importer_new(&context) == NULL);
}

static void test_invalid_barrier(void) {
    VkImageMemoryBarrier barrier;
    assert(md_vk_importer_acquire_barrier(NULL, 0, VK_ACCESS_SHADER_READ_BIT, &barrier) ==
           MD_ERR_INVALID);
    assert(md_vk_importer_release_barrier(NULL, 0, VK_ACCESS_SHADER_READ_BIT, &barrier) ==
           MD_ERR_INVALID);
}

int main(void) {
    test_fourcc_mapping();
    test_invalid_context();
    test_invalid_barrier();
    assert(md_vk_result_string(VK_SUCCESS) != NULL);
    return 0;
}
