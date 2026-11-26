#version 450
// Fullscreen triangle for 3D post-processing passes.

layout(location = 0) out vec2 outUV;

void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    outUV = vec2(p.x, 1.0 - p.y);
}
