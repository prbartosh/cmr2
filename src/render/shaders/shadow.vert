#version 450
// Directional sun shadow map depth pre-pass vertex shader.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inDiffuse;
layout(location = 3) in vec4 inSpecular;
layout(location = 4) in vec2 inTex0;
layout(location = 5) in vec2 inTex1;

layout(location = 0) out vec2 outTex0;

layout(std140, set = 1, binding = 0) uniform ShadowVertexUniforms {
    mat4 lightWorldViewProj;
};

void main()
{
    outTex0 = inTex0;
    gl_Position = lightWorldViewProj * vec4(inPosition, 1.0);
}
