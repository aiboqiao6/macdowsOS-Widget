#version 120
varying vec2 v_texcoord;

uniform sampler2D u_texture;
uniform vec2 u_direction;
uniform vec2 u_resolution;
uniform float u_radius;

void main() {
    vec2 uv = v_texcoord;
    vec4 color = vec4(0.0);
    vec2 off1 = vec2(1.411764705882353) * u_direction * u_radius;
    vec2 off2 = vec2(3.2941176470588234) * u_direction * u_radius;
    vec2 off3 = vec2(5.176470588235294) * u_direction * u_radius;
    color += texture2D(u_texture, uv) * 0.1964825501511404;
    color += texture2D(u_texture, uv + off1 / u_resolution) * 0.2969069646728344;
    color += texture2D(u_texture, uv - off1 / u_resolution) * 0.2969069646728344;
    color += texture2D(u_texture, uv + off2 / u_resolution) * 0.09447039785044732;
    color += texture2D(u_texture, uv - off2 / u_resolution) * 0.09447039785044732;
    color += texture2D(u_texture, uv + off3 / u_resolution) * 0.010381362401148057;
    color += texture2D(u_texture, uv - off3 / u_resolution) * 0.010381362401148057;
    gl_FragColor = color;
}
