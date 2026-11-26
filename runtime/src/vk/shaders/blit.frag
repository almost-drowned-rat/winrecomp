#version 450
// Game picture -> swapchain, with the game's gamma (Vid_SetGamma: out = in^(1/g)).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_tex;
layout(push_constant) uniform PC { vec4 inv_gamma; vec4 uv_scale; } pc;   // uv_scale.xy: part of the texture
void main()
{
    vec3 c = texture(u_tex, v_uv * pc.uv_scale.xy).rgb;
    o_color = vec4(pow(c, pc.inv_gamma.rgb), 1.0);
}
