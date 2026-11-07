// blit_case_debug.cpp — single controlled case: 64x64 tile quad at (8,8),
// u=v=0 origin, opaque blend, no chroma. Dump both paths' first texels.
#include "../src/g3x_raster_core.h"
#include <cstdio>
#include <cstring>
#include <vector>

struct FH {
    unsigned w = 128, h = 128;
    std::vector<uint32_t> buf;
    uint32_t constant_ = 0xFFu;
    unsigned const_alpha_v = 0xFFu;
    bool color_fill_v = false;
    unsigned sf = 4, df = 0;
    g3xrc::BlendClass bc = g3xrc::BC_OPAQUE;
    bool chroma_on_v = false;
    uint8_t hit[256] = {};
    uint32_t pal[256] = {};
    uint32_t pmult[256] = {};
    uint32_t pmult_vc = 0xFFFFFFFFu;

    unsigned width() const  { return w; }
    unsigned height() const { return h; }
    uint32_t* px()          { return buf.data(); }
    uint32_t constant() const { return constant_; }
    unsigned const_alpha() const { return const_alpha_v; }
    bool color_fill() const { return color_fill_v; }
    unsigned blend_sf() const { return sf; }
    unsigned blend_df() const { return df; }
    g3xrc::BlendClass blend_class() const { return bc; }
    bool chroma_on() const { return chroma_on_v; }
    const uint8_t* chroma_hit() const { return hit; }
    const uint32_t* palette() const { return pal; }
    const uint32_t* pal_mult() const { return pmult; }
    void ensure_pal_mult(uint32_t vc) {
        if (pmult_vc == vc) return;
        const unsigned vr = vc & 0xFFu, vg = (vc >> 8) & 0xFFu, vb = (vc >> 16) & 0xFFu;
        for (int i = 0; i < 256; ++i) {
            const uint32_t p = pal[i];
            pmult[i] = 0xFF000000u
                | ((((p & 0xFFu) * vr + 255u) >> 8))
                | ((((p >> 8) & 0xFFu) * vg + 255u) >> 8) << 8
                | ((((p >> 16) & 0xFFu) * vb + 255u) >> 8) << 16;
        }
        pmult_vc = vc;
    }
    bool has_tex() const { return true; }
    g3xrc::TexView cur{ nullptr, 0, 0, 0, 1 };
    g3xrc::TexView tex_view() const { return cur; }
};

int main() {
    FH h;
    for (int i = 0; i < 256; ++i) h.pal[i] = 0xFF000000u | ((i * 0x010101) & 0xFFFFFF);
    const unsigned lod = 6, tw = 64, th = 64;
    const float tsx = (float)(1u << lod) / 256.0f, tsy = tsx;   // 0.25
    std::vector<uint8_t> tex((size_t)tw * th);
    for (unsigned i = 0; i < tw * th; ++i) tex[i] = (uint8_t)i;

    const int x0 = 8, y0 = 8;
    const float du = 256.0f / 64.0f;                 // 4.0 u per pixel
    g3xrc::RVert v[4] = {
        { (float)x0,         (float)y0,         0.0f,             0.0f,             0xFFFFFFFFu },
        { (float)(x0 + 64),  (float)y0,         64.0f * du,       0.0f,             0xFFFFFFFFu },
        { (float)(x0 + 64),  (float)(y0 + 64),  64.0f * du,       64.0f * du,       0xFFFFFFFFu },
        { (float)x0,         (float)(y0 + 64),  0.0f,             64.0f * du,       0xFFFFFFFFu },
    };
    h.ensure_pal_mult(0xFFFFFFu);
    g3xrc::TexView tv{ tex.data(), tw, th, lod, 1 };
    h.cur = tv;

    // --- blit path ---
    for (auto& p : h.buf) h.buf.push_back(0xFF000000u);
    h.buf.assign((size_t)h.w * h.h, 0xFF112233u);
    if (!g3xrc::blit_quad_exec(h, v, tv, tsx, tsy)) { std::printf("blit REJECTED\n"); return 1; }
    std::printf("blit(0,0)=%08x blit(1,0)=%08x blit(0,1)=%08x\n",
                h.buf[(size_t)y0 * h.w + x0], h.buf[(size_t)y0 * h.w + x0 + 1],
                h.buf[(size_t)(y0 + 1) * h.w + x0]);

    // --- exact fan path ---
    h.buf.assign((size_t)h.w * h.h, 0xFF112233u);
    for (unsigned i = 1; i + 1 < 4; ++i)
        g3xrc::raster_tri(h, v[0], v[i], v[i + 1], tsx, tsy);
    std::printf("fan (0,0)=%08x fan (1,0)=%08x fan (0,1)=%08x\n",
                h.buf[(size_t)y0 * h.w + x0], h.buf[(size_t)y0 * h.w + x0 + 1],
                h.buf[(size_t)(y0 + 1) * h.w + x0]);
    int nd = 0;
    for (int y = 0; y < 64 && nd < 8; ++y)
        for (int x = 0; x < 64; ++x) {
            // recompute blit into a shadow — just re-run for diff map
            (void)x; (void)y;
        }
    // diff map: rerun blit into a copy
    std::vector<uint32_t> fan = h.buf;
    h.buf.assign((size_t)h.w * h.h, 0xFF112233u);
    g3xrc::blit_quad_exec(h, v, tv, tsx, tsy);
    for (int y = 0; y < 64 && nd < 10; ++y)
        for (int x = 0; x < 64; ++x) {
            const size_t o = (size_t)(y0 + y) * h.w + x0 + x;
            if (fan[o] != h.buf[o]) {
                std::printf("diff at +%d,%d fan=%08x blit=%08x\n", x, y, fan[o], h.buf[o]);
                if (++nd >= 10) break;
            }
        }
    std::printf("done nd=%d\n", nd);
    return 0;
}
