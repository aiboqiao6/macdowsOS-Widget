#version 120
varying vec2 v_texcoord;

uniform sampler2D u_texture;
uniform vec2 u_direction;
uniform vec2 u_resolution;
uniform float u_centerWeight;
uniform int u_sampleCount;
// Positive-side bilinear sample offset in pixels and its combined weight.
uniform vec2 u_samples[64];

void main() {
    vec2 uv = v_texcoord;
    vec4 color = texture2D(u_texture, uv) * u_centerWeight;
    // Economy quality normally needs only three pairs. Keep that common
    // path unrolled so older GL drivers don't dynamically index a 64-element
    // uniform array for a seven-fetch filter on a large live gallery.
    if (u_sampleCount == 3) {
        vec2 stepUV = u_direction / u_resolution;
        color += (texture2D(u_texture, uv + stepUV * u_samples[0].x)
                + texture2D(u_texture, uv - stepUV * u_samples[0].x)) * u_samples[0].y;
        color += (texture2D(u_texture, uv + stepUV * u_samples[1].x)
                + texture2D(u_texture, uv - stepUV * u_samples[1].x)) * u_samples[1].y;
        color += (texture2D(u_texture, uv + stepUV * u_samples[2].x)
                + texture2D(u_texture, uv - stepUV * u_samples[2].x)) * u_samples[2].y;
        gl_FragColor = color;
        return;
    }
    for (int i = 0; i < 64; ++i) {
        if (i >= u_sampleCount) break;
        vec2 offset = u_direction * u_samples[i].x / u_resolution;
        color += (texture2D(u_texture, uv + offset)
                + texture2D(u_texture, uv - offset)) * u_samples[i].y;
    }
    gl_FragColor = color;
}
