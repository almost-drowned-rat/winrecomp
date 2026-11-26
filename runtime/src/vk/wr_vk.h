/*
 * winrecomp runtime - minimal Vulkan 1.1 backend.
 *
 * Vulkan 1.1 core + VK_KHR_swapchain only (widest device coverage: older
 * Android Mali/Adreno drivers, old Intel). Functions are loaded through SDL's
 * loader, so nothing links against a Vulkan library.
 *
 * Model: two frames in flight, a persistently mapped streaming vertex buffer
 * and a staging buffer per frame. The game draws into an offscreen target
 * (colour + depth, render resolution); the present pass then puts it on the
 * swapchain image (scaled, pillar/letterboxed, or through a post chain) and
 * draws overlays:
 *
 *   cmd = wr_vk_frame_begin();
 *   wr_vk_target_begin(t, ...);  draws  wr_vk_target_end();     (any number)
 *   wr_vk_present_begin(clear);  blit + overlays  wr_vk_present_end();
 *   wr_vk_frame_end();
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
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdClearAttachments) \
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
    VkDescriptorSet alt_set;    /* the same image with a second sampler (lazy) */
    VkSampler alt_sampler;
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
    VkFormat depth_format;
    VkRenderPass present_clear;   /* swapchain, colour only, cleared */
    VkRenderPass present_load;    /* swapchain, colour only, kept (after a post chain) */
    VkRenderPass target_clear;    /* offscreen colour + depth, colour cleared */
    VkRenderPass target_load;     /* offscreen colour + depth, colour kept */
    int vsync;
    int need_recreate;
    int swap_readable;           /* swapchain images allow TRANSFER_SRC (debug dumps) */

    wr_vk_frame frames[WR_VK_FRAMES];
    uint32_t frame;              /* index into frames */
    uint32_t image;              /* acquired swapchain image */
    int in_frame;
    int in_pass;
    int presented_pass;          /* present pass recorded this frame */

    VkDescriptorSetLayout tex_layout;   /* binding 0: combined image sampler */
    VkDescriptorPool tex_pool;          /* permanent texture sets */
    VkCommandPool upload_pool;
} wr_vk;

extern wr_vk g_vk;

/* Bring up on the SDL window (created with SDL_WINDOW_VULKAN). */
int  wr_vk_init(struct SDL_Window *window, int vsync);
void wr_vk_shutdown(void);
void wr_vk_set_vsync(int on);

/* Offscreen render target: RGBA8 colour (sampled by the present pass) and a
 * depth buffer. Colour persists between passes; depth is cleared at each
 * wr_vk_target_begin. */
typedef struct wr_vk_target {
    uint32_t w, h;
    VkImage color, depth;
    VkImageView color_view, depth_view;
    VkDeviceMemory color_mem, depth_mem;
    VkFramebuffer fb;
    VkDescriptorSet set;          /* colour as a sampled texture */
} wr_vk_target;

wr_vk_target *wr_vk_target_create(uint32_t w, uint32_t h, VkSampler sampler);
void wr_vk_target_destroy(wr_vk_target *t);

/* Frame: the command buffer outside any render pass, or NULL when nothing
 * can be drawn this frame (minimised window, swapchain being rebuilt). */
VkCommandBuffer wr_vk_frame_begin(void);
void wr_vk_target_begin(wr_vk_target *t, int clear, const float clear_rgba[4]);
void wr_vk_target_end(void);
/* The swapchain image of this frame (for a post chain writing into it). */
VkImage wr_vk_swap_image(void);
VkImageView wr_vk_swap_view(void);
void wr_vk_present_begin(int clear, const float clear_rgba[4]);
void wr_vk_present_end(void);
void wr_vk_frame_end(void);

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
/* The texture's set with another sampler (created once per texture). */
VkDescriptorSet wr_vk_texture_alt_set(wr_vk_texture *t, VkSampler sampler);

/* Present pass: draw a target into (x, y, w, h) of the swapchain image with
 * the given gamma (1.0 = none). */
void wr_vk_blit(VkCommandBuffer cmd, wr_vk_target *t, int x, int y, int w, int h, float gamma);
/* The same for a texture (movies). */
void wr_vk_blit_texture(VkCommandBuffer cmd, wr_vk_texture *t, int x, int y, int w, int h, float gamma);
/* The top-left part (us, vs of the size, 0..1) of a texture, no gamma. */
void wr_vk_blit_texture_part(VkCommandBuffer cmd, wr_vk_texture *t, int x, int y, int w, int h, float us, float vs);

/* PERFORMANCE overlay (wr_overlay.c): FPS / FRAME TIME / GAME SPEED */
enum { WR_PERF_FPS = 1, WR_PERF_FRAMETIME = 2, WR_PERF_SPEED = 4, WR_PERF_ALL = 7 };
void wr_overlay_set(int mask);
void wr_overlay_set_pos(int x, int y);           /* the panel's top left in window pixels; -1 = the corner */
int  wr_overlay_get(void);
int  wr_overlay_parse(const char *v);              /* "fps+frametime", "all", "off" */
void wr_overlay_format(int mask, char *out, size_t cap);
void wr_overlay_set_tick_rate(double hz);         /* the game's own logic rate */
void wr_overlay_game_tick(void);                  /* one game logic tick ran */
void wr_overlay_prepare(void);                    /* once per frame, before the present pass */
void wr_overlay_draw(VkCommandBuffer cmd);        /* in the present pass, after the picture */
/* Largest aw:ah rectangle centred in sw x sh (pillar/letterbox). */
void wr_vk_fit_rect(uint32_t sw, uint32_t sh, uint32_t aw, uint32_t ah, int *x, int *y, int *w, int *h);

/* Debug: write the target as PNG after the current frame (records a copy;
 * call between wr_vk_target_end and wr_vk_frame_end; the file is written by
 * wr_vk_frame_end after the frame completed). */
void wr_vk_dump_target(wr_vk_target *t, const char *png_path);
/* The same for the swapchain image (after the present pass). */
void wr_vk_dump_swap(const char *png_path);

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
