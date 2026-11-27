#pragma once
// MSB-first bit writer / reader with a 64-bit accumulator.
// No byte stuffing: slice boundaries are known from the transport layer.

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "common/platform.hpp"

namespace lv {

class BitWriter {
public:
    // Caller guarantees `cap` is at least the worst-case output size (+4 bytes slack).
    BitWriter(uint8_t* buf, size_t cap) : start_(buf), p_(buf), end_(buf + cap) {}

    // Appends the low `n` bits of `bits` (n in [0, 32]; upper bits must be zero).
    LV_FORCE_INLINE void put(uint32_t bits, int n) {
        acc_ = (acc_ << n) | bits;
        n_ += n;
        if (n_ >= 32) {
            n_ -= 32;
            const uint32_t w = uint32_t(acc_ >> n_);
            assert(p_ + 4 <= end_);
            p_[0] = uint8_t(w >> 24);
            p_[1] = uint8_t(w >> 16);
            p_[2] = uint8_t(w >> 8);
            p_[3] = uint8_t(w);
            p_ += 4;
        }
    }

    // Pads with 1-bits to a byte boundary and drains the accumulator.
    void flush() {
        const int pad = (8 - (n_ & 7)) & 7;
        if (pad) put((1u << pad) - 1u, pad);
        while (n_ >= 8) {
            n_ -= 8;
            assert(p_ < end_);
            *p_++ = uint8_t(acc_ >> n_);
        }
    }

    size_t size() const { return size_t(p_ - start_); }

private:
    uint8_t* start_;
    uint8_t* p_;
    [[maybe_unused]] uint8_t* end_;
    uint64_t acc_ = 0;
    int n_ = 0;
};

class BitReader {
public:
    BitReader(const uint8_t* p, size_t n) : start_(p), p_(p), end_(p + n), total_bits_(uint64_t(n) * 8) {}

    // After refill() at least 57 bits are available (enough for one Huffman code + value bits).
    LV_FORCE_INLINE void refill() {
        while (n_ <= 56) {
            uint64_t b = 0;
            if (p_ < end_) b = *p_++;
            else ++over_;
            acc_ |= b << (56 - n_);
            n_ += 8;
        }
    }
    LV_FORCE_INLINE uint32_t peek(int n) const { return uint32_t(acc_ >> (64 - n)); }  // n in [1, 32]
    LV_FORCE_INLINE void skip(int n) {
        acc_ <<= n;
        n_ -= n;
    }
    LV_FORCE_INLINE uint32_t get(int n) {  // n in [1, 32]
        const uint32_t v = peek(n);
        skip(n);
        return v;
    }

    uint64_t bits_consumed() const { return (uint64_t(p_ - start_) + over_) * 8 - uint64_t(n_); }
    bool overrun() const { return bits_consumed() > total_bits_; }

private:
    const uint8_t* start_;
    const uint8_t* p_;
    const uint8_t* end_;
    uint64_t total_bits_;
    uint64_t acc_ = 0;
    int n_ = 0;
    uint64_t over_ = 0;
};

}  // namespace lv
