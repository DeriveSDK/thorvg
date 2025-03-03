/*
 * Copyright (c) 2020 - 2023 the ThorVG project. All rights reserved.

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifdef _WIN32
    #include <malloc.h>
#elif defined(__linux__)
    #include <alloca.h>
#else
    #include <stdlib.h>
#endif

#include "tvgMath.h"
#include "tvgRender.h"
#include "tvgSwCommon.h"

/************************************************************************/
/* Internal Class Implementation                                        */
/************************************************************************/
constexpr auto DOWN_SCALE_TOLERANCE = 0.5f;

struct FillLinear
{
    void operator()(const SwFill* fill, uint32_t* dst, uint32_t y, uint32_t x, uint32_t len, SwBlender op, uint8_t a)
    {
        fillLinear(fill, dst, y, x, len, op, a);
    }

    void operator()(const SwFill* fill, uint32_t* dst, uint32_t y, uint32_t x, uint32_t len, uint8_t* cmp, SwAlpha alpha, uint8_t csize, uint8_t opacity)
    {
        fillLinear(fill, dst, y, x, len, cmp, alpha, csize, opacity);
    }

    void operator()(const SwFill* fill, uint32_t* dst, uint32_t y, uint32_t x, uint32_t len, SwBlender op, SwBlender op2, uint8_t a)
    {
        fillLinear(fill, dst, y, x, len, op, op2, a);
    }

};

struct FillRadial
{
    void operator()(const SwFill* fill, uint32_t* dst, uint32_t y, uint32_t x, uint32_t len, SwBlender op, uint8_t a)
    {
        fillRadial(fill, dst, y, x, len, op, a);
    }

    void operator()(const SwFill* fill, uint32_t* dst, uint32_t y, uint32_t x, uint32_t len, uint8_t* cmp, SwAlpha alpha, uint8_t csize, uint8_t opacity)
    {
        fillRadial(fill, dst, y, x, len, cmp, alpha, csize, opacity);
    }

    void operator()(const SwFill* fill, uint32_t* dst, uint32_t y, uint32_t x, uint32_t len, SwBlender op, SwBlender op2, uint8_t a)
    {
        fillRadial(fill, dst, y, x, len, op, op2, a);
    }
};


static bool _rasterDirectImage(SwSurface* surface, const SwImage* image, const SwBBox& region,  uint8_t opacity = 255);


static inline uint8_t _alpha(uint8_t* a)
{
    return *a;
}


static inline uint8_t _ialpha(uint8_t* a)
{
    return ~(*a);
}


static inline uint8_t _abgrLuma(uint8_t* c)
{
    auto v = *(uint32_t*)c;
    return ((((v&0xff)*54) + (((v>>8)&0xff)*183) + (((v>>16)&0xff)*19))) >> 8; //0.2125*R + 0.7154*G + 0.0721*B
}


static inline uint8_t _argbLuma(uint8_t* c)
{
    auto v = *(uint32_t*)c;
    return ((((v&0xff)*19) + (((v>>8)&0xff)*183) + (((v>>16)&0xff)*54))) >> 8; //0.0721*B + 0.7154*G + 0.2125*R
}


static inline uint8_t _abgrInvLuma(uint8_t* c)
{
    return ~_abgrLuma(c);
}


static inline uint8_t _argbInvLuma(uint8_t* c)
{
    return ~_argbLuma(c);
}


static inline uint32_t _abgrJoin(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return (a << 24 | b << 16 | g << 8 | r);
}


static inline uint32_t _argbJoin(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return (a << 24 | r << 16 | g << 8 | b);
}

static inline bool _blending(const SwSurface* surface)
{
    return (surface->blender) ? true : false;
}


/* OPTIMIZE_ME: Probably, we can separate masking(8bits) / composition(32bits)
   This would help to enhance the performance by avoiding the unnecessary matting from the composition */
static inline bool _compositing(const SwSurface* surface)
{
    if (!surface->compositor || (int)surface->compositor->method <= (int)CompositeMethod::ClipPath) return false;
    return true;
}


static inline bool _matting(const SwSurface* surface)
{
    if ((int)surface->compositor->method < (int)CompositeMethod::AddMask) return true;
    else return false;
}


static inline bool _masking(const SwSurface* surface)
{
    if ((int)surface->compositor->method >= (int)CompositeMethod::AddMask) return true;
    else return false;
}


static inline uint32_t _opMaskAdd(uint32_t s, uint32_t d, uint8_t a)
{
        return s + ALPHA_BLEND(d, a);
}


static inline uint32_t _opMaskSubtract(TVG_UNUSED uint32_t s, uint32_t d, uint8_t a)
{
    return ALPHA_BLEND(d, a);
}


static inline uint32_t _opMaskDifference(uint32_t s, uint32_t d, uint8_t a)
{
    return ALPHA_BLEND(s, IA(d)) + ALPHA_BLEND(d, a);
}


static inline uint32_t _opAMaskAdd(uint32_t s, uint32_t d, uint8_t a)
{
    return INTERPOLATE(s, d, a);
}


static inline uint32_t _opAMaskSubtract(TVG_UNUSED uint32_t s, uint32_t d, uint8_t a)
{
    return ALPHA_BLEND(d, IA(ALPHA_BLEND(s, a)));
}


static inline uint32_t _opAMaskDifference(uint32_t s, uint32_t d, uint8_t a)
{
        auto t = ALPHA_BLEND(s, a);
        return ALPHA_BLEND(t, IA(d)) + ALPHA_BLEND(d, IA(t));
}


static inline SwBlender _getMaskOp(CompositeMethod method)
{
    switch (method) {
        case CompositeMethod::AddMask: return _opMaskAdd;
        case CompositeMethod::SubtractMask: return _opMaskSubtract;
        case CompositeMethod::DifferenceMask: return _opMaskDifference;
        default: return nullptr;
    }
}


static inline SwBlender _getAMaskOp(CompositeMethod method)
{
    switch (method) {
        case CompositeMethod::AddMask: return _opAMaskAdd;
        case CompositeMethod::SubtractMask: return _opAMaskSubtract;
        case CompositeMethod::DifferenceMask: return _opAMaskDifference;
        default: return nullptr;
    }
}


#include "tvgSwRasterTexmap.h"
#include "tvgSwRasterC.h"
#include "tvgSwRasterAvx.h"
#include "tvgSwRasterNeon.h"


static inline uint32_t _sampleSize(float scale)
{
    auto sampleSize = static_cast<uint32_t>(0.5f / scale);
    if (sampleSize == 0) sampleSize = 1;
    return sampleSize;
}


//Bilinear Interpolation
//OPTIMIZE_ME: Skip the function pointer access
static uint32_t _interpUpScaler(const uint32_t *img, TVG_UNUSED uint32_t stride, uint32_t w, uint32_t h, float sx, float sy, TVG_UNUSED uint32_t n, TVG_UNUSED uint32_t n2)
{
    auto rx = (uint32_t)(sx);
    auto ry = (uint32_t)(sy);
    auto rx2 = rx + 1;
    if (rx2 >= w) rx2 = w - 1;
    auto ry2 = ry + 1;
    if (ry2 >= h) ry2 = h - 1;

    auto dx = static_cast<uint32_t>((sx - rx) * 255.0f);
    auto dy = static_cast<uint32_t>((sy - ry) * 255.0f);

    auto c1 = img[rx + ry * w];
    auto c2 = img[rx2 + ry * w];
    auto c3 = img[rx2 + ry2 * w];
    auto c4 = img[rx + ry2 * w];

    return INTERPOLATE(INTERPOLATE(c3, c4, dx), INTERPOLATE(c2, c1, dx), dy);
}


//2n x 2n Mean Kernel
//OPTIMIZE_ME: Skip the function pointer access
static uint32_t _interpDownScaler(const uint32_t *img, uint32_t stride, uint32_t w, uint32_t h, float sx, float sy, uint32_t n, uint32_t n2)
{
    uint32_t rx = sx;
    uint32_t ry = sy;
    uint32_t c[4] = {0, 0, 0, 0};
    auto src = img + rx - n + (ry - n) * stride;

    for (auto y = ry - n; y < ry + n; ++y) {
        if (y >= h) continue;
        auto p = src;
        for (auto x = rx - n; x < rx + n; ++x, ++p) {
            if (x >= w) continue;
            c[0] += *p >> 24;
            c[1] += (*p >> 16) & 0xff;
            c[2] += (*p >> 8) & 0xff;
            c[3] += *p & 0xff;
        }
        src += stride;
    }
    for (auto i = 0; i < 4; ++i) {
        c[i] = (c[i] >> 2) / n2;
    }
    return (c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3];
}


/************************************************************************/
/* Rect                                                                 */
/************************************************************************/

static void _rasterMaskedRectDup(SwSurface* surface, const SwBBox& region, SwBlender opMask, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto cbuffer = surface->compositor->image.buf32 + (region.min.y * surface->compositor->image.stride + region.min.x);   //compositor buffer
    auto cstride = surface->compositor->image.stride;
    auto color = surface->join(r, g, b, a);
    auto ialpha = 255 - a;

    for (uint32_t y = 0; y < h; ++y) {
        auto cmp = cbuffer;
        for (uint32_t x = 0; x < w; ++x, ++cmp) {
            *cmp = opMask(color, *cmp, ialpha);
        }
        cbuffer += cstride;
    }
}


static void _rasterMaskedRectInt(SwSurface* surface, const SwBBox& region, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto cstride = surface->compositor->image.stride;

    for (uint32_t y = surface->compositor->bbox.min.y; y < surface->compositor->bbox.max.y; ++y) {
        auto cmp = surface->compositor->image.buf32 + (y * cstride + surface->compositor->bbox.min.x);
        if (y == region.min.y) {
            for (uint32_t y2 = y; y2 < region.max.y; ++y2) {
                auto tmp = cmp;
                auto x = surface->compositor->bbox.min.x;
                while (x < surface->compositor->bbox.max.x) {
                    if (x == region.min.x) {
                        for (uint32_t i = 0; i < w; ++i, ++tmp) {
                            *tmp = ALPHA_BLEND(*tmp, a);
                        }
                        x += w;
                    } else {
                        *tmp = 0;
                        ++tmp;
                        ++x;
                    }
                }
                cmp += cstride;
            }
            y += (h - 1);
        } else {
            rasterPixel32(cmp, 0x00000000, 0, w);
            cmp += cstride;
        }
    }
}


static bool _rasterMaskedRect(SwSurface* surface, const SwBBox& region, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    //32bit channels composition
    if (surface->channelSize != sizeof(uint32_t)) return false;

    TVGLOG("SW_ENGINE", "Masked(%d) Rect [Region: %lu %lu %lu %lu]", (int)surface->compositor->method, region.min.x, region.min.y, region.max.x - region.max.y, region.min.y);

    if (surface->compositor->method == CompositeMethod::IntersectMask) {
        _rasterMaskedRectInt(surface, region, r, g, b, a);
    } else if (auto opMask = _getMaskOp(surface->compositor->method)) {
        //Other Masking operations: Add, Subtract, Difference ...
        _rasterMaskedRectDup(surface, region, opMask, r, g, b, a);
    } else {
        return false;
    }

    //Masking Composition
    return _rasterDirectImage(surface, &surface->compositor->image, surface->compositor->bbox);
}


static bool _rasterMattedRect(SwSurface* surface, const SwBBox& region, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto csize = surface->compositor->image.channelSize;
    auto cbuffer = surface->compositor->image.buf8 + ((region.min.y * surface->compositor->image.stride + region.min.x) * csize);   //compositor buffer
    auto alpha = surface->alpha(surface->compositor->method);

    TVGLOG("SW_ENGINE", "Matted(%d) Rect [Region: %lu %lu %u %u]", (int)surface->compositor->method, region.min.x, region.min.y, w, h);
    
    //32bits channels
    if (surface->channelSize == sizeof(uint32_t)) {
        auto color = surface->join(r, g, b, a);
        auto buffer = surface->buf32 + (region.min.y * surface->stride) + region.min.x;
        for (uint32_t y = 0; y < h; ++y) {
            auto dst = &buffer[y * surface->stride];
            auto cmp = &cbuffer[y * surface->compositor->image.stride * csize];
            for (uint32_t x = 0; x < w; ++x, ++dst, cmp += csize) {
                *dst = INTERPOLATE(color, *dst, alpha(cmp));
            }
        }
    //8bits grayscale
    } else if (surface->channelSize == sizeof(uint8_t)) {
        auto buffer = surface->buf8 + (region.min.y * surface->stride) + region.min.x;
        for (uint32_t y = 0; y < h; ++y) {
            auto dst = &buffer[y * surface->stride];
            auto cmp = &cbuffer[y * surface->compositor->image.stride * csize];
            for (uint32_t x = 0; x < w; ++x, ++dst, cmp += csize) {
                *dst = INTERPOLATE8(a, *dst, alpha(cmp));
            }
        }
    }
    return true;
}


static bool _rasterBlendingRect(SwSurface* surface, const SwBBox& region, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (surface->channelSize != sizeof(uint32_t)) return false;

    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto color = surface->join(r, g, b, a);
    auto buffer = surface->buf32 + (region.min.y * surface->stride) + region.min.x;
    auto ialpha = 255 - a;

    for (uint32_t y = 0; y < h; ++y) {
        auto dst = &buffer[y * surface->stride];
        for (uint32_t x = 0; x < w; ++x, ++dst) {
            *dst = surface->blender(color, *dst, ialpha);
        }
    }
    return true;
}


static bool _rasterTranslucentRect(SwSurface* surface, const SwBBox& region, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
#if defined(THORVG_AVX_VECTOR_SUPPORT)
    return avxRasterTranslucentRect(surface, region, r, g, b, a);
#elif defined(THORVG_NEON_VECTOR_SUPPORT)
    return neonRasterTranslucentRect(surface, region, r, g, b, a);
#else
    return cRasterTranslucentRect(surface, region, r, g, b, a);
#endif
}


static bool _rasterSolidRect(SwSurface* surface, const SwBBox& region, uint8_t r, uint8_t g, uint8_t b)
{
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);

    //32bits channels
    if (surface->channelSize == sizeof(uint32_t)) {
        auto color = surface->join(r, g, b, 255);
        auto buffer = surface->buf32 + (region.min.y * surface->stride);
        for (uint32_t y = 0; y < h; ++y) {
            rasterPixel32(buffer + y * surface->stride, color, region.min.x, w);
        }
        return true;
    }
    //8bits grayscale
    if (surface->channelSize == sizeof(uint8_t)) {
        for (uint32_t y = 0; y < h; ++y) {
            rasterGrayscale8(surface->buf8, 255, region.min.y * surface->stride + region.min.x, w);
        }
        return true;
    }
    return false;
}


static bool _rasterRect(SwSurface* surface, const SwBBox& region, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (_compositing(surface)) {
        if (_matting(surface)) return _rasterMattedRect(surface, region, r, g, b, a);
        else return _rasterMaskedRect(surface, region, r, g, b, a);
    } else if (_blending(surface)) {
        return _rasterBlendingRect(surface, region, r, g, b, a);
    } else {
        if (a == 255) return _rasterSolidRect(surface, region, r, g, b);
        else return _rasterTranslucentRect(surface, region, r, g, b, a);
    }
    return false;
}


/************************************************************************/
/* Rle                                                                  */
/************************************************************************/

static void _rasterMaskedRleDup(SwSurface* surface, SwRleData* rle, SwBlender maskOp, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    auto span = rle->spans;
    auto cbuffer = surface->compositor->image.buf32;
    auto cstride = surface->compositor->image.stride;
    auto color = surface->join(r, g, b, a);
    uint32_t src;

    for (uint32_t i = 0; i < rle->size; ++i, ++span) {
        auto cmp = &cbuffer[span->y * cstride + span->x];
        if (span->coverage == 255) src = color;
        else src = ALPHA_BLEND(color, span->coverage);
        auto ialpha = IA(src);
        for (auto x = 0; x < span->len; ++x, ++cmp) {
            *cmp = maskOp(src, *cmp, ialpha);
        }
    }
}


static void _rasterMaskedRleInt(SwSurface* surface, SwRleData* rle, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    auto span = rle->spans;
    auto cbuffer = surface->compositor->image.buf32;
    auto cstride = surface->compositor->image.stride;
    auto color = surface->join(r, g, b, a);
    uint32_t src;

    for (uint32_t y = surface->compositor->bbox.min.y; y < surface->compositor->bbox.max.y; ++y) {
        auto cmp = &cbuffer[y * cstride];
        uint32_t x = surface->compositor->bbox.min.x;
        while (x < surface->compositor->bbox.max.x) {
            if (y == span->y && x == span->x && x + span->len <= surface->compositor->bbox.max.x) {
                if (span->coverage == 255) src = color;
                else src = ALPHA_BLEND(color, span->coverage);
                auto alpha = A(src);
                for (uint32_t i = 0; i < span->len; ++i) {
                    cmp[x + i] = ALPHA_BLEND(cmp[x + i], alpha);
                }
                x += span->len;
                ++span;
            } else {
                cmp[x] = 0;
                ++x;
            }
        }
    }
}


static bool _rasterMaskedRle(SwSurface* surface, SwRleData* rle, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    TVGLOG("SW_ENGINE", "Masked(%d) Rle", (int)surface->compositor->method);

    //32bit channels composition
    if (surface->channelSize != sizeof(uint32_t)) return false;

    if (surface->compositor->method == CompositeMethod::IntersectMask) {
        _rasterMaskedRleInt(surface, rle, r, g, b, a);
    } else if (auto opMask = _getMaskOp(surface->compositor->method)) {
        //Other Masking operations: Add, Subtract, Difference ...
        _rasterMaskedRleDup(surface, rle, opMask, r, g, b, a);
    } else {
        return false;
    }

    //Masking Composition
    return _rasterDirectImage(surface, &surface->compositor->image, surface->compositor->bbox);
}


static bool _rasterMattedRle(SwSurface* surface, SwRleData* rle, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    TVGLOG("SW_ENGINE", "Matted(%d) Rle", (int)surface->compositor->method);

    auto span = rle->spans;
    auto cbuffer = surface->compositor->image.buf8;
    auto csize = surface->compositor->image.channelSize;
    auto alpha = surface->alpha(surface->compositor->method);

    //32bit channels
    if (surface->channelSize == sizeof(uint32_t)) {
        uint32_t src;
        auto color = surface->join(r, g, b, a);
        for (uint32_t i = 0; i < rle->size; ++i, ++span) {
            auto dst = &surface->buf32[span->y * surface->stride + span->x];
            auto cmp = &cbuffer[(span->y * surface->compositor->image.stride + span->x) * csize];
            if (span->coverage == 255) src = color;
            else src = ALPHA_BLEND(color, span->coverage);
            for (uint32_t x = 0; x < span->len; ++x, ++dst, cmp += csize) {
                *dst = INTERPOLATE(src, *dst, alpha(cmp));
            }
        }
        return true;
    }
    //8bit grayscale
    if (surface->channelSize == sizeof(uint8_t)) {
        uint8_t src;
        for (uint32_t i = 0; i < rle->size; ++i, ++span) {
            auto dst = &surface->buf8[span->y * surface->stride + span->x];
            auto cmp = &cbuffer[(span->y * surface->compositor->image.stride + span->x) * csize];
            if (span->coverage == 255) src = a;
            else src = MULTIPLY(a, span->coverage);
            for (uint32_t x = 0; x < span->len; ++x, ++dst, cmp += csize) {
                *dst = INTERPOLATE8(src, *dst, alpha(cmp));
            }
        }
        return true;
    }
    return false;
}


static bool _rasterBlendingRle(SwSurface* surface, const SwRleData* rle, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (surface->channelSize != sizeof(uint32_t)) return false;

    auto span = rle->spans;
    auto color = surface->join(r, g, b, a);
    auto ialpha = 255 - a;

    for (uint32_t i = 0; i < rle->size; ++i, ++span) {
        auto dst = &surface->buf32[span->y * surface->stride + span->x];
        if (span->coverage == 255) {
            for (uint32_t x = 0; x < span->len; ++x, ++dst) {
                *dst = surface->blender(color, *dst, ialpha);
            }
        } else {
            for (uint32_t x = 0; x < span->len; ++x, ++dst) {
                auto tmp = surface->blender(color, *dst, ialpha);
                *dst = INTERPOLATE(tmp, *dst, span->coverage);
            }
        }
    }
    return true;
}


static bool _rasterTranslucentRle(SwSurface* surface, const SwRleData* rle, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
#if defined(THORVG_AVX_VECTOR_SUPPORT)
    return avxRasterTranslucentRle(surface, rle, r, g, b, a);
#elif defined(THORVG_NEON_VECTOR_SUPPORT)
    return neonRasterTranslucentRle(surface, rle, r, g, b, a);
#else
    return cRasterTranslucentRle(surface, rle, r, g, b, a);
#endif
}


static bool _rasterSolidRle(SwSurface* surface, const SwRleData* rle, uint8_t r, uint8_t g, uint8_t b)
{
    auto span = rle->spans;

    //32bit channels
    if (surface->channelSize == sizeof(uint32_t)) {
        auto color = surface->join(r, g, b, 255);
        for (uint32_t i = 0; i < rle->size; ++i, ++span) {
            if (span->coverage == 255) {
                rasterPixel32(surface->buf32 + span->y * surface->stride, color, span->x, span->len);
            } else {
                auto dst = &surface->buf32[span->y * surface->stride + span->x];
                auto src = ALPHA_BLEND(color, span->coverage);
                auto ialpha = 255 - span->coverage;
                for (uint32_t x = 0; x < span->len; ++x, ++dst) {
                    *dst = src + ALPHA_BLEND(*dst, ialpha);
                }
            }
        }
    //8bit grayscale
    } else if (surface->channelSize == sizeof(uint8_t)) {
        for (uint32_t i = 0; i < rle->size; ++i, ++span) {
            rasterGrayscale8(surface->buf8, span->coverage, span->y * surface->stride + span->x, span->len);
        }
    }
    return true;
}


static bool _rasterRle(SwSurface* surface, SwRleData* rle, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (!rle) return false;

    if (_compositing(surface)) {
        if (_matting(surface)) return _rasterMattedRle(surface, rle, r, g, b, a);
        else return _rasterMaskedRle(surface, rle, r, g, b, a);
    } else if (_blending(surface)) {
        return _rasterBlendingRle(surface, rle, r, g, b, a);
    } else {
        if (a == 255) return _rasterSolidRle(surface, rle, r, g, b);
        else return _rasterTranslucentRle(surface, rle, r, g, b, a);
    }
    return false;
}


/************************************************************************/
/* RLE Transformed Image                                                */
/************************************************************************/

static bool _transformedRleImage(SwSurface* surface, const SwImage* image, const Matrix* transform, uint8_t opacity)
{
    auto ret = _rasterTexmapPolygon(surface, image, transform, nullptr, opacity);

    //Masking Composition
    if (_compositing(surface) && _masking(surface)) {
        return _rasterDirectImage(surface, &surface->compositor->image, surface->compositor->bbox);
    }

    return ret;

}


/************************************************************************/
/* RLE Scaled Image                                                     */
/************************************************************************/

static void _rasterScaledMaskedRleImageDup(SwSurface* surface, const SwImage* image, const Matrix* itransform, const SwBBox& region, SwBlender maskOp, SwBlender amaskOp, uint8_t opacity)
{
    auto scaleMethod = image->scale < DOWN_SCALE_TOLERANCE ? _interpDownScaler : _interpUpScaler;
    auto sampleSize = _sampleSize(image->scale);
    auto sampleSize2 = sampleSize * sampleSize;
    auto span = image->rle->spans;

    for (uint32_t i = 0; i < image->rle->size; ++i, ++span) {
        auto sy = span->y * itransform->e22 + itransform->e23;
        if ((uint32_t)sy >= image->h) continue;
        auto cmp = &surface->compositor->image.buf32[span->y * surface->compositor->image.stride + span->x];
        auto a = MULTIPLY(span->coverage, opacity);
        if (a == 255) {
            for (uint32_t x = static_cast<uint32_t>(span->x); x < static_cast<uint32_t>(span->x) + span->len; ++x, ++cmp) {
                auto sx = x * itransform->e11 + itransform->e13;
                if ((uint32_t)sx >= image->w) continue;
                auto src = scaleMethod(image->buf32, image->stride, image->w, image->h, sx, sy, sampleSize, sampleSize2);
                *cmp = maskOp(src, *cmp, 255);
            }
        } else {
            for (uint32_t x = static_cast<uint32_t>(span->x); x < static_cast<uint32_t>(span->x) + span->len; ++x, ++cmp) {
                auto sx = x * itransform->e11 + itransform->e13;
                if ((uint32_t)sx >= image->w) continue;
                auto src = scaleMethod(image->buf32, image->stride, image->w, image->h, sx, sy, sampleSize, sampleSize2);
                *cmp = amaskOp(src, *cmp, a);
            }
        }
    }
}


static void _rasterScaledMaskedRleImageInt(SwSurface* surface, const SwImage* image, const Matrix* itransform, const SwBBox& region, uint8_t opacity)
{
    auto scaleMethod = image->scale < DOWN_SCALE_TOLERANCE ? _interpDownScaler : _interpUpScaler;
    auto sampleSize = _sampleSize(image->scale);
    auto sampleSize2 = sampleSize * sampleSize;
    auto span = image->rle->spans;
    auto cbuffer = surface->compositor->image.buf32;
    auto cstride = surface->compositor->image.stride;

    for (uint32_t y = surface->compositor->bbox.min.y; y < surface->compositor->bbox.max.y; ++y) {
        auto cmp = &cbuffer[y * cstride];
        for (uint32_t x = surface->compositor->bbox.min.x; x < surface->compositor->bbox.max.x; ++x) {
            if (y == span->y && x == span->x && x + span->len <= surface->compositor->bbox.max.x) {
                auto sy = span->y * itransform->e22 + itransform->e23;
                if ((uint32_t)sy >= image->h) continue;
                auto alpha = MULTIPLY(span->coverage, opacity);
                if (alpha == 255) {
                    for (uint32_t i = 0; i < span->len; ++i) {
                        auto sx = (x + i) * itransform->e11 + itransform->e13;
                        if ((uint32_t)sx >= image->w) continue;
                        auto src = scaleMethod(image->buf32, image->stride, image->w, image->h, sx, sy, sampleSize, sampleSize2);
                        cmp[x + i] = ALPHA_BLEND(cmp[x + i], A(src));
                    }
                } else {
                    for (uint32_t i = 0; i < span->len; ++i) {
                        auto sx = (x + i) * itransform->e11 + itransform->e13;
                        if ((uint32_t)sx >= image->w) continue;
                        auto src = scaleMethod(image->buf32, image->stride, image->w, image->h, sx, sy, sampleSize, sampleSize2);
                        cmp[x + i] = ALPHA_BLEND(cmp[x + i], A(ALPHA_BLEND(src, alpha)));
                    }
                }
                x += span->len - 1;
                ++span;
            } else {
                cmp[x] = 0;
            }
        }
    }
}


static bool _rasterScaledMaskedRleImage(SwSurface* surface, const SwImage* image, const Matrix* itransform, const SwBBox& region, uint8_t opacity)
{
    TVGLOG("SW_ENGINE", "Scaled Masked(%d) Rle Image", (int)surface->compositor->method);

    if (surface->compositor->method == CompositeMethod::IntersectMask) {
        _rasterScaledMaskedRleImageInt(surface, image, itransform, region, opacity);
    } else if (auto opMask = _getMaskOp(surface->compositor->method)) {
        //Other Masking operations: Add, Subtract, Difference ...
        _rasterScaledMaskedRleImageDup(surface, image, itransform, region, opMask, _getAMaskOp(surface->compositor->method), opacity);
    } else {
        return false;
    }
    //Masking Composition
    return _rasterDirectImage(surface, &surface->compositor->image, surface->compositor->bbox);
}


static bool _rasterScaledMattedRleImage(SwSurface* surface, const SwImage* image, const Matrix* itransform, const SwBBox& region, uint8_t opacity)
{
    TVGLOG("SW_ENGINE", "Scaled Matted(%d) Rle Image", (int)surface->compositor->method);

    auto span = image->rle->spans;
    auto csize = surface->compositor->image.channelSize;
    auto alpha = surface->alpha(surface->compositor->method);

    auto scaleMethod = image->scale < DOWN_SCALE_TOLERANCE ? _interpDownScaler : _interpUpScaler;
    auto sampleSize = _sampleSize(image->scale);
    auto sampleSize2 = sampleSize * sampleSize;

    for (uint32_t i = 0; i < image->rle->size; ++i, ++span) {
        auto sy = span->y * itransform->e22 + itransform->e23;
        if ((uint32_t)sy >= image->h) continue;
        auto dst = &surface->buf32[span->y * surface->stride + span->x];
        auto cmp = &surface->compositor->image.buf8[(span->y * surface->compositor->image.stride + span->x) * csize];
        auto a = MULTIPLY(span->coverage, opacity);
        if (a == 255) {
            for (uint32_t x = static_cast<uint32_t>(span->x); x < static_cast<uint32_t>(span->x) + span->len; ++x, ++dst, cmp += csize) {
                auto sx = x * itransform->e11 + itransform->e13;
                if ((uint32_t)sx >= image->w) continue;
                auto tmp = ALPHA_BLEND(scaleMethod(image->buf32, image->stride, image->w, image->h, sx, sy, sampleSize, sampleSize2), alpha(cmp));
                *dst = tmp + ALPHA_BLEND(*dst, IA(tmp));
            }
        } else {
            for (uint32_t x = static_cast<uint32_t>(span->x); x < static_cast<uint32_t>(span->x) + span->len; ++x, ++dst, cmp += csize) {
                auto sx = x * itransform->e11 + itransform->e13;
                if ((uint32_t)sx >= image->w) continue;
                auto src = scaleMethod(image->buf32, image->stride, image->w, image->h, sx, sy, sampleSize, sampleSize2);
                auto tmp = ALPHA_BLEND(src, MULTIPLY(alpha(cmp), a));
                *dst = tmp + ALPHA_BLEND(*dst, IA(tmp));
            }
        }
    }

    return true;
}


static bool _rasterScaledBlendingRleImage(SwSurface* surface, const SwImage* image, const Matrix* itransform, const SwBBox& region, uint8_t opacity)
{
    auto span = image->rle->spans;
    auto scaleMethod = image->scale < DOWN_SCALE_TOLERANCE ? _interpDownScaler : _interpUpScaler;
    auto sampleSize = _sampleSize(image->scale);
    auto sampleSize2 = sampleSize * sampleSize;

    for (uint32_t i = 0; i < image->rle->size; ++i, ++span) {
        auto sy = span->y * itransform->e22 + itransform->e23;
        if ((uint32_t)sy >= image->h) continue;
        auto dst = &surface->buf32[span->y * surface->stride + span->x];
        auto alpha = MULTIPLY(span->coverage, opacity);
        if (alpha == 255) {
            for (uint32_t x = static_cast<uint32_t>(span->x); x < static_cast<uint32_t>(span->x) + span->len; ++x, ++dst) {
                auto sx = x * itransform->e11 + itransform->e13;
                if ((uint32_t)sx >= image->w) continue;
                auto src = scaleMethod(image->buf32, image->stride, image->w, image->h, sx, sy, sampleSize, sampleSize2);
                auto tmp = surface->blender(src, *dst, 255);
                *dst = INTERPOLATE(tmp, *dst, A(src));
            }
        } else if (opacity == 255) {
            for (uint32_t x = static_cast<uint32_t>(span->x); x < static_cast<uint32_t>(span->x) + span->len; ++x, ++dst) {
                auto sx = x * itransform->e11 + itransform->e13;
                if ((uint32_t)sx >= image->w) continue;
                auto src = scaleMethod(image->buf32, image->stride, image->w, image->h, sx, sy, sampleSize, sampleSize2);
                auto tmp = surface->blender(src, *dst, 255);
                *dst = INTERPOLATE(tmp, *dst, MULTIPLY(span->coverage, A(src)));
            }
        } else {
            for (uint32_t x = static_cast<uint32_t>(span->x); x < static_cast<uint32_t>(span->x) + span->len; ++x, ++dst) {
                auto sx = x * itransform->e11 + itransform->e13;
                if ((uint32_t)sx >= image->w) continue;
                auto src = ALPHA_BLEND(scaleMethod(image->buf32, image->stride, image->w, image->h, sx, sy, sampleSize, sampleSize2), opacity);
                auto tmp = surface->blender(src, *dst, 255);
                *dst = INTERPOLATE(tmp, *dst, MULTIPLY(span->coverage, A(src)));
            }
        }
    }
    return true;
}


static bool _rasterScaledRleImage(SwSurface* surface, const SwImage* image, const Matrix* itransform, const SwBBox& region, uint8_t opacity)
{
    auto span = image->rle->spans;
    auto scaleMethod = image->scale < DOWN_SCALE_TOLERANCE ? _interpDownScaler : _interpUpScaler;
    auto sampleSize = _sampleSize(image->scale);
    auto sampleSize2 = sampleSize * sampleSize;

    for (uint32_t i = 0; i < image->rle->size; ++i, ++span) {
        auto sy = span->y * itransform->e22 + itransform->e23;
        if ((uint32_t)sy >= image->h) continue;
        auto dst = &surface->buf32[span->y * surface->stride + span->x];
        auto alpha = MULTIPLY(span->coverage, opacity);
        if (alpha == 255) {
            for (uint32_t x = static_cast<uint32_t>(span->x); x < static_cast<uint32_t>(span->x) + span->len; ++x, ++dst) {
                auto sx = x * itransform->e11 + itransform->e13;
                if ((uint32_t)sx >= image->w) continue;
                auto src = scaleMethod(image->buf32, image->stride, image->w, image->h, sx, sy, sampleSize, sampleSize2);
                *dst = src + ALPHA_BLEND(*dst, IA(src));
            }
        } else {
            for (uint32_t x = static_cast<uint32_t>(span->x); x < static_cast<uint32_t>(span->x) + span->len; ++x, ++dst) {
                auto sx = x * itransform->e11 + itransform->e13;
                if ((uint32_t)sx >= image->w) continue;
                auto src = ALPHA_BLEND(scaleMethod(image->buf32, image->stride, image->w, image->h, sx, sy, sampleSize, sampleSize2), alpha);
                *dst = src + ALPHA_BLEND(*dst, IA(src));
            }
        }
    }
    return true;
}


static bool _scaledRleImage(SwSurface* surface, const SwImage* image, const Matrix* transform, const SwBBox& region, uint8_t opacity)
{
    Matrix itransform;

    if (transform) {
        if (!mathInverse(transform, &itransform)) return false;
    } else mathIdentity(&itransform);

    if (_compositing(surface)) {
        if (_matting(surface)) return _rasterScaledMattedRleImage(surface, image, &itransform, region, opacity);
        else return _rasterScaledMaskedRleImage(surface, image, &itransform, region, opacity);
    } else if (_blending(surface)) {
        return _rasterScaledBlendingRleImage(surface, image, &itransform, region, opacity);
    } else {
        return _rasterScaledRleImage(surface, image, &itransform, region, opacity);
    }
    return false;
}


/************************************************************************/
/* RLE Direct Image                                                     */
/************************************************************************/

static void _rasterDirectMaskedRleImageDup(SwSurface* surface, const SwImage* image, SwBlender maskOp, SwBlender amaskOp, uint8_t opacity)
{
    auto span = image->rle->spans;
    auto cbuffer = surface->compositor->image.buf32;
    auto ctride = surface->compositor->image.stride;

    for (uint32_t i = 0; i < image->rle->size; ++i, ++span) {
        auto src = image->buf32 + (span->y + image->oy) * image->stride + (span->x + image->ox);
        auto cmp = &cbuffer[span->y * ctride + span->x];
        auto alpha = MULTIPLY(span->coverage, opacity);
        if (alpha == 255) {
            for (uint32_t x = 0; x < span->len; ++x, ++src, ++cmp) {
                *cmp = maskOp(*src, *cmp, IA(*src));
            }
        } else {
            for (uint32_t x = 0; x < span->len; ++x, ++src, ++cmp) {
                *cmp = amaskOp(*src, *cmp, alpha);
            }
        }
    }
}


static void _rasterDirectMaskedRleImageInt(SwSurface* surface, const SwImage* image, uint8_t opacity)
{
    auto span = image->rle->spans;
    auto cbuffer = surface->compositor->image.buf32;
    auto ctride = surface->compositor->image.stride;

    for (uint32_t y = surface->compositor->bbox.min.y; y < surface->compositor->bbox.max.y; ++y) {
        auto cmp = &cbuffer[y * ctride];
        auto x = surface->compositor->bbox.min.x;
        while (x < surface->compositor->bbox.max.x) {
            if (y == span->y && x == span->x && x + span->len <= surface->compositor->bbox.max.x) {
                auto alpha = MULTIPLY(span->coverage, opacity);
                auto src = image->buf32 + (span->y + image->oy) * image->stride + (span->x + image->ox);
                if (alpha == 255) {
                    for (uint32_t i = 0; i < span->len; ++i, ++src) {
                        cmp[x + i] = ALPHA_BLEND(cmp[x + i], A(*src));
                    }
                } else {
                    for (uint32_t i = 0; i < span->len; ++i, ++src) {
                        auto t = ALPHA_BLEND(*src, alpha);
                        cmp[x + i] = ALPHA_BLEND(cmp[x + i], A(t));
                    }
                }
                x += span->len;
                ++span;
            } else {
                cmp[x] = 0;
                ++x;
            }
        }
    }
}


static bool _rasterDirectMaskedRleImage(SwSurface* surface, const SwImage* image, uint8_t opacity)
{
    TVGLOG("SW_ENGINE", "Direct Masked(%d) Rle Image", (int)surface->compositor->method);

    if (surface->compositor->method == CompositeMethod::IntersectMask) {
        _rasterDirectMaskedRleImageInt(surface, image, opacity);
    } else if (auto opMask = _getMaskOp(surface->compositor->method)) {
        //Other Masking operations: Add, Subtract, Difference ...
       _rasterDirectMaskedRleImageDup(surface, image, opMask, _getAMaskOp(surface->compositor->method), opacity);
    } else {
        return false;
    }

    //Masking Composition
    return _rasterDirectImage(surface, &surface->compositor->image, surface->compositor->bbox);
}


static bool _rasterDirectMattedRleImage(SwSurface* surface, const SwImage* image, uint8_t opacity)
{
    TVGLOG("SW_ENGINE", "Direct Matted(%d) Rle Image", (int)surface->compositor->method);

    auto span = image->rle->spans;
    auto csize = surface->compositor->image.channelSize;
    auto cbuffer = surface->compositor->image.buf8;
    auto alpha = surface->alpha(surface->compositor->method);

    for (uint32_t i = 0; i < image->rle->size; ++i, ++span) {
        auto dst = &surface->buf32[span->y * surface->stride + span->x];
        auto cmp = &cbuffer[(span->y * surface->compositor->image.stride + span->x) * csize];
        auto img = image->buf32 + (span->y + image->oy) * image->stride + (span->x + image->ox);
        auto a = MULTIPLY(span->coverage, opacity);
        if (a == 255) {
            for (uint32_t x = 0; x < span->len; ++x, ++dst, ++img, cmp += csize) {
                auto tmp = ALPHA_BLEND(*img, alpha(cmp));
                *dst = tmp + ALPHA_BLEND(*dst, IA(tmp));
            }
        } else {
            for (uint32_t x = 0; x < span->len; ++x, ++dst, ++img, cmp += csize) {
                auto tmp = ALPHA_BLEND(*img, MULTIPLY(a, alpha(cmp)));
                *dst = tmp + ALPHA_BLEND(*dst, IA(tmp));
            }
        }
    }
    return true;
}


static bool _rasterDirectBlendingRleImage(SwSurface* surface, const SwImage* image, uint8_t opacity)
{
    auto span = image->rle->spans;

    for (uint32_t i = 0; i < image->rle->size; ++i, ++span) {
        auto dst = &surface->buf32[span->y * surface->stride + span->x];
        auto img = image->buf32 + (span->y + image->oy) * image->stride + (span->x + image->ox);
        auto alpha = MULTIPLY(span->coverage, opacity);
        if (alpha == 255) {
            for (uint32_t x = 0; x < span->len; ++x, ++dst, ++img) {
                *dst = surface->blender(*img, *dst, IA(*img));
            }
        } else if (opacity == 255) {
            for (uint32_t x = 0; x < span->len; ++x, ++dst, ++img) {
                auto tmp = surface->blender(*img, *dst, 255);
                *dst = INTERPOLATE(tmp, *dst, MULTIPLY(span->coverage, A(*img)));
            }
        } else {
            for (uint32_t x = 0; x < span->len; ++x, ++dst, ++img) {
                auto src = ALPHA_BLEND(*img, opacity);
                auto tmp = surface->blender(src, *dst, IA(src));
                *dst = INTERPOLATE(tmp, *dst, MULTIPLY(span->coverage, A(src)));
            }
        }
    }
    return true;
}


static bool _rasterDirectRleImage(SwSurface* surface, const SwImage* image, uint8_t opacity)
{
    auto span = image->rle->spans;

    for (uint32_t i = 0; i < image->rle->size; ++i, ++span) {
        auto dst = &surface->buf32[span->y * surface->stride + span->x];
        auto img = image->buf32 + (span->y + image->oy) * image->stride + (span->x + image->ox);
        auto alpha = MULTIPLY(span->coverage, opacity);
        if (alpha == 255) {
            for (uint32_t x = 0; x < span->len; ++x, ++dst, ++img) {
                *dst = *img + ALPHA_BLEND(*dst, IA(*img));
            }
        } else {
            for (uint32_t x = 0; x < span->len; ++x, ++dst, ++img) {
                auto src = ALPHA_BLEND(*img, alpha);
                *dst = src + ALPHA_BLEND(*dst, IA(src));
            }
        }
    }
    return true;
}


static bool _directRleImage(SwSurface* surface, const SwImage* image, uint8_t opacity)
{
    if (_compositing(surface)) {
        if (_matting(surface)) return _rasterDirectMattedRleImage(surface, image, opacity);
        else return _rasterDirectMaskedRleImage(surface, image, opacity);
    } else if (_blending(surface)) {
        return _rasterDirectBlendingRleImage(surface, image, opacity);
    } else {
        return _rasterDirectRleImage(surface, image, opacity);
    }
    return false;
}


/************************************************************************/
/* Transformed Image                                                    */
/************************************************************************/

static bool _transformedImage(SwSurface* surface, const SwImage* image, const Matrix* transform, const SwBBox& region, uint8_t opacity)
{
    auto ret = _rasterTexmapPolygon(surface, image, transform, &region, opacity);

    //Masking Composition
    if (_compositing(surface) && _masking(surface)) {
        return _rasterDirectImage(surface, &surface->compositor->image, surface->compositor->bbox);
    }

    return ret;
}


static bool _transformedImageMesh(SwSurface* surface, const SwImage* image, const RenderMesh* mesh, const Matrix* transform, const SwBBox* region, uint8_t opacity)
{
    //TODO: Not completed for all cases.
    return _rasterTexmapPolygonMesh(surface, image, mesh, transform, region, opacity);
}


/************************************************************************/
/*Scaled Image                                                          */
/************************************************************************/

static void _rasterScaledMaskedImageDup(SwSurface* surface, const SwImage* image, const Matrix* itransform, const SwBBox& region, SwBlender maskOp, SwBlender amaskOp, uint8_t opacity)
{
    auto scaleMethod = image->scale < DOWN_SCALE_TOLERANCE ? _interpDownScaler : _interpUpScaler;
    auto sampleSize = _sampleSize(image->scale);
    auto sampleSize2 = sampleSize * sampleSize;
    auto cstride = surface->compositor->image.stride;
    auto cbuffer = surface->compositor->image.buf32 + (region.min.y * cstride + region.min.x);

    for (auto y = region.min.y; y < region.max.y; ++y) {
        auto sy = y * itransform->e22 + itransform->e23;
        if ((uint32_t)sy >= image->h) continue;
        auto cmp = cbuffer;
        if (opacity == 255) {
            for (auto x = region.min.x; x < region.max.x; ++x, ++cmp) {
                auto sx = x * itransform->e11 + itransform->e13;
                if ((uint32_t)sx >= image->w) continue;
                auto src = scaleMethod(image->buf32, image->stride, image->w, image->h, sx, sy, sampleSize, sampleSize2);
                *cmp = maskOp(src, *cmp, IA(src));
            }
        } else {
            for (auto x = region.min.x; x < region.max.x; ++x, ++cmp) {
                auto sx = x * itransform->e11 + itransform->e13;
                if ((uint32_t)sx >= image->w) continue;
                auto src = scaleMethod(image->buf32, image->stride, image->w, image->h, sx, sy, sampleSize, sampleSize2);
                *cmp = amaskOp(src, *cmp, opacity);
            }
        }
        cbuffer += cstride;
    }
}

static void _rasterScaledMaskedImageInt(SwSurface* surface, const SwImage* image, const Matrix* itransform, const SwBBox& region, uint8_t opacity)
{
    auto scaleMethod = image->scale < DOWN_SCALE_TOLERANCE ? _interpDownScaler : _interpUpScaler;
    auto sampleSize = _sampleSize(image->scale);
    auto sampleSize2 = sampleSize * sampleSize;
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto cstride = surface->compositor->image.stride;
    auto cbuffer = surface->compositor->image.buf32 + (surface->compositor->bbox.min.y * cstride + surface->compositor->bbox.min.x);

    for (uint32_t y = surface->compositor->bbox.min.y; y < surface->compositor->bbox.max.y; ++y) {
        if (y == region.min.y) {
            auto cbuffer2 = cbuffer;
            for (uint32_t y2 = y; y2 < region.max.y; ++y2) {
                auto sy = y2 * itransform->e22 + itransform->e23;
                if ((uint32_t)sy >= image->h) continue;
                auto tmp = cbuffer2;
                auto x = surface->compositor->bbox.min.x;
                while (x < surface->compositor->bbox.max.x) {
                    if (x == region.min.x) {
                        if (opacity == 255) {
                            for (uint32_t i = 0; i < w; ++i, ++tmp) {
                                auto sx = (x + i) * itransform->e11 + itransform->e13;
                                if ((uint32_t)sx >= image->w) continue;
                                auto src = scaleMethod(image->buf32, image->stride, image->w, image->h, sx, sy, sampleSize, sampleSize2);
                                *tmp = ALPHA_BLEND(*tmp, A(src));
                            }
                        } else {
                            for (uint32_t i = 0; i < w; ++i, ++tmp) {
                                auto sx = (x + i) * itransform->e11 + itransform->e13;
                                if ((uint32_t)sx >= image->w) continue;
                                auto src = ALPHA_BLEND(scaleMethod(image->buf32, image->stride, image->w, image->h, sx, sy, sampleSize, sampleSize2), opacity);
                                *tmp = ALPHA_BLEND(*tmp, A(src));
                            }
                        }
                        x += w;
                    } else {
                        *tmp = 0;
                        ++tmp;
                        ++x;
                    }
                }
                cbuffer2 += cstride;
            }
            y += (h - 1);
        } else {
            auto tmp = cbuffer;
            for (uint32_t x = surface->compositor->bbox.min.x; x < surface->compositor->bbox.max.x; ++x, ++tmp) {
                *tmp = 0;
            }
        }
        cbuffer += cstride;
    }
}


static bool _rasterScaledMaskedImage(SwSurface* surface, const SwImage* image, const Matrix* itransform, const SwBBox& region, uint8_t opacity)
{
    TVGLOG("SW_ENGINE", "Scaled Masked(%d) Image [Region: %lu %lu %lu %lu]", (int)surface->compositor->method, region.min.x, region.min.y, region.max.x - region.min.x, region.max.y - region.min.y);

    if (surface->compositor->method == CompositeMethod::IntersectMask) {
        _rasterScaledMaskedImageInt(surface, image, itransform, region, opacity);
    } else if (auto opMask = _getMaskOp(surface->compositor->method)) {
        //Other Masking operations: Add, Subtract, Difference ...
       _rasterScaledMaskedImageDup(surface, image, itransform, region, opMask, _getAMaskOp(surface->compositor->method), opacity);
    } else {
        return false;
    }

    //Masking Composition
    return _rasterDirectImage(surface, &surface->compositor->image, surface->compositor->bbox);
}


static bool _rasterScaledMattedImage(SwSurface* surface, const SwImage* image, const Matrix* itransform, const SwBBox& region, uint8_t opacity)
{
    auto dbuffer = surface->buf32 + (region.min.y * surface->stride + region.min.x);
    auto csize = surface->compositor->image.channelSize;
    auto cbuffer = surface->compositor->image.buf8 + (region.min.y * surface->compositor->image.stride + region.min.x) * csize;
    auto alpha = surface->alpha(surface->compositor->method);

    TVGLOG("SW_ENGINE", "Scaled Matted(%d) Image [Region: %lu %lu %lu %lu]", (int)surface->compositor->method, region.min.x, region.min.y, region.max.x - region.min.x, region.max.y - region.min.y);

    auto scaleMethod = image->scale < DOWN_SCALE_TOLERANCE ? _interpDownScaler : _interpUpScaler;
    auto sampleSize = _sampleSize(image->scale);
    auto sampleSize2 = sampleSize * sampleSize;

    for (auto y = region.min.y; y < region.max.y; ++y) {
        auto sy = y * itransform->e22 + itransform->e23;
        if ((uint32_t)sy >= image->h) continue;
        auto dst = dbuffer;
        auto cmp = cbuffer;
        if (opacity == 255) {
            for (auto x = region.min.x; x < region.max.x; ++x, ++dst, cmp += csize) {
                auto sx = x * itransform->e11 + itransform->e13;
                if ((uint32_t)sx >= image->w) continue;
                auto src = scaleMethod(image->buf32, image->stride, image->w, image->h, sx, sy, sampleSize, sampleSize2);
                auto temp = ALPHA_BLEND(src, alpha(cmp));
                *dst = temp + ALPHA_BLEND(*dst, IA(temp));
            }
        } else {
            for (auto x = region.min.x; x < region.max.x; ++x, ++dst, cmp += csize) {
                auto sx = x * itransform->e11 + itransform->e13;
                if ((uint32_t)sx >= image->w) continue;
                auto src = scaleMethod(image->buf32, image->stride, image->w, image->h, sx, sy, sampleSize, sampleSize2);
                auto temp = ALPHA_BLEND(src, MULTIPLY(opacity, alpha(cmp)));
                *dst = temp + ALPHA_BLEND(*dst, IA(temp));
            }
        }
        dbuffer += surface->stride;
        cbuffer += surface->compositor->image.stride * csize;
    }
    return true;
}


static bool _rasterScaledBlendingImage(SwSurface* surface, const SwImage* image, const Matrix* itransform, const SwBBox& region, uint8_t opacity)
{
    auto dbuffer = surface->buf32 + (region.min.y * surface->stride + region.min.x);
    auto scaleMethod = image->scale < DOWN_SCALE_TOLERANCE ? _interpDownScaler : _interpUpScaler;
    auto sampleSize = _sampleSize(image->scale);
    auto sampleSize2 = sampleSize * sampleSize;

    for (auto y = region.min.y; y < region.max.y; ++y, dbuffer += surface->stride) {
        auto sy = y * itransform->e22 + itransform->e23;
        if ((uint32_t)sy >= image->h) continue;
        auto dst = dbuffer;
        if (opacity == 255) {
            for (auto x = region.min.x; x < region.max.x; ++x, ++dst) {
                auto sx = x * itransform->e11 + itransform->e13;
                if ((uint32_t)sx >= image->w) continue;
                auto src = scaleMethod(image->buf32, image->stride, image->w, image->h, sx, sy, sampleSize, sampleSize2);
                auto tmp = surface->blender(src, *dst, 255);
                *dst = INTERPOLATE(tmp, *dst, A(src));
            }
        } else {
            for (auto x = region.min.x; x < region.max.x; ++x, ++dst) {
                auto sx = x * itransform->e11 + itransform->e13;
                if ((uint32_t)sx >= image->w) continue;
                auto src = ALPHA_BLEND(scaleMethod(image->buf32, image->stride, image->w, image->h, sx, sy, sampleSize, sampleSize2), opacity);
                auto tmp = surface->blender(src, *dst, 255);
                *dst = INTERPOLATE(tmp, *dst, A(src));
            }
        }
    }
    return true;
}


static bool _rasterScaledImage(SwSurface* surface, const SwImage* image, const Matrix* itransform, const SwBBox& region, uint8_t opacity)
{
    auto dbuffer = surface->buf32 + (region.min.y * surface->stride + region.min.x);
    auto scaleMethod = image->scale < DOWN_SCALE_TOLERANCE ? _interpDownScaler : _interpUpScaler;
    auto sampleSize = _sampleSize(image->scale);
    auto sampleSize2 = sampleSize * sampleSize;

    for (auto y = region.min.y; y < region.max.y; ++y, dbuffer += surface->stride) {
        auto sy = y * itransform->e22 + itransform->e23;
        if ((uint32_t)sy >= image->h) continue;
        auto dst = dbuffer;
        if (opacity == 255) {
            for (auto x = region.min.x; x < region.max.x; ++x, ++dst) {
                auto sx = x * itransform->e11 + itransform->e13;
                if ((uint32_t)sx >= image->w) continue;
                auto src = scaleMethod(image->buf32, image->stride, image->w, image->h, sx, sy, sampleSize, sampleSize2);
                *dst = src + ALPHA_BLEND(*dst, IA(src));
            }
        } else {
            for (auto x = region.min.x; x < region.max.x; ++x, ++dst) {
                auto sx = x * itransform->e11 + itransform->e13;
                if ((uint32_t)sx >= image->w) continue;
                auto src = ALPHA_BLEND(scaleMethod(image->buf32, image->stride, image->w, image->h, sx, sy, sampleSize, sampleSize2), opacity);
                *dst = src + ALPHA_BLEND(*dst, IA(src));
            }
        }
    }
    return true;
}


static bool _scaledImage(SwSurface* surface, const SwImage* image, const Matrix* transform, const SwBBox& region, uint8_t opacity)
{
    Matrix itransform;

    if (transform) {
        if (!mathInverse(transform, &itransform)) return false;
    } else mathIdentity(&itransform);

    if (_compositing(surface)) {
        if (_matting(surface)) return _rasterScaledMattedImage(surface, image, &itransform, region, opacity);
        else return _rasterScaledMaskedImage(surface, image, &itransform, region, opacity);
    } else if (_blending(surface)) {
        return _rasterScaledBlendingImage(surface, image, &itransform, region, opacity);
    } else {
        return _rasterScaledImage(surface, image, &itransform, region, opacity);
    }
    return false;
}


/************************************************************************/
/* Direct Image                                                         */
/************************************************************************/

static void _rasterDirectMaskedImageDup(SwSurface* surface, const SwImage* image, const SwBBox& region, SwBlender maskOp, SwBlender amaskOp, uint8_t opacity)
{
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto cstride = surface->compositor->image.stride;

    auto cbuffer = surface->compositor->image.buf32 + (region.min.y * cstride + region.min.x); //compositor buffer
    auto sbuffer = image->buf32 + (region.min.y + image->oy) * image->stride + (region.min.x + image->ox);

    for (uint32_t y = 0; y < h; ++y) {
        auto cmp = cbuffer;
        auto src = sbuffer;
        if (opacity == 255) {
            for (uint32_t x = 0; x < w; ++x, ++src, ++cmp) {
                *cmp = maskOp(*src, *cmp, IA(*src));
            }
        } else {
            for (uint32_t x = 0; x < w; ++x, ++src, ++cmp) {
                *cmp = amaskOp(*src, *cmp, opacity);
            }
        }
        cbuffer += cstride;
        sbuffer += image->stride;
    }
}


static void _rasterDirectMaskedImageInt(SwSurface* surface, const SwImage* image, const SwBBox& region, uint8_t opacity)
{
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto cstride = surface->compositor->image.stride;
    auto cbuffer = surface->compositor->image.buf32 + (surface->compositor->bbox.min.y * cstride + surface->compositor->bbox.min.x);

    for (uint32_t y = surface->compositor->bbox.min.y; y < surface->compositor->bbox.max.y; ++y) {
        if (y == region.min.y) {
            auto cbuffer2 = cbuffer;
            for (uint32_t y2 = y; y2 < region.max.y; ++y2) {
                auto tmp = cbuffer2;
                auto x = surface->compositor->bbox.min.x;
                while (x < surface->compositor->bbox.max.x) {
                    if (x == region.min.x) {
                        auto src = &image->buf32[(y2 + image->oy) * image->stride + (x + image->ox)];
                        if (opacity == 255) {
                            for (uint32_t i = 0; i < w; ++i, ++tmp, ++src) {
                                *tmp = ALPHA_BLEND(*tmp, A(*src));
                            }
                        } else {
                            for (uint32_t i = 0; i < w; ++i, ++tmp, ++src) {
                                auto t = ALPHA_BLEND(*src, opacity);
                                *tmp = ALPHA_BLEND(*tmp, A(t));
                            }
                        }
                        x += w;
                    } else {
                        *tmp = 0;
                        ++tmp;
                        ++x;
                    }
                }
                cbuffer2 += cstride;
            }
            y += (h - 1);
        } else {
            rasterPixel32(cbuffer, 0x00000000, 0, surface->compositor->bbox.max.x - surface->compositor->bbox.min.x);
        }
        cbuffer += cstride;
    }
}


static bool _rasterDirectMaskedImage(SwSurface* surface, const SwImage* image, const SwBBox& region, uint8_t opacity)
{
    TVGLOG("SW_ENGINE", "Direct Masked(%d) Image  [Region: %lu %lu %lu %lu]", (int)surface->compositor->method, region.min.x, region.min.y, region.max.x - region.min.x, region.max.y - region.min.y);

    if (surface->compositor->method == CompositeMethod::IntersectMask) {
        _rasterDirectMaskedImageInt(surface, image, region, opacity);
    } else if (auto opMask = _getMaskOp(surface->compositor->method)) {
        //Other Masking operations: Add, Subtract, Difference ...
       _rasterDirectMaskedImageDup(surface, image, region, opMask, _getAMaskOp(surface->compositor->method), opacity);
    } else {
        return false;
    }
    //Masking Composition
    return _rasterDirectImage(surface, &surface->compositor->image, surface->compositor->bbox);
}


static bool _rasterDirectMattedImage(SwSurface* surface, const SwImage* image, const SwBBox& region, uint8_t opacity)
{
    auto buffer = surface->buf32 + (region.min.y * surface->stride) + region.min.x;
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto csize = surface->compositor->image.channelSize;
    auto alpha = surface->alpha(surface->compositor->method);

    TVGLOG("SW_ENGINE", "Direct Matted(%d) Image  [Region: %lu %lu %u %u]", (int)surface->compositor->method, region.min.x, region.min.y, w, h);

    auto sbuffer = image->buf32 + (region.min.y + image->oy) * image->stride + (region.min.x + image->ox);
    auto cbuffer = surface->compositor->image.buf8 + (region.min.y * surface->compositor->image.stride + region.min.x) * csize; //compositor buffer

    for (uint32_t y = 0; y < h; ++y) {
        auto dst = buffer;
        auto cmp = cbuffer;
        auto src = sbuffer;
        if (opacity == 255) {
            for (uint32_t x = 0; x < w; ++x, ++dst, ++src, cmp += csize) {
                auto tmp = ALPHA_BLEND(*src, alpha(cmp));
                *dst = tmp + ALPHA_BLEND(*dst, IA(tmp));
            }
        } else {
            for (uint32_t x = 0; x < w; ++x, ++dst, ++src, cmp += csize) {
                auto tmp = ALPHA_BLEND(*src, MULTIPLY(opacity, alpha(cmp)));
                *dst = tmp + ALPHA_BLEND(*dst, IA(tmp));
            }
        }
        buffer += surface->stride;
        cbuffer += surface->compositor->image.stride * csize;
        sbuffer += image->stride;
    }
    return true;
}


static bool _rasterDirectBlendingImage(SwSurface* surface, const SwImage* image, const SwBBox& region, uint8_t opacity)
{
    auto dbuffer = &surface->buf32[region.min.y * surface->stride + region.min.x];
    auto sbuffer = image->buf32 + (region.min.y + image->oy) * image->stride + (region.min.x + image->ox);

    for (auto y = region.min.y; y < region.max.y; ++y) {
        auto dst = dbuffer;
        auto src = sbuffer;
        if (opacity == 255) {
            for (auto x = region.min.x; x < region.max.x; x++, dst++, src++) {
                auto tmp = surface->blender(*src, *dst, 255);
                *dst = INTERPOLATE(tmp, *dst, A(*src));
            }
        } else {
            for (auto x = region.min.x; x < region.max.x; ++x, ++dst, ++src) {
                auto tmp = ALPHA_BLEND(*src, opacity);
                auto tmp2 = surface->blender(tmp, *dst, 255);
                *dst = INTERPOLATE(tmp2, *dst, A(tmp));
            }
        }
        dbuffer += surface->stride;
        sbuffer += image->stride;
    }
    return true;
}


static bool _rasterDirectImage(SwSurface* surface, const SwImage* image, const SwBBox& region, uint8_t opacity)
{
    auto dbuffer = &surface->buf32[region.min.y * surface->stride + region.min.x];
    auto sbuffer = image->buf32 + (region.min.y + image->oy) * image->stride + (region.min.x + image->ox);

    for (auto y = region.min.y; y < region.max.y; ++y) {
        auto dst = dbuffer;
        auto src = sbuffer;
        if (opacity == 255) {
            for (auto x = region.min.x; x < region.max.x; x++, dst++, src++) {
                *dst = *src + ALPHA_BLEND(*dst, IA(*src));
            }
        } else {
            for (auto x = region.min.x; x < region.max.x; ++x, ++dst, ++src) {
                auto tmp = ALPHA_BLEND(*src, opacity);
                *dst = tmp + ALPHA_BLEND(*dst, IA(tmp));
            }
        }
        dbuffer += surface->stride;
        sbuffer += image->stride;
    }
    return true;
}


//Blenders for the following scenarios: [Composition / Non-Composition] * [Opaque / Translucent]
static bool _directImage(SwSurface* surface, const SwImage* image, const SwBBox& region, uint8_t opacity)
{
    if (_compositing(surface)) {
        if (_matting(surface)) return _rasterDirectMattedImage(surface, image, region, opacity);
        else return _rasterDirectMaskedImage(surface, image, region, opacity);
    } else if (_blending(surface)) {
        return _rasterDirectBlendingImage(surface, image, region, opacity);
    } else {
        return _rasterDirectImage(surface, image, region, opacity);
    }
    return false;
}


//Blenders for the following scenarios: [RLE / Whole] * [Direct / Scaled / Transformed]
static bool _rasterImage(SwSurface* surface, SwImage* image, const Matrix* transform, const SwBBox& region, uint8_t opacity)
{
    //RLE Image
    if (image->rle) {
        if (image->direct) return _directRleImage(surface, image, opacity);
        else if (image->scaled) return _scaledRleImage(surface, image, transform, region, opacity);
        else return _transformedRleImage(surface, image, transform, opacity);
    //Whole Image
    } else {
        if (image->direct) return _directImage(surface, image, region, opacity);
        else if (image->scaled) return _scaledImage(surface, image, transform, region, opacity);
        else return _transformedImage(surface, image, transform, region, opacity);
    }
}


/************************************************************************/
/* Rect Gradient                                                        */
/************************************************************************/

template<typename fillMethod>
static void _rasterGradientMaskedRectDup(SwSurface* surface, const SwBBox& region, const SwFill* fill, SwBlender maskOp)
{
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto cstride = surface->compositor->image.stride;
    auto cbuffer = surface->compositor->image.buf32 + (region.min.y * cstride + region.min.x);

    for (uint32_t y = 0; y < h; ++y) {
        fillMethod()(fill, cbuffer, region.min.y + y, region.min.x, w, maskOp, 255);
        cbuffer += surface->stride;
    }
}


template<typename fillMethod>
static void _rasterGradientMaskedRectInt(SwSurface* surface, const SwBBox& region, const SwFill* fill)
{
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto cstride = surface->compositor->image.stride;

    for (uint32_t y = surface->compositor->bbox.min.y; y < surface->compositor->bbox.max.y; ++y) {
        auto cmp = surface->compositor->image.buf32 + (y * cstride + surface->compositor->bbox.min.x);
        if (y == region.min.y) {
            for (uint32_t y2 = y; y2 < region.max.y; ++y2) {
                auto tmp = cmp;
                auto x = surface->compositor->bbox.min.x;
                while (x < surface->compositor->bbox.max.x) {
                    if (x == region.min.x) {
                        fillMethod()(fill, tmp, y2, x, w, opMaskPreIntersect, 255);
                        x += w;
                        tmp += w;
                    } else {
                        *tmp = 0;
                        ++tmp;
                        ++x;
                    }
                }
                cmp += cstride;
            }
            y += (h - 1);
        } else {
            rasterPixel32(cmp, 0x00000000, 0, surface->compositor->bbox.max.x -surface->compositor->bbox.min.x);
            cmp += cstride;
        }
    }
}


template<typename fillMethod>
static bool _rasterGradientMaskedRect(SwSurface* surface, const SwBBox& region, const SwFill* fill)
{
    auto method = surface->compositor->method;

    TVGLOG("SW_ENGINE", "Masked(%d) Gradient [Region: %lu %lu %lu %lu]", (int)surface->compositor->method, region.min.x, region.min.y, region.max.x - region.min.x, region.max.y - region.min.y);

    if (method == CompositeMethod::AddMask) _rasterGradientMaskedRectDup<fillMethod>(surface, region, fill, opMaskPreAdd);
    else if (method == CompositeMethod::SubtractMask) _rasterGradientMaskedRectDup<fillMethod>(surface, region, fill, opMaskPreSubtract);
    else if (method == CompositeMethod::DifferenceMask) _rasterGradientMaskedRectDup<fillMethod>(surface, region, fill, opMaskPreDifference);
    else if (method == CompositeMethod::IntersectMask) _rasterGradientMaskedRectInt<fillMethod>(surface, region, fill);
    else return false;

    //Masking Composition
    return _rasterDirectImage(surface, &surface->compositor->image, surface->compositor->bbox, 255);
}


template<typename fillMethod>
static bool _rasterGradientMattedRect(SwSurface* surface, const SwBBox& region, const SwFill* fill)
{
    auto buffer = surface->buf32 + (region.min.y * surface->stride) + region.min.x;
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto csize = surface->compositor->image.channelSize;
    auto cbuffer = surface->compositor->image.buf8 + (region.min.y * surface->compositor->image.stride + region.min.x) * csize;
    auto alpha = surface->alpha(surface->compositor->method);

    TVGLOG("SW_ENGINE", "Matted(%d) Gradient [Region: %lu %lu %u %u]", (int)surface->compositor->method, region.min.x, region.min.y, w, h);

    for (uint32_t y = 0; y < h; ++y) {
        fillMethod()(fill, buffer, region.min.y + y, region.min.x, w, cbuffer, alpha, csize, 255);
        buffer += surface->stride;
        cbuffer += surface->stride * csize;
    }
    return true;
}


template<typename fillMethod>
static bool _rasterBlendingGradientRect(SwSurface* surface, const SwBBox& region, const SwFill* fill)
{
    auto buffer = surface->buf32 + (region.min.y * surface->stride) + region.min.x;
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);

    if (fill->translucent) {
        for (uint32_t y = 0; y < h; ++y) {
            fillMethod()(fill, buffer + y * surface->stride, region.min.y + y, region.min.x, w, opBlendPreNormal, surface->blender, 255);
        }
    } else {
        for (uint32_t y = 0; y < h; ++y) {
            fillMethod()(fill, buffer + y * surface->stride, region.min.y + y, region.min.x, w, opBlendSrcOver, surface->blender, 255);
        }
    }
    return true;
}

template<typename fillMethod>
static bool _rasterTranslucentGradientRect(SwSurface* surface, const SwBBox& region, const SwFill* fill)
{
    auto buffer = surface->buf32 + (region.min.y * surface->stride) + region.min.x;
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);

    for (uint32_t y = 0; y < h; ++y) {
        fillMethod()(fill, buffer, region.min.y + y, region.min.x, w, opBlendPreNormal, 255);
        buffer += surface->stride;
    }
    return true;
}


template<typename fillMethod>
static bool _rasterSolidGradientRect(SwSurface* surface, const SwBBox& region, const SwFill* fill)
{
    auto buffer = surface->buf32 + (region.min.y * surface->stride) + region.min.x;
    auto w = static_cast<uint32_t>(region.max.x - region.min.x);
    auto h = static_cast<uint32_t>(region.max.y - region.min.y);

    for (uint32_t y = 0; y < h; ++y) {
        fillMethod()(fill, buffer + y * surface->stride, region.min.y + y, region.min.x, w, opBlendSrcOver, 255);
    }
    return true;
}


static bool _rasterLinearGradientRect(SwSurface* surface, const SwBBox& region, const SwFill* fill)
{
    if (fill->linear.len < FLT_EPSILON) return false;

    if (_compositing(surface)) {
        if (_matting(surface)) return _rasterGradientMattedRect<FillLinear>(surface, region, fill);
        else return _rasterGradientMaskedRect<FillLinear>(surface, region, fill);
    } else if (_blending(surface)) {
        return _rasterBlendingGradientRect<FillLinear>(surface, region, fill);
    } else {
        if (fill->translucent) return _rasterTranslucentGradientRect<FillLinear>(surface, region, fill);
        else _rasterSolidGradientRect<FillLinear>(surface, region, fill);
    }
    return false;
}


static bool _rasterRadialGradientRect(SwSurface* surface, const SwBBox& region, const SwFill* fill)
{
    if (fill->radial.a < FLT_EPSILON) return false;

    if (_compositing(surface)) {
        if (_matting(surface)) return _rasterGradientMattedRect<FillRadial>(surface, region, fill);
        else return _rasterGradientMaskedRect<FillRadial>(surface, region, fill);
    } else if (_blending(surface)) {
        return _rasterBlendingGradientRect<FillRadial>(surface, region, fill);
    } else {
        if (fill->translucent) return _rasterTranslucentGradientRect<FillRadial>(surface, region, fill);
        else _rasterSolidGradientRect<FillRadial>(surface, region, fill);
    }
    return false;
}



/************************************************************************/
/* Rle Gradient                                                         */
/************************************************************************/

template<typename fillMethod>
static void _rasterGradientMaskedRleDup(SwSurface* surface, const SwRleData* rle, const SwFill* fill, SwBlender maskOp)
{
    auto span = rle->spans;
    auto cstride = surface->compositor->image.stride;
    auto cbuffer = surface->compositor->image.buf32;

    for (uint32_t i = 0; i < rle->size; ++i, ++span) {
        auto cmp = &cbuffer[span->y * cstride + span->x];
        fillMethod()(fill, cmp, span->y, span->x, span->len, maskOp, span->coverage);
    }
}


template<typename fillMethod>
static void _rasterGradientMaskedRleInt(SwSurface* surface, const SwRleData* rle, const SwFill* fill)
{
    auto span = rle->spans;
    auto cstride = surface->compositor->image.stride;
    auto cbuffer = surface->compositor->image.buf32;

    for (uint32_t y = surface->compositor->bbox.min.y; y < surface->compositor->bbox.max.y; ++y) {
        auto cmp = &cbuffer[y * cstride];
        uint32_t x = surface->compositor->bbox.min.x;
        while (x < surface->compositor->bbox.max.x) {
            if (y == span->y && x == span->x && x + span->len <= surface->compositor->bbox.max.x) {
                fillMethod()(fill, cmp, span->y, span->x, span->len, opMaskIntersect, span->coverage);
                x += span->len;
                ++span;
            } else {
                cmp[x] = 0;
                ++x;
            }
        }
    }
}


template<typename fillMethod>
static bool _rasterGradientMaskedRle(SwSurface* surface, const SwRleData* rle, const SwFill* fill)
{
    TVGLOG("SW_ENGINE", "Masked(%d) Rle Linear Gradient", (int)surface->compositor->method);

    auto method = surface->compositor->method;

    if (method == CompositeMethod::AddMask) _rasterGradientMaskedRleDup<fillMethod>(surface, rle, fill, opMaskAdd);
    else if (method == CompositeMethod::SubtractMask) _rasterGradientMaskedRleDup<fillMethod>(surface, rle, fill, opMaskSubtract);
    else if (method == CompositeMethod::DifferenceMask) _rasterGradientMaskedRleDup<fillMethod>(surface, rle, fill, opMaskDifference);
    else if (method == CompositeMethod::IntersectMask) _rasterGradientMaskedRleInt<fillMethod>(surface, rle, fill);
    else return false;

    //Masking Composition
    return _rasterDirectImage(surface, &surface->compositor->image, surface->compositor->bbox, 255);
}


template<typename fillMethod>
static bool _rasterGradientMattedRle(SwSurface* surface, const SwRleData* rle, const SwFill* fill)
{
    TVGLOG("SW_ENGINE", "Matted(%d) Rle Linear Gradient", (int)surface->compositor->method);

    auto span = rle->spans;
    auto csize = surface->compositor->image.channelSize;
    auto cbuffer = surface->compositor->image.buf8;
    auto alpha = surface->alpha(surface->compositor->method);

    for (uint32_t i = 0; i < rle->size; ++i, ++span) {
        auto dst = &surface->buf32[span->y * surface->stride + span->x];
        auto cmp = &cbuffer[(span->y * surface->compositor->image.stride + span->x) * csize];
        fillMethod()(fill, dst, span->y, span->x, span->len, cmp, alpha, csize, span->coverage);
    }
    return true;
}


template<typename fillMethod>
static bool _rasterBlendingGradientRle(SwSurface* surface, const SwRleData* rle, const SwFill* fill)
{
    auto span = rle->spans;

    for (uint32_t i = 0; i < rle->size; ++i, ++span) {
        auto dst = &surface->buf32[span->y * surface->stride + span->x];
        fillMethod()(fill, dst, span->y, span->x, span->len, opBlendPreNormal, surface->blender, span->coverage);
    }
    return true;
}


template<typename fillMethod>
static bool _rasterTranslucentGradientRle(SwSurface* surface, const SwRleData* rle, const SwFill* fill)
{
    auto span = rle->spans;

    for (uint32_t i = 0; i < rle->size; ++i, ++span) {
        auto dst = &surface->buf32[span->y * surface->stride + span->x];
        if (span->coverage == 255) fillMethod()(fill, dst, span->y, span->x, span->len, opBlendPreNormal, 255);
        else fillMethod()(fill, dst, span->y, span->x, span->len, opBlendNormal, span->coverage);
    }
    return true;
}


template<typename fillMethod>
static bool _rasterSolidGradientRle(SwSurface* surface, const SwRleData* rle, const SwFill* fill)
{
    auto span = rle->spans;

    for (uint32_t i = 0; i < rle->size; ++i, ++span) {
        auto dst = &surface->buf32[span->y * surface->stride + span->x];
        if (span->coverage == 255) fillMethod()(fill, dst, span->y, span->x, span->len, opBlendSrcOver, 255);
        else fillMethod()(fill, dst, span->y, span->x, span->len, opBlendInterp, span->coverage);
    }
    return true;
}


static bool _rasterLinearGradientRle(SwSurface* surface, const SwRleData* rle, const SwFill* fill)
{
    if (!rle || fill->linear.len < FLT_EPSILON) return false;

    if (_compositing(surface)) {
        if (_matting(surface)) return _rasterGradientMattedRle<FillLinear>(surface, rle, fill);
        else return _rasterGradientMaskedRle<FillLinear>(surface, rle, fill);
    } else if (_blending(surface)) {
        return _rasterBlendingGradientRle<FillLinear>(surface, rle, fill);
    } else {
        if (fill->translucent) return _rasterTranslucentGradientRle<FillLinear>(surface, rle, fill);
        else return _rasterSolidGradientRle<FillLinear>(surface, rle, fill);
    }
    return false;
}


static bool _rasterRadialGradientRle(SwSurface* surface, const SwRleData* rle, const SwFill* fill)
{
    if (!rle || fill->radial.a < FLT_EPSILON) return false;

    if (_compositing(surface)) {
        if (_matting(surface)) return _rasterGradientMattedRle<FillRadial>(surface, rle, fill);
        else return _rasterGradientMaskedRle<FillRadial>(surface, rle, fill);
    } else if (_blending(surface)) {
        _rasterBlendingGradientRle<FillRadial>(surface, rle, fill);
    } else {
        if (fill->translucent) _rasterTranslucentGradientRle<FillRadial>(surface, rle, fill);
        else return _rasterSolidGradientRle<FillRadial>(surface, rle, fill);
    }
    return false;
}


/************************************************************************/
/* External Class Implementation                                        */
/************************************************************************/


void rasterGrayscale8(uint8_t *dst, uint8_t val, uint32_t offset, int32_t len)
{
    //OPTIMIZE_ME: Support SIMD
    cRasterPixels(dst, val, offset, len);
}


void rasterPixel32(uint32_t *dst, uint32_t val, uint32_t offset, int32_t len)
{
#if defined(THORVG_AVX_VECTOR_SUPPORT)
    avxRasterPixel32(dst, val, offset, len);
#elif defined(THORVG_NEON_VECTOR_SUPPORT)
    neonRasterPixel32(dst, val, offset, len);
#else
    cRasterPixels(dst, val, offset, len);
#endif
}


bool rasterCompositor(SwSurface* surface)
{
    //See CompositeMethod, Alpha:3, InvAlpha:4, Luma:5, InvLuma:6
    surface->alphas[0] = _alpha;
    surface->alphas[1] = _ialpha;

    if (surface->cs == ColorSpace::ABGR8888 || surface->cs == ColorSpace::ABGR8888S) {
        surface->join = _abgrJoin;
        surface->alphas[2] = _abgrLuma;
        surface->alphas[3] = _abgrInvLuma;
    } else if (surface->cs == ColorSpace::ARGB8888 || surface->cs == ColorSpace::ARGB8888S) {
        surface->join = _argbJoin;
        surface->alphas[2] = _argbLuma;
        surface->alphas[3] = _argbInvLuma;
    } else {
        TVGERR("SW_ENGINE", "Unsupported Colorspace(%d) is expected!", surface->cs);
        return false;
    }
    return true;
}


bool rasterClear(SwSurface* surface, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!surface || !surface->buf32 || surface->stride == 0 || surface->w == 0 || surface->h == 0) return false;

    //32 bits
    if (surface->channelSize == sizeof(uint32_t)) {
        //full clear
        if (w == surface->stride) {
            rasterPixel32(surface->buf32, 0x00000000, surface->stride * y, w * h);
        //partial clear
        } else {
            for (uint32_t i = 0; i < h; i++) {
                rasterPixel32(surface->buf32, 0x00000000, (surface->stride * y + x) + (surface->stride * i), w);
            }
        }
    //8 bits
    } else if (surface->channelSize == sizeof(uint8_t)) {
        //full clear
        if (w == surface->stride) {
            rasterGrayscale8(surface->buf8, 0x00, surface->stride * y, w * h);
        //partial clear
        } else {
            for (uint32_t i = 0; i < h; i++) {
                rasterGrayscale8(surface->buf8, 0x00, (surface->stride * y + x) + (surface->stride * i), w);
            }
        }
    }
    return true;
}


void rasterUnpremultiply(Surface* surface)
{
    if (surface->channelSize != sizeof(uint32_t)) return;

    TVGLOG("SW_ENGINE", "Unpremultiply [Size: %d x %d]", surface->w, surface->h);

    //OPTIMIZE_ME: +SIMD
    for (uint32_t y = 0; y < surface->h; y++) {
        auto buffer = surface->buf32 + surface->stride * y;
        for (uint32_t x = 0; x < surface->w; ++x) {
            uint8_t a = buffer[x] >> 24;
            if (a == 255) {
                continue;
            } else if (a == 0) {
                buffer[x] = 0x00ffffff;
            } else {
                uint16_t r = ((buffer[x] >> 8) & 0xff00) / a;
                uint16_t g = ((buffer[x]) & 0xff00) / a;
                uint16_t b = ((buffer[x] << 8) & 0xff00) / a;
                if (r > 0xff) r = 0xff;
                if (g > 0xff) g = 0xff;
                if (b > 0xff) b = 0xff;
                buffer[x] = (a << 24) | (r << 16) | (g << 8) | (b);
            }
        }
    }
    surface->premultiplied = false;
}


void rasterPremultiply(Surface* surface)
{
    if (surface->channelSize != sizeof(uint32_t)) return;

    TVGLOG("SW_ENGINE", "Premultiply [Size: %d x %d]", surface->w, surface->h);

    //OPTIMIZE_ME: +SIMD
    auto buffer = surface->buf32;
    for (uint32_t y = 0; y < surface->h; ++y, buffer += surface->stride) {
        auto dst = buffer;
        for (uint32_t x = 0; x < surface->w; ++x, ++dst) {
            auto c = *dst;
            auto a = (c >> 24);
            *dst = (c & 0xff000000) + ((((c >> 8) & 0xff) * a) & 0xff00) + ((((c & 0x00ff00ff) * a) >> 8) & 0x00ff00ff);
        }
    }
    surface->premultiplied = true;
}


bool rasterGradientShape(SwSurface* surface, SwShape* shape, unsigned id)
{
    if (surface->channelSize == sizeof(uint8_t)) {
        TVGERR("SW_ENGINE", "Not supported grayscale gradient!");
        return false;
    }

    if (!shape->fill) return false;

    if (shape->fastTrack) {
        if (id == TVG_CLASS_ID_LINEAR) return _rasterLinearGradientRect(surface, shape->bbox, shape->fill);
        else if (id == TVG_CLASS_ID_RADIAL)return _rasterRadialGradientRect(surface, shape->bbox, shape->fill);
    } else {
        if (id == TVG_CLASS_ID_LINEAR) return _rasterLinearGradientRle(surface, shape->rle, shape->fill);
        else if (id == TVG_CLASS_ID_RADIAL) return _rasterRadialGradientRle(surface, shape->rle, shape->fill);
    }
    return false;
}


bool rasterGradientStroke(SwSurface* surface, SwShape* shape, unsigned id)
{
    if (surface->channelSize == sizeof(uint8_t)) {
        TVGERR("SW_ENGINE", "Not supported grayscale gradient!");
        return false;
    }

    if (!shape->stroke || !shape->stroke->fill || !shape->strokeRle) return false;

    if (id == TVG_CLASS_ID_LINEAR) return _rasterLinearGradientRle(surface, shape->strokeRle, shape->stroke->fill);
    else if (id == TVG_CLASS_ID_RADIAL) return _rasterRadialGradientRle(surface, shape->strokeRle, shape->stroke->fill);

    return false;
}


bool rasterShape(SwSurface* surface, SwShape* shape, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (a < 255) {
        r = MULTIPLY(r, a);
        g = MULTIPLY(g, a);
        b = MULTIPLY(b, a);
    }

    if (shape->fastTrack) return _rasterRect(surface, shape->bbox, r, g, b, a);
    else return _rasterRle(surface, shape->rle, r, g, b, a);
}


bool rasterStroke(SwSurface* surface, SwShape* shape, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (a < 255) {
        r = MULTIPLY(r, a);
        g = MULTIPLY(g, a);
        b = MULTIPLY(b, a);
    }

    return _rasterRle(surface, shape->strokeRle, r, g, b, a);
}


bool rasterImage(SwSurface* surface, SwImage* image, const RenderMesh* mesh, const Matrix* transform, const SwBBox& bbox, uint8_t opacity)
{
    if (surface->channelSize == sizeof(uint8_t)) {
        TVGERR("SW_ENGINE", "Not supported grayscale image!");
        return false;
    }

    //Verify Boundary
    if (bbox.max.x < 0 || bbox.max.y < 0 || bbox.min.x >= static_cast<SwCoord>(surface->w) || bbox.min.y >= static_cast<SwCoord>(surface->h)) return false;

    //TOOD: switch (image->format)
    //TODO: case: _rasterRGBImageMesh()
    //TODO: case: _rasterGrayscaleImageMesh()
    //TODO: case: _rasterAlphaImageMesh()
    if (mesh && mesh->triangleCnt > 0) return _transformedImageMesh(surface, image, mesh, transform, &bbox, opacity);
    else return _rasterImage(surface, image, transform, bbox, opacity);
}


bool rasterConvertCS(Surface* surface, ColorSpace to)
{
    //TOOD: Support SIMD accelerations
    auto from = surface->cs;

    if ((from == ColorSpace::ABGR8888 && to == ColorSpace::ARGB8888) || (from == ColorSpace::ABGR8888S && to == ColorSpace::ARGB8888S)) {
        surface->cs = to;
        return cRasterABGRtoARGB(surface);
    }
    if ((from == ColorSpace::ARGB8888 && to == ColorSpace::ABGR8888) || (from == ColorSpace::ARGB8888S && to == ColorSpace::ABGR8888S)) {
        surface->cs = to;
        return cRasterARGBtoABGR(surface);
    }

    return false;
}