/* Vulkan backend smoke test: window, swapchain, texture upload, 120 frames. */
#include "wr.h"
#include "vk/wr_vk.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

void wr_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); }
void wr_fatal(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); abort(); }
uint32_t g_wr_last_error;
ptrdiff_t g_xbox_mem_offset;

int main(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) { printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_Window *w = SDL_CreateWindow("wr_vk test", 640, 480, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!w || !wr_vk_init(w, 1)) return 1;
    VkSampler s = wr_vk_sampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    static uint32_t px[64 * 64];
    for (int i = 0; i < 64 * 64; i++) px[i] = ((i ^ (i >> 6)) & 8) ? 0xFF00FFFFu : 0xFF000000u;
    wr_vk_texture *t = wr_vk_texture_create(64, 64, px, s);
    uint64_t t0 = SDL_GetTicksNS();
    int frames = 0;
    for (int f = 0; f < 120; f++) {
        SDL_Event e; while (SDL_PollEvent(&e)) {}
        float c[4] = { f / 120.0f, 0.2f, 0.4f, 1 };
        VkCommandBuffer cmd = wr_vk_begin_frame(c);
        if (!cmd) continue;
        if (f == 30) wr_vk_texture_update(t, 0, 0, 32, 32, px, 64);
        if (f == 60) { wr_vk_texture_destroy(t); t = wr_vk_texture_create(128, 128, NULL, s); }
        wr_vk_end_frame();
        frames++;
    }
    double sec = (SDL_GetTicksNS() - t0) / 1e9;
    printf("vk: %d frames in %.2f s (%.1f fps, vsync)\n", frames, sec, frames / sec);
    wr_vk_texture_destroy(t);
    wr_vk_shutdown();
    SDL_Quit();
    return frames == 120 ? 0 : 1;
}
