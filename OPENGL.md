# OpenGL

Set `renderer = "opengl"` in `settings.toml`, or pick **OpenGL** under
Graphics, Display. In a build with the debug features, the `XBE_RENDERER=opengl`
environment variable overrides the setting for one launch. Vulkan remains the default. OpenGL is for GPUs whose
Vulkan driver is missing or incomplete, such as Ivy Bridge's HD 4000 on Mesa.

The renderer needs an OpenGL 4.1 core context with `ARB_clip_control`, and it
uses GLSL 4.20 when the context offers it. Mesa's crocus driver gives the HD 4000
OpenGL 4.2. The window is made for the renderer at launch, so a change in the
menu takes effect after a restart.

The backend follows the D3D11 backend ([D3D11.md](D3D11.md)). It uses the same
Xbox texture and vertex decoders, generated HLSL, interpolated draw replay and
graphics settings: MSAA, SMAA, FSR 1.0, post-processing, ambient occlusion,
motion blur, outlines, the unstretched HUD, lighting, fur and water. The shared
HLSL goes through glslang to SPIR-V and through SPIRV-Cross to GLSL. Each
translated shader is saved in `opengl-shaders-v1.bin` in the cache directory,
and the driver's own shader cache keeps what it compiles. The renderer has no
DLSS or HDR output.

The outlines and the depth texture's stencil bytes need `ARB_stencil_texturing`
(OpenGL 4.3). On a driver without it, the log says so and both are left out.
With the debug features, `XBE_GL_DEBUG=1` asks for a debug context and logs the
driver's errors and warnings, and `XBE_PERF=1` adds render timing to
`logs/game.log`.

`tools/opengl-shader-smoke.cpp` checks the shader translation on a real GL
context. It translates, compiles and links representative generated shaders and
every full-screen pass. It also draws three checks on the GPU: the picture's
orientation, the point-sprite corners and the depth and stencil byte layouts.
The build command is at the top of the file. To approximate Ivy Bridge's limits
with Mesa's software renderer, run it with:

```sh
__GLX_VENDOR_LIBRARY_NAME=mesa __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json \
LIBGL_ALWAYS_SOFTWARE=1 MESA_GL_VERSION_OVERRIDE=4.2 MESA_GLSL_VERSION_OVERRIDE=420 ./opengl-shader-smoke
```
