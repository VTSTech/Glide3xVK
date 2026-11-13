// scalecopy_test.cpp — host equivalence + perf test for the P14 present-path
// optimization: gamma_scale_copy's per-pixel `x*s_w/rw` mul+div loop vs the
// cached column/row-map gather loop. Byte-exactness is the contract (same
// integer expressions build the maps; the pre-gamma'd source row packs the
// same three LUT lanes into the same byte order), so the test byte-compares
// full compose buffers — bars included — across a grid of source/window
// sizes, gamma tables, and repeat calls that exercise map invalidation.
//
// Build:  g++ -O2 -std=c++17 scripts/scalecopy_test.cpp -o /tmp/scalecopy_test
// Run:    /tmp/scalecopy_test

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <vector>

struct Ctx {
    unsigned s_w, s_h;
    std::vector<uint32_t> px;          // ARGB raster
    uint8_t lut[3][256];
    bool gamma_identity;
};

static void fill_ctx(Ctx& c, unsigned w, unsigned h, uint64_t seed, bool identity) {
    c.s_w = w; c.s_h = h; c.gamma_identity = identity;
    c.px.resize((size_t)w * h);
    uint64_t s = seed * 6364136223846793005ull + 1442695040888963407ull;
    for (auto& p : c.px) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        p = (uint32_t)(s >> 32) | 0xFF000000u;
    }
    for (int ch = 0; ch < 3; ++ch)
        for (int i = 0; i < 256; ++i)
            c.lut[ch][i] = identity ? (uint8_t)i : (uint8_t)((i * 219 + 31) & 0xFF);
}

// ---- OLD: the 0.4.8 loop, verbatim (minus the memcpy 1:1 fast path) --------

static void old_copy(const Ctx& c, uint8_t* dst, unsigned dw, unsigned dh) {
    const unsigned s_w = c.s_w, s_h = c.s_h;
    unsigned rw = dw, rh = dh, ox = 0, oy = 0;
    const uint64_t src_x = (uint64_t)s_w * dh;
    const uint64_t dst_x = (uint64_t)dw * s_h;
    if (src_x != dst_x) {
        if (src_x > dst_x) { rh = (unsigned)((uint64_t)dw * s_h / s_w); if (!rh) rh = 1; oy = (dh - rh) / 2; }
        else               { rw = (unsigned)((uint64_t)dh * s_w / s_h); if (!rw) rw = 1; ox = (dw - rw) / 2; }
        uint32_t* d32 = (uint32_t*)dst;
        for (unsigned y = 0; y < oy; ++y) std::memset(d32 + (size_t)y * dw, 0, (size_t)dw * 4);
        for (unsigned y = oy + rh; y < dh; ++y) std::memset(d32 + (size_t)y * dw, 0, (size_t)dw * 4);
        for (unsigned y = oy; y < oy + rh; ++y) {
            if (ox) std::memset(d32 + (size_t)y * dw, 0, (size_t)ox * 4);
            if (dw > ox + rw)
                std::memset(d32 + (size_t)y * dw + ox + rw, 0, (size_t)(dw - ox - rw) * 4);
        }
    }
    uint8_t* d = dst + ((size_t)oy * dw + ox) * 4;
    for (unsigned y = 0; y < rh; ++y) {
        const unsigned sy = y * s_h / rh;
        const uint32_t* row = c.px.data() + (size_t)sy * s_w;
        for (unsigned x = 0; x < rw; ++x, d += 4) {
            const unsigned sx = x * s_w / rw;
            const uint32_t p = row[sx];
            d[0] = c.lut[2][p & 0xFF];
            d[1] = c.lut[1][(p >> 8) & 0xFF];
            d[2] = c.lut[0][(p >> 16) & 0xFF];
            d[3] = 0xFF;
        }
        d = dst + ((size_t)(oy + y + 1) * dw + ox) * 4;
    }
}

// ---- NEW: maps + pre-gamma'd row + uint32 gather (g3x_frame.cpp verbatim) --

static unsigned* s_scale_cmap = nullptr;
static unsigned   s_scale_cmap_n = 0;
static unsigned   s_scale_cmap_key_w = 0;
static unsigned* s_scale_rmap = nullptr;
static unsigned   s_scale_rmap_n = 0;
static unsigned   s_scale_rmap_key_h = 0;
static uint32_t*  s_scale_row = nullptr;
static unsigned   s_scale_row_n = 0;

static void scale_maps_ensure(const Ctx& c, unsigned rw, unsigned rh) {
    const unsigned s_w = c.s_w, s_h = c.s_h;
    if (!s_scale_cmap || s_scale_cmap_n != rw || s_scale_cmap_key_w != s_w) {
        delete[] s_scale_cmap;
        s_scale_cmap = new unsigned[rw];
        for (unsigned x = 0; x < rw; ++x) s_scale_cmap[x] = x * s_w / rw;
        s_scale_cmap_n = rw;
        s_scale_cmap_key_w = s_w;
    }
    if (!s_scale_rmap || s_scale_rmap_n != rh || s_scale_rmap_key_h != s_h) {
        delete[] s_scale_rmap;
        s_scale_rmap = new unsigned[rh];
        for (unsigned y = 0; y < rh; ++y) s_scale_rmap[y] = y * s_h / rh;
        s_scale_rmap_n = rh;
        s_scale_rmap_key_h = s_h;
    }
    if (!s_scale_row || s_scale_row_n != s_w) {
        delete[] s_scale_row;
        s_scale_row = new uint32_t[s_w];
        s_scale_row_n = s_w;
    }
}

static void new_copy(const Ctx& c, uint8_t* dst, unsigned dw, unsigned dh) {
    if (c.gamma_identity && dw == c.s_w && dh == c.s_h) {
        std::memcpy(dst, c.px.data(), (size_t)c.s_w * c.s_h * 4);
        return;
    }
    const unsigned s_w = c.s_w, s_h = c.s_h;
    unsigned rw = dw, rh = dh, ox = 0, oy = 0;
    const uint64_t src_x = (uint64_t)s_w * dh;
    const uint64_t dst_x = (uint64_t)dw * s_h;
    if (src_x != dst_x) {
        if (src_x > dst_x) { rh = (unsigned)((uint64_t)dw * s_h / s_w); if (!rh) rh = 1; oy = (dh - rh) / 2; }
        else               { rw = (unsigned)((uint64_t)dh * s_w / s_h); if (!rw) rw = 1; ox = (dw - rw) / 2; }
        uint32_t* d32 = (uint32_t*)dst;
        for (unsigned y = 0; y < oy; ++y) std::memset(d32 + (size_t)y * dw, 0, (size_t)dw * 4);
        for (unsigned y = oy + rh; y < dh; ++y) std::memset(d32 + (size_t)y * dw, 0, (size_t)dw * 4);
        for (unsigned y = oy; y < oy + rh; ++y) {
            if (ox) std::memset(d32 + (size_t)y * dw, 0, (size_t)ox * 4);
            if (dw > ox + rw)
                std::memset(d32 + (size_t)y * dw + ox + rw, 0, (size_t)(dw - ox - rw) * 4);
        }
    }
    scale_maps_ensure(c, rw, rh);
    uint8_t* d = dst + ((size_t)oy * dw + ox) * 4;
    unsigned last_sy = 0xFFFFFFFFu;
    for (unsigned y = 0; y < rh; ++y) {
        const unsigned sy = s_scale_rmap[y];
        if (sy != last_sy) {
            const uint32_t* row = c.px.data() + (size_t)sy * s_w;
            for (unsigned x = 0; x < s_w; ++x) {
                const uint32_t p = row[x];
                s_scale_row[x] = 0xFF000000u
                    | (uint32_t)c.lut[2][p & 0xFF]
                    | ((uint32_t)c.lut[1][(p >> 8) & 0xFF] << 8)
                    | ((uint32_t)c.lut[0][(p >> 16) & 0xFF] << 16);
            }
            last_sy = sy;
        }
        for (unsigned x = 0; x < rw; ++x, d += 4)
            std::memcpy(d, s_scale_row + s_scale_cmap[x], 4);
        d = dst + ((size_t)(oy + y + 1) * dw + ox) * 4;
    }
}

// ---- grid test --------------------------------------------------------------

int main() {
    const unsigned srcs[][2] = {
        {320,200},{400,300},{512,384},{640,480},{800,600},{640,400},{1024,768},{400,600}
    };
    const unsigned wins[][2] = {
        {640,480},{800,600},{1280,720},{1280,960},{1440,1080},{1920,1080},{2560,1440},
        {1371,823},{1024,768},{699,511},{1000,1000},{2048,1152}
    };
    long cases = 0, fails = 0;
    for (int gi = 0; gi < 2; ++gi) {
        for (auto& sw : srcs) {
            for (auto& wn : wins) {
                Ctx c;
                fill_ctx(c, sw[0], sw[1], cases + 1, gi == 0);
                std::vector<uint8_t> a((size_t)wn[0] * wn[1] * 4, 0xAB);
                std::vector<uint8_t> b((size_t)wn[0] * wn[1] * 4, 0xCD);
                old_copy(c, a.data(), wn[0], wn[1]);
                new_copy(c, b.data(), wn[0], wn[1]);
                ++cases;
                if (a != b) {
                    ++fails;
                    size_t diff = 0;
                    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) ++diff;
                    std::printf("MISMATCH src=%ux%u win=%ux%u gamma=%s bytes=%zu/%zu\n",
                                sw[0], sw[1], wn[0], wn[1], gi ? "off" : "id", diff, a.size());
                }
            }
        }
    }
    // map-invalidation sweep: alternate source sizes between calls, same buffers
    {
        Ctx c1, c2;
        fill_ctx(c1, 800, 600, 77, true);
        fill_ctx(c2, 640, 480, 78, false);
        std::vector<uint8_t> a(1920u * 1080u * 4), b(1920u * 1080u * 4);
        for (int i = 0; i < 4; ++i) {
            const Ctx& c = (i & 1) ? c2 : c1;
            old_copy(c, a.data(), 1920, 1080);
            new_copy(c, b.data(), 1920, 1080);
            ++cases;
            if (a != b) { ++fails; std::printf("MISMATCH invalidation iter=%d\n", i); }
        }
    }
    std::printf("cases=%ld fails=%ld\n", cases, fails);

    // perf: the field-log compose (800x600 -> 1920x1080 legacy), 200 frames
    Ctx c;
    fill_ctx(c, 800, 600, 99, true);
    std::vector<uint8_t> dst(1920u * 1080u * 4);
    const int N = 200;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) old_copy(c, dst.data(), 1920, 1080);
    auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) new_copy(c, dst.data(), 1920, 1080);
    auto t2 = std::chrono::steady_clock::now();
    auto ms = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count() / N;
    };
    std::printf("compose 800x600->1920x1080: old=%.2fms new=%.2fms per frame (%.1fx)\n",
                ms(t0, t1), ms(t1, t2), ms(t0, t1) / ms(t1, t2));
    return fails ? 1 : 0;
}
