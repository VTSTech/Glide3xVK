// fastpath_fuzz.cpp — differential test: blit_quad_exec (fast path) vs
// raster_tri (the exact barycentric path) on identical input quads.
//
// WHY: the 16-frame corpus pin (raster_hashes_stage1) is a menu session —
// it pins the P8 blit path but its RGB_565 draws never pass the fast-path
// geometry tests (565hits=0 with the probe build), so the pin proves nothing
// about the new 565 branch. This fuzzer fills that hole: random 1:1
// axis-aligned integer quads over random textures, run through BOTH paths
// into two buffers, byte-compared. The fast path's contract is "bit-identical
// or reject" — any mismatch here is a bug.
//
// Coverage: P8 (uniform vc via pal_mult, chroma on/off) and RGB565 (vc
// deliberately per-corner varying — fill_span ignores vc for 2-bpp textures,
// and the fast path must too), blend classes OPAQUE/ADD/LERP/MUL/GEN, fan and
// strip triangulations, wrap-around sampling, all texture lods.
//
// Build:  g++ -O2 -std=c++17 -I src scripts/fastpath_fuzz.cpp -o /tmp/fastpath_fuzz
// Run:    /tmp/fastpath_fuzz [iterations]

#include "g3x_raster_core.h"


#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// ---- minimal Host over local buffers ----------------------------------------

struct FuzzHost {
    unsigned W = 64, H = 64;
    std::vector<uint32_t> buf;
    uint32_t constant_v = 0;
    unsigned const_alpha_v = 255;
    bool color_fill_v = false;
    unsigned sf = 4, df = 0;
    g3xrc::BlendClass bc = g3xrc::BC_OPAQUE;
    bool chroma_on_v = false;
    uint8_t hit[256] = {};
    uint32_t pal[256] = {};
    uint32_t pal_mult_v[256] = {};
    uint32_t pal_mult_vc = 0xFFFFFFFFu;
    uint32_t pal_epoch = 0;
    // texture
    std::vector<uint8_t> tex8;
    std::vector<uint8_t> tex16;   // bytes, 2/texel
    unsigned tw = 8, th = 8, tbpp = 1, tlod = 3;

    unsigned width() const { return W; }
    unsigned height() const { return H; }
    uint32_t* px() { return buf.data(); }
    uint32_t constant() const { return constant_v; }
    unsigned const_alpha() const { return const_alpha_v; }
    bool color_fill() const { return color_fill_v; }
    unsigned blend_sf() const { return sf; }
    unsigned blend_df() const { return df; }
    g3xrc::BlendClass blend_class() const { return bc; }
    bool chroma_on() const { return chroma_on_v; }
    const uint8_t* chroma_hit() const { return hit; }
    const uint32_t* palette() const { return pal; }
    const uint32_t* pal_mult() const { return pal_mult_v; }
    void ensure_pal_mult(uint32_t vc) {
        if (pal_mult_vc == vc && pal_epoch == 1) return;
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
    bool has_tex() const { return true; }
    g3xrc::TexView tex_view() const {
        return g3xrc::TexView{ tbpp == 1 ? tex8.data() : tex16.data(), tw, th, tlod, tbpp };
    }
};

static uint64_t rng_s = 0x9e3779b97f4a7c15ull;
static int x0c = 0, y0c = 0;
static inline uint32_t rnd() {
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17;
    return (uint32_t)(rng_s >> 32);
}
static inline uint32_t rndn(uint32_t n) { return n ? rnd() % n : 0; }

// Build one random case into `h`, return the 4 verts of the quad.
static void make_case(FuzzHost& h, g3xrc::RVert v[4], bool& strip, unsigned iter) {
    // frame + buffer
    h.W = 64 + rndn(193);          // 64..256
    h.H = 64 + rndn(193);
    h.buf.resize((size_t)h.W * h.H);
    for (auto& p : h.buf) p = rnd() | 0xFF000000u;   // busy dst: blends see real data

    // blend class
    switch (rndn(5)) {
        case 0:  h.sf = 4; h.df = 0; h.bc = g3xrc::BC_OPAQUE; break;
        case 1:  h.sf = 4; h.df = 4; h.bc = g3xrc::BC_ADD;    break;
        case 2:  h.sf = 1; h.df = 5; h.bc = g3xrc::BC_LERP;   break;
        case 3:  h.sf = 0; h.df = 2; h.bc = g3xrc::BC_MUL;    break;
        default: h.sf = 2; h.df = 1; h.bc = g3xrc::BC_GEN;    break;  // generic
    }
    h.const_alpha_v = (uint8_t)rnd();
    h.constant_v = ((uint32_t)rndn(256) << 24) | (rnd() & 0x00FFFFFFu);
    h.color_fill_v = false;

    // texture: square lod 3..8 (fast path needs dudx*tsx == 1.0f exactly)
    h.tlod = 3 + rndn(6);
    h.tw = 1u << h.tlod;
    h.th = 1u << h.tlod;
    h.tbpp = (iter % 2) ? 2 : 1;   // alternate classes
    if (h.tbpp == 1) {
        h.tex8.resize((size_t)h.tw * h.th);
        for (auto& t : h.tex8) t = (uint8_t)rndn(256);
    } else {
        h.tex16.resize((size_t)h.tw * h.th * 2);
        for (auto& t : h.tex16) t = (uint8_t)rndn(256);
    }

    // chroma (P8-only path; harmless on 565 — never consulted there)
    h.chroma_on_v = rndn(2) == 0;
    for (int i = 0; i < 256; ++i) h.hit[i] = 0;
    if (h.chroma_on_v) {
        h.hit[rndn(256)] = 1;
        if (rndn(2)) h.hit[0] = 1;
    }

    // palette + vc
    const uint32_t vc = rnd() & 0x00FFFFFFu;
    for (int i = 0; i < 256; ++i) h.pal[i] = rnd() | 0xFF000000u;
    h.pal_epoch = 0;             // force ensure_pal_mult rebuild
    h.ensure_pal_mult(vc);

    // quad: axis-aligned integer, 1:1, in-frame; u/v spans 256 (full mip space)
    const unsigned qs = 1u << h.tlod;              // quad side == texture side
    if (qs + 2 > h.W || qs + 2 > h.H) { v[0].x = -1; return; }   // skip: won't fit
    const int x0 = 1 + (int)rndn(h.W - qs - 2);
    const int y0 = 1 + (int)rndn(h.H - qs - 2);
    x0c = x0; y0c = y0;
    const int x1 = x0 + (int)qs;
    const int y1 = y0 + (int)qs;
    const float ts = (float)qs / 256.0f;
    (void)ts;
    const uint32_t vcc = h.tbpp == 1 ? (vc | 0xFF000000u) : (uint32_t)rnd();
    // u/v follow the POSITION (u: left edge 0 / right edge 256, v: top 0 /
    // bottom 256) regardless of the walk order; 565 quads get per-corner
    // VARYING vc — proves vc is ignored there exactly like the exact path;
    // P8 keeps uniform (fast-path requirement).
    const bool flip = rndn(2) == 0;
    unsigned vi[4][2];
    if (flip) { vi[0][0]=1;vi[0][1]=1; vi[1][0]=0;vi[1][1]=1; vi[2][0]=1;vi[2][1]=0; vi[3][0]=0;vi[3][1]=0; }
    else      { vi[0][0]=0;vi[0][1]=0; vi[1][0]=1;vi[1][1]=0; vi[2][0]=1;vi[2][1]=1; vi[3][0]=0;vi[3][1]=1; }
    for (unsigned i = 0; i < 4; ++i) {
        const float px = vi[i][0] ? (float)x1 : (float)x0;
        const float py = vi[i][1] ? (float)y1 : (float)y0;
        v[i] = { px, py, vi[i][0] ? 256.0f : 0.0f, vi[i][1] ? 256.0f : 0.0f, vcc };
    }
    if (h.tbpp == 2) v[1].argb = rnd();
    strip = rndn(2) == 0;
}

static inline void set_ts(float& tsx, float& tsy, const FuzzHost& h) {
    tsx = tsy = (float)(1u << h.tlod) / 256.0f;
}

int main(int argc, char** argv) {
    const unsigned iters = (argc > 1) ? (unsigned)atoi(argv[1]) : 20000u;
    FuzzHost h;
    unsigned ran = 0, mism = 0, by_class[5] = {}, by_bpp[2] = {};
    for (unsigned it = 0; it < iters * 4; ++it) {
        g3xrc::RVert v[4];
        bool strip = false;
        make_case(h, v, strip, it);
        if (v[0].x < 0) continue;                    // case would not fit
        float tsx, tsy; set_ts(tsx, tsy, h);

        std::vector<uint32_t> fast = h.buf;          // fast path target
        std::vector<uint32_t> slow = h.buf;          // exact path target

        // fast: blit_quad_exec on a host view over `fast`
        struct HostView : FuzzHost { uint32_t* b; } fv;
        // (plain copy of the host; point px at the fast buffer)
        FuzzHost fh = h;
        fh.buf = fast;                                // copy — px() points inside

        // exact: two triangles, same triangulation the dispatcher runs
        // Dispatcher's exact triangulation (g3x_frame_draw_*):
        //   fan  : (v0,v1,v2) + (v0,v2,v3)   <- shared edge v0-v2, owned per
        //          the top-left rule on those DIRECTED edges — the ORDER of
        //          the second triangle matters (v0,v3,v2 would flip diagonal
        //          ownership and double-blend it).
        //   strip: (v0,v1,v2) + (v1,v2,v3), each winding-normalized first
        //          (g3x_frame_draw_pointers).
        g3xrc::RVert t1[3] = { v[0], v[1], v[2] };
        g3xrc::RVert t2q[3] = { v[0], v[2], v[3] };   // fan order (v0,v2,v3) — the
        if (strip) t2q[0] = v[1];                     // strip fires (v1,v2,v3) instead
        const g3xrc::RVert* tri[2][3] = { { &t1[0], &t1[1], &t1[2] },
                                          { &t2q[0], &t2q[1], &t2q[2] } };
        // winding normalize for strips (the dispatcher's rule)
        g3xrc::RVert t2[2][3];
        for (unsigned k = 0; k < 2; ++k) {
            g3xrc::RVert a = *tri[k][0], b = *tri[k][1], c = *tri[k][2];
            const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (strip && area < 0.0f) { g3xrc::RVert tmp = b; b = c; c = tmp; }
            t2[k][0] = a; t2[k][1] = b; t2[k][2] = c;
        }

        // run fast
        {
            struct FastHost : FuzzHost {
                uint32_t* fast_px;
                uint32_t* px() { return fast_px; }
            } h2;
            *static_cast<FuzzHost*>(&h2) = h;
            h2.fast_px = fh.buf.data();
            const bool ok = g3xrc::blit_quad_exec(h2, v, h.tex_view(), tsx, tsy, strip);
            if (!ok) continue;                        // rejected — nothing to compare
        }
        // run exact
        {
            struct SlowHost : FuzzHost {
                uint32_t* slow_px;
                uint32_t* px() { return slow_px; }
            } h3;
            *static_cast<FuzzHost*>(&h3) = h;
            h3.slow_px = slow.data();
            for (unsigned k = 0; k < 2; ++k)
                g3xrc::raster_tri(h3, t2[k][0], t2[k][1], t2[k][2], tsx, tsy);
        }

        ++ran;
        ++by_bpp[h.tbpp == 1 ? 0 : 1];
        ++by_class[(unsigned)h.bc > 4 ? 4 : (unsigned)h.bc];
        if (fh.buf != slow) {
            ++mism;
            if (mism <= 2) {
                std::printf("MISMATCH iter=%u bpp=%u strip=%u sf=%u df=%u W=%u H=%u lod=%u quad=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)\n",
                            it, h.tbpp, (unsigned)strip, h.sf, h.df, h.W, h.H, h.tlod,
                            v[0].x, v[0].y, v[1].x, v[1].y, v[2].x, v[2].y, v[3].x, v[3].y);
                int shown = 0;
                for (size_t i = 0; i < fh.buf.size() && shown < 4; ++i) {
                    if (fh.buf[i] != slow[i]) {
                        const unsigned x = (unsigned)(i % h.W), y = (unsigned)(i / h.W);
                        // inline recompute of the exact path's texel + blend
                        const unsigned ty = (y - y0c) & (h.th - 1);
                        const unsigned tx = (x - x0c) & (h.tw - 1);
                        uint32_t srcv;
                        if (h.tbpp == 2) {
                            const size_t ti = (size_t)ty * h.tw + tx;
                            const uint16_t p5 = (uint16_t)(h.tex16[2*ti] | (h.tex16[2*ti+1] << 8));
                            const unsigned r5=(p5>>11)&0x1Fu,g6=(p5>>5)&0x3Fu,b5=p5&0x1Fu;
                            srcv = 0xFF000000u|((r5<<3|r5>>2)<<16)|((g6<<2|g6>>4)<<8)|(b5<<3|b5>>2);
                        } else {
                            srcv = h.pal_mult_v[h.tex8[(size_t)ty*h.tw+tx]];
                        }
                        const unsigned a = h.const_alpha_v, ia = 255u - a;
                        const uint32_t dst = h.buf[i];
                        uint32_t lerp;
                        {
                            unsigned b=(((srcv&0xFFu)*a+255u)>>8)+(((dst&0xFFu)*ia+255u)>>8);
                            unsigned g=((((srcv>>8)&0xFFu)*a+255u)>>8)+((((dst>>8)&0xFFu)*ia+255u)>>8);
                            unsigned r=((((srcv>>16)&0xFFu)*a+255u)>>8)+((((dst>>16)&0xFFu)*ia+255u)>>8);
                            if(b>255u)b=255u; if(g>255u)g=255u; if(r>255u)r=255u;
                            lerp = 0xFF000000u|(r<<16)|(g<<8)|b;
                        }
                        std::printf("   (%3u,%3u) pre=%08x fast=%08x slow=%08x | texel(%u,%u) src=%08x a=%u lerp=%08x %s\n",
                                    x, y, dst, fh.buf[i], slow[i], tx, ty, srcv, a, lerp,
                                    lerp == fh.buf[i] ? "FAST MATCHES recompute" :
                                    (lerp == slow[i] ? "SLOW matches recompute" : "NEITHER"));
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
    return mism ? 1 : 0;
}
