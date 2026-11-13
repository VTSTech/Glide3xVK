// g3x_blit_quad.h — the tile-class textured quad blit (CPU-route fast path).
//
// Restored from the pre-rollback g3x_raster_core.h (P13 perf) after the
// Route-B removal took the g3xrc namespace with it. The logic is the
// original, verbatim — only the interface shrank: the host no longer needs
// constant()/blend_sf()/blend_df()/palette() (the exec never used them), the
// GEN blend case dispatches to st.blend_px(dst, src), and the blend class
// arrives as the numeric code 0..4 (see blit_blend_class below — the frame
// module's BlendClass enum has the same values in the same order, pinned by
// static_assert in g3x_frame.cpp).
//
// Axis-aligned, integer-corner, 1:1-mapped P8/RGB565 quad — the ground/wall/UI
// tile class the P2.7 square-space pin establishes (texel = coord * 2^lod/256
// with the quad spanning exactly the texture's texel extent, so one texel per
// pixel). The barycentric path draws this class identically; the exec below
// strips the float machinery out of the inner loop:
//   - coverage for integer corners collapses to pixel centers strictly inside
//     [x0,x1) x [y0,y1) — every center is >= 0.5px from every edge, so the
//     top-left ownership rule can never fire;
//   - u depends only on x and v only on y (per-corner consistency check), and
//     the per-pixel TEXEL step must be bit-exactly +1.0f (du * tsx == 1.0f),
//     so the row walk becomes two integer counters reproducing fill_span's
//     accumulated floors;
//   - the first covered pixel's texel coordinate must sit >= 1e-3 texels away
//     from an integer boundary (true 1:1 quads sample at texel centers,
//     frac == 0.5) — this rejects the "exact float boundary" class where the
//     barycentric evaluation's few-ulp noise could legally pick either texel;
//   - a verification walk replays the accumulated u/v floors across the whole
//     span (O(w+h), noise next to the fill) proving the counters stay +1.
// ANY failed check returns false and the caller runs the ordinary
// per-triangle path — identical output either way (the differential fuzz in
// scripts/fastpath_fuzz.cpp pins fast == exact against the CURRENT
// fill_span/raster_tri semantics).
//
// `strip` selects the triangulation the dispatcher will run (fan:
// (0,1,2)+(0,2,3), strip: (0,1,2)+(1,2,3) with per-tri winding normalize).
// The two triangles cover the full rect only when their SHARED edge is a
// diagonal (both corner bits differ); the exec rejects any other vertex
// order (its union misses part of the rect), keeping those on the exact
// path.
#ifndef G3X_BLIT_QUAD_H
#define G3X_BLIT_QUAD_H

#include <cstdint>
#include <cstddef>
#include <cmath>

namespace g3xbq {

// Vertex — layout-identical to g3x_frame.cpp's RVert {float x,y,u,v; uint32_t
// argb} (pinned by static_assert there); the frame module casts between them.
struct Vert { float x, y, u, v; uint32_t argb; };

struct TexView {
    const uint8_t* data;
    unsigned w, h, lod, bpp;
};

// Numeric blend-class codes (== g3x_frame.cpp's BlendClass enumerator values).
enum : unsigned {
    BQ_OPAQUE = 0,   // blend_px: src | 0xFF000000
    BQ_ADD    = 1,   // dst + src, clamped
    BQ_LERP   = 2,   // (src*a + dst*(255-a) + rounding) per lane
    BQ_MUL    = 3,   // (dst*src + rounding) per lane
    BQ_GEN    = 4,   // the exact per-pixel generic blend -> st.blend_px
};

// Host contract (the frame module's FrameHost / the fuzz harness's mock):
//   unsigned width() const; unsigned height() const;
//   uint32_t* px();                      // framebuffer, row stride == width
//   bool color_fill() const;
//   unsigned blend_class() const;        // BQ_* code
//   unsigned const_alpha() const;
//   bool chroma_on() const;
//   const uint8_t* chroma_hit() const;   // 256 entries, consulted only for P8
//   const uint32_t* pal_mult() const;    // 256-entry vc-multiplied palette LUT
//   void ensure_pal_mult(uint32_t vc);   // build the LUT for this vertex RGB
//   uint32_t blend_px(uint32_t dst, uint32_t src) const;  // exact path blend
template <class H>
inline bool blit_quad_exec(H& st, const Vert* v, const TexView& t, float tsx, float tsy,
                           bool strip) {
    // Two texture classes take the fast path:
    //   bpp 1 (P_8)  — the paletted tile/sprite class; palette-index texels,
    //                  chroma punch, uniform-vc pal_mult LUT.
    //   bpp 2 (RGB_565) — the 16-bpp class D2 submits for light maps / video
    //                  quads (fill_span's bpp==2 branch treats EVERY 2-byte
    //                  texture as RGB_565 — mirrored here exactly): direct
    //                  color texel expansion, NO chroma, NO vertex-color
    //                  multiply, so the per-pixel source needs no LUT at all.
    // Everything else (geometry/slope/boundary proofs below) is texture
    // format agnostic — the barycentric path's coverage for the accepted
    // shape is [x0,x1) x [y0,y1) either way.
    if (!st.px() || !t.data || (t.bpp != 1 && t.bpp != 2)) return false;
    if (st.color_fill()) return false;
    const bool pal8 = (t.bpp == 1);

    // uniform vertex color (the pal_mult LUT path) — per-pixel vc falls back.
    // RGB_565 texels ignore vertex color entirely (fill_span does), so the
    // check is only needed on the paletted class.
    if (pal8 && !(v[0].argb == v[1].argb && v[1].argb == v[2].argb &&
                  v[2].argb == v[3].argb)) return false;
    if (pal8) st.ensure_pal_mult(v[0].argb & 0x00FFFFFFu);

    // bounds; integer corners; in-frame (clipped quads keep the exact path)
    float minx = v[0].x, maxx = v[0].x, miny = v[0].y, maxy = v[0].y;
    for (unsigned i = 1; i < 4; ++i) {
        if (v[i].x < minx) minx = v[i].x;
        if (v[i].x > maxx) maxx = v[i].x;
        if (v[i].y < miny) miny = v[i].y;
        if (v[i].y > maxy) maxy = v[i].y;
    }
    if (minx < 0.0f || miny < 0.0f) return false;
    if ((float)(int)minx != minx || (float)(int)maxx != maxx ||
        (float)(int)miny != miny || (float)(int)maxy != maxy) return false;
    const int x0 = (int)minx, x1 = (int)maxx, y0 = (int)miny, y1 = (int)maxy;
    if (x1 <= x0 || y1 <= y0) return false;
    if (x1 > (int)st.width() || y1 > (int)st.height()) return false;

    // every vertex on a bbox corner, all four corners distinct, and all
    // vertices sharing an x share u / sharing a y share v (u<->x, v<->y).
    unsigned corner_seen = 0;
    unsigned cidx[4] = {};
    float u_x0 = 0.0f, u_x1 = 0.0f, v_y0 = 0.0f, v_y1 = 0.0f;
    for (unsigned i = 0; i < 4; ++i) {
        if (v[i].x != minx && v[i].x != maxx) return false;
        if (v[i].y != miny && v[i].y != maxy) return false;
        const unsigned cx = (v[i].x == maxx) ? 1u : 0u;
        const unsigned cy = (v[i].y == maxy) ? 2u : 0u;
        const unsigned bit = 1u << (cx | cy);
        if (corner_seen & bit) return false;      // duplicated corner = triangle coverage
        corner_seen |= bit;
        cidx[i] = cx | cy;
        if (cx == 0u) u_x0 = v[i].u; else u_x1 = v[i].u;
        if (cy == 0u) v_y0 = v[i].v; else v_y1 = v[i].v;
    }
    if (corner_seen != 0xFu) return false;
    // the two triangles' shared edge must be the diagonal — fan shares
    // (v0,v2), strip shares (v1,v2); otherwise their union is not the rect
    if (((strip ? cidx[1] ^ cidx[2] : cidx[0] ^ cidx[2]) & 3u) != 3u) return false;

    // 1:1 texel slope, bit-exact (the whole point of the fast path)
    if (tsx <= 0.0f || tsy <= 0.0f) return false;
    const float dudx_u = (u_x1 - u_x0) / (float)(x1 - x0);
    const float dvdy_v = (v_y1 - v_y0) / (float)(y1 - y0);
    if (dudx_u * tsx != 1.0f || dvdy_v * tsy != 1.0f) return false;

    // first covered pixel's texel coordinate: center of the (u,x0+0.5) affine
    // and it must sit clear of the floor boundary (true 1:1 lands on frac 0.5)
    const float uf = (u_x0 + 0.5f * dudx_u) * tsx + 4096.0f;
    const float vf = (v_y0 + 0.5f * dvdy_v) * tsy + 4096.0f;
    const float ufrac = uf - floorf(uf);
    const float vfrac = vf - floorf(vf);
    if (ufrac < 1.0e-3f || ufrac > 1.0f - 1.0e-3f) return false;
    if (vfrac < 1.0e-3f || vfrac > 1.0f - 1.0e-3f) return false;

    int tx0 = (int)uf - 4096;
    int ty0 = (int)vf - 4096;

    // verification walk: fill_span accumulates u += dudx per pixel; prove the
    // accumulated floors advance by exactly +1 across the whole span
    {
        float u = u_x0 + 0.5f * dudx_u;
        int prev = tx0;
        for (int k = 1; k < x1 - x0; ++k) {
            u += dudx_u;
            const int cur = (int)(u * tsx + 4096.0f) - 4096;
            if (cur != prev + 1) return false;
            prev = cur;
        }
        float vv = v_y0 + 0.5f * dvdy_v;
        prev = ty0;
        for (int k = 1; k < y1 - y0; ++k) {
            vv += dvdy_v;
            const int cur = (int)(vv * tsy + 4096.0f) - 4096;
            if (cur != prev + 1) return false;
            prev = cur;
        }
    }

    const uint32_t* pmult = st.pal_mult();
    const uint8_t*  hit   = st.chroma_hit();
    const bool      chroma = st.chroma_on();
    const uint8_t*  tdata = t.data;
    const unsigned  tw = t.w, th = t.h;
    const unsigned  twm = tw - 1, thm = th - 1;
    uint32_t* px = st.px();
    const unsigned w = st.width();

    // texel-row stride in BYTES: P8 rows are tw bytes; RGB_565 rows are 2*tw
    // (fill_span indexes the 565 texel as tdata[2*(ty*tw+tx)], same stride).
    const size_t tstride = (size_t)tw * (pal8 ? 1u : 2u);
    for (int y = y0; y < y1; ++y, ++ty0) {
        const uint8_t* trow = tdata + (size_t)(ty0 & (int)thm) * tstride;
        uint32_t* row = px + (size_t)y * w;   // row[x], x in [x0,x1) — same as fill_span
        if (!pal8) {
            // RGB_565 rows: texel = two bytes LE (fill_span bpp==2 verbatim),
            // expanded 5/6/5 -> 8 bits by bit replication, no chroma, no vc.
            unsigned tx = (unsigned)(tx0 & (int)twm);
            switch (st.blend_class()) {
                case BQ_OPAQUE:
                    for (int x = x0; x < x1; ++x) {
                        const uint16_t p5 = (uint16_t)(trow[2 * tx] | (trow[2 * tx + 1] << 8));
                        const unsigned r5 = (p5 >> 11) & 0x1Fu, g6 = (p5 >> 5) & 0x3Fu, b5 = p5 & 0x1Fu;
                        row[x] = 0xFF000000u | ((r5 << 3 | r5 >> 2) << 16)
                               | ((g6 << 2 | g6 >> 4) << 8) | (b5 << 3 | b5 >> 2);
                        tx = (tx + 1u) & twm;
                    }
                    break;
                case BQ_ADD: {
                    for (int x = x0; x < x1; ++x) {
                        const uint16_t p5 = (uint16_t)(trow[2 * tx] | (trow[2 * tx + 1] << 8));
                        const unsigned r5 = (p5 >> 11) & 0x1Fu, g6 = (p5 >> 5) & 0x3Fu, b5 = p5 & 0x1Fu;
                        const uint32_t src = 0xFF000000u | ((r5 << 3 | r5 >> 2) << 16)
                                           | ((g6 << 2 | g6 >> 4) << 8) | (b5 << 3 | b5 >> 2);
                        const uint32_t dst = row[x];
                        unsigned b = (dst & 0xFFu) + (src & 0xFFu);
                        unsigned g = ((dst >> 8) & 0xFFu) + ((src >> 8) & 0xFFu);
                        unsigned r = ((dst >> 16) & 0xFFu) + ((src >> 16) & 0xFFu);
                        if (b > 255u) b = 255u;
                        if (g > 255u) g = 255u;
                        if (r > 255u) r = 255u;
                        row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
                        tx = (tx + 1u) & twm;
                    }
                    break;
                }
                case BQ_LERP: {
                    const unsigned a = st.const_alpha(), ia = 255u - a;
                    for (int x = x0; x < x1; ++x) {
                        const uint16_t p5 = (uint16_t)(trow[2 * tx] | (trow[2 * tx + 1] << 8));
                        const unsigned r5 = (p5 >> 11) & 0x1Fu, g6 = (p5 >> 5) & 0x3Fu, b5 = p5 & 0x1Fu;
                        const uint32_t src = 0xFF000000u | ((r5 << 3 | r5 >> 2) << 16)
                                           | ((g6 << 2 | g6 >> 4) << 8) | (b5 << 3 | b5 >> 2);
                        const uint32_t dst = row[x];
                        unsigned b = (((src & 0xFFu) * a + 255u) >> 8) + (((dst & 0xFFu) * ia + 255u) >> 8);
                        unsigned g = ((((src >> 8) & 0xFFu) * a + 255u) >> 8) + ((((dst >> 8) & 0xFFu) * ia + 255u) >> 8);
                        unsigned r = ((((src >> 16) & 0xFFu) * a + 255u) >> 8) + ((((dst >> 16) & 0xFFu) * ia + 255u) >> 8);
                        if (b > 255u) b = 255u;
                        if (g > 255u) g = 255u;
                        if (r > 255u) r = 255u;
                        row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
                        tx = (tx + 1u) & twm;
                    }
                    break;
                }
                case BQ_MUL: {
                    for (int x = x0; x < x1; ++x) {
                        const uint16_t p5 = (uint16_t)(trow[2 * tx] | (trow[2 * tx + 1] << 8));
                        const unsigned r5 = (p5 >> 11) & 0x1Fu, g6 = (p5 >> 5) & 0x3Fu, b5 = p5 & 0x1Fu;
                        const uint32_t src = 0xFF000000u | ((r5 << 3 | r5 >> 2) << 16)
                                           | ((g6 << 2 | g6 >> 4) << 8) | (b5 << 3 | b5 >> 2);
                        const uint32_t dst = row[x];
                        unsigned b = ((dst & 0xFFu) * (src & 0xFFu) + 255u) >> 8;
                        unsigned g = (((dst >> 8) & 0xFFu) * ((src >> 8) & 0xFFu) + 255u) >> 8;
                        unsigned r = (((dst >> 16) & 0xFFu) * ((src >> 16) & 0xFFu) + 255u) >> 8;
                        row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
                        tx = (tx + 1u) & twm;
                    }
                    break;
                }
                default:             // BQ_GEN: the exact per-pixel generic blend
                    for (int x = x0; x < x1; ++x) {
                        const uint16_t p5 = (uint16_t)(trow[2 * tx] | (trow[2 * tx + 1] << 8));
                        const unsigned r5 = (p5 >> 11) & 0x1Fu, g6 = (p5 >> 5) & 0x3Fu, b5 = p5 & 0x1Fu;
                        const uint32_t src = 0xFF000000u | ((r5 << 3 | r5 >> 2) << 16)
                                           | ((g6 << 2 | g6 >> 4) << 8) | (b5 << 3 | b5 >> 2);
                        row[x] = st.blend_px(row[x], src);
                        tx = (tx + 1u) & twm;
                    }
                    break;
            }
            continue;
        }
        unsigned tx = (unsigned)(tx0 & (int)twm);
        switch (st.blend_class()) {
            case BQ_OPAQUE:
                for (int x = x0; x < x1; ++x) {
                    const unsigned idx = trow[tx];
                    if (!chroma || !hit[idx]) row[x] = pmult[idx] | 0xFF000000u;
                    tx = (tx + 1u) & twm;
                }
                break;
            case BQ_ADD: {       // dst + src, clamped (blend_px BC_ADD, verbatim)
                for (int x = x0; x < x1; ++x) {
                    const unsigned idx = trow[tx];
                    if (!chroma || !hit[idx]) {
                        const uint32_t src = pmult[idx], dst = row[x];
                        unsigned b = (dst & 0xFFu) + (src & 0xFFu);
                        unsigned g = ((dst >> 8) & 0xFFu) + ((src >> 8) & 0xFFu);
                        unsigned r = ((dst >> 16) & 0xFFu) + ((src >> 16) & 0xFFu);
                        if (b > 255u) b = 255u;
                        if (g > 255u) g = 255u;
                        if (r > 255u) r = 255u;
                        row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
                    }
                    tx = (tx + 1u) & twm;
                }
                break;
            }
            case BQ_LERP: {      // (src*a + dst*(255-a) + rounding) per lane
                const unsigned a = st.const_alpha(), ia = 255u - a;
                for (int x = x0; x < x1; ++x) {
                    const unsigned idx = trow[tx];
                    if (!chroma || !hit[idx]) {
                        const uint32_t src = pmult[idx], dst = row[x];
                        unsigned b = (((src & 0xFFu) * a + 255u) >> 8) + (((dst & 0xFFu) * ia + 255u) >> 8);
                        unsigned g = ((((src >> 8) & 0xFFu) * a + 255u) >> 8) + ((((dst >> 8) & 0xFFu) * ia + 255u) >> 8);
                        unsigned r = ((((src >> 16) & 0xFFu) * a + 255u) >> 8) + ((((dst >> 16) & 0xFFu) * ia + 255u) >> 8);
                        if (b > 255u) b = 255u;
                        if (g > 255u) g = 255u;
                        if (r > 255u) r = 255u;
                        row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
                    }
                    tx = (tx + 1u) & twm;
                }
                break;
            }
            case BQ_MUL: {       // (dst*src + rounding) per lane
                for (int x = x0; x < x1; ++x) {
                    const unsigned idx = trow[tx];
                    if (!chroma || !hit[idx]) {
                        const uint32_t src = pmult[idx], dst = row[x];
                        unsigned b = ((dst & 0xFFu) * (src & 0xFFu) + 255u) >> 8;
                        unsigned g = (((dst >> 8) & 0xFFu) * ((src >> 8) & 0xFFu) + 255u) >> 8;
                        unsigned r = (((dst >> 16) & 0xFFu) * ((src >> 16) & 0xFFu) + 255u) >> 8;
                        row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
                    }
                    tx = (tx + 1u) & twm;
                }
                break;
            }
            default:             // BQ_GEN: the exact per-pixel generic blend
                for (int x = x0; x < x1; ++x) {
                    const unsigned idx = trow[tx];
                    if (!chroma || !hit[idx]) row[x] = st.blend_px(row[x], pmult[idx]);
                    tx = (tx + 1u) & twm;
                }
                break;
        }
    }
    return true;
}

} // namespace g3xbq

#endif // G3X_BLIT_QUAD_H
