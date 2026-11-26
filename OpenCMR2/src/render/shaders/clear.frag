#version 450

layout(location = 0) out vec4 outColor;
#ifdef OPENCMR2_MRT_DEPTH
layout(location = 1) out float outLinearDepth;
#endif

layout(std140, set = 3, binding = 0) uniform ClearUniforms {
    vec4 color;
};

void main()
{
    outColor = color;
#ifdef OPENCMR2_MRT_DEPTH
    outLinearDepth = 10000.0;
#endif
}
