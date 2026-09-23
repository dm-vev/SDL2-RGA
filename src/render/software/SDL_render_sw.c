/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2023 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#if SDL_VIDEO_RENDER_SW && !SDL_RENDER_DISABLED

#include "../SDL_sysrender.h"
#include "SDL_render_sw_c.h"
#include "SDL_hints.h"

#include "SDL_draw.h"
#include "SDL_blendfillrect.h"
#include "SDL_blendline.h"
#include "SDL_blendpoint.h"
#include "SDL_drawline.h"
#include "SDL_drawpoint.h"
#include "SDL_rotate.h"
#include "SDL_triangle.h"

#if SDL_VIDEO_RENDER_RGA
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <rga/im2d.h>
#include <rga/rga.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#if SDL_VIDEO_DRIVER_DIRECTFB
#include "../../video/directfb/SDL_DirectFB_window.h"
#endif
#endif

/* SDL surface based renderer implementation */

typedef struct
{
    const SDL_Rect *viewport;
    const SDL_Rect *cliprect;
    SDL_bool surface_cliprect_dirty;
} SW_DrawStateCache;

#if SDL_VIDEO_RENDER_RGA
typedef struct RGA_TextureData
{
    SDL_Texture *texture;
    int fd;
    void *pixels;
    size_t bytes;
    int pitch;
    int format;
    SDL_bool dirty;
    SDL_bool cpu_access;
    rga_buffer_handle_t handle;
    SDL_Surface *scaled;
    SDL_Rect scale_source;
    int scale_width, scale_height;
    SDL_bool scale_seen;
    struct RGA_TextureData *next;
} RGA_TextureData;
#endif

typedef struct
{
    SDL_Surface *surface;
    SDL_Surface *window;
#if SDL_VIDEO_RENDER_RGA
    int rga_fd;
    size_t rga_bytes;
    void *rga_pixels;
    RGA_TextureData *rga_textures;
    int fill_fd;
    void *fill_pixels;
    size_t fill_bytes;
    Uint32 fill_color;
    SDL_bool fill_valid;
    SDL_bool rga_force;
    SDL_bool rga_cache;
    SDL_bool rga_cpu_access;
    SDL_bool fill_cpu_access;
    rga_buffer_handle_t rga_handle;
    rga_buffer_handle_t fill_handle;
    Uint64 rga_fill_count;
    Uint64 rga_alpha_count;
    Uint64 rga_copy_count;
    Uint64 rga_rotate_count;
    Uint64 rga_cache_count;
    Uint64 rga_import_count;
    size_t scale_cache_bytes;
#endif
} SW_RenderData;

#if SDL_VIDEO_RENDER_RGA
/* The CPU and RGA share a DMA heap buffer. A surface allocated by DirectFB
 * cannot be assumed to be importable by RGA on the Lyra's no-MMU RGA2. */
static int RGA_Sync(int fd, Uint64 flags)
{
    struct dma_buf_sync sync;
    sync.flags = flags;
    return ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

/* Ownership changes, not draw calls, delimit CPU cache maintenance. All
 * hardware submissions remain synchronous, including SDL_RenderFlush. */
static int RGA_Access(int fd, SDL_bool *cpu_access, SDL_bool cpu)
{
    if (*cpu_access != cpu) {
        if (RGA_Sync(fd, (cpu ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | DMA_BUF_SYNC_RW) < 0) {
            return SDL_SetError("RGA: DMA ownership transition failed");
        }
        *cpu_access = cpu;
    }
    return 0;
}

static int RGA_CPU(SW_RenderData *data)
{
    return data->rga_fd < 0 ? 0 : RGA_Access(data->rga_fd, &data->rga_cpu_access, SDL_TRUE);
}

static rga_buffer_handle_t RGA_Import(int pitch, int height, int format, int fd)
{
    im_handle_param_t param;
    SDL_zero(param);
    param.width = pitch / (format == RK_FORMAT_RGB_565 ? 2 : 4);
    param.height = height;
    param.format = format;
    return importbuffer_fd(fd, &param);
}

static rga_buffer_t RGA_Buffer(rga_buffer_handle_t handle, int fd, int width,
                                int height, int stride, int format)
{
    /* Older drivers may not support persistent imports. */
    return handle ? wrapbuffer_handle_t(handle, width, height, stride, height, format) :
                    wrapbuffer_fd_t(fd, width, height, stride, height, format);
}

static int RGA_Allocate(int width, int height, int bytes_per_pixel,
                        int *fd, void **pixels, int *pitch, size_t *bytes)
{
    struct dma_heap_allocation_data alloc;
    int heap;

    if (width < 2 || height < 2 || width > 1280 || height > 1280 ||
        (bytes_per_pixel != 2 && bytes_per_pixel != 4)) {
        return -1;
    }
    *pitch = (width * bytes_per_pixel + 3) & ~3;
    *bytes = (size_t)*pitch * (size_t)height;
    heap = open("/dev/dma_heap/linux,cma", O_RDWR | O_CLOEXEC);
    if (heap < 0) {
        return -1;
    }
    SDL_zero(alloc);
    alloc.len = *bytes;
    alloc.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
        close(heap);
        return -1;
    }
    close(heap);
    *fd = (int)alloc.fd;
    *pixels = mmap(NULL, *bytes, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);
    if (*pixels == MAP_FAILED) {
        close(*fd);
        *fd = -1;
        return -1;
    }
    if (RGA_Sync(*fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW) < 0) {
        munmap(*pixels, *bytes);
        close(*fd);
        *fd = -1;
        return -1;
    }
    SDL_memset(*pixels, 0, *bytes);
    return 0;
}

static RGA_TextureData *RGA_FindTexture(SW_RenderData *data, SDL_Texture *texture)
{
    RGA_TextureData *item = data->rga_textures;
    while (item != NULL && item->texture != texture) {
        item = item->next;
    }
    return item;
}

static void RGA_DropScale(SW_RenderData *data, RGA_TextureData *item)
{
    if (item->scaled != NULL) {
        data->scale_cache_bytes -= (size_t)item->scaled->pitch * item->scaled->h;
        SDL_FreeSurface(item->scaled);
        item->scaled = NULL;
    }
    item->scale_seen = SDL_FALSE;
}

static void RGA_ReleaseTexture(SW_RenderData *data, RGA_TextureData *item)
{
    RGA_DropScale(data, item);
    if (item->handle) releasebuffer_handle(item->handle);
    munmap(item->pixels, item->bytes);
    close(item->fd);
    SDL_free(item);
}

static void RGA_UpdateTexture(RGA_TextureData *item)
{
    SDL_Surface *surface = (SDL_Surface *)item->texture->driverdata;
    const int width = surface->w;
    int row;

    for (row = 0; row < surface->h; ++row) {
        const Uint8 *src = (const Uint8 *)surface->pixels + row * surface->pitch;
        Uint8 *dst = (Uint8 *)item->pixels + row * item->pitch;
        if (item->format == RK_FORMAT_RGB_565) {
            SDL_memcpy(dst, src, (size_t)width * 2u);
        } else {
            const Uint32 *in = (const Uint32 *)src;
            Uint32 *out = (Uint32 *)dst;
            int col;
            /* RGA SRC_OVER expects premultiplied RGBA; the SDL surface
             * remains straight-alpha for software blend operations. */
            for (col = 0; col < width; ++col) {
                const Uint32 pixel = in[col];
                const Uint32 alpha = pixel >> 24;
                const Uint32 red = (((pixel >> 16) & 255u) * alpha + 127u) / 255u;
                const Uint32 green = (((pixel >> 8) & 255u) * alpha + 127u) / 255u;
                const Uint32 blue = ((pixel & 255u) * alpha + 127u) / 255u;
                out[col] = (alpha << 24) | (red << 16) | (green << 8) | blue;
            }
        }
    }
    item->dirty = SDL_FALSE;
}

/* Return 1 for hardware, 2 for cached pixels, 0 for SDL software fallback,
 * and -1 for errors. Ownership must be acquired before any CPU pixel access. */
static int RGA_Copy(SW_RenderData *data, SDL_Texture *texture,
                    const SDL_Rect *sr, const SDL_Rect *dr, SDL_ScaleMode scale_mode,
                    const SDL_Rect *clip, Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha,
                    SDL_BlendMode blend, int transform)
{
    RGA_TextureData *item = RGA_FindTexture(data, texture);
    rga_buffer_t source, target, pat;
    im_rect src_rect, dst_rect, empty;
    IM_STATUS status;
    int usage;
    SDL_bool cacheable, capture = SDL_FALSE;

    if (item != NULL && item->dirty) RGA_DropScale(data, item);

    if (item == NULL || sr->w < 2 || sr->h < 2 || dr->w < 16 || dr->h < 16 ||
        sr->x < 0 || sr->y < 0 || sr->w > texture->w - sr->x ||
        sr->h > texture->h - sr->y ||
        dr->x < clip->x || dr->y < clip->y ||
        dr->w > clip->w - (dr->x - clip->x) ||
        dr->h > clip->h - (dr->y - clip->y) ||
        dr->x < 0 || dr->y < 0 ||
        dr->w > data->window->w - dr->x || dr->h > data->window->h - dr->y ||
        red != 255 || green != 255 || blue != 255 || alpha != 255 ||
        ((sr->w != dr->w || sr->h != dr->h) && scale_mode != SDL_ScaleModeLinear) ||
        (transform != 0 && (sr->w != dr->w || sr->h != dr->h)) ||
        dr->w > sr->w * 16 || dr->h > sr->h * 16 ||
        sr->w > dr->w * 16 || sr->h > dr->h * 16) {
        return 0;
    }
    if (!data->rga_force) {
        const int area = dr->w * dr->h;
        const SDL_bool scaled = sr->w != dr->w || sr->h != dr->h;
        if ((transform != 0 && area < 16384) ||
            (scaled && area < 4096) ||
            (!scaled && transform == 0 && item->format == RK_FORMAT_RGB_565 && area < 120000) ||
            (!scaled && transform == 0 && item->format != RK_FORMAT_RGB_565 && area < 16384)) {
            return 0;
        }
    }
    if (item->format == RK_FORMAT_RGB_565) {
        if (blend != SDL_BLENDMODE_NONE) {
            return 0;
        }
        usage = 0;
    } else {
        if (blend != SDL_BLENDMODE_BLEND) {
            return 0;
        }
        usage = IM_ALPHA_BLEND_SRC_OVER;
    }
    /* Cache only background-independent, opaque linear scaling. Keep one
     * variant per texture, at most 512 KiB per renderer, and promote only
     * after a second identical request. Animated/updated textures cannot hit.
     * FORCE bypasses the cache so diagnostics still exercise the hardware. */
    cacheable = data->rga_cache && !data->rga_force && item->format == RK_FORMAT_RGB_565 &&
                transform == 0 && (sr->w != dr->w || sr->h != dr->h);
    if (cacheable) {
        if (item->scale_seen && SDL_RectEquals(&item->scale_source, sr) &&
            item->scale_width == dr->w && item->scale_height == dr->h) {
            if (item->scaled != NULL) {
                SDL_Rect dest = *dr;
                if (RGA_CPU(data) < 0) return -1;
                if (SDL_BlitSurface(item->scaled, NULL, data->window, &dest) < 0) return -1;
                data->rga_cache_count++;
                return 2;
            }
            capture = SDL_TRUE;
        } else {
            RGA_DropScale(data, item);
            item->scale_source = *sr;
            item->scale_width = dr->w;
            item->scale_height = dr->h;
            item->scale_seen = SDL_TRUE;
        }
    }
    if (item->dirty) {
        if (RGA_Access(item->fd, &item->cpu_access, SDL_TRUE) < 0) return -1;
        RGA_UpdateTexture(item);
    }
    if (RGA_Access(item->fd, &item->cpu_access, SDL_FALSE) < 0 ||
        RGA_Access(data->rga_fd, &data->rga_cpu_access, SDL_FALSE) < 0) return -1;
    source = RGA_Buffer(item->handle, item->fd, texture->w, texture->h,
                             item->pitch / (item->format == RK_FORMAT_RGB_565 ? 2 : 4),
                             item->format);
    target = RGA_Buffer(data->rga_handle, data->rga_fd, data->window->w, data->window->h,
                         data->window->pitch / 2, RK_FORMAT_RGB_565);
    SDL_zero(pat);
    src_rect.x = sr->x;
    src_rect.y = sr->y;
    src_rect.width = sr->w;
    src_rect.height = sr->h;
    dst_rect.x = dr->x;
    dst_rect.y = dr->y;
    dst_rect.width = dr->w;
    dst_rect.height = dr->h;
    SDL_zero(empty);
    status = improcess(source, target, pat, src_rect, dst_rect, empty, usage | transform);
    if (status != IM_STATUS_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "RGA blit failed (%d): %s; using CPU",
                    (int)status, imStrError_t(status));
        return 0;
    }
    if (capture && data->scale_cache_bytes + (size_t)((dr->w * 2 + 3) & ~3) * dr->h <= 512u * 1024u) {
        SDL_Surface *cached = SDL_CreateRGBSurfaceWithFormat(0, dr->w, dr->h, 16, SDL_PIXELFORMAT_RGB565);
        if (cached != NULL) {
            int row;
            if (RGA_CPU(data) < 0) {
                SDL_FreeSurface(cached);
                return -1;
            }
            for (row = 0; row < dr->h; ++row) {
                SDL_memcpy((Uint8 *)cached->pixels + row * cached->pitch,
                           (Uint8 *)data->window->pixels + (dr->y + row) * data->window->pitch + dr->x * 2,
                           (size_t)dr->w * 2u);
            }
            item->scaled = cached;
            data->scale_cache_bytes += (size_t)cached->pitch * cached->h;
        }
    }
    return 1;
}

static int RGA_AlphaFill(SW_RenderData *data, const SDL_Rect *rect,
                         Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha)
{
    rga_buffer_t source, target, pat;
    im_rect src_rect = { 0, 0, 32, 32 };
    im_rect dst_rect, empty;
    IM_STATUS status;
    Uint32 color;

    if (data->fill_fd < 0 || rect->w < 16 || rect->h < 16 ||
        rect->w > 512 || rect->h > 512 || alpha == 0) {
        return 0;
    }
    if (!data->rga_force && rect->w * rect->h < 8192) {
        return 0;
    }
    color = ((Uint32)alpha << 24) |
            ((((Uint32)red * alpha + 127u) / 255u) << 16) |
            ((((Uint32)green * alpha + 127u) / 255u) << 8) |
            (((Uint32)blue * alpha + 127u) / 255u);
    if (!data->fill_valid || data->fill_color != color) {
        Uint32 *pixels = (Uint32 *)data->fill_pixels;
        size_t i;
        if (RGA_Access(data->fill_fd, &data->fill_cpu_access, SDL_TRUE) < 0) return -1;
        for (i = 0; i < 32u * 32u; ++i) {
            pixels[i] = color;
        }
        data->fill_color = color;
        data->fill_valid = SDL_TRUE;
    }
    if (RGA_Access(data->fill_fd, &data->fill_cpu_access, SDL_FALSE) < 0 ||
        RGA_Access(data->rga_fd, &data->rga_cpu_access, SDL_FALSE) < 0) return -1;
    source = RGA_Buffer(data->fill_handle, data->fill_fd, 32, 32, 32, RK_FORMAT_RGBA_8888);
    target = RGA_Buffer(data->rga_handle, data->rga_fd, data->window->w, data->window->h,
                         data->window->pitch / 2, RK_FORMAT_RGB_565);
    SDL_zero(pat);
    SDL_zero(empty);
    dst_rect.x = rect->x;
    dst_rect.y = rect->y;
    dst_rect.width = rect->w;
    dst_rect.height = rect->h;
    status = improcess(source, target, pat, src_rect, dst_rect, empty, IM_ALPHA_BLEND_SRC_OVER);
    if (status != IM_STATUS_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "RGA alpha fill failed (%d): %s; using CPU",
                    (int)status, imStrError_t(status));
        return 0;
    }
    return 1;
}

/* Return 1 on hardware success, 0 if CPU should draw, -1 if the CPU mapping
 * could not be re-acquired and must not be accessed. */
static int RGA_Fill(SW_RenderData *data, const SDL_Rect *rect, Uint8 r, Uint8 g, Uint8 b)
{
    rga_buffer_t target;
    im_rect area;
    IM_STATUS result;
    int color;

    if (data->rga_fd < 0 || rect->w < 16 || rect->h < 16 ||
        (!data->rga_force && rect->w * rect->h < 250000)) {
        return 0;
    }
    if (RGA_Access(data->rga_fd, &data->rga_cpu_access, SDL_FALSE) < 0) return -1;
    target = RGA_Buffer(data->rga_handle, data->rga_fd, data->window->w, data->window->h,
                         data->window->pitch / 2, RK_FORMAT_RGB_565);
    area.x = rect->x;
    area.y = rect->y;
    area.width = rect->w;
    area.height = rect->h;
    color = (int)(0xff000000u | ((Uint32)r << 16) | ((Uint32)g << 8) | (Uint32)b);
    result = imfill_t(target, area, color, 1);
    if (result != IM_STATUS_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "RGA fill failed (%d): %s; using CPU",
                    (int)result, imStrError_t(result));
        return 0;
    }
    return 1;
}
#endif

static SDL_Surface *SW_ActivateRenderer(SDL_Renderer *renderer)
{
    SW_RenderData *data = (SW_RenderData *)renderer->driverdata;

    if (!data->surface) {
        data->surface = data->window;
    }
    if (!data->surface) {
        SDL_Surface *surface = SDL_GetWindowSurface(renderer->window);
        if (surface) {
            data->surface = data->window = surface;
        }
    }
    return data->surface;
}

static int SW_CPUAccess(SDL_Renderer *renderer)
{
#if SDL_VIDEO_RENDER_RGA
    SW_RenderData *data = (SW_RenderData *)renderer->driverdata;
    if (data->surface == data->window) return RGA_CPU(data);
#endif
    return 0;
}

static void SW_WindowEvent(SDL_Renderer *renderer, const SDL_WindowEvent *event)
{
    SW_RenderData *data = (SW_RenderData *)renderer->driverdata;

    if (event->event == SDL_WINDOWEVENT_SIZE_CHANGED) {
#if SDL_VIDEO_RENDER_RGA
        if (data->rga_fd >= 0) {
            int width, height, pitch, fd = -1;
            size_t bytes;
            void *pixels;
            SDL_Surface *replacement;
            SDL_Surface *previous;
            SDL_GetWindowSizeInPixels(renderer->window, &width, &height);
            if (width == data->window->w && height == data->window->h) {
                return;
            }
            if (RGA_Allocate(width, height, 2, &fd, &pixels, &pitch, &bytes) < 0) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "RGA: cannot resize DMA surface to %dx%d", width, height);
                return;
            }
            replacement = SDL_CreateRGBSurfaceFrom(pixels, width, height, 16, pitch,
                                                    0xf800u, 0x07e0u, 0x001fu, 0);
            if (replacement == NULL) {
                munmap(pixels, bytes);
                close(fd);
                return;
            }
            previous = data->window;
            if (data->surface == previous) {
                data->surface = replacement;
            }
            data->window = replacement;
            SDL_FreeSurface(previous);
            if (data->rga_handle) releasebuffer_handle(data->rga_handle);
            munmap(data->rga_pixels, data->rga_bytes);
            close(data->rga_fd);
            data->rga_fd = fd;
            data->rga_pixels = pixels;
            data->rga_bytes = bytes;
            data->rga_cpu_access = SDL_TRUE;
            data->rga_handle = RGA_Import(pitch, height, RK_FORMAT_RGB_565, fd);
            if (data->rga_handle) data->rga_import_count++;
            return;
        }
#endif
        data->surface = NULL;
        data->window = NULL;
    }
}

static int SW_GetOutputSize(SDL_Renderer *renderer, int *w, int *h)
{
    SW_RenderData *data = (SW_RenderData *)renderer->driverdata;

    if (data->surface) {
        if (w) {
            *w = data->surface->w;
        }
        if (h) {
            *h = data->surface->h;
        }
        return 0;
    }

    if (renderer->window) {
        SDL_GetWindowSizeInPixels(renderer->window, w, h);
        return 0;
    }

    return SDL_SetError("Software renderer doesn't have an output surface");
}

static int SW_CreateTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    int bpp;
    Uint32 Rmask, Gmask, Bmask, Amask;
#if SDL_VIDEO_RENDER_RGA
    SW_RenderData *render_data = (SW_RenderData *)renderer->driverdata;
#endif

    if (!SDL_PixelFormatEnumToMasks(texture->format, &bpp, &Rmask, &Gmask, &Bmask, &Amask)) {
        return SDL_SetError("Unknown texture format");
    }

    texture->driverdata =
        SDL_CreateRGBSurface(0, texture->w, texture->h, bpp, Rmask, Gmask,
                             Bmask, Amask);
    if (!texture->driverdata) {
        return -1;
    }
    SDL_SetSurfaceColorMod(texture->driverdata, texture->color.r, texture->color.g, texture->color.b);
    SDL_SetSurfaceAlphaMod(texture->driverdata, texture->color.a);
    SDL_SetSurfaceBlendMode(texture->driverdata, texture->blendMode);

    /* Only RLE encode textures without an alpha channel since the RLE coder
     * discards the color values of pixels with an alpha value of zero.
     */
    if (texture->access == SDL_TEXTUREACCESS_STATIC && !Amask
#if SDL_VIDEO_RENDER_RGA
        && render_data->rga_fd < 0
#endif
       ) {
        SDL_SetSurfaceRLE(texture->driverdata, 1);
    }

#if SDL_VIDEO_RENDER_RGA
    if (render_data->rga_fd >= 0 &&
        (texture->format == SDL_PIXELFORMAT_RGB565 ||
         texture->format == SDL_PIXELFORMAT_ARGB8888)) {
        RGA_TextureData *item = (RGA_TextureData *)SDL_calloc(1, sizeof(*item));
        if (item != NULL) {
            int pitch;
            const int bytes_per_pixel = texture->format == SDL_PIXELFORMAT_RGB565 ? 2 : 4;
            item->fd = -1;
            if (RGA_Allocate(texture->w, texture->h, bytes_per_pixel,
                             &item->fd, &item->pixels, &pitch, &item->bytes) == 0) {
                item->texture = texture;
                item->pitch = pitch;
                item->format = bytes_per_pixel == 2 ? RK_FORMAT_RGB_565 : RK_FORMAT_RGBA_8888;
                item->dirty = SDL_TRUE;
                item->cpu_access = SDL_TRUE;
                item->handle = RGA_Import(pitch, texture->h, item->format, item->fd);
                if (item->handle) render_data->rga_import_count++;
                item->next = render_data->rga_textures;
                render_data->rga_textures = item;
            } else {
                SDL_free(item); /* Resource exhaustion: texture still works through SDL. */
            }
        }
    }
#endif
    return 0;
}

static int SW_UpdateTexture(SDL_Renderer *renderer, SDL_Texture *texture,
                            const SDL_Rect *rect, const void *pixels, int pitch)
{
    SDL_Surface *surface = (SDL_Surface *)texture->driverdata;
    Uint8 *src, *dst;
    int row;
    size_t length;

    if (SDL_MUSTLOCK(surface)) {
        SDL_LockSurface(surface);
    }
    src = (Uint8 *)pixels;
    dst = (Uint8 *)surface->pixels +
          rect->y * surface->pitch +
          rect->x * surface->format->BytesPerPixel;
    length = (size_t)rect->w * surface->format->BytesPerPixel;
    for (row = 0; row < rect->h; ++row) {
        SDL_memcpy(dst, src, length);
        src += pitch;
        dst += surface->pitch;
    }
    if (SDL_MUSTLOCK(surface)) {
        SDL_UnlockSurface(surface);
    }
#if SDL_VIDEO_RENDER_RGA
    {
        SW_RenderData *data = (SW_RenderData *)renderer->driverdata;
        if (data->rga_fd >= 0) {
            RGA_TextureData *item = RGA_FindTexture(data, texture);
            if (item != NULL) {
                item->dirty = SDL_TRUE;
            }
        }
    }
#endif
    return 0;
}

static int SW_LockTexture(SDL_Renderer *renderer, SDL_Texture *texture,
                          const SDL_Rect *rect, void **pixels, int *pitch)
{
    SDL_Surface *surface = (SDL_Surface *)texture->driverdata;

    *pixels =
        (void *)((Uint8 *)surface->pixels + rect->y * surface->pitch +
                 rect->x * surface->format->BytesPerPixel);
    *pitch = surface->pitch;
    return 0;
}

static void SW_UnlockTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
#if SDL_VIDEO_RENDER_RGA
    SW_RenderData *data = (SW_RenderData *)renderer->driverdata;
    if (data->rga_fd >= 0) {
        RGA_TextureData *item = RGA_FindTexture(data, texture);
        if (item != NULL) {
            item->dirty = SDL_TRUE;
        }
    }
#endif
}

static void SW_SetTextureScaleMode(SDL_Renderer *renderer, SDL_Texture *texture, SDL_ScaleMode scaleMode)
{
}

static int SW_SetRenderTarget(SDL_Renderer *renderer, SDL_Texture *texture)
{
    SW_RenderData *data = (SW_RenderData *)renderer->driverdata;

    if (texture) {
        data->surface = (SDL_Surface *)texture->driverdata;
    } else {
        data->surface = data->window;
    }
    return 0;
}

static int SW_QueueSetViewport(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    return 0; /* nothing to do in this backend. */
}

static int SW_QueueDrawPoints(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FPoint *points, int count)
{
    SDL_Point *verts = (SDL_Point *)SDL_AllocateRenderVertices(renderer, count * sizeof(SDL_Point), 0, &cmd->data.draw.first);
    int i;

    if (verts == NULL) {
        return -1;
    }

    cmd->data.draw.count = count;

    for (i = 0; i < count; i++, verts++, points++) {
        verts->x = (int)points->x;
        verts->y = (int)points->y;
    }

    return 0;
}

static int SW_QueueFillRects(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FRect *rects, int count)
{
    SDL_Rect *verts = (SDL_Rect *)SDL_AllocateRenderVertices(renderer, count * sizeof(SDL_Rect), 0, &cmd->data.draw.first);
    int i;

    if (verts == NULL) {
        return -1;
    }

    cmd->data.draw.count = count;

    for (i = 0; i < count; i++, verts++, rects++) {
        verts->x = (int)rects->x;
        verts->y = (int)rects->y;
        verts->w = SDL_max((int)rects->w, 1);
        verts->h = SDL_max((int)rects->h, 1);
    }

    return 0;
}

static int SW_QueueCopy(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture,
                        const SDL_Rect *srcrect, const SDL_FRect *dstrect)
{
    SDL_Rect *verts = (SDL_Rect *)SDL_AllocateRenderVertices(renderer, 2 * sizeof(SDL_Rect), 0, &cmd->data.draw.first);

    if (verts == NULL) {
        return -1;
    }

    cmd->data.draw.count = 1;

    SDL_copyp(verts, srcrect);
    verts++;

    verts->x = (int)dstrect->x;
    verts->y = (int)dstrect->y;
    verts->w = (int)dstrect->w;
    verts->h = (int)dstrect->h;

    return 0;
}

typedef struct CopyExData
{
    SDL_Rect srcrect;
    SDL_Rect dstrect;
    double angle;
    SDL_FPoint center;
    SDL_RendererFlip flip;
    float scale_x;
    float scale_y;
} CopyExData;

static int SW_QueueCopyEx(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture,
                          const SDL_Rect *srcrect, const SDL_FRect *dstrect,
                          const double angle, const SDL_FPoint *center, const SDL_RendererFlip flip, float scale_x, float scale_y)
{
    CopyExData *verts = (CopyExData *)SDL_AllocateRenderVertices(renderer, sizeof(CopyExData), 0, &cmd->data.draw.first);

    if (verts == NULL) {
        return -1;
    }

    cmd->data.draw.count = 1;

    SDL_copyp(&verts->srcrect, srcrect);

    verts->dstrect.x = (int)dstrect->x;
    verts->dstrect.y = (int)dstrect->y;
    verts->dstrect.w = (int)dstrect->w;
    verts->dstrect.h = (int)dstrect->h;
    verts->angle = angle;
    SDL_copyp(&verts->center, center);
    verts->flip = flip;
    verts->scale_x = scale_x;
    verts->scale_y = scale_y;

    return 0;
}

static int Blit_to_Screen(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *surface, SDL_Rect *dstrect,
                          float scale_x, float scale_y, SDL_ScaleMode scaleMode)
{
    int retval;
    /* Renderer scaling, if needed */
    if (scale_x != 1.0f || scale_y != 1.0f) {
        SDL_Rect r;
        r.x = (int)((float)dstrect->x * scale_x);
        r.y = (int)((float)dstrect->y * scale_y);
        r.w = (int)((float)dstrect->w * scale_x);
        r.h = (int)((float)dstrect->h * scale_y);
        retval = SDL_PrivateUpperBlitScaled(src, srcrect, surface, &r, scaleMode);
    } else {
        retval = SDL_BlitSurface(src, srcrect, surface, dstrect);
    }
    return retval;
}

static int SW_RenderCopyEx(SDL_Renderer *renderer, SDL_Surface *surface, SDL_Texture *texture,
                           const SDL_Rect *srcrect, const SDL_Rect *final_rect,
                           const double angle, const SDL_FPoint *center, const SDL_RendererFlip flip, float scale_x, float scale_y)
{
    SDL_Surface *src = (SDL_Surface *)texture->driverdata;
    SDL_Rect tmp_rect;
    SDL_Surface *src_clone, *src_rotated, *src_scaled;
    SDL_Surface *mask = NULL, *mask_rotated = NULL;
    int retval = 0;
    SDL_BlendMode blendmode;
    Uint8 alphaMod, rMod, gMod, bMod;
    int applyModulation = SDL_FALSE;
    int blitRequired = SDL_FALSE;
    int isOpaque = SDL_FALSE;

    if (surface == NULL) {
        return -1;
    }

    tmp_rect.x = 0;
    tmp_rect.y = 0;
    tmp_rect.w = final_rect->w;
    tmp_rect.h = final_rect->h;

    /* It is possible to encounter an RLE encoded surface here and locking it is
     * necessary because this code is going to access the pixel buffer directly.
     */
    if (SDL_MUSTLOCK(src)) {
        SDL_LockSurface(src);
    }

    /* Clone the source surface but use its pixel buffer directly.
     * The original source surface must be treated as read-only.
     */
    src_clone = SDL_CreateRGBSurfaceFrom(src->pixels, src->w, src->h, src->format->BitsPerPixel, src->pitch,
                                         src->format->Rmask, src->format->Gmask,
                                         src->format->Bmask, src->format->Amask);
    if (src_clone == NULL) {
        if (SDL_MUSTLOCK(src)) {
            SDL_UnlockSurface(src);
        }
        return -1;
    }

    SDL_GetSurfaceBlendMode(src, &blendmode);
    SDL_GetSurfaceAlphaMod(src, &alphaMod);
    SDL_GetSurfaceColorMod(src, &rMod, &gMod, &bMod);

    /* SDLgfx_rotateSurface only accepts 32-bit surfaces with a 8888 layout. Everything else has to be converted. */
    if (src->format->BitsPerPixel != 32 || SDL_PIXELLAYOUT(src->format->format) != SDL_PACKEDLAYOUT_8888 || !src->format->Amask) {
        blitRequired = SDL_TRUE;
    }

    /* If scaling and cropping is necessary, it has to be taken care of before the rotation. */
    if (!(srcrect->w == final_rect->w && srcrect->h == final_rect->h && srcrect->x == 0 && srcrect->y == 0)) {
        blitRequired = SDL_TRUE;
    }

    /* srcrect is not selecting the whole src surface, so cropping is needed */
    if (!(srcrect->w == src->w && srcrect->h == src->h && srcrect->x == 0 && srcrect->y == 0)) {
        blitRequired = SDL_TRUE;
    }

    /* The color and alpha modulation has to be applied before the rotation when using the NONE, MOD or MUL blend modes. */
    if ((blendmode == SDL_BLENDMODE_NONE || blendmode == SDL_BLENDMODE_MOD || blendmode == SDL_BLENDMODE_MUL) && (alphaMod & rMod & gMod & bMod) != 255) {
        applyModulation = SDL_TRUE;
        SDL_SetSurfaceAlphaMod(src_clone, alphaMod);
        SDL_SetSurfaceColorMod(src_clone, rMod, gMod, bMod);
    }

    /* Opaque surfaces are much easier to handle with the NONE blend mode. */
    if (blendmode == SDL_BLENDMODE_NONE && !src->format->Amask && alphaMod == 255) {
        isOpaque = SDL_TRUE;
    }

    /* The NONE blend mode requires a mask for non-opaque surfaces. This mask will be used
     * to clear the pixels in the destination surface. The other steps are explained below.
     */
    if (blendmode == SDL_BLENDMODE_NONE && !isOpaque) {
        mask = SDL_CreateRGBSurface(0, final_rect->w, final_rect->h, 32,
                                    0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
        if (mask == NULL) {
            retval = -1;
        } else {
            SDL_SetSurfaceBlendMode(mask, SDL_BLENDMODE_MOD);
        }
    }

    /* Create a new surface should there be a format mismatch or if scaling, cropping,
     * or modulation is required. It's possible to use the source surface directly otherwise.
     */
    if (!retval && (blitRequired || applyModulation)) {
        SDL_Rect scale_rect = tmp_rect;
        src_scaled = SDL_CreateRGBSurface(0, final_rect->w, final_rect->h, 32,
                                          0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
        if (src_scaled == NULL) {
            retval = -1;
        } else {
            SDL_SetSurfaceBlendMode(src_clone, SDL_BLENDMODE_NONE);
            retval = SDL_PrivateUpperBlitScaled(src_clone, srcrect, src_scaled, &scale_rect, texture->scaleMode);
            SDL_FreeSurface(src_clone);
            src_clone = src_scaled;
            src_scaled = NULL;
        }
    }

    /* SDLgfx_rotateSurface is going to make decisions depending on the blend mode. */
    SDL_SetSurfaceBlendMode(src_clone, blendmode);

    if (!retval) {
        SDL_Rect rect_dest;
        double cangle, sangle;

        SDLgfx_rotozoomSurfaceSizeTrig(tmp_rect.w, tmp_rect.h, angle, center,
                                       &rect_dest, &cangle, &sangle);
        src_rotated = SDLgfx_rotateSurface(src_clone, angle,
                                           (texture->scaleMode == SDL_ScaleModeNearest) ? 0 : 1, flip & SDL_FLIP_HORIZONTAL, flip & SDL_FLIP_VERTICAL,
                                           &rect_dest, cangle, sangle, center);
        if (src_rotated == NULL) {
            retval = -1;
        }
        if (!retval && mask != NULL) {
            /* The mask needed for the NONE blend mode gets rotated with the same parameters. */
            mask_rotated = SDLgfx_rotateSurface(mask, angle,
                                                SDL_FALSE, 0, 0,
                                                &rect_dest, cangle, sangle, center);
            if (mask_rotated == NULL) {
                retval = -1;
            }
        }
        if (!retval) {

            tmp_rect.x = final_rect->x + rect_dest.x;
            tmp_rect.y = final_rect->y + rect_dest.y;
            tmp_rect.w = rect_dest.w;
            tmp_rect.h = rect_dest.h;

            /* The NONE blend mode needs some special care with non-opaque surfaces.
             * Other blend modes or opaque surfaces can be blitted directly.
             */
            if (blendmode != SDL_BLENDMODE_NONE || isOpaque) {
                if (applyModulation == SDL_FALSE) {
                    /* If the modulation wasn't already applied, make it happen now. */
                    SDL_SetSurfaceAlphaMod(src_rotated, alphaMod);
                    SDL_SetSurfaceColorMod(src_rotated, rMod, gMod, bMod);
                }
                /* Renderer scaling, if needed */
                retval = Blit_to_Screen(src_rotated, NULL, surface, &tmp_rect, scale_x, scale_y, texture->scaleMode);
            } else {
                /* The NONE blend mode requires three steps to get the pixels onto the destination surface.
                 * First, the area where the rotated pixels will be blitted to get set to zero.
                 * This is accomplished by simply blitting a mask with the NONE blend mode.
                 * The colorkey set by the rotate function will discard the correct pixels.
                 */
                SDL_Rect mask_rect = tmp_rect;
                SDL_SetSurfaceBlendMode(mask_rotated, SDL_BLENDMODE_NONE);
                /* Renderer scaling, if needed */
                retval = Blit_to_Screen(mask_rotated, NULL, surface, &mask_rect, scale_x, scale_y, texture->scaleMode);
                if (!retval) {
                    /* The next step copies the alpha value. This is done with the BLEND blend mode and
                     * by modulating the source colors with 0. Since the destination is all zeros, this
                     * will effectively set the destination alpha to the source alpha.
                     */
                    SDL_SetSurfaceColorMod(src_rotated, 0, 0, 0);
                    mask_rect = tmp_rect;
                    /* Renderer scaling, if needed */
                    retval = Blit_to_Screen(src_rotated, NULL, surface, &mask_rect, scale_x, scale_y, texture->scaleMode);
                    if (!retval) {
                        /* The last step gets the color values in place. The ADD blend mode simply adds them to
                         * the destination (where the color values are all zero). However, because the ADD blend
                         * mode modulates the colors with the alpha channel, a surface without an alpha mask needs
                         * to be created. This makes all source pixels opaque and the colors get copied correctly.
                         */
                        SDL_Surface *src_rotated_rgb;
                        src_rotated_rgb = SDL_CreateRGBSurfaceFrom(src_rotated->pixels, src_rotated->w, src_rotated->h,
                                                                   src_rotated->format->BitsPerPixel, src_rotated->pitch,
                                                                   src_rotated->format->Rmask, src_rotated->format->Gmask,
                                                                   src_rotated->format->Bmask, 0);
                        if (src_rotated_rgb == NULL) {
                            retval = -1;
                        } else {
                            SDL_SetSurfaceBlendMode(src_rotated_rgb, SDL_BLENDMODE_ADD);
                            /* Renderer scaling, if needed */
                            retval = Blit_to_Screen(src_rotated_rgb, NULL, surface, &tmp_rect, scale_x, scale_y, texture->scaleMode);
                            SDL_FreeSurface(src_rotated_rgb);
                        }
                    }
                }
                SDL_FreeSurface(mask_rotated);
            }
            if (src_rotated != NULL) {
                SDL_FreeSurface(src_rotated);
            }
        }
    }

    if (SDL_MUSTLOCK(src)) {
        SDL_UnlockSurface(src);
    }
    if (mask != NULL) {
        SDL_FreeSurface(mask);
    }
    if (src_clone != NULL) {
        SDL_FreeSurface(src_clone);
    }
    return retval;
}

typedef struct GeometryFillData
{
    SDL_Point dst;
    SDL_Color color;
} GeometryFillData;

typedef struct GeometryCopyData
{
    SDL_Point src;
    SDL_Point dst;
    SDL_Color color;
} GeometryCopyData;

static int SW_QueueGeometry(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture,
                            const float *xy, int xy_stride, const SDL_Color *color, int color_stride, const float *uv, int uv_stride,
                            int num_vertices, const void *indices, int num_indices, int size_indices,
                            float scale_x, float scale_y)
{
    int i;
    int count = indices ? num_indices : num_vertices;
    void *verts;
    size_t sz = texture != NULL ? sizeof(GeometryCopyData) : sizeof(GeometryFillData);

    verts = SDL_AllocateRenderVertices(renderer, count * sz, 0, &cmd->data.draw.first);
    if (verts == NULL) {
        return -1;
    }

    cmd->data.draw.count = count;
    size_indices = indices ? size_indices : 0;

    if (texture) {
        GeometryCopyData *ptr = (GeometryCopyData *)verts;
        for (i = 0; i < count; i++) {
            int j;
            float *xy_;
            SDL_Color col_;
            float *uv_;
            if (size_indices == 4) {
                j = ((const Uint32 *)indices)[i];
            } else if (size_indices == 2) {
                j = ((const Uint16 *)indices)[i];
            } else if (size_indices == 1) {
                j = ((const Uint8 *)indices)[i];
            } else {
                j = i;
            }

            xy_ = (float *)((char *)xy + j * xy_stride);
            col_ = *(SDL_Color *)((char *)color + j * color_stride);

            uv_ = (float *)((char *)uv + j * uv_stride);

            ptr->src.x = (int)(uv_[0] * texture->w);
            ptr->src.y = (int)(uv_[1] * texture->h);

            ptr->dst.x = (int)(xy_[0] * scale_x);
            ptr->dst.y = (int)(xy_[1] * scale_y);
            trianglepoint_2_fixedpoint(&ptr->dst);

            ptr->color = col_;

            ptr++;
        }
    } else {
        GeometryFillData *ptr = (GeometryFillData *)verts;

        for (i = 0; i < count; i++) {
            int j;
            float *xy_;
            SDL_Color col_;
            if (size_indices == 4) {
                j = ((const Uint32 *)indices)[i];
            } else if (size_indices == 2) {
                j = ((const Uint16 *)indices)[i];
            } else if (size_indices == 1) {
                j = ((const Uint8 *)indices)[i];
            } else {
                j = i;
            }

            xy_ = (float *)((char *)xy + j * xy_stride);
            col_ = *(SDL_Color *)((char *)color + j * color_stride);

            ptr->dst.x = (int)(xy_[0] * scale_x);
            ptr->dst.y = (int)(xy_[1] * scale_y);
            trianglepoint_2_fixedpoint(&ptr->dst);

            ptr->color = col_;

            ptr++;
        }
    }
    return 0;
}

static void PrepTextureForCopy(const SDL_RenderCommand *cmd)
{
    const Uint8 r = cmd->data.draw.r;
    const Uint8 g = cmd->data.draw.g;
    const Uint8 b = cmd->data.draw.b;
    const Uint8 a = cmd->data.draw.a;
    const SDL_BlendMode blend = cmd->data.draw.blend;
    SDL_Texture *texture = cmd->data.draw.texture;
    SDL_Surface *surface = (SDL_Surface *)texture->driverdata;
    const SDL_bool colormod = ((r & g & b) != 0xFF);
    const SDL_bool alphamod = (a != 0xFF);
    const SDL_bool blending = ((blend == SDL_BLENDMODE_ADD) || (blend == SDL_BLENDMODE_MOD) || (blend == SDL_BLENDMODE_MUL));

    if (colormod || alphamod || blending) {
        SDL_SetSurfaceRLE(surface, 0);
    }

    /* !!! FIXME: we can probably avoid some of these calls. */
    SDL_SetSurfaceColorMod(surface, r, g, b);
    SDL_SetSurfaceAlphaMod(surface, a);
    SDL_SetSurfaceBlendMode(surface, blend);
}

static void SetDrawState(SDL_Surface *surface, SW_DrawStateCache *drawstate)
{
    if (drawstate->surface_cliprect_dirty) {
        const SDL_Rect *viewport = drawstate->viewport;
        const SDL_Rect *cliprect = drawstate->cliprect;
        SDL_assert_release(viewport != NULL); /* the higher level should have forced a SDL_RENDERCMD_SETVIEWPORT */

        if (cliprect != NULL) {
            SDL_Rect clip_rect;
            clip_rect.x = cliprect->x + viewport->x;
            clip_rect.y = cliprect->y + viewport->y;
            clip_rect.w = cliprect->w;
            clip_rect.h = cliprect->h;
            SDL_IntersectRect(viewport, &clip_rect, &clip_rect);
            SDL_SetClipRect(surface, &clip_rect);
        } else {
            SDL_SetClipRect(surface, drawstate->viewport);
        }
        drawstate->surface_cliprect_dirty = SDL_FALSE;
    }
}

static int SW_RunCommandQueue(SDL_Renderer *renderer, SDL_RenderCommand *cmd, void *vertices, size_t vertsize)
{
    SDL_Surface *surface = SW_ActivateRenderer(renderer);
    SW_DrawStateCache drawstate;
#if SDL_VIDEO_RENDER_RGA
    SW_RenderData *rga_data = (SW_RenderData *)renderer->driverdata;
    if (rga_data->rga_fd >= 0 && renderer->target != NULL) {
        RGA_TextureData *target_item = RGA_FindTexture(rga_data, renderer->target);
        if (target_item != NULL) {
            target_item->dirty = SDL_TRUE;
        }
    }
#endif

    if (surface == NULL) {
        return -1;
    }

    drawstate.viewport = NULL;
    drawstate.cliprect = NULL;
    drawstate.surface_cliprect_dirty = SDL_TRUE;

    while (cmd) {
        switch (cmd->command) {
            case SDL_RENDERCMD_SETDRAWCOLOR: {
                break;  /* Not used in this backend. */
            }

            case SDL_RENDERCMD_SETVIEWPORT: {
                drawstate.viewport = &cmd->data.viewport.rect;
                drawstate.surface_cliprect_dirty = SDL_TRUE;
                break;
            }

            case SDL_RENDERCMD_SETCLIPRECT: {
                drawstate.cliprect = cmd->data.cliprect.enabled ? &cmd->data.cliprect.rect : NULL;
                drawstate.surface_cliprect_dirty = SDL_TRUE;
                break;
            }

            case SDL_RENDERCMD_CLEAR: {
                const Uint8 r = cmd->data.color.r;
                const Uint8 g = cmd->data.color.g;
                const Uint8 b = cmd->data.color.b;
                const Uint8 a = cmd->data.color.a;
                /* By definition the clear ignores the clip rect */
                SDL_SetClipRect(surface, NULL);
#if SDL_VIDEO_RENDER_RGA
                if (rga_data->rga_fd >= 0 && surface == rga_data->window) {
                    SDL_Rect full = { 0, 0, surface->w, surface->h };
                    const int accelerated = RGA_Fill(rga_data, &full, r, g, b);
                    if (accelerated < 0) {
                        return -1;
                    }
                    if (accelerated > 0) {
                        rga_data->rga_fill_count++;
                        drawstate.surface_cliprect_dirty = SDL_TRUE;
                        break;
                    }
                }
#endif
                if (SW_CPUAccess(renderer) < 0) return -1;
                SDL_FillRect(surface, NULL, SDL_MapRGBA(surface->format, r, g, b, a));
                drawstate.surface_cliprect_dirty = SDL_TRUE;
                break;
            }

            case SDL_RENDERCMD_DRAW_POINTS: {
                const Uint8 r = cmd->data.draw.r;
                const Uint8 g = cmd->data.draw.g;
                const Uint8 b = cmd->data.draw.b;
                const Uint8 a = cmd->data.draw.a;
                const int count = (int) cmd->data.draw.count;
                SDL_Point *verts = (SDL_Point *) (((Uint8 *) vertices) + cmd->data.draw.first);
                const SDL_BlendMode blend = cmd->data.draw.blend;
                SetDrawState(surface, &drawstate);
                if (SW_CPUAccess(renderer) < 0) return -1;

                /* Apply viewport */
                if (drawstate.viewport != NULL && (drawstate.viewport->x || drawstate.viewport->y)) {
                    int i;
                    for (i = 0; i < count; i++) {
                        verts[i].x += drawstate.viewport->x;
                        verts[i].y += drawstate.viewport->y;
                    }
                }

                if (blend == SDL_BLENDMODE_NONE) {
                    SDL_DrawPoints(surface, verts, count, SDL_MapRGBA(surface->format, r, g, b, a));
                } else {
                    SDL_BlendPoints(surface, verts, count, blend, r, g, b, a);
                }
                break;
            }

            case SDL_RENDERCMD_DRAW_LINES: {
                const Uint8 r = cmd->data.draw.r;
                const Uint8 g = cmd->data.draw.g;
                const Uint8 b = cmd->data.draw.b;
                const Uint8 a = cmd->data.draw.a;
                const int count = (int) cmd->data.draw.count;
                SDL_Point *verts = (SDL_Point *) (((Uint8 *) vertices) + cmd->data.draw.first);
                const SDL_BlendMode blend = cmd->data.draw.blend;
                SetDrawState(surface, &drawstate);
                if (SW_CPUAccess(renderer) < 0) return -1;

                /* Apply viewport */
                if (drawstate.viewport != NULL && (drawstate.viewport->x || drawstate.viewport->y)) {
                    int i;
                    for (i = 0; i < count; i++) {
                        verts[i].x += drawstate.viewport->x;
                        verts[i].y += drawstate.viewport->y;
                    }
                }

                if (blend == SDL_BLENDMODE_NONE) {
                    SDL_DrawLines(surface, verts, count, SDL_MapRGBA(surface->format, r, g, b, a));
                } else {
                    SDL_BlendLines(surface, verts, count, blend, r, g, b, a);
                }
                break;
            }

            case SDL_RENDERCMD_FILL_RECTS: {
                const Uint8 r = cmd->data.draw.r;
                const Uint8 g = cmd->data.draw.g;
                const Uint8 b = cmd->data.draw.b;
                const Uint8 a = cmd->data.draw.a;
                const int count = (int) cmd->data.draw.count;
                SDL_Rect *verts = (SDL_Rect *) (((Uint8 *) vertices) + cmd->data.draw.first);
                const SDL_BlendMode blend = cmd->data.draw.blend;
                SetDrawState(surface, &drawstate);

                /* Apply viewport */
                if (drawstate.viewport != NULL && (drawstate.viewport->x || drawstate.viewport->y)) {
                    int i;
                    for (i = 0; i < count; i++) {
                        verts[i].x += drawstate.viewport->x;
                        verts[i].y += drawstate.viewport->y;
                    }
                }

#if SDL_VIDEO_RENDER_RGA
                if (rga_data->rga_fd >= 0 && surface == rga_data->window &&
                    (blend == SDL_BLENDMODE_NONE || blend == SDL_BLENDMODE_BLEND)) {
                    int i;
                    for (i = 0; i < count; ++i) {
                        SDL_Rect clip, clipped;
                        SDL_GetClipRect(surface, &clip);
                        if (!SDL_IntersectRect(&verts[i], &clip, &clipped)) {
                            continue;
                        }
                        {
                            const int accelerated = (blend == SDL_BLENDMODE_BLEND && a < 255) ?
                                RGA_AlphaFill(rga_data, &clipped, r, g, b, a) :
                                RGA_Fill(rga_data, &clipped, r, g, b);
                            if (accelerated < 0) {
                                return -1;
                            }
                            if (accelerated > 0) {
                                if (blend == SDL_BLENDMODE_BLEND && a < 255) {
                                    rga_data->rga_alpha_count++;
                                } else {
                                    rga_data->rga_fill_count++;
                                }
                                continue;
                            }
                        }
                        if (SW_CPUAccess(renderer) < 0) return -1;
                        if (blend == SDL_BLENDMODE_NONE) {
                            SDL_FillRect(surface, &verts[i], SDL_MapRGBA(surface->format, r, g, b, a));
                        } else {
                            SDL_BlendFillRects(surface, &verts[i], 1, blend, r, g, b, a);
                        }
                    }
                    break;
                }
#endif
                if (SW_CPUAccess(renderer) < 0) return -1;
                if (blend == SDL_BLENDMODE_NONE) {
                    SDL_FillRects(surface, verts, count, SDL_MapRGBA(surface->format, r, g, b, a));
                } else {
                    SDL_BlendFillRects(surface, verts, count, blend, r, g, b, a);
                }
                break;
            }

            case SDL_RENDERCMD_COPY: {
                SDL_Rect *verts = (SDL_Rect *) (((Uint8 *) vertices) + cmd->data.draw.first);
                const SDL_Rect *srcrect = verts;
                SDL_Rect *dstrect = verts + 1;
                SDL_Texture *texture = cmd->data.draw.texture;
                SDL_Surface *src = (SDL_Surface *) texture->driverdata;

                SetDrawState(surface, &drawstate);

#if SDL_VIDEO_RENDER_RGA
                if (rga_data->rga_fd >= 0 && surface == rga_data->window) {
                    SDL_Rect output_clip;
                    SDL_Rect candidate = *dstrect;
                    int accelerated;
                    if (drawstate.viewport != NULL) {
                        candidate.x += drawstate.viewport->x;
                        candidate.y += drawstate.viewport->y;
                    }
                    SDL_GetClipRect(surface, &output_clip);
                    accelerated = RGA_Copy(rga_data, texture, srcrect, &candidate,
                                           texture->scaleMode, &output_clip,
                                           cmd->data.draw.r, cmd->data.draw.g,
                                           cmd->data.draw.b, cmd->data.draw.a, cmd->data.draw.blend, 0);
                    if (accelerated < 0) {
                        return -1;
                    }
                    if (accelerated > 0) {
                        if (accelerated == 1) rga_data->rga_copy_count++;
                        break;
                    }
                }
#endif
                if (SW_CPUAccess(renderer) < 0) return -1;
                PrepTextureForCopy(cmd);

                /* Apply viewport */
                if (drawstate.viewport != NULL && (drawstate.viewport->x || drawstate.viewport->y)) {
                    dstrect->x += drawstate.viewport->x;
                    dstrect->y += drawstate.viewport->y;
                }

                if ( srcrect->w == dstrect->w && srcrect->h == dstrect->h ) {
                    SDL_BlitSurface(src, srcrect, surface, dstrect);
                } else {
                    /* If scaling is ever done, permanently disable RLE (which doesn't support scaling)
                     * to avoid potentially frequent RLE encoding/decoding.
                     */
                    SDL_SetSurfaceRLE(surface, 0);

                    /* Prevent to do scaling + clipping on viewport boundaries as it may lose proportion */
                    if (dstrect->x < 0 || dstrect->y < 0 || dstrect->x + dstrect->w > surface->w || dstrect->y + dstrect->h > surface->h) {
                        SDL_Surface *tmp = SDL_CreateRGBSurfaceWithFormat(0, dstrect->w, dstrect->h, 0, src->format->format);
                        /* Scale to an intermediate surface, then blit */
                        if (tmp) {
                            SDL_Rect r;
                            SDL_BlendMode blendmode;
                            Uint8 alphaMod, rMod, gMod, bMod;

                            SDL_GetSurfaceBlendMode(src, &blendmode);
                            SDL_GetSurfaceAlphaMod(src, &alphaMod);
                            SDL_GetSurfaceColorMod(src, &rMod, &gMod, &bMod);

                            r.x = 0;
                            r.y = 0;
                            r.w = dstrect->w;
                            r.h = dstrect->h;

                            SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
                            SDL_SetSurfaceColorMod(src, 255, 255, 255);
                            SDL_SetSurfaceAlphaMod(src, 255);

                            SDL_PrivateUpperBlitScaled(src, srcrect, tmp, &r, texture->scaleMode);

                            SDL_SetSurfaceColorMod(tmp, rMod, gMod, bMod);
                            SDL_SetSurfaceAlphaMod(tmp, alphaMod);
                            SDL_SetSurfaceBlendMode(tmp, blendmode);

                            SDL_BlitSurface(tmp, NULL, surface, dstrect);
                            SDL_FreeSurface(tmp);
                            /* No need to set back r/g/b/a/blendmode to 'src' since it's done in PrepTextureForCopy() */
                        }
                    } else{
                        SDL_PrivateUpperBlitScaled(src, srcrect, surface, dstrect, texture->scaleMode);
                    }
                }
                break;
            }

            case SDL_RENDERCMD_COPY_EX: {
                CopyExData *copydata = (CopyExData *) (((Uint8 *) vertices) + cmd->data.draw.first);
                SetDrawState(surface, &drawstate);
#if SDL_VIDEO_RENDER_RGA
                if (rga_data->rga_fd >= 0 && surface == rga_data->window &&
                    copydata->scale_x == 1.0f && copydata->scale_y == 1.0f &&
                    copydata->srcrect.w == copydata->dstrect.w &&
                    copydata->srcrect.h == copydata->dstrect.h &&
                    copydata->center.x == (float)copydata->dstrect.w / 2.0f &&
                    copydata->center.y == (float)copydata->dstrect.h / 2.0f) {
                    int transform = -1;
                    SDL_Rect output_clip, candidate = copydata->dstrect;
                    if (copydata->angle == 0.0) transform = 0;
                    if (copydata->angle == 90.0 && candidate.w == candidate.h) transform = IM_HAL_TRANSFORM_ROT_90;
                    if (copydata->angle == 180.0) transform = IM_HAL_TRANSFORM_ROT_180;
                    if (copydata->angle == 270.0 && candidate.w == candidate.h) transform = IM_HAL_TRANSFORM_ROT_270;
                    if (copydata->flip != SDL_FLIP_NONE) {
                        transform = -1;
                        if (copydata->angle == 0.0) {
                            switch ((int)copydata->flip) {
                            case SDL_FLIP_HORIZONTAL: transform = IM_HAL_TRANSFORM_FLIP_H; break;
                            case SDL_FLIP_VERTICAL: transform = IM_HAL_TRANSFORM_FLIP_V; break;
                            case (SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL):
                                transform = IM_HAL_TRANSFORM_FLIP_H_V;
                                break;
                            default: break;
                            }
                        }
                    }
                    if (transform >= 0) {
                        int accelerated;
                        if (drawstate.viewport != NULL) {
                            candidate.x += drawstate.viewport->x;
                            candidate.y += drawstate.viewport->y;
                        }
                        SDL_GetClipRect(surface, &output_clip);
                        accelerated = RGA_Copy(rga_data, cmd->data.draw.texture,
                                               &copydata->srcrect, &candidate,
                                               cmd->data.draw.texture->scaleMode,
                                               &output_clip, cmd->data.draw.r,
                                               cmd->data.draw.g, cmd->data.draw.b,
                                               cmd->data.draw.a, cmd->data.draw.blend, transform);
                        if (accelerated < 0) return -1;
                        if (accelerated > 0) {
                            if (transform == 0 && accelerated == 1) rga_data->rga_copy_count++;
                            else if (transform != 0) rga_data->rga_rotate_count++;
                            break;
                        }
                    }
                }
#endif
                if (SW_CPUAccess(renderer) < 0) return -1;
                PrepTextureForCopy(cmd);

                /* Apply viewport */
                if (drawstate.viewport != NULL && (drawstate.viewport->x || drawstate.viewport->y)) {
                    copydata->dstrect.x += drawstate.viewport->x;
                    copydata->dstrect.y += drawstate.viewport->y;
                }

                SW_RenderCopyEx(renderer, surface, cmd->data.draw.texture, &copydata->srcrect,
                                &copydata->dstrect, copydata->angle, &copydata->center, copydata->flip,
                                copydata->scale_x, copydata->scale_y);
                break;
            }

            case SDL_RENDERCMD_GEOMETRY: {
                int i;
                SDL_Rect *verts = (SDL_Rect *) (((Uint8 *) vertices) + cmd->data.draw.first);
                const int count = (int) cmd->data.draw.count;
                SDL_Texture *texture = cmd->data.draw.texture;
                const SDL_BlendMode blend = cmd->data.draw.blend;

                SetDrawState(surface, &drawstate);
                if (SW_CPUAccess(renderer) < 0) return -1;

                if (texture) {
                    SDL_Surface *src = (SDL_Surface *) texture->driverdata;

                    GeometryCopyData *ptr = (GeometryCopyData *) verts;

                    PrepTextureForCopy(cmd);

                    /* Apply viewport */
                    if (drawstate.viewport != NULL && (drawstate.viewport->x || drawstate.viewport->y)) {
                        SDL_Point vp;
                        vp.x = drawstate.viewport->x;
                        vp.y = drawstate.viewport->y;
                        trianglepoint_2_fixedpoint(&vp);
                        for (i = 0; i < count; i++) {
                            ptr[i].dst.x += vp.x;
                            ptr[i].dst.y += vp.y;
                        }
                    }

                    for (i = 0; i < count; i += 3, ptr += 3) {
                        SDL_SW_BlitTriangle(
                                src,
                                &(ptr[0].src), &(ptr[1].src), &(ptr[2].src),
                                surface,
                                &(ptr[0].dst), &(ptr[1].dst), &(ptr[2].dst),
                                ptr[0].color, ptr[1].color, ptr[2].color);
                    }
                } else {
                    GeometryFillData *ptr = (GeometryFillData *) verts;

                    /* Apply viewport */
                    if (drawstate.viewport != NULL && (drawstate.viewport->x || drawstate.viewport->y)) {
                        SDL_Point vp;
                        vp.x = drawstate.viewport->x;
                        vp.y = drawstate.viewport->y;
                        trianglepoint_2_fixedpoint(&vp);
                        for (i = 0; i < count; i++) {
                            ptr[i].dst.x += vp.x;
                            ptr[i].dst.y += vp.y;
                        }
                    }

                    for (i = 0; i < count; i += 3, ptr += 3) {
                        SDL_SW_FillTriangle(surface, &(ptr[0].dst), &(ptr[1].dst), &(ptr[2].dst), blend, ptr[0].color, ptr[1].color, ptr[2].color);
                    }
                }
                break;
            }

            case SDL_RENDERCMD_NO_OP:
                break;
        }

        cmd = cmd->next;
    }

    return 0;
}

static int SW_RenderReadPixels(SDL_Renderer *renderer, const SDL_Rect *rect,
                               Uint32 format, void *pixels, int pitch)
{
    SDL_Surface *surface = SW_ActivateRenderer(renderer);
    Uint32 src_format;
    void *src_pixels;

    if (surface == NULL) {
        return -1;
    }
    if (SW_CPUAccess(renderer) < 0) return -1;

    /* NOTE: The rect is already adjusted according to the viewport by
     * SDL_RenderReadPixels.
     */

    if (rect->x < 0 || rect->x + rect->w > surface->w ||
        rect->y < 0 || rect->y + rect->h > surface->h) {
        return SDL_SetError("Tried to read outside of surface bounds");
    }

    src_format = surface->format->format;
    src_pixels = (void *)((Uint8 *)surface->pixels +
                          rect->y * surface->pitch +
                          rect->x * surface->format->BytesPerPixel);

    return SDL_ConvertPixels(rect->w, rect->h,
                             src_format, src_pixels, surface->pitch,
                             format, pixels, pitch);
}

static int SW_RenderPresent(SDL_Renderer *renderer)
{
    SDL_Window *window = renderer->window;

    if (window == NULL) {
        return -1;
    }
#if SDL_VIDEO_RENDER_RGA
    {
        SW_RenderData *data = (SW_RenderData *)renderer->driverdata;
        if (data->rga_fd >= 0) {
            SDL_Surface *output;
            if (RGA_CPU(data) < 0) return -1;
#if SDL_VIDEO_DRIVER_DIRECTFB
            if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "directfb") == 0) {
                DFB_WindowData *windata = (DFB_WindowData *)window->driverdata;
                DFBSurfacePixelFormat format;
                void *destination;
                int pitch, row, width, height;
                if (windata == NULL || windata->surface == NULL || windata->window_surface == NULL ||
                    windata->surface->GetPixelFormat(windata->surface, &format) != DFB_OK ||
                    format != DSPF_RGB16 ||
                    windata->surface->GetSize(windata->surface, &width, &height) != DFB_OK ||
                    width != data->window->w || height != data->window->h) {
                    return SDL_SetError("RGA: DirectFB RGB565 drawing surface unavailable");
                }
                if (windata->surface->Lock(windata->surface, DSLF_WRITE, &destination, &pitch) != DFB_OK) {
                    return SDL_SetError("RGA: DirectFB lock failed");
                }
                if (pitch < data->window->w * 2) {
                    windata->surface->Unlock(windata->surface);
                    return SDL_SetError("RGA: DirectFB pitch is too small");
                }
                for (row = 0; row < height; ++row) {
                    SDL_memcpy((Uint8 *)destination + row * pitch,
                               (const Uint8 *)data->window->pixels + row * data->window->pitch,
                               (size_t)width * 2u);
                }
                if (windata->surface->Unlock(windata->surface) != DFB_OK ||
                    windata->window_surface->Flip(windata->window_surface, NULL,
                                                  DSFLIP_BLIT | DSFLIP_ONSYNC) != DFB_OK) {
                    return SDL_SetError("RGA: DirectFB present failed");
                }
                return 0;
            }
#endif
            output = SDL_GetWindowSurface(window);
            if (output == NULL || output->w != data->window->w || output->h != data->window->h) {
                return SDL_SetError("RGA: window surface unavailable or resized");
            }
            if (SDL_BlitSurface(data->window, NULL, output, NULL) != 0) {
                return -1;
            }
        }
    }
#endif
    return SDL_UpdateWindowSurface(window);
}

static void SW_DestroyTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    SDL_Surface *surface = (SDL_Surface *)texture->driverdata;

#if SDL_VIDEO_RENDER_RGA
    SW_RenderData *data = (SW_RenderData *)renderer->driverdata;
    if (data->rga_fd >= 0) {
        RGA_TextureData **link = &data->rga_textures;
        while (*link != NULL) {
            if ((*link)->texture == texture) {
                RGA_TextureData *item = *link;
                *link = item->next;
                RGA_ReleaseTexture(data, item);
                break;
            }
            link = &(*link)->next;
        }
    }
#endif
    SDL_FreeSurface(surface);
}

static void SW_DestroyRenderer(SDL_Renderer *renderer)
{
    SW_RenderData *data = (SW_RenderData *)renderer->driverdata;

#if SDL_VIDEO_RENDER_RGA
    if (data != NULL && data->rga_fd >= 0) {
        RGA_TextureData *item = data->rga_textures;
        if (SDL_GetHintBoolean("SDL_RGA_STATS", SDL_FALSE)) {
            SDL_Log("RGA: fill=%llu alpha=%llu copy=%llu rotate=%llu cache=%llu imports=%llu",
                    (unsigned long long)data->rga_fill_count,
                    (unsigned long long)data->rga_alpha_count,
                    (unsigned long long)data->rga_copy_count,
                    (unsigned long long)data->rga_rotate_count,
                    (unsigned long long)data->rga_cache_count,
                    (unsigned long long)data->rga_import_count);
        }
        while (item != NULL) {
            RGA_TextureData *next = item->next;
            RGA_ReleaseTexture(data, item);
            item = next;
        }
        SDL_FreeSurface(data->window);
        if (data->rga_handle) releasebuffer_handle(data->rga_handle);
        munmap(data->rga_pixels, data->rga_bytes);
        close(data->rga_fd);
        if (data->fill_fd >= 0) {
            if (data->fill_handle) releasebuffer_handle(data->fill_handle);
            munmap(data->fill_pixels, data->fill_bytes);
            close(data->fill_fd);
        }
    }
#endif
    SDL_free(data);
    SDL_free(renderer);
}

SDL_Renderer *SW_CreateRendererForSurface(SDL_Surface *surface)
{
    SDL_Renderer *renderer;
    SW_RenderData *data;

    if (surface == NULL) {
        SDL_InvalidParamError("surface");
        return NULL;
    }

    renderer = (SDL_Renderer *)SDL_calloc(1, sizeof(*renderer));
    if (renderer == NULL) {
        SDL_OutOfMemory();
        return NULL;
    }

    data = (SW_RenderData *)SDL_calloc(1, sizeof(*data));
    if (data == NULL) {
        SW_DestroyRenderer(renderer);
        SDL_OutOfMemory();
        return NULL;
    }
    data->surface = surface;
    data->window = surface;
#if SDL_VIDEO_RENDER_RGA
    data->rga_fd = -1;
    data->fill_fd = -1;
#endif

    renderer->WindowEvent = SW_WindowEvent;
    renderer->GetOutputSize = SW_GetOutputSize;
    renderer->CreateTexture = SW_CreateTexture;
    renderer->UpdateTexture = SW_UpdateTexture;
    renderer->LockTexture = SW_LockTexture;
    renderer->UnlockTexture = SW_UnlockTexture;
    renderer->SetTextureScaleMode = SW_SetTextureScaleMode;
    renderer->SetRenderTarget = SW_SetRenderTarget;
    renderer->QueueSetViewport = SW_QueueSetViewport;
    renderer->QueueSetDrawColor = SW_QueueSetViewport; /* SetViewport and SetDrawColor are (currently) no-ops. */
    renderer->QueueDrawPoints = SW_QueueDrawPoints;
    renderer->QueueDrawLines = SW_QueueDrawPoints; /* lines and points queue vertices the same way. */
    renderer->QueueFillRects = SW_QueueFillRects;
    renderer->QueueCopy = SW_QueueCopy;
    renderer->QueueCopyEx = SW_QueueCopyEx;
    renderer->QueueGeometry = SW_QueueGeometry;
    renderer->RunCommandQueue = SW_RunCommandQueue;
    renderer->RenderReadPixels = SW_RenderReadPixels;
    renderer->RenderPresent = SW_RenderPresent;
    renderer->DestroyTexture = SW_DestroyTexture;
    renderer->DestroyRenderer = SW_DestroyRenderer;
    renderer->info = SW_RenderDriver.info;
    renderer->driverdata = data;

    SW_ActivateRenderer(renderer);

    return renderer;
}

static SDL_Renderer *SW_CreateRenderer(SDL_Window *window, Uint32 flags)
{
    const char *hint;
    SDL_Surface *surface;
    SDL_bool no_hint_set;

    /* Set the vsync hint based on our flags, if it's not already set */
    hint = SDL_GetHint(SDL_HINT_RENDER_VSYNC);
    if (hint == NULL || !*hint) {
        no_hint_set = SDL_TRUE;
    } else {
        no_hint_set = SDL_FALSE;
    }

    if (no_hint_set) {
        SDL_SetHint(SDL_HINT_RENDER_VSYNC, (flags & SDL_RENDERER_PRESENTVSYNC) ? "1" : "0");
    }

    surface = SDL_GetWindowSurface(window);

    /* Reset the vsync hint if we set it above */
    if (no_hint_set) {
        SDL_SetHint(SDL_HINT_RENDER_VSYNC, "");
    }

    if (surface == NULL) {
        return NULL;
    }
    return SW_CreateRendererForSurface(surface);
}

SDL_RenderDriver SW_RenderDriver = {
    SW_CreateRenderer,
    {
     "software",
     SDL_RENDERER_SOFTWARE | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE,
     8,
     {
      SDL_PIXELFORMAT_ARGB8888,
      SDL_PIXELFORMAT_ABGR8888,
      SDL_PIXELFORMAT_RGBA8888,
      SDL_PIXELFORMAT_BGRA8888,
      SDL_PIXELFORMAT_RGB888,
      SDL_PIXELFORMAT_BGR888,
      SDL_PIXELFORMAT_RGB565,
      SDL_PIXELFORMAT_RGB555
     },
     0,
     0}
};

#if SDL_VIDEO_RENDER_RGA
static SDL_Renderer *RGA_CreateRenderer(SDL_Window *window, Uint32 flags)
{
    SDL_Renderer *renderer;
    SW_RenderData *data;
    SDL_Surface *surface;
    void *pixels;
    int width, height, pitch, fd = -1;
    size_t bytes;

    (void)flags;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    if (width < 2 || height < 2 || width > 1280 || height > 1280) {
        SDL_SetError("RGA supports window sizes from 2x2 through 1280x1280");
        return NULL;
    }
#if SDL_VIDEO_DRIVER_DIRECTFB
    if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "directfb") == 0) {
        DFB_WindowData *windata = (DFB_WindowData *)window->driverdata;
        if (windata == NULL || windata->surface == NULL || windata->window_surface == NULL) {
            SDL_SetError("RGA: DirectFB drawing surface unavailable");
            return NULL;
        }
    } else
#endif
    if (SDL_GetWindowSurface(window) == NULL) {
        return NULL;
    }
    if (RGA_Allocate(width, height, 2, &fd, &pixels, &pitch, &bytes) < 0) {
        SDL_SetError("RGA: cannot allocate DMA buffer");
        return NULL;
    }
    surface = SDL_CreateRGBSurfaceFrom(pixels, width, height, 16, pitch,
                                         0xf800u, 0x07e0u, 0x001fu, 0);
    if (surface == NULL) {
        munmap(pixels, bytes);
        close(fd);
        return NULL;
    }
    renderer = SW_CreateRendererForSurface(surface);
    if (renderer == NULL) {
        SDL_FreeSurface(surface);
        munmap(pixels, bytes);
        close(fd);
        return NULL;
    }
    data = (SW_RenderData *)renderer->driverdata;
    data->rga_fd = fd;
    data->rga_bytes = bytes;
    data->rga_pixels = pixels;
    data->rga_cpu_access = SDL_TRUE;
    data->rga_handle = RGA_Import(pitch, height, RK_FORMAT_RGB_565, fd);
    if (data->rga_handle) data->rga_import_count++;
    data->rga_force = SDL_GetHintBoolean("SDL_RGA_FORCE", SDL_FALSE);
    data->rga_cache = SDL_GetHintBoolean("SDL_RGA_CACHE", SDL_TRUE);
    data->fill_fd = -1;
    renderer->info = RGA_RenderDriver.info;
    renderer->window = window;
    {
        int fill_pitch;
        if (RGA_Allocate(32, 32, 4, &data->fill_fd, &data->fill_pixels,
                         &fill_pitch, &data->fill_bytes) != 0) {
            data->fill_fd = -1; /* Alpha fills remain available via software. */
        } else {
            data->fill_cpu_access = SDL_TRUE;
            data->fill_handle = RGA_Import(fill_pitch, 32, RK_FORMAT_RGBA_8888, data->fill_fd);
            if (data->fill_handle) data->rga_import_count++;
        }
    }
    return renderer;
}

SDL_RenderDriver RGA_RenderDriver = {
    RGA_CreateRenderer,
    {
        "rga",
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE,
        2,
        { SDL_PIXELFORMAT_RGB565, SDL_PIXELFORMAT_ARGB8888 },
        1280,
        1280
    }
};
#endif

#endif /* SDL_VIDEO_RENDER_SW && !SDL_RENDER_DISABLED */

/* vi: set ts=4 sw=4 expandtab: */
