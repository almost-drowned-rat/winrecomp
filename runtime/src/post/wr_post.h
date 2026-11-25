/* winrecomp runtime - post pass (RetroArch slang shaders through librashader,
 * shader_pass.c taken from xboxrecomp option-c src/nv2a_vk). */
#ifndef WR_POST_H
#define WR_POST_H

/* Shader status: OFF, LOADING, ON, FAILED (the preset or a frame failed,
 * logged; the picture stays unshaded) or NO_LIBRARY (no usable librashader
 * next to the exe). */
enum { WR_POST_SHADER_OFF, WR_POST_SHADER_LOADING, WR_POST_SHADER_ON, WR_POST_SHADER_FAILED,
       WR_POST_SHADER_NO_LIBRARY };

/* A parameter of the preset in place (its #pragma parameter lines). */
typedef struct {
    char name[64];
    char desc[128];
    float initial, minimum, maximum, step;
} WrPostShaderParam;

int wr_post_shader_params(WrPostShaderParam *out, int max, unsigned *gen);
/* 1 if librashader's file is next to the exe (menu lines greyed otherwise). */
int wr_post_shader_library_present(void);

#endif
