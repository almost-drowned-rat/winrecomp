/*
 * winrecomp runtime - minimal Vulkan 1.1 backend.
 *
 * Vulkan 1.1 core + VK_KHR_swapchain only (widest device coverage: older
 * Android Mali/Adreno drivers, old Intel). Functions are loaded through SDL's
 * loader, so nothing links against a Vulkan library.
 *
 * Model: one render pass (colour = swapchain image, optional depth), two
 * frames in flight, a persistently mapped streaming vertex buffer per frame,
 * descriptor pool reset per frame. The game layer records draws between
 * wr_vk_begin_frame and wr_vk_end_frame.
 */
#ifndef WR_VK_H
#define WR_VK_H

struct SDL_Window;

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WR_VK_FUNCS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) \
    X(vkEnumerateDeviceExtensionProperties) X(vkDestroySurfaceKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)

#define WR_VK_DEV_FUNCS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueueWaitIdle) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateImage) X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkAllocateMemory) X(vkFreeMemory) \
    X(vkMapMemory) X(vkUnmapMemory) X(vkFlushMappedMemoryRanges) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) \
    X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdBindDescriptorSets) X(vkCmdDraw) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdPushConstants) X(vkCmdPipelineBarrier) \
    X(vkCmdCopyBufferToImage) X(vkCmdClearAttachments) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) X(vkFreeDescriptorSets) X(vkCreateSampler) X(vkDestroySampler)

#define X(n) extern PFN_##n n;
WR_VK_FUNCS(X)
WR_VK_DEV_FUNCS(X)
#undef X

#define WR_VK_FRAMES 2

typedef struct wr_vk_buffer {
    VkBuffer buf;
    VkDeviceMemory mem;
    void *map;
    VkDeviceSize size;
} wr_vk_buffer;

typedef struct wr_vk_texture {
    VkImage image;
    VkImageView view;
    VkDeviceMemory mem;
    uint32_t w, h;
    VkDescriptorSet set;        /* permanent set (texture + the given sampler) */
} wr_vk_texture;

typedef struct wr_vk_frame {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acquired, rendered;
    wr_vk_buffer verts;          /* streaming vertex buffer */
    VkDeviceSize vused;
    wr_vk_buffer upload;         /* staging for texture uploads */
    VkDeviceSize uused;
    /* textures queued for destruction once this frame's fence passed */
    wr_vk_texture *dead[256];
    int ndead;
} wr_vk_frame;

typedef struct wr_vk {
    int ready;
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice phys;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties memprops;
    VkDevice dev;
    uint32_t qfamily;
    VkQueue queue;

    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    uint32_t nimages;
    VkImage images[8];
    VkImageView views[8];
    VkFramebuffer fbs[8];
    VkImage depth;
    VkImageView depth_view;
    VkDeviceMemory depth_mem;
    VkFormat depth_format;
    VkRenderPass pass;
    int vsync;
    int need_recreate;

    wr_vk_frame frames[WR_VK_FRAMES];
    uint32_t frame;              /* index into frames */
    uint32_t image;              /* acquired swapchain image */
    int in_frame;

    VkDescriptorSetLayout tex_layout;   /* binding 0: combined image sampler */
    VkDescriptorPool tex_pool;          /* permanent texture sets */
    VkCommandPool upload_pool;
} wr_vk;

extern wr_vk g_vk;

/* Bring up on the SDL window (created with SDL_WINDOW_VULKAN). */
int  wr_vk_init(struct SDL_Window *window, int vsync);
void wr_vk_shutdown(void);
void wr_vk_set_vsync(int on);

/* Frame: returns the command buffer inside the render pass (cleared to
 * clear_rgba), or NULL when nothing can be drawn (minimised window). */
VkCommandBuffer wr_vk_begin_frame(const float clear_rgba[4]);
void wr_vk_end_frame(void);

/* Streaming vertices for the current frame: copies n bytes, returns the
 * byte offset in the frame's vertex buffer (VK_WHOLE_SIZE when full). */
VkDeviceSize wr_vk_push_verts(const void *data, VkDeviceSize n);
VkBuffer wr_vk_vertex_buffer(void);

/* Textures (RGBA8). Upload is recorded into the current frame or, outside a
 * frame, submitted at once. */
wr_vk_texture *wr_vk_texture_create(uint32_t w, uint32_t h, const uint32_t *rgba, VkSampler sampler);
void wr_vk_texture_update(wr_vk_texture *t, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          const uint32_t *rgba, uint32_t src_pitch_px);
void wr_vk_texture_destroy(wr_vk_texture *t);

/* Pipelines */
VkShaderModule wr_vk_shader(const uint32_t *spirv, size_t bytes);
VkSampler wr_vk_sampler(VkFilter filter, VkSamplerAddressMode mode);

uint32_t wr_vk_memtype(uint32_t bits, VkMemoryPropertyFlags want);
int wr_vk_buffer_create(wr_vk_buffer *b, VkDeviceSize size, VkBufferUsageFlags usage, int host);
void wr_vk_buffer_destroy(wr_vk_buffer *b);

#ifdef __cplusplus
}
#endif
#endif
