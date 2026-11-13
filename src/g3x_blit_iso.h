// g3x_blit_iso.h — P20 (0.4.10): the iso-lattice triangle exec.
//
// The field log (0.4.7-21 session, D2VP-QoL f1/f3-era game code) pinned the
// ground/wall geometry: every triangle of the f1 ground-grid strips (40/28
// pointer legs over a 6x6 lattice) and of the f5-style corner-fan diamonds
// (fanrej#1-3: N(843,-172) E(923,-132) S(843,-92) W(763,-132)) has INTEGER
// vertices, two slanted edges with |dx| == 2*|dy| (slope +-2 px/row — the
// 2:1 isometric lattice), and one axis-aligned edge (the grid triangles'
// horizontal base, the fan halves' shared vertical diagonal). For that
// family the exact path's per-row cost is dominated by machinery the
// geometry makes redundant:
//
//   * Every edge crossing x(y) = x0 +- 2*(py - y0) lands on an EXACT integer
//     at every pixel row (py = k+0.5 gives odd-integer offsets), and pixel
//     centers px = k+0.5 are never integers — so NO pixel center can lie on
//     any edge (slanted, vertical, or horizontal) and the top-left
//     ownership rule is never consulted. The exact per-pixel inside_exact
//     test therefore reduces to pure sign tests, and the per-row span is a
//     closed-form integer interval [L, R) — no float edge intersections,
//     no boundary fixup loops (raster_tri runs up to 12 inside_exact calls
//     per row; this exec runs exactly one).
//   * Attributes stay BIT-identical by construction: the exec computes the
//     first covered pixel's barycentrics with the same wa/wb/wc expression
//     text as g3x_frame.cpp's inside_exact (same floats in, same floats
//     out), derives u0/v0/vc0 and the per-pixel slopes with raster_tri's
//     verbatim formulas from the same winding-normalized vertices, and
//     steps u/v with fill_span's verbatim accumulation (u += dudx). No
//     slope or frac-margin proofs on the texture mapping are needed — any
//     affine vertex data interpolates identically because the arithmetic
//     IS the exact path's arithmetic; only the pixel SET differs, and the
//     fuzz harness pins span == fixup-loop truth over the whole family.
//
// Rejected (keeps the exact path): non-integer coords, other slopes, right
// triangles (two axis-aligned edges), non-centered fan apex, degenerate
// area, non-uniform vertex color on P8, non-P8/565 textures, color-fill
// mode. Signed sign-robustness: the smallest nonzero |barycentric| over the
// family is >= |dy_edge| * 0.4-ish while float error is ~1e-4 — the sign
// tests cannot flip.
//
// Host contract: identical to g3x_blit_quad.h (width/height/px/color_fill/
// blend_class/const_alpha/chroma_on/chroma_hit/pal_mult/ensure_pal_mult/
// blend_px).
#ifndef G3X_BLIT_ISO_H
#define G3X_BLIT_ISO_H

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <cstring>

namespace g3xbi {

// Layout-identical to g3x_frame.cpp's RVert (pinned by static_assert there).
struct Vert { float x, y, u, v; uint32_t argb; };

struct TexView {
    const uint8_t* data;
    unsigned w, h, lod, bpp;
};

enum : unsigned {
    BI_OPAQUE = 0,
    BI_ADD    = 1,
    BI_LERP   = 2,
    BI_MUL    = 3,
    BI_GEN    = 4,
};

// Top-left ownership rule — g3x_frame.cpp's owns_edge, verbatim (values are
// inert for the accepted family: no pixel ever sits on an edge).
inline bool iso_owns_edge(const Vert& p, const Vert& q) {
    const float dy = q.y - p.y;
    if (dy != 0.0f) return dy < 0.0f;
    return (q.x - p.x) > 0.0f;
}

// Exact per-pixel test — g3x_frame.cpp's inside_exact, verbatim expression
// text (the first covered pixel's barycentrics feed the row attributes, so
// the floats must match the exact path bit-for-bit).
inline bool iso_inside_exact(int x, float py, const Vert& A, const Vert& B, const Vert& C,
                             float inv, bool own0, bool own1, bool own2,
                             float& wa, float& wb, float& wc) {
    const float px = (float)x + 0.5f;
    wa = ((C.x - B.x) * (py - B.y) - (C.y - B.y) * (px - B.x)) * inv;
    wb = ((A.x - C.x) * (py - C.y) - (A.y - C.y) * (px - C.x)) * inv;
    wc = ((B.x - A.x) * (py - A.y) - (B.y - A.y) * (px - A.x)) * inv;
    if (wa < 0.0f || wb < 0.0f || wc < 0.0f) return false;
    if ((wa == 0.0f && !own1) || (wb == 0.0f && !own2) || (wc == 0.0f && !own0)) return false;
    return true;
}

template <class H, bool Paint>
inline bool blit_iso_stage(H& st, const Vert& A0, const Vert& B0, const Vert& C0,
                           const TexView& t, float tsx, float tsy) {
    if (!st.px() || !t.data || (t.bpp != 1 && t.bpp != 2)) return false;
    if (st.color_fill()) return false;
    const bool pal8 = (t.bpp == 1);

    // winding normalize — raster_tri verbatim
    float area = (B0.x - A0.x) * (C0.y - A0.y) - (B0.y - A0.y) * (C0.x - A0.x);
    if (area == 0.0f) return false;
    Vert A = A0, B = B0, C = C0;
    const bool swapped = area < 0.0f;
    if (swapped) { B = C0; C = B0; area = -area; }
    const float inv = 1.0f / area;

    // integer coordinates (all six)
    if ((float)(int)A.x != A.x || (float)(int)A.y != A.y ||
        (float)(int)B.x != B.x || (float)(int)B.y != B.y ||
        (float)(int)C.x != C.x || (float)(int)C.y != C.y) return false;
    if (A.x < -65536.0f || A.x > 65536.0f || A.y < -65536.0f || A.y > 65536.0f ||
        B.x < -65536.0f || B.x > 65536.0f || B.y < -65536.0f || B.y > 65536.0f ||
        C.x < -65536.0f || C.x > 65536.0f || C.y < -65536.0f || C.y > 65536.0f) return false;

    // uniform vertex color on the paletted class (pal_mult LUT path)
    if (pal8 && !(A.argb == B.argb && B.argb == C.argb)) return false;
    if (pal8) st.ensure_pal_mult(A.argb & 0x00FFFFFFu);

    // edge classification over the normalized triangle
    const float e0dx = B.x - A.x, e0dy = B.y - A.y;   // A->B
    const float e1dx = C.x - B.x, e1dy = C.y - B.y;   // B->C
    const float e2dx = A.x - C.x, e2dy = A.y - C.y;   // C->A

    int row_lo, row_hi;      // half-open integer row range (unclipped)
    int span_base;           // per-row closed form pivots on these
    bool case_h, apex_up;    // H: apex + horizontal base; V: apex + vertical edge
    float ax, ay;            // apex
    int o_signed;            // +1: span extends right of the pivot, -1: left
    float pivot;             // H: apex.x (centered span); V: edge x (one-sided)

    if (e0dy == 0.0f) {
        if (e1dy == 0.0f || e2dy == 0.0f) return false;   // second horizontal / degenerate
        if (e0dx == 0.0f) return false;                    // zero-length base
        // apex = C; slanted edges C->A and B->C... apex must satisfy
        // |dx| == 2*|dy| on both apex-to-base-endpoint edges (e2 and e1).
        ax = C.x; ay = C.y;
        if (e2dy == 0.0f) return false;
        if (!(e2dx == 2.0f * e2dy || e2dx == -2.0f * e2dy)) return false;
        if (!(e1dx == 2.0f * e1dy || e1dx == -2.0f * e1dy)) return false;
        case_h = true; apex_up = ay < B.y;
        row_lo = apex_up ? (int)ay : (int)B.y;
        row_hi = apex_up ? (int)B.y : (int)ay;
        pivot = ax; o_signed = 0;   // H spans are symmetric: [ax-o, ax+o)
    } else if (e1dy == 0.0f) {
        if (e2dy == 0.0f) return false;
        if (e1dx == 0.0f) return false;
        ax = A.x; ay = A.y;
        if (e0dy == 0.0f) return false;
        if (!(e0dx == 2.0f * e0dy || e0dx == -2.0f * e0dy)) return false;
        if (!(e2dx == 2.0f * e2dy || e2dx == -2.0f * e2dy)) return false;
        case_h = true; apex_up = ay < C.y;
        row_lo = apex_up ? (int)ay : (int)C.y;
        row_hi = apex_up ? (int)C.y : (int)ay;
        pivot = ax; o_signed = 0;
    } else if (e2dy == 0.0f) {
        if (e2dx == 0.0f) return false;
        ax = B.x; ay = B.y;
        if (!(e1dx == 2.0f * e1dy || e1dx == -2.0f * e1dy)) return false;
        if (!(e0dx == 2.0f * e0dy || e0dx == -2.0f * e0dy)) return false;
        case_h = true; apex_up = ay < A.y;
        row_lo = apex_up ? (int)ay : (int)A.y;
        row_hi = apex_up ? (int)A.y : (int)ay;
        pivot = ax; o_signed = 0;
    } else if (e0dx == 0.0f) {
        // vertical edge A->B; apex = C — must be horizontally centered
        if (e1dx == 0.0f || e2dx == 0.0f) return false;
        const float ytop = A.y < B.y ? A.y : B.y;
        const float ybot = A.y < B.y ? B.y : A.y;
        ax = C.x; ay = C.y;
        if (!(ay - ytop == ybot - ay)) return false;                 // centered
        if (!(2.0f * (ay - ytop) == (ax > A.x ? ax - A.x : A.x - ax) )) return false;
        // (ax != vx guaranteed: collinear would have zeroed area)
        case_h = false; apex_up = false;
        row_lo = (int)ytop; row_hi = (int)ybot;
        pivot = A.x;
        o_signed = (ax > pivot) ? 1 : -1;
    } else if (e1dx == 0.0f) {
        if (e2dx == 0.0f || e0dx == 0.0f) return false;
        const float ytop = B.y < C.y ? B.y : C.y;
        const float ybot = B.y < C.y ? C.y : B.y;
        ax = A.x; ay = A.y;
        if (!(ay - ytop == ybot - ay)) return false;
        if (!(2.0f * (ay - ytop) == (ax > B.x ? ax - B.x : B.x - ax))) return false;
        case_h = false; apex_up = false;
        row_lo = (int)ytop; row_hi = (int)ybot;
        pivot = B.x;
        o_signed = (ax > pivot) ? 1 : -1;
    } else if (e2dx == 0.0f) {
        if (e0dx == 0.0f || e1dx == 0.0f) return false;
        const float ytop = C.y < A.y ? C.y : A.y;
        const float ybot = C.y < A.y ? A.y : C.y;
        ax = B.x; ay = B.y;
        if (!(ay - ytop == ybot - ay)) return false;
        if (!(2.0f * (ay - ytop) == (ax > C.x ? ax - C.x : C.x - ax))) return false;
        case_h = false; apex_up = false;
        row_lo = (int)ytop; row_hi = (int)ybot;
        pivot = C.x;
        o_signed = (ax > pivot) ? 1 : -1;
    } else {
        return false;   // no axis-aligned edge: outside the lattice family
    }
    if (row_hi - row_lo < 1) return false;

    if (!Paint) return true;   // dry: proofs passed

    // attribute slopes — raster_tri verbatim (normalized A,B,C). Vertex color
    // interpolation is intentionally absent: the P8 class requires uniform vc
    // (paints through the pal_mult LUT, fill_span's vc0 is only consulted for
    // varying vc) and RGB_565 texels ignore vertex color entirely, so vc0/
    // dvc would be dead computation on every accepted triangle.
    const float dudx = (A.u * (B.y - C.y) + B.u * (C.y - A.y) + C.u * (A.y - B.y)) * inv;
    const float dvdx = (A.v * (B.y - C.y) + B.v * (C.y - A.y) + C.v * (A.y - B.y)) * inv;
    bool own0 = iso_owns_edge(A, B);
    bool own1 = iso_owns_edge(B, C);
    bool own2 = iso_owns_edge(C, A);
    if (swapped) { own0 = !own0; own1 = !own1; own2 = !own2; }

    const uint32_t* pmult = st.pal_mult();
    const uint8_t*  hit   = st.chroma_hit();
    const bool      chroma = st.chroma_on() && pal8;
    const uint8_t*  tdata = t.data;
    const unsigned  tw = t.w, th = t.h;
    const unsigned  twm = tw - 1, thm = th - 1;
    uint32_t* px = st.px();
    const int w = (int)st.width();
    const int h = (int)st.height();

    int y = row_lo > 0 ? row_lo : 0;
    const int yend = row_hi < h ? row_hi : h;
    for (; y < yend; ++y) {
        const float py = (float)y + 0.5f;
        int L, R;   // half-open span, exact integers
        if (case_h) {
            int o;
            if (apex_up) o = 2 * (y - (int)ay) + 1;
            else         o = 2 * ((int)ay - y) - 1;
            const int p = (int)pivot;
            L = p - o; R = p + o;
        } else {
            const int ytop = row_lo, ybot = row_hi;
            const int o = (y < (int)ay) ? 2 * (y - ytop) + 1
                                        : 2 * (ybot - 1 - y) + 1;
            const int p = (int)pivot;
            if (o_signed > 0) { L = p; R = p + o; }
            else              { L = p - o; R = p; }
        }
        int sx = L > 0 ? L : 0;
        int ex = R < w ? R : w;
        if (sx >= ex) continue;

        float wa, wb, wc;
        if (!iso_inside_exact(sx, py, A, B, C, inv, own0, own1, own2, wa, wb, wc)) continue;
        const float u0 = A.u * wa + B.u * wb + C.u * wc;
        const float v0 = A.v * wa + B.v * wb + C.v * wc;

        // fill_span body, verbatim (uniform vc: pmult for P8; 565 expansion)
        uint32_t* row = px + (size_t)y * (unsigned)w;
        float u = u0, v = v0;
        for (int x = sx; x < ex; ++x) {
            uint32_t src;
            if (pal8) {
                const unsigned tx = (unsigned)(int)(u * tsx + 4096.0f) & twm;
                const unsigned ty = (unsigned)(int)(v * tsy + 4096.0f) & thm;
                const unsigned idx = tdata[(size_t)ty * tw + tx];
                if (chroma && hit[idx]) { u += dudx; v += dvdx; continue; }
                src = pmult[idx];
            } else {
                const unsigned tx = (unsigned)(int)(u * tsx + 4096.0f) & twm;
                const unsigned ty = (unsigned)(int)(v * tsy + 4096.0f) & thm;
                const uint16_t p5 = (uint16_t)(((const uint16_t*)tdata)[(size_t)ty * tw + tx]);
                const unsigned r5 = (p5 >> 11) & 0x1Fu;
                const unsigned g6 = (p5 >> 5) & 0x3Fu;
                const unsigned b5 = p5 & 0x1Fu;
                src = 0xFF000000u
                    | ((r5 << 3 | r5 >> 2) << 16)
                    | ((g6 << 2 | g6 >> 4) << 8)
                    | (b5 << 3 | b5 >> 2);
            }
            row[x] = st.blend_px(row[x], src);
            u += dudx; v += dvdx;
        }
    }
    return true;
}

// Paint wrappers — H is DEDUCED from the host argument (never spell the base
// type at the call site: blit_iso_stage<FuzzHost,...>(fastHost, ...) would
// statically bind px()/blend_px() to the BASE and paint the wrong buffer —
// the exact bug the first fuzz run caught).
template <class H>
inline bool blit_iso_exec(H& st, const Vert& a, const Vert& b, const Vert& c,
                          const TexView& t, float tsx, float tsy) {
    return blit_iso_stage<H, true>(st, a, b, c, t, tsx, tsy);
}
template <class H>
inline bool blit_iso_prove(H& st, const Vert& a, const Vert& b, const Vert& c,
                           const TexView& t, float tsx, float tsy) {
    return blit_iso_stage<H, false>(st, a, b, c, t, tsx, tsy);
}

}   // namespace g3xbi
#endif   // G3X_BLIT_ISO_H
