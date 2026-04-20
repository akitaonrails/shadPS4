// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;        // 1.0 = standard sRGB encode; game may set via sceVideoOutAdjustColor
    float exposure;     // manual bias on top of the auto-exposure buffer value
    bool hdr;
    uint tonemap_mode;  // 0 = per-channel ACES, 1 = luma-preserving ACES
    uint bypass;        // 1 = skip tonemap/exposure entirely; output raw sRGB-encoded input
} pp;

// Auto-exposure buffer, written each frame by the auto_exposure compute pass
// (or pinned to 1.0 by CPU when auto-exposure is disabled). Shader applies
// exposure = pp.exposure * autoexp.smoothed_exposure.
layout (binding = 1) buffer AutoExposure {
    float smoothed_exposure;
    float last_scene_luma;
    uint  frame_count;
    uint  _pad;
} autoexp;

// Standard sRGB encode with a runtime-tweakable gamma. pp.gamma = 1.0 is the
// textbook sRGB OETF (exponent 1/2.4 in the power segment). Other values
// slightly bend the curve and are reachable either from the game via
// sceVideoOutAdjustColor or from the SHADPS4_PP_GAMMA_OVERRIDE env knob.
const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 srgb_encode(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// ACES filmic tone-map (Narkowicz approximation).
float aces_scalar(float x) {
    const float aa = 2.51, bb = 0.03, cc = 2.43, dd = 0.59, ee = 0.14;
    return clamp((x * (aa * x + bb)) / (x * (cc * x + dd) + ee), 0.0, 1.0);
}
vec3 aces_per_channel(vec3 x) {
    return vec3(aces_scalar(x.r), aces_scalar(x.g), aces_scalar(x.b));
}
vec3 aces_luma_preserving(vec3 x) {
    const float luma = dot(x, vec3(0.2126, 0.7152, 0.0722));
    if (luma <= 0.0) return vec3(0.0);
    const float tm = aces_scalar(luma);
    return clamp(x * (tm / luma), 0.0, 1.0);
}

// 4x4 Bayer dither (~±1 LSB) to break visible banding in smooth gradients
// on the 8-bit swapchain. Static pattern so recording pipelines don't get
// temporal noise.
const float bayer4[16] = float[16](
     0.0,  8.0,  2.0, 10.0,
    12.0,  4.0, 14.0,  6.0,
     3.0, 11.0,  1.0,  9.0,
    15.0,  7.0, 13.0,  5.0
);
float dither_offset(ivec2 p) {
    int idx = (p.y & 3) * 4 + (p.x & 3);
    return (bayer4[idx] - 7.5) / 8.0 / 255.0;
}

void main() {
    vec4 color_linear = texture(texSampler, uv);
    if (pp.hdr) {
        color = color_linear;
        return;
    }
    if (pp.bypass == 1u) {
        // Diagnostic passthrough. sRGB-encode only — no exposure, no tonemap.
        // This shows exactly what the game is writing to the video-out
        // framebuffer, so we can see whether "scene is all black" is really
        // the game's output or a pipeline artefact.
        color = vec4(srgb_encode(clamp(color_linear.rgb, 0.0, 1.0)), color_linear.a);
        return;
    }
    // Canonical SDR path: linear × exposure → ACES → sRGB encode → dither.
    // Nothing else. Every additional curve that's crept in previously was
    // fighting another, so we keep this deliberately minimal.
    const float final_exposure = pp.exposure * autoexp.smoothed_exposure;
    vec3 exposed = color_linear.rgb * final_exposure;
    vec3 tonemapped = (pp.tonemap_mode == 1u)
        ? aces_luma_preserving(exposed)
        : aces_per_channel(exposed);
    vec3 encoded = srgb_encode(tonemapped);
    encoded += vec3(dither_offset(ivec2(gl_FragCoord.xy)));
    color = vec4(encoded, color_linear.a);
}
