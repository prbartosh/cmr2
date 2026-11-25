#version 450
// Directional sun shadow map depth pre-pass fragment shader (supports alpha-tested foliage/fences).

layout(location = 0) in vec2 inTex0;

layout(set = 2, binding = 0) uniform sampler2D tex0;

layout(std140, set = 3, binding = 0) uniform ShadowFragmentUniforms {
    vec4 alphaParams;   // x: alphaTestEnabled (0/1), y: alphaRef (0..1), z: compareFunc, w: unused
};

bool Compare(int func, float a, float b)
{
    switch (func) {
    case 1: return false;
    case 2: return a < b;
    case 3: return a == b;
    case 4: return a <= b;
    case 5: return a > b;
    case 6: return a != b;
    case 7: return a >= b;
    default: return true;
    }
}

void main()
{
    if (alphaParams.x > 0.5) {
        float a = texture(tex0, inTex0).a;
        if (!Compare(int(alphaParams.z), floor(a * 255.0 + 0.5), floor(alphaParams.y * 255.0 + 0.5)))
            discard;
    }
}
