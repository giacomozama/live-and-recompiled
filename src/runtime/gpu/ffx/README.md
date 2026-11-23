FSR 3.1's upscaler shaders from AMD's FidelityFX SDK v1.1.4 (tag `v1.1.4`, MIT, LICENSE.txt), copied as they are:
`sdk/include/FidelityFX/gpu` (the files the passes include) and the seven passes run here from
`sdk/src/backends/vk/shaders/fsr3upscaler` (in `passes/`). CMake embeds them in the executable (XbeRecomp.cmake) and
gpu/fsr3.cpp compiles and runs them, with three declarations of the callbacks header changed as it compiles (Patched).
