[English](README.md) | [Русский](README.ru.md)

# SDL2-RGA

A fork of **SDL 2.28.5** with an optional hardware-accelerated renderer built
on Rockchip RGA. The project is primarily intended for **PicoCalc / Lyra B**
and other Linux devices with Rockchip RGA, `librga`, and a DMA heap.

SDL is a cross-platform library for graphics, audio, keyboard, mouse,
controllers, and other input devices. This fork adds an `rga` renderer to the
`SDL_Renderer` API while keeping SDL2's standard APIs and software renderer
available.

## RGA renderer

The renderer uses DMA buffers shared by the CPU and RGA. On Lyra B, DirectFB
provides the window and display output, while RGA operations are performed by
the SDL renderer itself.

Supported operations can be accelerated in hardware, including clearing,
rectangle fills, texture copies, linear scaling, supported 90°/180°/270°
rotations, and flips. RGB565 and ARGB8888 textures are supported. Operations
that are not suitable for RGA continue through SDL's software renderer.

RGA is a 2D blitter, not a GPU for arbitrary geometry. Points, lines,
triangles, arbitrary-angle rotations, and unsupported blend modes are handled
in software. The maximum RGA buffer size is 1280 × 1280.

## Build

RGA support requires Linux, CMake, the Rockchip RGA headers, and `librga`.
DirectFB headers and libraries are also required when using DirectFB for display
output. Example build for a Linux device with DirectFB:

```sh
cmake -S . -B build \
  -DSDL_RGA=ON \
  -DSDL_DIRECTFB=ON \
  -DSDL_DIRECTFB_SHARED=OFF
cmake --build build --parallel
```

Select the renderer before creating it:

```c
SDL_SetHint(SDL_HINT_RENDER_DRIVER, "rga");
SDL_Renderer *renderer = SDL_CreateRenderer(
    window, -1, SDL_RENDERER_ACCELERATED);
```

On Lyra B with DirectFB, launch an application with:

```sh
SDL_VIDEODRIVER=directfb ./your_app
```

Diagnostic hints include `SDL_RGA_STATS=1` (hardware operation counters),
`SDL_RGA_FORCE=1` (force eligible hardware operations), and `SDL_RGA_CACHE=0`
(disable the scaling cache). Set them before creating the renderer.

## Documentation

- [Detailed implementation, build, and hardware testing notes (Russian)](README-Lyra-RGA.md)
- [SDL2 and platform documentation](docs/README.md)
- [License](LICENSE.txt)

This fork is based on the upstream [`release-2.28.5`](https://github.com/libsdl-org/SDL/releases/tag/release-2.28.5) tag.
