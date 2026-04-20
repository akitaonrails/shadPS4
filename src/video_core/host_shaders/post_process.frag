// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;
    bool hdr;
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
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
    } else {
        vec3 encoded = gamma(color_linear.rgb);
        encoded += vec3(dither_offset(ivec2(gl_FragCoord.xy)));
        color = vec4(encoded, color_linear.a);
    }
}
