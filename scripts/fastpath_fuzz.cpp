// fastpath_fuzz.cpp — differential test: g3xbq::blit_quad_exec (the restored
// P13 fast path) vs the exact barycentric path on identical input quads.
//
// The reference below is a VERBATIM port of the current g3x_frame.cpp
// semantics (fill_span texel walk + raster_tri scanline coverage + blend_px /
// blend_gen lane math, parameterized over a mock host instead of file
// statics). The fast path's contract is "bit-identical or reject" — any
// mismatch here is a bug.
//
// Coverage: P8 (uniform vc via pal_mult, chroma on/off) and RGB565 (vc
// deliberately per-corner varying — fill_span ignores vc for 2-bpp textures,
// and the fast path must too), blend classes OPAQUE/ADD/LERP/MUL/GEN, fan and
// strip triangulations, wrap-around sampling, all texture lods.
//
// Build:  g++ -O2 -std=c++17 -I src scripts/fastpath_fuzz.cpp -o /tmp/fastpath_fuzz
// Run:    /tmp/fastpath_fuzz [iterations]

#include "../src/g3x_blit_quad.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// ---- mock host --------------------------------------------------------------

struct FuzzHost {
    unsigned W = 64, H = 64;
    std::vector<uint32_t> buf;
    unsigned const_alpha_v = 255;
    bool color_fill_v = false;
    unsigned sf = 4, df = 0;
    unsigned bc = g3xbq::BQ_OPAQUE;
    bool chroma_on_v = false;
    uint8_t hit[256] = {};
    uint32_t pal[256] = {};
    uint32_t pal_mult_v[256] = {};
    uint32_t pal_mult_vc = 0xFFFFFFFFu;
    unsigned pal_epoch = 0;
    // texture
    std::vector<uint8_t> tex8;
    std::vector<uint8_t> tex16;   // bytes, 2/texel
    unsigned tw = 8, th = 8, tbpp = 1, tlod = 3;

    unsigned width() const { return W; }
    unsigned height() const { return H; }
    uint32_t* px() { return buf.data(); }
    unsigned const_alpha() const { return const_alpha_v; }
    bool color_fill() const { return color_fill_v; }
    unsigned blend_class() const { return bc; }
    bool chroma_on() const { return chroma_on_v; }
    const uint8_t* chroma_hit() const { return hit; }
    const uint32_t* pal_mult() const { return pal_mult_v; }
    void ensure_pal_mult(uint32_t vc) {
        for (int i = 0; i < 256; ++i) {
            const uint32_t p = pal[i];
            const unsigned vr = vc & 0xFFu, vg = (vc >> 8) & 0xFFu, vb = (vc >> 16) & 0xFFu;
            pal_mult_v[i] = 0xFF000000u
                | ((((p & 0xFFu) * vr + 255u) >> 8))
                | ((((p >> 8) & 0xFFu) * vg + 255u) >> 8) << 8
                | ((((p >> 16) & 0xFFu) * vb + 255u) >> 8) << 16;
        }
        pal_mult_vc = vc;
        pal_epoch = 1;
    }

    // === VERBATIM port of g3x_frame.cpp's blend machinery ===
    // Generic fallback — the P2.1 shade_blend body.
    uint32_t blend_gen(uint32_t dst, uint32_t src) const {
        const unsigned s[3] = { src & 0xFF, (src >> 8) & 0xFF, (src >> 16) & 0xFF };
        const unsigned d[3] = { dst & 0xFF, (dst >> 8) & 0xFF, (dst >> 16) & 0xFF };
        unsigned out[3];
        for (int i = 0; i < 3; ++i) {
            unsigned sc, dc;
            switch (sf) {
                case 0:  sc = 0; break;
                case 1:  sc = const_alpha_v; break;
                case 2:  sc = s[i]; break;
                case 4:  sc = 255; break;
                case 5:  sc = 255 - const_alpha_v; break;
                default: sc = 255; break;
            }
            switch (df) {
                case 0:  dc = 0; break;
                case 1:  dc = const_alpha_v; break;
                case 2:  dc = s[i]; break;
                case 3:  dc = (dst >> 24) & 0xFF; break;
                case 4:  dc = 255; break;
                case 5:  dc = 255 - const_alpha_v; break;
                default: dc = 0; break;
            }
            unsigned v = (s[i] * sc + 255) >> 8;
            v += (d[i] * dc + 255) >> 8;
            out[i] = v > 255 ? 255 : v;
        }
        return out[0] | (out[1] << 8) | (out[2] << 16) | 0xFF000000u;
    }
    uint32_t blend_px(uint32_t dst, uint32_t src) const {
        switch (bc) {
            case g3xbq::BQ_OPAQUE:
                return src | 0xFF000000u;
            case g3xbq::BQ_ADD: {
                unsigned b = (dst & 0xFFu) + (src & 0xFFu);
                unsigned g = ((dst >> 8) & 0xFFu) + ((src >> 8) & 0xFFu);
                unsigned r = ((dst >> 16) & 0xFFu) + ((src >> 16) & 0xFFu);
                if (b > 255u) b = 255u;
                if (g > 255u) g = 255u;
                if (r > 255u) r = 255u;
                return 0xFF000000u | (r << 16) | (g << 8) | b;
            }
            case g3xbq::BQ_LERP: {
                const unsigned a = const_alpha_v, ia = 255u - a;
                unsigned b = (((src & 0xFFu) * a + 255u) >> 8) + (((dst & 0xFFu) * ia + 255u) >> 8);
                unsigned g = ((((src >> 8) & 0xFFu) * a + 255u) >> 8) + ((((dst >> 8) & 0xFFu) * ia + 255u) >> 8);
                unsigned r = ((((src >> 16) & 0xFFu) * a + 255u) >> 8) + ((((dst >> 16) & 0xFFu) * ia + 255u) >> 8);
                if (b > 255u) b = 255u;
                if (g > 255u) g = 255u;
                if (r > 255u) r = 255u;
                return 0xFF000000u | (r << 16) | (g << 8) | b;
            }
            case g3xbq::BQ_MUL: {
                unsigned b = ((dst & 0xFFu) * (src & 0xFFu) + 255u) >> 8;
                unsigned g = (((dst >> 8) & 0xFFu) * ((src >> 8) & 0xFFu) + 255u) >> 8;
                unsigned r = (((dst >> 16) & 0xFFu) * ((src >> 16) & 0xFFu) + 255u) >> 8;
                return 0xFF000000u | (r << 16) | (g << 8) | b;
            }
            default:
                return blend_gen(dst, src);
        }
    }
};

// ---- VERBATIM port of g3x_frame.cpp's exact path (raster_tri + fill_span) ---

struct RVertRef { float x, y, u, v; uint32_t argb; };

static inline bool owns_edge(const RVertRef& p, const RVertRef& q) {
    const float dy = q.y - p.y;
    if (dy != 0.0f) return dy < 0.0f;
    return (q.x - p.x) > 0.0f;
}

static inline bool inside_exact(int x, float py, const RVertRef& A, const RVertRef& B, const RVertRef& C,
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

template <class H>
static void ref_fill_span(H& h, uint32_t* row, int sx, int ex,
                          float u, float v, float dudx, float dvdx,
                          bool textured, unsigned tw, unsigned th, unsigned tbpp,
                          const uint8_t* tdata, uint32_t fill_src,
                          float tsx, float tsy) {
    const bool chroma = h.chroma_on_v && tbpp == 1;
    const uint8_t* hit = h.chroma_hit();
    const uint32_t* pmult = h.pal_mult();
    (void)fill_src; (void)textured;   // fuzz quads are always textured
    for (int x = sx; x < ex; ++x) {
        uint32_t src;
        if (tbpp == 1 && tdata) {
            const unsigned tx = (unsigned)(int)(u * tsx + 4096.0f) & (tw - 1);
            const unsigned ty = (unsigned)(int)(v * tsy + 4096.0f) & (th - 1);
            const unsigned idx = tdata[(size_t)ty * tw + tx];
            if (chroma && hit[idx]) { u += dudx; v += dvdx; continue; }
            src = pmult[idx];
        } else if (tbpp == 2 && tdata) {
            const unsigned tx = (unsigned)(int)(u * tsx + 4096.0f) & (tw - 1);
            const unsigned ty = (unsigned)(int)(v * tsy + 4096.0f) & (th - 1);
            const uint16_t p5 = ((const uint16_t*)tdata)[(size_t)ty * tw + tx];
            const unsigned r5 = (p5 >> 11) & 0x1Fu;
            const unsigned g6 = (p5 >> 5) & 0x3Fu;
            const unsigned b5 = p5 & 0x1Fu;
            src = 0xFF000000u
                | ((r5 << 3 | r5 >> 2) << 16)
                | ((g6 << 2 | g6 >> 4) << 8)
                | (b5 << 3 | b5 >> 2);
        } else {
            src = 0xFF000000u;
        }
        row[x] = h.blend_px(row[x], src);
        u += dudx; v += dvdx;
    }
}

template <class H>
static void ref_raster_tri(H& h, const RVertRef& A0, const RVertRef& B0, const RVertRef& C0,
                           float tsx, float tsy) {
    uint32_t* s_px = h.px();
    const unsigned s_w = h.width(), s_h = h.height();
    float area = (B0.x - A0.x) * (C0.y - A0.y) - (B0.y - A0.y) * (C0.x - A0.x);
    if (area == 0.0f) return;
    RVertRef A = A0, B = B0, C = C0;
    const bool swapped = area < 0.0f;
    if (swapped) { B = C0; C = B0; area = -area; }
    const float inv = 1.0f / area;
    const bool textured = !h.color_fill_v;

    float minx = A.x < B.x ? (A.x < C.x ? A.x : C.x) : (B.x < C.x ? B.x : C.x);
    float miny = A.y < B.y ? (A.y < C.y ? A.y : C.y) : (B.y < C.y ? B.y : C.y);
    float maxx = A.x > B.x ? (A.x > C.x ? A.x : C.x) : (B.x > C.x ? B.x : C.x);
    float maxy = A.y > B.y ? (A.y > C.y ? A.y : C.y) : (B.y > C.y ? B.y : C.y);
    int bx0 = (int)minx;     if (bx0 < 0) bx0 = 0;
    int by0 = (int)miny;     if (by0 < 0) by0 = 0;
    int bx1 = (int)maxx + 1; if (bx1 > (int)s_w) bx1 = (int)s_w;
    int by1 = (int)maxy + 1; if (by1 > (int)s_h) by1 = (int)s_h;
    if (bx0 >= bx1 || by0 >= by1) return;

    bool own0 = owns_edge(A, B);
    bool own1 = owns_edge(B, C);
    bool own2 = owns_edge(C, A);
    if (swapped) { own0 = !own0; own1 = !own1; own2 = !own2; }

    const uint32_t fill_src = 0xFF000000u;   // unused: fuzz quads are textured

    const float dudx = (A.u * (B.y - C.y) + B.u * (C.y - A.y) + C.u * (A.y - B.y)) * inv;
    const float dvdx = (A.v * (B.y - C.y) + B.v * (C.y - A.y) + C.v * (A.y - B.y)) * inv;

    for (int y = by0; y < by1; ++y) {
        const float py = (float)y + 0.5f;
        float lo = -3.0e38f, hi = 3.0e38f;
        bool empty = false;
        {
            const float dy = B.y - A.y;
            if (dy > 0.0f)      { const float xc = A.x + (B.x - A.x) * ((py - A.y) / dy); if (xc < hi) hi = xc; }
            else if (dy < 0.0f) { const float xc = A.x + (B.x - A.x) * ((py - A.y) / dy); if (xc > lo) lo = xc; }
            else {
                const float e = (B.x - A.x) * (py - A.y);
                if (e < 0.0f || (e == 0.0f && !own0)) empty = true;
            }
        }
        {
            const float dy = C.y - B.y;
            if (dy > 0.0f)      { const float xc = B.x + (C.x - B.x) * ((py - B.y) / dy); if (xc < hi) hi = xc; }
            else if (dy < 0.0f) { const float xc = B.x + (C.x - B.x) * ((py - B.y) / dy); if (xc > lo) lo = xc; }
            else {
                const float e = (C.x - B.x) * (py - B.y);
                if (e < 0.0f || (e == 0.0f && !own1)) empty = true;
            }
        }
        {
            const float dy = A.y - C.y;
            if (dy > 0.0f)      { const float xc = C.x + (A.x - C.x) * ((py - C.y) / dy); if (xc < hi) hi = xc; }
            else if (dy < 0.0f) { const float xc = C.x + (A.x - C.x) * ((py - C.y) / dy); if (xc > lo) lo = xc; }
            else {
                const float e = (A.x - C.x) * (py - C.y);
                if (e < 0.0f || (e == 0.0f && !own2)) empty = true;
            }
        }
        if (empty || lo > hi) continue;

        int sx = (int)ceilf(lo - 0.5f) - 1;
        int ex = (int)ceilf(hi - 0.5f) + 2;
        if (sx < bx0) sx = bx0;
        if (ex > bx1) ex = bx1;

        float wa, wb, wc, t1, t2, t3;
        bool ok = false;
        for (int k = 0; k < 3 && sx < ex; ++k) {
            if (inside_exact(sx, py, A, B, C, inv, own0, own1, own2, wa, wb, wc)) { ok = true; break; }
            ++sx;
        }
        if (!ok) continue;
        for (int k = 0; k < 3 && sx > bx0; ++k) {
            if (!inside_exact(sx - 1, py, A, B, C, inv, own0, own1, own2, t1, t2, t3)) break;
            --sx; wa = t1; wb = t2; wc = t3;
        }
        for (int k = 0; k < 3 && ex > sx; ++k) {
            if (inside_exact(ex - 1, py, A, B, C, inv, own0, own1, own2, t1, t2, t3)) break;
            --ex;
        }
        for (int k = 0; k < 3 && ex < bx1; ++k) {
            if (!inside_exact(ex, py, A, B, C, inv, own0, own1, own2, t1, t2, t3)) break;
            ++ex;
        }

        const float u0 = A.u * wa + B.u * wb + C.u * wc;
        const float v0 = A.v * wa + B.v * wb + C.v * wc;
        ref_fill_span(h, s_px + (size_t)y * s_w, sx, ex, u0, v0, dudx, dvdx,
                      textured, h.tw, h.th, h.tbpp, h.tbpp == 1 ? h.tex8.data() : h.tex16.data(),
                      fill_src, tsx, tsy);
    }
}

// ---- case generation (adapted from the pre-rollback harness) ----------------

static uint64_t rng_s = 0x9e3779b97f4a7c15ull;
static int x0c = 0, y0c = 0;
static inline uint32_t rnd() {
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17;
    return (uint32_t)(rng_s >> 32);
}
static inline uint32_t rndn(uint32_t n) { return n ? rnd() % n : 0; }

static void make_case(FuzzHost& h, g3xbq::Vert v[4], bool& strip, unsigned iter) {
    h.W = 64 + rndn(193);          // 64..256
    h.H = 64 + rndn(193);
    h.buf.resize((size_t)h.W * h.H);
    for (auto& p : h.buf) p = rnd() | 0xFF000000u;   // busy dst: blends see real data

    switch (rndn(5)) {
        case 0:  h.sf = 4; h.df = 0; h.bc = g3xbq::BQ_OPAQUE; break;
        case 1:  h.sf = 4; h.df = 4; h.bc = g3xbq::BQ_ADD;    break;
        case 2:  h.sf = 1; h.df = 5; h.bc = g3xbq::BQ_LERP;   break;
        case 3:  h.sf = 0; h.df = 2; h.bc = g3xbq::BQ_MUL;    break;
        default: h.sf = 2; h.df = 1; h.bc = g3xbq::BQ_GEN;    break;
    }
    h.const_alpha_v = (uint8_t)rnd();
    h.color_fill_v = false;

    // square lod 0..8 — lod 0 (1x1) and lod 1 (2x2) stress the wrap masks
    h.tlod = rndn(9);
    h.tw = 1u << h.tlod;
    h.th = 1u << h.tlod;
    h.tbpp = (iter % 2) ? 2 : 1;
    if (h.tbpp == 1) {
        h.tex8.resize((size_t)h.tw * h.th);
        for (auto& t : h.tex8) t = (uint8_t)rndn(256);
    } else {
        h.tex16.resize((size_t)h.tw * h.th * 2);
        for (auto& t : h.tex16) t = (uint8_t)rndn(256);
    }

    h.chroma_on_v = rndn(2) == 0;
    for (int i = 0; i < 256; ++i) h.hit[i] = 0;
    if (h.chroma_on_v) {
        h.hit[rndn(256)] = 1;
        if (rndn(2)) h.hit[0] = 1;
    }

    const uint32_t vc = rnd() & 0x00FFFFFFu;
    for (int i = 0; i < 256; ++i) h.pal[i] = rnd() | 0xFF000000u;
    h.pal_epoch = 0;
    h.ensure_pal_mult(vc);

    // quad: axis-aligned integer, 1:1 in u; v per family — the P13 1:1 class
    // plus the P16 v-stretch classes (texel step sy a power of two: 4x/2x/1x
    // upscale, 2x/4x downscale), with occasional dyadic v0 bases (non-zero
    // wrap offsets). Family 9 uses a NON-dyadic v0: the exec's exactness
    // proofs require dyadic v coords, so those cases must be REJECTED (they
    // skip the comparison) — their presence pins the reject guard.
    const unsigned qs = 1u << h.tlod;
    float vspan = 256.0f;
    float vy0 = 0.0f;
    unsigned qh = qs;                     // quad height in px
    const unsigned fam = rndn(10);
    if (fam >= 4) {                       // 6/10 v-stretch families
        static const float sys[6] = { 0.25f, 0.5f, 2.0f, 4.0f, 0.125f, 1.0f };
        const float sy = sys[fam - 4];
        if (sy < 1.0f) qh = qs * 2;       // upscale needs a taller quad
        if (qh + 2 > h.H || qs + 2 > h.W) { v[0].x = -1; return; }
        vspan = sy * (float)qh * (256.0f / (float)qs);   // exact dyadic product
        const unsigned off = rndn(4);
        if (off == 0)      vy0 = 0.5f;                          // half-texel base
        else if (off == 1) vy0 = (float)(1u << (h.tlod + 1));   // deeper wrap
        else if (off == 2) vy0 = -128.0f;                       // negative base
        else if (off == 3 && fam == 9) vy0 = 0.03f;             // non-dyadic: reject
    }
    if (qs + 2 > h.W || qh + 2 > h.H) { v[0].x = -1; return; }   // skip: won't fit
    const int x0 = 1 + (int)rndn(h.W - qs - 2);
    const int y0 = 1 + (int)rndn(h.H - qh - 2);
    x0c = x0; y0c = y0;
    const int x1 = x0 + (int)qs;
    const int y1 = y0 + (int)qh;
    const uint32_t vcc = h.tbpp == 1 ? (vc | 0xFF000000u) : (uint32_t)rnd();
    const bool flip = rndn(2) == 0;
    unsigned vi[4][2];
    if (flip) { vi[0][0]=1;vi[0][1]=1; vi[1][0]=0;vi[1][1]=1; vi[2][0]=1;vi[2][1]=0; vi[3][0]=0;vi[3][1]=0; }
    else      { vi[0][0]=0;vi[0][1]=0; vi[1][0]=1;vi[1][1]=0; vi[2][0]=1;vi[2][1]=1; vi[3][0]=0;vi[3][1]=1; }
    for (unsigned i = 0; i < 4; ++i) {
        const float px = vi[i][0] ? (float)x1 : (float)x0;
        const float py = vi[i][1] ? (float)y1 : (float)y0;
        v[i] = { px, py, vi[i][0] ? 256.0f : 0.0f,
                 vi[i][1] ? vy0 + vspan : vy0, vcc };
    }
    if (h.tbpp == 2) v[1].argb = rnd();   // per-corner vc: 565 must ignore it
    strip = rndn(2) == 0;
}

int main(int argc, char** argv) {
    const unsigned iters = (argc > 1) ? (unsigned)atoi(argv[1]) : 20000u;
    FuzzHost h;
    unsigned ran = 0, mism = 0, by_class[5] = {}, by_bpp[2] = {}, lods[9] = {};
    for (unsigned it = 0; it < iters * 4; ++it) {
        g3xbq::Vert v[4];
        bool strip = false;
        make_case(h, v, strip, it);
        if (v[0].x < 0) continue;                    // case would not fit
        const float tsx = (float)(1u << h.tlod) / 256.0f;
        const float tsy = tsx;

        std::vector<uint32_t> fast = h.buf;          // fast path target
        std::vector<uint32_t> slow = h.buf;          // exact path target

        // Dispatcher's exact triangulation (g3x_frame_draw_fan / draw_pointers):
        //   fan  : (v0,v1,v2) + (v0,v2,v3) — the ORDER of the second triangle
        //          matters (v0,v3,v2 would flip diagonal ownership and
        //          double-blend it).
        //   strip: (v0,v1,v2) + (v1,v2,v3), each winding-normalized first.
        const RVertRef r[4] = { {v[0].x,v[0].y,v[0].u,v[0].v,v[0].argb},
                                {v[1].x,v[1].y,v[1].u,v[1].v,v[1].argb},
                                {v[2].x,v[2].y,v[2].u,v[2].v,v[2].argb},
                                {v[3].x,v[3].y,v[3].u,v[3].v,v[3].argb} };
        RVertRef tris[2][3];
        tris[0][0] = r[0]; tris[0][1] = r[1]; tris[0][2] = r[2];
        if (strip) { tris[1][0] = r[1]; tris[1][1] = r[2]; tris[1][2] = r[3]; }
        else       { tris[1][0] = r[0]; tris[1][1] = r[2]; tris[1][2] = r[3]; }

        // run fast
        {
            struct FastHost : FuzzHost {
                uint32_t* fast_px;
                uint32_t* px() { return fast_px; }
            } h2;
            *static_cast<FuzzHost*>(&h2) = h;
            h2.fast_px = fast.data();
            const bool ok = g3xbq::blit_quad_exec(h2, v,
                g3xbq::TexView{ h.tbpp == 1 ? h.tex8.data() : h.tex16.data(),
                                h.tw, h.th, h.tlod, h.tbpp },
                tsx, tsy, strip);
            if (!ok) continue;                        // rejected — nothing to compare
        }
        // run exact (winding-normalize strips like the dispatcher)
        {
            struct SlowHost : FuzzHost {
                uint32_t* slow_px;
                uint32_t* px() { return slow_px; }
            } h3;
            *static_cast<FuzzHost*>(&h3) = h;
            h3.slow_px = slow.data();
            for (unsigned k = 0; k < 2; ++k) {
                RVertRef a = tris[k][0], b = tris[k][1], c = tris[k][2];
                const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
                if (strip && area < 0.0f) { RVertRef tmp = b; b = c; c = tmp; }
                ref_raster_tri(h3, a, b, c, tsx, tsy);
            }
        }

        ++ran;
        ++by_bpp[h.tbpp == 1 ? 0 : 1];
        ++by_class[h.bc];
        ++lods[h.tlod];
        if (fast != slow) {
            ++mism;
            if (mism <= 2) {
                std::printf("MISMATCH iter=%u bpp=%u strip=%u sf=%u df=%u W=%u H=%u lod=%u quad=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)\n",
                            it, h.tbpp, (unsigned)strip, h.sf, h.df, h.W, h.H, h.tlod,
                            v[0].x, v[0].y, v[1].x, v[1].y, v[2].x, v[2].y, v[3].x, v[3].y);
                int shown = 0;
                for (size_t i = 0; i < fast.size() && shown < 4; ++i) {
                    if (fast[i] != slow[i]) {
                        const unsigned x = (unsigned)(i % h.W), y = (unsigned)(i / h.W);
                        const unsigned ty = (y - (unsigned)y0c) & (h.th - 1);
                        const unsigned tx = (x - (unsigned)x0c) & (h.tw - 1);
                        uint32_t srcv;
                        if (h.tbpp == 2) {
                            const size_t ti = (size_t)ty * h.tw + tx;
                            const uint16_t p5 = (uint16_t)(h.tex16[2*ti] | (h.tex16[2*ti+1] << 8));
                            const unsigned r5=(p5>>11)&0x1Fu,g6=(p5>>5)&0x3Fu,b5=p5&0x1Fu;
                            srcv = 0xFF000000u|((r5<<3|r5>>2)<<16)|((g6<<2|g6>>4)<<8)|(b5<<3|b5>>2);
                        } else {
                            srcv = h.pal_mult_v[h.tex8[(size_t)ty*h.tw+tx]];
                        }
                        std::printf("   (%3u,%3u) pre-dst=%08x fast=%08x slow=%08x | texel(%u,%u) src=%08x\n",
                                    x, y, h.buf[i], fast[i], slow[i], tx, ty, srcv);
                        ++shown;
                    }
                }
            }
            if (mism >= 2) break;
        }
    }
    std::printf("ran=%u mismatches=%u  p8=%u rgb565=%u | opaque=%u add=%u lerp=%u mul=%u gen=%u\n",
                ran, mism, by_bpp[0], by_bpp[1],
                by_class[0], by_class[1], by_class[2], by_class[3], by_class[4]);
    std::printf("lods: 0=%u 1=%u 2=%u 3=%u 4=%u 5=%u 6=%u 7=%u 8=%u\n",
                lods[0], lods[1], lods[2], lods[3], lods[4], lods[5], lods[6], lods[7], lods[8]);
    return mism ? 1 : 0;
}
