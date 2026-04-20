// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;
    float exposure;
    bool hdr;
    uint tonemap_mode;  // 0 = per-channel ACES, 1 = luma-preserving ACES
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// ACES filmic tone-mapper (Narkowicz approximation). Rolls off bright
// linear values with a smooth shoulder so an exposure multiplier can
// rescue under-bright scenes without blowing out already-bright UI or
// highlights. Identity-ish below ~0.2, soft saturation above ~1.0.
float aces_scalar(float x) {
    const float aa = 2.51, bb = 0.03, cc = 2.43, dd = 0.59, ee = 0.14;
    return clamp((x * (aa * x + bb)) / (x * (cc * x + dd) + ee), 0.0, 1.0);
}
vec3 aces_per_channel(vec3 x) {
    return vec3(aces_scalar(x.r), aces_scalar(x.g), aces_scalar(x.b));
}

// Luma-preserving tone-map: tone-map luminance only, scale chroma so the
// hue is preserved. Avoids the slight desaturation that per-channel ACES
// causes when one channel clips earlier than the others.
vec3 aces_luma_preserving(vec3 x) {
    const float luma = dot(x, vec3(0.2126, 0.7152, 0.0722));
    if (luma <= 0.0) return vec3(0.0);
    const float tm = aces_scalar(luma);
    // Scale chroma. The clamp prevents extreme hue channels from producing
    // out-of-gamut output when luma is heavily compressed at the top.
    return clamp(x * (tm / luma), 0.0, 1.0);
}

void main() {
    vec4 color_linear = texture(texSampler, uv);
    if (pp.hdr) {
        color = color_linear;
    } else {
        vec3 exposed = color_linear.rgb * pp.exposure;
        vec3 tonemapped = (pp.tonemap_mode == 1u)
            ? aces_luma_preserving(exposed)
            : aces_per_channel(exposed);
        color = vec4(gamma(tonemapped), color_linear.a);
    }
}
