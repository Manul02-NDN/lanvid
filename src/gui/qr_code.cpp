#include "gui/qr_code.hpp"

#include <algorithm>
#include <cstdlib>

namespace lv::gui {

namespace {

struct VersionInfo {
    int ec_per_block;
    int blocks;
    int data_per_block;
    int align;  // centre of the single alignment pattern (0 = none)
};
// Error correction level M, versions 1..6 (all blocks of a version have equal size).
constexpr VersionInfo kM[7] = {{0, 0, 0, 0},   {10, 1, 16, 0}, {16, 1, 28, 18}, {26, 1, 44, 22},
                               {18, 2, 32, 26}, {24, 2, 43, 30}, {16, 4, 27, 34}};

uint8_t gf_mul(uint8_t x, uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; --i) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return uint8_t(z);
}

std::vector<uint8_t> rs_divisor(int degree) {
    std::vector<uint8_t> r(size_t(degree), 0);
    r[size_t(degree) - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (int j = 0; j < degree; ++j) {
            r[size_t(j)] = gf_mul(r[size_t(j)], root);
            if (j + 1 < degree) r[size_t(j)] ^= r[size_t(j) + 1];
        }
        root = gf_mul(root, 0x02);
    }
    return r;
}

std::vector<uint8_t> rs_remainder(const std::vector<uint8_t>& data, const std::vector<uint8_t>& div) {
    std::vector<uint8_t> r(div.size(), 0);
    for (uint8_t b : data) {
        const uint8_t factor = b ^ r[0];
        r.erase(r.begin());
        r.push_back(0);
        for (size_t i = 0; i < r.size(); ++i) r[i] ^= gf_mul(div[i], factor);
    }
    return r;
}

struct Grid {
    int size;
    std::vector<uint8_t> dark, func;
    explicit Grid(int s) : size(s), dark(size_t(s) * s, 0), func(size_t(s) * s, 0) {}
    void set(int x, int y, bool d) {
        if (x < 0 || y < 0 || x >= size || y >= size) return;
        dark[size_t(y) * size + x] = d ? 1 : 0;
        func[size_t(y) * size + x] = 1;
    }
    bool is_func(int x, int y) const { return func[size_t(y) * size + x] != 0; }
};

void finder(Grid& g, int cx, int cy) {
    for (int dy = -4; dy <= 4; ++dy)
        for (int dx = -4; dx <= 4; ++dx) {
            const int d = std::max(std::abs(dx), std::abs(dy));
            g.set(cx + dx, cy + dy, d != 2 && d != 4);
        }
}

void alignment(Grid& g, int cx, int cy) {
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) g.set(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
}

void format_bits(Grid& g, int mask) {
    const int data = (0 << 3) | mask;  // level M = 00
    int rem = data;
    for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    const int bits = ((data << 10) | rem) ^ 0x5412;
    auto bit = [&](int i) { return ((bits >> i) & 1) != 0; };
    const int s = g.size;
    for (int i = 0; i <= 5; ++i) g.set(8, i, bit(i));
    g.set(8, 7, bit(6));
    g.set(8, 8, bit(7));
    g.set(7, 8, bit(8));
    for (int i = 9; i < 15; ++i) g.set(14 - i, 8, bit(i));
    for (int i = 0; i < 8; ++i) g.set(s - 1 - i, 8, bit(i));
    for (int i = 8; i < 15; ++i) g.set(8, s - 15 + i, bit(i));
    g.set(8, s - 8, true);  // dark module
}

}  // namespace

QrCode qr_encode(const std::string& text) {
    QrCode out;
    const size_t n = text.size();
    int ver = 0;
    for (int v = 1; v <= 6; ++v) {
        const size_t cap_bits = size_t(kM[v].blocks * kM[v].data_per_block) * 8;
        if (4 + 8 + n * 8 <= cap_bits) {
            ver = v;
            break;
        }
    }
    if (!ver) return out;
    const VersionInfo& vi = kM[ver];
    const size_t data_cw = size_t(vi.blocks * vi.data_per_block);

    // ---- data bit stream: mode 0100 (byte), 8-bit count, bytes, terminator, padding ----
    std::vector<uint8_t> bits;
    auto put = [&](uint32_t v, int nb) {
        for (int i = nb - 1; i >= 0; --i) bits.push_back(uint8_t((v >> i) & 1));
    };
    put(0x4, 4);
    put(uint32_t(n), 8);
    for (unsigned char c : text) put(c, 8);
    const size_t cap = data_cw * 8;
    for (int i = 0; i < 4 && bits.size() < cap; ++i) bits.push_back(0);
    while (bits.size() % 8) bits.push_back(0);
    std::vector<uint8_t> data;
    for (size_t i = 0; i < bits.size(); i += 8) {
        uint8_t b = 0;
        for (int k = 0; k < 8; ++k) b = uint8_t((b << 1) | bits[i + size_t(k)]);
        data.push_back(b);
    }
    for (uint8_t pad = 0xEC; data.size() < data_cw; pad ^= 0xEC ^ 0x11) data.push_back(pad);

    // ---- Reed-Solomon per block + interleaving ----
    const auto div = rs_divisor(vi.ec_per_block);
    std::vector<std::vector<uint8_t>> dblocks, eblocks;
    for (int b = 0; b < vi.blocks; ++b) {
        std::vector<uint8_t> blk(data.begin() + b * vi.data_per_block, data.begin() + (b + 1) * vi.data_per_block);
        eblocks.push_back(rs_remainder(blk, div));
        dblocks.push_back(std::move(blk));
    }
    std::vector<uint8_t> cw;
    for (int i = 0; i < vi.data_per_block; ++i)
        for (int b = 0; b < vi.blocks; ++b) cw.push_back(dblocks[size_t(b)][size_t(i)]);
    for (int i = 0; i < vi.ec_per_block; ++i)
        for (int b = 0; b < vi.blocks; ++b) cw.push_back(eblocks[size_t(b)][size_t(i)]);

    // ---- function patterns ----
    const int size = 17 + 4 * ver;
    Grid g(size);
    for (int i = 0; i < size; ++i) {
        g.set(6, i, i % 2 == 0);
        g.set(i, 6, i % 2 == 0);
    }
    finder(g, 3, 3);
    finder(g, size - 4, 3);
    finder(g, 3, size - 4);
    if (vi.align) alignment(g, vi.align, vi.align);
    format_bits(g, 0);  // reserves the format areas too

    // ---- codeword placement (zig-zag, skipping the vertical timing column) ----
    size_t bi = 0;
    const size_t total_bits = cw.size() * 8;
    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < size; ++vert) {
            for (int j = 0; j < 2; ++j) {
                const int x = right - j;
                const bool upward = ((right + 1) & 2) == 0;
                const int y = upward ? size - 1 - vert : vert;
                if (g.is_func(x, y)) continue;
                bool d = false;
                if (bi < total_bits) d = ((cw[bi >> 3] >> (7 - (bi & 7))) & 1) != 0;
                ++bi;
                if ((x + y) % 2 == 0) d = !d;  // mask 0
                g.dark[size_t(y) * size + x] = d ? 1 : 0;
            }
        }
    }
    out.size = size;
    out.version = ver;
    out.modules = std::move(g.dark);
    return out;
}

}  // namespace lv::gui
