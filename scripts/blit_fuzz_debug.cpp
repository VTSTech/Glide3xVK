// blit_fuzz_debug.cpp — CPU-route perf debug: fuzz blit_quad_exec against the
// barycentric raster_tri path on synthetic 1:1 tile quads, dump mismatches.
// Build: g++ -O2 -std=c++17 -I src scripts/blit_fuzz_debug.cpp -o /tmp/blit_fuzz
#include "../src/g3x_raster_core.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

struct FH {
    unsigned w = 0, h = 0;
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
    unsigned pal_epoch = 1;

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

static uint32_t rng_state = 0x12345678u;
static uint32_t xrnd() {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    FH h;
    h.w = 320; h.h = 320;
    h.buf.resize((size_t)h.w * h.h);
    for (int i = 0; i < 256; ++i) {
        h.pal[i] = 0xFF000000u | (xrnd() & 0xFFFFFFu);
        h.hit[i] = (uint8_t)((xrnd() & 7) == 0);   // ~12% chroma hits
    }
    h.hit[0] = 1;

    const unsigned lods[] = { 4, 5, 6, 7, 8 };       // 16..256
    long tested = 0, accepted = 0, mism_fan = 0, mism_strip = 0;
    int first_dump = 12;

    for (long it = 0; it < 8000; ++it) {
        const unsigned lod = lods[xrnd() % 5];
        unsigned tw = 1u << lod, th = tw;
        // random sub-rect texture (non-square sometimes)
        if (xrnd() & 1) th = 1u << (lod - (xrnd() % 3));
        if (!th) th = 1;
        const float tsx = (float)(1u << lod) / 256.0f, tsy = tsx;
        std::vector<uint8_t> tex((size_t)tw * th);
        for (auto& b : tex) b = (uint8_t)(xrnd() & 0xFF);

        const unsigned blend_pick = xrnd() % 5;
        static const unsigned sfs[5] = { 4, 4, 1, 0, 2 }, dfs[5] = { 0, 4, 5, 2, 3 };
        h.sf = sfs[blend_pick]; h.df = dfs[blend_pick];
        h.bc = g3xrc::blend_class_of(h.sf, h.df);
        h.const_alpha_v = xrnd() & 0xFF;
        h.chroma_on_v = (xrnd() & 1) != 0;
        const uint32_t vc = (xrnd() & 1) ? 0xFFFFFFFFu : (0xFF000000u | (xrnd() & 0xFFFFFFu));

        // quad position: integer corners, exactly texel-sized (1:1)
        const unsigned qw = tw, qh = th;
        if (h.w < qw + 2 || h.h < qh + 2) continue;
        const int x0 = (int)(xrnd() % (h.w - qw - 1));
        const int y0 = (int)(xrnd() % (h.h - qh - 1));
        const float du_u = 256.0f / (float)(1u << lod);   // square-mip space: u per texel = u per px at 1:1
        const float dv_v = 256.0f / (float)(1u << lod);
        // v span: the game's tall textures keep the SQUARE space: v per pixel = 256/2^lod (texel = v*2^lod/256 both axes).
        float u_off = (float)(int)(xrnd() % 257);
        float v_off = (float)(int)(xrnd() % 257);
        // occasional off-grid offset (sub-texel) — the class the frac gate rejects
        const bool perturb = (xrnd() % 8) == 0;
        if (perturb) { u_off += 0.37f; v_off += 0.61f; }


        g3xrc::RVert q[4];
        // fan order: TL, TR, BR, BL  (u grows with x, v with y)
        const float px0 = (float)x0, py0 = (float)y0;
        const float px1 = px0 + qw, py1 = py0 + qh;
        q[0] = { px0, py0, u_off,                 v_off,                 vc };
        q[1] = { px1, py0, u_off + qw * du_u,     v_off,                 vc };
        q[2] = { px1, py1, u_off + qw * du_u,     v_off + (float)qh * dv_v, vc };
        q[3] = { px0, py1, u_off,                 v_off + (float)qh * dv_v, vc };
        // random ring rotation + occasional reversal
        const unsigned rot = xrnd() % 4;
        g3xrc::RVert v[4];
        for (unsigned i = 0; i < 4; ++i) v[i] = q[(i + rot) & 3];
        if (xrnd() & 1) { std::swap(v[1], v[3]); }
        // strip order: zigzag from the rotated ring ({0,3,1,2} index pattern),
        // rotations 0/2 + reversals keep (v1,v2) diagonal
        g3xrc::RVert sv[4];
        for (unsigned i = 0; i < 4; ++i) sv[i] = q[(i + rot) & 3];
        {
            g3xrc::RVert t0 = sv[0], t1 = sv[1], t2 = sv[2], t3 = sv[3];
            sv[0] = t0; sv[1] = t3; sv[2] = t1; sv[3] = t2;
        }

        h.ensure_pal_mult(vc & 0x00FFFFFFu);
        for (auto& p : h.buf) p = 0xFF000000u | (xrnd() & 0xFFFFFFu);
        std::vector<uint32_t> before(h.buf);

        g3xrc::TexView tv{ tex.data(), tw, th, lod, 1 };
        h.cur = tv;
        ++tested;
        if (!g3xrc::blit_quad_exec(h, v, tv, tsx, tsy, false)) continue;   // fan form
        ++accepted;
        std::vector<uint32_t> blit = h.buf;

        // exact path, FAN form
        h.buf = before;
        for (unsigned i = 1; i + 1 < 4; ++i)
            g3xrc::raster_tri(h, v[0], v[i], v[i + 1], tsx, tsy);
        if (h.buf != blit) {
            ++mism_fan;
            if (first_dump > 0) {
                --first_dump;
                std::printf("FAN MISMATCH: tex %ux%u lod%u quad %d,%d..%d,%u blend %u/%u bc %d ca %u chm %u perturb %d rot %u rev %d\n",
                            tw, th, lod, x0, y0, x0 + (int)qw, qh, h.sf, h.df, (int)h.bc,
                            h.const_alpha_v, (unsigned)h.chroma_on_v, (int)perturb, rot,
                            (int)((xrnd() & 1)));
                for (unsigned y = 0; y < qh && first_dump >= 0; ++y)
                    for (unsigned x = 0; x < qw; ++x) {
                        const size_t o = (size_t)(y0 + y) * h.w + x0 + x;
                        if (h.buf[o] != blit[o]) {
                            std::printf("  first diff at +%u,%u: blit %08x exact %08x\n",
                                        x, y, blit[o], h.buf[o]);
                            y = qh; break;
                        }
                    }
            }
        }

        // exact path, STRIP form (with the dispatcher's winding normalization)
        h.buf = before;
        for (unsigned i = 0; i + 2 < 4; ++i) {
            const g3xrc::RVert& a = sv[i]; const g3xrc::RVert& b = sv[i + 1]; const g3xrc::RVert& c = sv[i + 2];
            const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (area >= 0.0f) g3xrc::raster_tri(h, a, b, c, tsx, tsy);
            else              g3xrc::raster_tri(h, a, c, b, tsx, tsy);
        }
        if (h.buf != blit) {
            ++mism_strip;
            if (first_dump > 0) {
                --first_dump;
                std::printf("STRIP MISMATCH: tex %ux%u lod%u quad %d,%d blend %u/%u bc %d ca %u chm %u perturb %d\n",
                            tw, th, lod, x0, y0, h.sf, h.df, (int)h.bc, h.const_alpha_v,
                            (unsigned)h.chroma_on_v, (int)perturb);
            }
        }
    }
    std::printf("tested=%ld accepted=%ld mism_fan=%ld mism_strip=%ld\n",
                tested, accepted, mism_fan, mism_strip);
    return 0;
}
