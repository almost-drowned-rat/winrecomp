/*
 * winrecomp runtime - minimal Vulkan 1.1 backend (see wr_vk.h).
 */
#include "wr_vk.h"
#include "wr.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define X(n) PFN_##n n;
WR_VK_FUNCS(X)
WR_VK_DEV_FUNCS(X)
#undef X

static PFN_vkCreateInstance p_vkCreateInstance;
static PFN_vkGetInstanceProcAddr p_gipa;

wr_vk g_vk;

/* frame dumps (wr_vk_dump_target / _swap): up to two per frame (an F12
 * capture writes the game picture and the shown one), written at frame end */
#define DUMP_SLOTS 2
static struct { wr_vk_buffer buf; char path[1024]; uint32_t w, h; int bgra, pending; } g_dump[DUMP_SLOTS];
static void write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);
static SDL_Window *g_win;

/* per-frame upload command buffers (recorded lazily, submitted before draws) */
static VkCommandPool g_upool[WR_VK_FRAMES];
static VkCommandBuffer g_ucmd[WR_VK_FRAMES];
static int g_urec[WR_VK_FRAMES];

#define VK_CHECK(expr, what) do { VkResult _r = (expr); if (_r != VK_SUCCESS) { \
    wr_log("vulkan: %s failed (%d)", (what), (int)_r); return 0; } } while (0)

/* ---- memory ------------------------------------------------------------------------ */

uint32_t wr_vk_memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < g_vk.memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g_vk.memprops.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

int wr_vk_buffer_create(wr_vk_buffer *b, VkDeviceSize size, VkBufferUsageFlags usage, int host)
{
    memset(b, 0, sizeof *b);
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(g_vk.dev, &bi, NULL, &b->buf), "vkCreateBuffer");
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_vk.dev, b->buf, &mr);
    VkMemoryPropertyFlags want = host ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                      : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = wr_vk_memtype(mr.memoryTypeBits, want);
    if (ai.memoryTypeIndex == UINT32_MAX) {
        wr_log("vulkan: no memory type for buffer");
        return 0;
    }
    VK_CHECK(vkAllocateMemory(g_vk.dev, &ai, NULL, &b->mem), "vkAllocateMemory(buffer)");
    VK_CHECK(vkBindBufferMemory(g_vk.dev, b->buf, b->mem, 0), "vkBindBufferMemory");
    if (host)
        VK_CHECK(vkMapMemory(g_vk.dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map), "vkMapMemory");
    b->size = size;
    return 1;
}

void wr_vk_buffer_destroy(wr_vk_buffer *b)
{
    if (b->buf)
        vkDestroyBuffer(g_vk.dev, b->buf, NULL);
    if (b->mem)
        vkFreeMemory(g_vk.dev, b->mem, NULL);
    memset(b, 0, sizeof *b);
}

/* ---- swapchain -------------------------------------------------------------------------- */

static void destroy_swapchain_parts(void)
{
    for (uint32_t i = 0; i < g_vk.nimages; i++) {
        if (g_vk.fbs[i]) vkDestroyFramebuffer(g_vk.dev, g_vk.fbs[i], NULL);
        if (g_vk.views[i]) vkDestroyImageView(g_vk.dev, g_vk.views[i], NULL);
        g_vk.fbs[i] = VK_NULL_HANDLE;
        g_vk.views[i] = VK_NULL_HANDLE;
    }
}

static int create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps), "surface caps");

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_win, &w, &h);
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX || ext.width == 0) {
        ext.width = (uint32_t)w;
        ext.height = (uint32_t)h;
    }
#if defined(__ANDROID__)
    /* Android rule: identity transform, extent = window size (no rotation). */
    ext.width = (uint32_t)w;
    ext.height = (uint32_t)h;
#endif
    if (ext.width < caps.minImageExtent.width) ext.width = caps.minImageExtent.width;
    if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
    if (caps.maxImageExtent.width && ext.width > caps.maxImageExtent.width) ext.width = caps.maxImageExtent.width;
    if (caps.maxImageExtent.height && ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    if (!ext.width || !ext.height)
        return 0;   /* minimised */

    uint32_t nmodes = 0;
    VkPresentModeKHR modes[16];
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &nmodes, NULL);
    if (nmodes > 16) nmodes = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &nmodes, modes);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!g_vk.vsync) {
        for (uint32_t i = 0; i < nmodes; i++)
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
        if (mode == VK_PRESENT_MODE_FIFO_KHR)
            for (uint32_t i = 0; i < nmodes; i++)
                if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = modes[i];
    }

    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount)
        count = caps.maxImageCount;
    if (count > 8)
        count = 8;

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        static const VkCompositeAlphaFlagBitsKHR order[] = { VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR };
        for (int i = 0; i < 3; i++)
            if (caps.supportedCompositeAlpha & order[i]) { alpha = order[i]; break; }
    }

    VkSwapchainKHR old = g_vk.swapchain;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = g_vk.surface;
    ci.minImageCount = count;
    ci.imageFormat = g_vk.format;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
        ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;   /* debug dumps of the shown picture */
    g_vk.swap_readable = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    ci.compositeAlpha = alpha;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    VK_CHECK(vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain), "vkCreateSwapchainKHR");
    if (old) {
        destroy_swapchain_parts();
        vkDestroySwapchainKHR(g_vk.dev, old, NULL);
    }
    g_vk.extent = ext;

    g_vk.nimages = 8;
    VK_CHECK(vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.nimages, g_vk.images), "swapchain images");

    for (uint32_t i = 0; i < g_vk.nimages; i++) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = g_vk.images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = g_vk.format;
        vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.views[i]), "swapchain view");
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = g_vk.present_clear;   /* compatible with present_load */
        fi.attachmentCount = 1;
        fi.pAttachments = &g_vk.views[i];
        fi.width = ext.width;
        fi.height = ext.height;
        fi.layers = 1;
        VK_CHECK(vkCreateFramebuffer(g_vk.dev, &fi, NULL, &g_vk.fbs[i]), "framebuffer");
    }
    wr_log("vulkan: swapchain %ux%u, %u images, present mode %d", ext.width, ext.height, g_vk.nimages, (int)mode);
    g_vk.need_recreate = 0;
    return 1;
}

static int recreate_swapchain(void)
{
    vkDeviceWaitIdle(g_vk.dev);
    return create_swapchain();
}

/* ---- init ---------------------------------------------------------------------------------- */

/* colour (+ depth) pass; colour cleared or kept; the colour ends in `final` */
static VkRenderPass make_pass(VkFormat color, VkImageLayout initial, VkImageLayout final, int clear, int depth)
{
    VkAttachmentDescription a[2];
    memset(a, 0, sizeof a);
    a[0].format = color;
    a[0].samples = VK_SAMPLE_COUNT_1_BIT;
    a[0].loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    a[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[0].initialLayout = clear ? VK_IMAGE_LAYOUT_UNDEFINED : initial;
    a[0].finalLayout = final;
    a[1].format = g_vk.depth_format;
    a[1].samples = VK_SAMPLE_COUNT_1_BIT;
    a[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp;
    memset(&sp, 0, sizeof sp);
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &cref;
    sp.pDepthStencilAttachment = depth ? &dref : NULL;
    VkSubpassDependency dep[2];
    memset(dep, 0, sizeof dep);
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                        | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                        | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    /* the colour is sampled afterwards (target) or presented (swapchain) */
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ci.attachmentCount = depth ? 2 : 1;
    ci.pAttachments = a;
    ci.subpassCount = 1;
    ci.pSubpasses = &sp;
    ci.dependencyCount = 2;
    ci.pDependencies = dep;
    VkRenderPass rp = VK_NULL_HANDLE;
    if (vkCreateRenderPass(g_vk.dev, &ci, NULL, &rp) != VK_SUCCESS)
        wr_log("vulkan: vkCreateRenderPass failed");
    return rp;
}

static int create_render_passes(void)
{
    g_vk.present_clear = make_pass(g_vk.format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 1, 0);
    /* after a post chain the swapchain image is in COLOR_ATTACHMENT_OPTIMAL */
    g_vk.present_load = make_pass(g_vk.format, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0);
    g_vk.target_clear = make_pass(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, 1);
    g_vk.target_load = make_pass(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    return g_vk.present_clear && g_vk.present_load && g_vk.target_clear && g_vk.target_load;
}

int wr_vk_init(SDL_Window *window, int vsync)
{
    memset(&g_vk, 0, sizeof g_vk);
    g_win = window;
    g_vk.vsync = vsync;
    if (!window) {
        wr_log("vulkan: no window");
        return 0;
    }
    p_gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!p_gipa) {
        wr_log("vulkan: no loader (%s)", SDL_GetError());
        return 0;
    }
    p_vkCreateInstance = (PFN_vkCreateInstance)p_gipa(NULL, "vkCreateInstance");

    Uint32 next = 0;
    const char *const *ext = SDL_Vulkan_GetInstanceExtensions(&next);
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "winrecomp";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ii = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ii.pApplicationInfo = &app;
    ii.enabledExtensionCount = next;
    ii.ppEnabledExtensionNames = ext;
    const char *layer = "VK_LAYER_KHRONOS_validation";
    const char *val = getenv("WR_VK_VALIDATION");
    if (val && *val == '1') {
        ii.enabledLayerCount = 1;
        ii.ppEnabledLayerNames = &layer;
    }
    VkResult ir = p_vkCreateInstance(&ii, NULL, &g_vk.instance);
    if (ir == VK_ERROR_LAYER_NOT_PRESENT && ii.enabledLayerCount) {
        wr_log("vulkan: validation layer not installed, running without it");
        ii.enabledLayerCount = 0;
        ir = p_vkCreateInstance(&ii, NULL, &g_vk.instance);
    }
    VK_CHECK(ir, "vkCreateInstance (Vulkan 1.1)");
#define X(n) n = (PFN_##n)p_gipa(g_vk.instance, #n);
    WR_VK_FUNCS(X)
#undef X
    if (!SDL_Vulkan_CreateSurface(window, g_vk.instance, NULL, &g_vk.surface)) {
        wr_log("vulkan: surface failed: %s", SDL_GetError());
        return 0;
    }

    uint32_t n = 8;
    VkPhysicalDevice devs[8];
    vkEnumeratePhysicalDevices(g_vk.instance, &n, devs);
    int best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (VK_API_VERSION_MAJOR(p.apiVersion) == 1 && VK_API_VERSION_MINOR(p.apiVersion) < 1)
            continue;
        uint32_t nq = 16;
        VkQueueFamilyProperties q[16];
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, q);
        for (uint32_t f = 0; f < nq; f++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], f, g_vk.surface, &present);
            if (!(q[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present)
                continue;
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                      : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
            if (score > best_score) {
                best_score = score;
                best = (int)i;
                g_vk.qfamily = f;
            }
            break;
        }
    }
    if (best < 0) {
        wr_log("vulkan: no Vulkan 1.1 device that can present to the window");
        return 0;
    }
    g_vk.phys = devs[best];
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &g_vk.memprops);
    wr_log("vulkan: %s (API %u.%u.%u, driver 0x%X)", g_vk.props.deviceName,
           VK_API_VERSION_MAJOR(g_vk.props.apiVersion), VK_API_VERSION_MINOR(g_vk.props.apiVersion),
           VK_API_VERSION_PATCH(g_vk.props.apiVersion), g_vk.props.driverVersion);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = g_vk.qfamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    const char *dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dext;
    VK_CHECK(vkCreateDevice(g_vk.phys, &dci, NULL, &g_vk.dev), "vkCreateDevice");
#define X(n) n = (PFN_##n)vkGetDeviceProcAddr(g_vk.dev, #n);
    WR_VK_DEV_FUNCS(X)
#undef X
    vkGetDeviceQueue(g_vk.dev, g_vk.qfamily, 0, &g_vk.queue);

    /* colour format: UNORM (the game's colours are already gamma-encoded) */
    uint32_t nf = 32;
    VkSurfaceFormatKHR fmts[32];
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fmts);
    g_vk.format = fmts[0].format;
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            g_vk.format = fmts[i].format;
            break;
        }
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, VK_FORMAT_D32_SFLOAT, &fp);
    g_vk.depth_format = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
                            ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_D16_UNORM;

    if (!create_render_passes())
        return 0;
    if (!create_swapchain())
        g_vk.need_recreate = 1;

    for (int i = 0; i < WR_VK_FRAMES; i++) {
        wr_vk_frame *f = &g_vk.frames[i];
        VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = g_vk.qfamily;
        VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &f->pool), "command pool");
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = f->pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, &f->cmd), "command buffer");
        /* uploads: own pool, reset implicitly when re-begun (never while in flight:
         * a slot's upload buffer is begun only after the slot's fence passed) */
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &g_upool[i]), "upload command pool");
        ai.commandPool = g_upool[i];
        VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, &g_ucmd[i]), "upload command buffer");
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(g_vk.dev, &fi, NULL, &f->fence), "fence");
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &f->acquired), "semaphore");
        VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &f->rendered), "semaphore");
        if (!wr_vk_buffer_create(&f->verts, 8u << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 1))
            return 0;
        if (!wr_vk_buffer_create(&f->upload, 16u << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1))
            return 0;
    }

    VkDescriptorSetLayoutBinding b = { 0 };
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 1;
    li.pBindings = &b;
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &g_vk.tex_layout), "set layout");
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192 };
    VkDescriptorPoolCreateInfo dp = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dp.maxSets = 8192;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;
    VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &dp, NULL, &g_vk.tex_pool), "descriptor pool");

    g_vk.ready = 1;
    return 1;
}

static void free_dead(wr_vk_frame *f)
{
    for (int i = 0; i < f->ndead; i++) {
        wr_vk_texture *t = f->dead[i];
        if (t->set) vkFreeDescriptorSets(g_vk.dev, g_vk.tex_pool, 1, &t->set);
        if (t->alt_set) vkFreeDescriptorSets(g_vk.dev, g_vk.tex_pool, 1, &t->alt_set);
        if (t->view) vkDestroyImageView(g_vk.dev, t->view, NULL);
        if (t->image) vkDestroyImage(g_vk.dev, t->image, NULL);
        if (t->mem) vkFreeMemory(g_vk.dev, t->mem, NULL);
        free(t);
    }
    f->ndead = 0;
}

void wr_vk_shutdown(void)
{
    if (!g_vk.dev)
        return;
    vkDeviceWaitIdle(g_vk.dev);
    for (int i = 0; i < WR_VK_FRAMES; i++) {
        wr_vk_frame *f = &g_vk.frames[i];
        free_dead(f);
        wr_vk_buffer_destroy(&f->verts);
        wr_vk_buffer_destroy(&f->upload);
        vkDestroyFence(g_vk.dev, f->fence, NULL);
        vkDestroySemaphore(g_vk.dev, f->acquired, NULL);
        vkDestroySemaphore(g_vk.dev, f->rendered, NULL);
        vkDestroyCommandPool(g_vk.dev, f->pool, NULL);
        vkDestroyCommandPool(g_vk.dev, g_upool[i], NULL);
    }
    destroy_swapchain_parts();
    if (g_vk.swapchain) vkDestroySwapchainKHR(g_vk.dev, g_vk.swapchain, NULL);
    vkDestroyDescriptorPool(g_vk.dev, g_vk.tex_pool, NULL);
    vkDestroyDescriptorSetLayout(g_vk.dev, g_vk.tex_layout, NULL);
    vkDestroyRenderPass(g_vk.dev, g_vk.present_clear, NULL);
    vkDestroyRenderPass(g_vk.dev, g_vk.present_load, NULL);
    vkDestroyRenderPass(g_vk.dev, g_vk.target_clear, NULL);
    vkDestroyRenderPass(g_vk.dev, g_vk.target_load, NULL);
    vkDestroyDevice(g_vk.dev, NULL);
    vkDestroySurfaceKHR(g_vk.instance, g_vk.surface, NULL);
    vkDestroyInstance(g_vk.instance, NULL);
    memset(&g_vk, 0, sizeof g_vk);
}

void wr_vk_set_vsync(int on)
{
    if (g_vk.vsync != on) {
        g_vk.vsync = on;
        g_vk.need_recreate = 1;
    }
}

/* ---- uploads -------------------------------------------------------------------------- */

static VkCommandBuffer upload_cmd(void)
{
    uint32_t i = g_vk.frame;
    if (!g_urec[i]) {
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(g_ucmd[i], &bi);
        g_urec[i] = 1;
    }
    return g_ucmd[i];
}

/* Submit pending uploads now and wait (staging full). */
static void flush_uploads_now(void)
{
    uint32_t i = g_vk.frame;
    if (!g_urec[i])
        return;
    vkEndCommandBuffer(g_ucmd[i]);
    g_urec[i] = 0;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_ucmd[i];
    vkQueueSubmit(g_vk.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_vk.queue);
    g_vk.frames[i].uused = 0;
}

static void *staging(VkDeviceSize n, VkDeviceSize *offset)
{
    wr_vk_frame *f = &g_vk.frames[g_vk.frame];
    VkDeviceSize off = (f->uused + 15) & ~(VkDeviceSize)15;
    if (off + n > f->upload.size) {
        if (n > f->upload.size)
            return NULL;
        /* Submit what is recorded and wait: the frame's draws that use those
         * textures come later in the queue, so they still see the data. */
        flush_uploads_now();
        off = 0;
    }
    f->uused = off + n;
    *offset = off;
    return (uint8_t *)f->upload.map + off;
}

static void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = sa;
    b.dstAccessMask = da;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

static void upload_region(wr_vk_texture *t, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          const uint32_t *rgba, uint32_t pitch_px, int first)
{
    VkDeviceSize off;
    VkDeviceSize n = (VkDeviceSize)w * h * 4u;
    uint32_t *dst = (uint32_t *)staging(n, &off);
    if (!dst) {
        wr_log("vulkan: texture %ux%u too large for staging", w, h);
        return;
    }
    for (uint32_t r = 0; r < h; r++)
        memcpy(dst + (size_t)r * w, rgba + (size_t)r * pitch_px, (size_t)w * 4u);
    VkCommandBuffer cmd = upload_cmd();
    barrier(cmd, t->image, first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, first ? 0 : VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            first ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy c = { 0 };
    c.bufferOffset = off;
    c.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.imageOffset = (VkOffset3D){ (int32_t)x, (int32_t)y, 0 };
    c.imageExtent = (VkExtent3D){ w, h, 1 };
    vkCmdCopyBufferToImage(cmd, g_vk.frames[g_vk.frame].upload.buf, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

wr_vk_texture *wr_vk_texture_create(uint32_t w, uint32_t h, const uint32_t *rgba, VkSampler sampler)
{
    if (!g_vk.ready || !w || !h)
        return NULL;
    wr_vk_texture *t = (wr_vk_texture *)calloc(1, sizeof *t);
    t->w = w;
    t->h = h;
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = (VkExtent3D){ w, h, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(g_vk.dev, &ii, NULL, &t->image) != VK_SUCCESS)
        goto fail;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_vk.dev, t->image, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = wr_vk_memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g_vk.dev, &ai, NULL, &t->mem) != VK_SUCCESS)
        goto fail;
    vkBindImageMemory(g_vk.dev, t->image, t->mem, 0);
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = t->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &t->view) != VK_SUCCESS)
        goto fail;
    VkDescriptorSetAllocateInfo da = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    da.descriptorPool = g_vk.tex_pool;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &g_vk.tex_layout;
    if (vkAllocateDescriptorSets(g_vk.dev, &da, &t->set) != VK_SUCCESS) {
        wr_log("vulkan: out of texture descriptor sets");
        goto fail;
    }
    VkDescriptorImageInfo di = { sampler, t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wd = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wd.dstSet = t->set;
    wd.descriptorCount = 1;
    wd.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wd.pImageInfo = &di;
    vkUpdateDescriptorSets(g_vk.dev, 1, &wd, 0, NULL);
    if (rgba) {
        upload_region(t, 0, 0, w, h, rgba, w, 1);
    } else {
        /* transition to a sampleable layout so an untouched texture is valid */
        VkCommandBuffer cmd = upload_cmd();
        barrier(cmd, t->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    return t;
fail:
    wr_log("vulkan: texture %ux%u creation failed", w, h);
    wr_vk_texture_destroy(t);
    return NULL;
}

void wr_vk_texture_update(wr_vk_texture *t, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          const uint32_t *rgba, uint32_t pitch_px)
{
    if (t && w && h && x + w <= t->w && y + h <= t->h)
        upload_region(t, x, y, w, h, rgba, pitch_px, 0);
}

VkDescriptorSet wr_vk_texture_alt_set(wr_vk_texture *t, VkSampler sampler)
{
    if (t->alt_set && t->alt_sampler == sampler)
        return t->alt_set;
    if (!t->alt_set) {
        VkDescriptorSetAllocateInfo da = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        da.descriptorPool = g_vk.tex_pool;
        da.descriptorSetCount = 1;
        da.pSetLayouts = &g_vk.tex_layout;
        if (vkAllocateDescriptorSets(g_vk.dev, &da, &t->alt_set) != VK_SUCCESS)
            return t->set;
    }
    VkDescriptorImageInfo di = { sampler, t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wd = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wd.dstSet = t->alt_set;
    wd.descriptorCount = 1;
    wd.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wd.pImageInfo = &di;
    vkUpdateDescriptorSets(g_vk.dev, 1, &wd, 0, NULL);
    t->alt_sampler = sampler;
    return t->alt_set;
}

void wr_vk_texture_destroy(wr_vk_texture *t)
{
    if (!t)
        return;
    wr_vk_frame *f = &g_vk.frames[g_vk.frame];
    if (f->ndead == 256) {
        vkDeviceWaitIdle(g_vk.dev);
        free_dead(f);
    }
    f->dead[f->ndead++] = t;
}

/* ---- frames ------------------------------------------------------------------------------- */

VkCommandBuffer wr_vk_frame_begin(void)
{
    if (!g_vk.ready || g_vk.in_frame)
        return NULL;
    if (g_vk.need_recreate && !recreate_swapchain())
        return NULL;
    wr_vk_frame *f = &g_vk.frames[g_vk.frame];
    vkWaitForFences(g_vk.dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
    free_dead(f);

    VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, f->acquired, VK_NULL_HANDLE, &g_vk.image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        g_vk.need_recreate = 1;
        return NULL;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        wr_log("vulkan: acquire failed (%d)", (int)r);
        return NULL;
    }
    if (r == VK_SUBOPTIMAL_KHR)
        g_vk.need_recreate = 1;
    vkResetFences(g_vk.dev, 1, &f->fence);
    vkResetCommandPool(g_vk.dev, f->pool, 0);
    f->vused = 0;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f->cmd, &bi);
    g_vk.in_frame = 1;
    g_vk.in_pass = 0;
    g_vk.presented_pass = 0;
    return f->cmd;
}

void wr_vk_target_begin(wr_vk_target *t, int clear, const float rgba[4])
{
    if (!g_vk.in_frame || g_vk.in_pass || !t)
        return;
    VkClearValue cv[2];
    memset(cv, 0, sizeof cv);
    if (rgba)
        memcpy(cv[0].color.float32, rgba, sizeof(float) * 4);
    cv[1].depthStencil.depth = 1.0f;
    VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp.renderPass = clear ? g_vk.target_clear : g_vk.target_load;
    rp.framebuffer = t->fb;
    rp.renderArea.extent = (VkExtent2D){ t->w, t->h };
    rp.clearValueCount = 2;
    rp.pClearValues = cv;
    vkCmdBeginRenderPass(g_vk.frames[g_vk.frame].cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    g_vk.in_pass = 1;
}

void wr_vk_target_end(void)
{
    if (g_vk.in_pass) {
        vkCmdEndRenderPass(g_vk.frames[g_vk.frame].cmd);
        g_vk.in_pass = 0;
    }
}

VkImage wr_vk_swap_image(void) { return g_vk.images[g_vk.image]; }
VkImageView wr_vk_swap_view(void) { return g_vk.views[g_vk.image]; }

void wr_vk_present_begin(int clear, const float rgba[4])
{
    if (!g_vk.in_frame || g_vk.in_pass)
        return;
    VkClearValue cv;
    memset(&cv, 0, sizeof cv);
    if (rgba)
        memcpy(cv.color.float32, rgba, sizeof(float) * 4);
    VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp.renderPass = clear ? g_vk.present_clear : g_vk.present_load;
    rp.framebuffer = g_vk.fbs[g_vk.image];
    rp.renderArea.extent = g_vk.extent;
    rp.clearValueCount = 1;
    rp.pClearValues = &cv;
    vkCmdBeginRenderPass(g_vk.frames[g_vk.frame].cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    g_vk.in_pass = 1;
    g_vk.presented_pass = 1;
}

void wr_vk_present_end(void) { wr_vk_target_end(); }

void wr_vk_frame_end(void)
{
    if (!g_vk.in_frame)
        return;
    wr_vk_frame *f = &g_vk.frames[g_vk.frame];
    if (g_vk.in_pass)
        wr_vk_target_end();
    if (!g_vk.presented_pass) {   /* nothing put on the swapchain image: present black */
        float black[4] = { 0, 0, 0, 1 };
        wr_vk_present_begin(1, black);
        wr_vk_present_end();
    }
    vkEndCommandBuffer(f->cmd);
    VkCommandBuffer cmds[2];
    uint32_t nc = 0;
    if (g_urec[g_vk.frame]) {
        vkEndCommandBuffer(g_ucmd[g_vk.frame]);
        g_urec[g_vk.frame] = 0;
        cmds[nc++] = g_ucmd[g_vk.frame];
    }
    cmds[nc++] = f->cmd;
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f->acquired;
    si.pWaitDstStageMask = &ws;
    si.commandBufferCount = nc;
    si.pCommandBuffers = cmds;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &f->rendered;
    if (vkQueueSubmit(g_vk.queue, 1, &si, f->fence) != VK_SUCCESS)
        wr_log("vulkan: submit failed");
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &f->rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_vk.swapchain;
    pi.pImageIndices = &g_vk.image;
    VkResult r = vkQueuePresentKHR(g_vk.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        g_vk.need_recreate = 1;
    g_vk.in_frame = 0;
    if (g_dump[0].pending || g_dump[1].pending)
        vkDeviceWaitIdle(g_vk.dev);
    for (int d = 0; d < DUMP_SLOTS; d++) {
        if (!g_dump[d].pending)
            continue;
        if (g_dump[d].bgra) {
            uint8_t *p = (uint8_t *)g_dump[d].buf.map;
            for (size_t i = 0; i < (size_t)g_dump[d].w * g_dump[d].h; i++) {
                uint8_t tmp = p[4 * i];
                p[4 * i] = p[4 * i + 2];
                p[4 * i + 2] = tmp;
            }
        }
        write_png(g_dump[d].path, (const uint8_t *)g_dump[d].buf.map, g_dump[d].w, g_dump[d].h);
        wr_log("vulkan: frame written to %s", g_dump[d].path);
        g_dump[d].pending = 0;
    }
    g_vk.frame = (g_vk.frame + 1) % WR_VK_FRAMES;
    /* the next slot's staging buffer is reusable once its fence passed:
     * uploads recorded before its frame_begin go into it */
    wr_vk_frame *nf = &g_vk.frames[g_vk.frame];
    vkWaitForFences(g_vk.dev, 1, &nf->fence, VK_TRUE, UINT64_MAX);
    nf->uused = 0;
}

/* ---- debug dump ----------------------------------------------------------------------------- */


static uint32_t crc_table[256];
static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t k = i;
            for (int j = 0; j < 8; j++)
                k = (k & 1) ? 0xEDB88320u ^ (k >> 1) : k >> 1;
            crc_table[i] = k;
        }
    c = ~c;
    while (n--)
        c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void put32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    fwrite(b, 1, 4, f);
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n)
{
    put32(f, n);
    fwrite(type, 1, 4, f);
    if (n)
        fwrite(data, 1, n, f);
    uint32_t c = crc32_update(0, (const uint8_t *)type, 4);
    c = crc32_update(c, data, n);
    put32(f, c);
}

/* RGBA8 -> PNG, deflate "stored" blocks (no compression: a debug dump). */
static void write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                         (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 6, 0, 0, 0 };
    chunk(f, "IHDR", ihdr, 13);
    size_t raw_n = (size_t)h * (1 + (size_t)w * 4);
    uint8_t *raw = (uint8_t *)malloc(raw_n);
    for (uint32_t y = 0; y < h; y++) {
        raw[(size_t)y * (1 + w * 4)] = 0;
        memcpy(raw + (size_t)y * (1 + w * 4) + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
    }
    size_t nblk = (raw_n + 65534) / 65535;
    size_t zn = 2 + nblk * 5 + raw_n + 4;
    uint8_t *z = (uint8_t *)malloc(zn), *o = z;
    *o++ = 0x78; *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) {
        a = (a + raw[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    for (size_t off = 0; off < raw_n; off += 65535) {
        size_t n = raw_n - off < 65535 ? raw_n - off : 65535;
        *o++ = off + n >= raw_n ? 1 : 0;
        *o++ = (uint8_t)n; *o++ = (uint8_t)(n >> 8);
        *o++ = (uint8_t)~n; *o++ = (uint8_t)(~n >> 8);
        memcpy(o, raw + off, n);
        o += n;
    }
    uint32_t ad = (b << 16) | a;
    *o++ = (uint8_t)(ad >> 24); *o++ = (uint8_t)(ad >> 16); *o++ = (uint8_t)(ad >> 8); *o++ = (uint8_t)ad;
    chunk(f, "IDAT", z, (uint32_t)(o - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
}

static void dump_image(VkImage img, uint32_t w, uint32_t h, VkImageLayout layout, int bgra, const char *path);

void wr_vk_dump_target(wr_vk_target *t, const char *path)
{
    if (t)
        dump_image(t->color, t->w, t->h, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, path);
}

void wr_vk_dump_swap(const char *path)
{
    if (g_vk.swap_readable)
        dump_image(g_vk.images[g_vk.image], g_vk.extent.width, g_vk.extent.height, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                   g_vk.format == VK_FORMAT_B8G8R8A8_UNORM, path);
}

static void dump_image(VkImage img, uint32_t w, uint32_t h, VkImageLayout layout, int bgra, const char *path)
{
    struct { VkImage color; uint32_t w, h; } tt = { img, w, h }, *t = &tt;
    if (!g_vk.in_frame || g_vk.in_pass)
        return;
    int d = 0;
    while (d < DUMP_SLOTS && g_dump[d].pending)
        d++;
    if (d == DUMP_SLOTS)
        return;   /* both slots taken this frame */
    VkDeviceSize need = (VkDeviceSize)t->w * t->h * 4;
    if (g_dump[d].buf.size < need) {
        vkDeviceWaitIdle(g_vk.dev);
        wr_vk_buffer_destroy(&g_dump[d].buf);
        if (!wr_vk_buffer_create(&g_dump[d].buf, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1))
            return;
    }
    VkCommandBuffer cmd = g_vk.frames[g_vk.frame].cmd;
    barrier(cmd, t->color, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy c = { 0 };
    c.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.imageExtent = (VkExtent3D){ t->w, t->h, 1 };
    vkCmdCopyImageToBuffer(cmd, t->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_dump[d].buf.buf, 1, &c);
    barrier(cmd, t->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    snprintf(g_dump[d].path, sizeof g_dump[d].path, "%s", path);
    g_dump[d].w = t->w;
    g_dump[d].h = t->h;
    g_dump[d].bgra = bgra;
    g_dump[d].pending = 1;
}

/* ---- offscreen targets ------------------------------------------------------------- */

static int make_image(VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                      VkImage *img, VkDeviceMemory *mem, VkImageView *view)
{
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = (VkExtent3D){ w, h, 1 };
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    VK_CHECK(vkCreateImage(g_vk.dev, &ii, NULL, img), "target image");
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_vk.dev, *img, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = UINT32_MAX;
    if (usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)
        ai.memoryTypeIndex = wr_vk_memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX)
        ai.memoryTypeIndex = wr_vk_memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(g_vk.dev, &ai, NULL, mem), "target memory");
    VK_CHECK(vkBindImageMemory(g_vk.dev, *img, *mem, 0), "target bind");
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = *img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = (VkImageSubresourceRange){ aspect, 0, 1, 0, 1 };
    VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, view), "target view");
    return 1;
}

wr_vk_target *wr_vk_target_create(uint32_t w, uint32_t h, VkSampler sampler)
{
    if (!g_vk.ready || !w || !h)
        return NULL;
    wr_vk_target *t = (wr_vk_target *)calloc(1, sizeof *t);
    t->w = w;
    t->h = h;
    if (!make_image(VK_FORMAT_R8G8B8A8_UNORM, w, h,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, &t->color, &t->color_mem, &t->color_view) ||
        !make_image(g_vk.depth_format, w, h,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, &t->depth, &t->depth_mem, &t->depth_view)) {
        wr_vk_target_destroy(t);
        return NULL;
    }
    VkImageView att[2] = { t->color_view, t->depth_view };
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fi.renderPass = g_vk.target_clear;
    fi.attachmentCount = 2;
    fi.pAttachments = att;
    fi.width = w;
    fi.height = h;
    fi.layers = 1;
    if (vkCreateFramebuffer(g_vk.dev, &fi, NULL, &t->fb) != VK_SUCCESS) {
        wr_vk_target_destroy(t);
        return NULL;
    }
    VkDescriptorSetAllocateInfo da = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    da.descriptorPool = g_vk.tex_pool;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &g_vk.tex_layout;
    if (vkAllocateDescriptorSets(g_vk.dev, &da, &t->set) == VK_SUCCESS) {
        VkDescriptorImageInfo di = { sampler, t->color_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet wd = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        wd.dstSet = t->set;
        wd.descriptorCount = 1;
        wd.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wd.pImageInfo = &di;
        vkUpdateDescriptorSets(g_vk.dev, 1, &wd, 0, NULL);
    }
    /* the load pass expects SHADER_READ_ONLY: start there (black) */
    VkCommandBuffer cmd = upload_cmd();
    barrier(cmd, t->color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    wr_log("vulkan: render target %ux%u", w, h);
    return t;
}

void wr_vk_target_destroy(wr_vk_target *t)
{
    if (!t)
        return;
    vkDeviceWaitIdle(g_vk.dev);
    if (t->set) vkFreeDescriptorSets(g_vk.dev, g_vk.tex_pool, 1, &t->set);
    if (t->fb) vkDestroyFramebuffer(g_vk.dev, t->fb, NULL);
    if (t->color_view) vkDestroyImageView(g_vk.dev, t->color_view, NULL);
    if (t->depth_view) vkDestroyImageView(g_vk.dev, t->depth_view, NULL);
    if (t->color) vkDestroyImage(g_vk.dev, t->color, NULL);
    if (t->depth) vkDestroyImage(g_vk.dev, t->depth, NULL);
    if (t->color_mem) vkFreeMemory(g_vk.dev, t->color_mem, NULL);
    if (t->depth_mem) vkFreeMemory(g_vk.dev, t->depth_mem, NULL);
    free(t);
}

VkDeviceSize wr_vk_push_verts(const void *data, VkDeviceSize n)
{
    wr_vk_frame *f = &g_vk.frames[g_vk.frame];
    VkDeviceSize off = (f->vused + 15) & ~(VkDeviceSize)15;
    if (off + n > f->verts.size)
        return VK_WHOLE_SIZE;
    memcpy((uint8_t *)f->verts.map + off, data, (size_t)n);
    f->vused = off + n;
    return off;
}

VkBuffer wr_vk_vertex_buffer(void) { return g_vk.frames[g_vk.frame].verts.buf; }

VkShaderModule wr_vk_shader(const uint32_t *spirv, size_t bytes)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = bytes;
    ci.pCode = spirv;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_vk.dev, &ci, NULL, &m) != VK_SUCCESS)
        wr_log("vulkan: shader module failed");
    return m;
}

VkSampler wr_vk_sampler(VkFilter filter, VkSamplerAddressMode mode)
{
    VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    ci.magFilter = filter;
    ci.minFilter = filter;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = mode;
    ci.addressModeV = mode;
    ci.addressModeW = mode;
    ci.maxLod = 0.0f;
    VkSampler s = VK_NULL_HANDLE;
    if (vkCreateSampler(g_vk.dev, &ci, NULL, &s) != VK_SUCCESS)
        wr_log("vulkan: sampler failed");
    return s;
}
