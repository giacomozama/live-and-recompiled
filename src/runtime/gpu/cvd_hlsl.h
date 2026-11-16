#pragma once

// Colour vision deficiency simulation (gpu::g_cvd_sim), HLSL pasted into the passes that put the picture on screen
// (kBlitHlsl, kFsrHlsl's rcas, kHdrHlsl's encode). CvdLinear takes linear RGB and the mode: 1 protanopia,
// 2 deuteranopia, 3 tritanopia (Machado, Oliveira and Fernandes 2009, severity 1), 4 achromatopsia (Rec.709
// luminance); 0 or another leaves it. CvdDisplay does the same to a display's (gamma 2.2) colours.
#define GPU_CVD_HLSL                                                                     \
    "float3 CvdLinear(float3 c, float mode) {\n"                                         \
    "    if (mode < 0.5 || mode > 4.5) return c;\n"                                      \
    "    if (mode > 3.5) return dot(c, float3(0.2126, 0.7152, 0.0722));\n"               \
    "    float3x3 m = mode < 1.5 ? float3x3( 0.152286,  1.052583, -0.204868,\n"          \
    "                                        0.114503,  0.786281,  0.099216,\n"          \
    "                                       -0.003882, -0.048116,  1.051998)\n"          \
    "               : mode < 2.5 ? float3x3( 0.367322,  0.860646, -0.227968,\n"          \
    "                                        0.280085,  0.672501,  0.047413,\n"          \
    "                                       -0.011820,  0.042940,  0.968881)\n"          \
    "                            : float3x3( 1.255528, -0.076749, -0.178779,\n"          \
    "                                       -0.078411,  0.930809,  0.147602,\n"          \
    "                                        0.004733,  0.691367,  0.303900);\n"         \
    "    return saturate(mul(m, c));\n"                                                  \
    "}\n"                                                                                \
    "float3 CvdDisplay(float3 c, float mode) {\n"                                        \
    "    if (mode < 0.5) return c;\n"                                                    \
    "    return pow(CvdLinear(pow(saturate(c), 2.2), mode), 1.0 / 2.2);\n"               \
    "}\n"
