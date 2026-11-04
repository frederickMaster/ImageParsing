#include "uife/utils/Sha256.h"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace uife {
namespace {

constexpr std::array<std::uint32_t, 64> round_constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
    0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
    0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
    0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

std::uint32_t load_be32(const std::byte* data) {
    return (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[0])) << 24U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[1])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[2])) << 8U) |
           static_cast<std::uint32_t>(std::to_integer<unsigned char>(data[3]));
}

} // namespace

Sha256::Sha256()
    : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
             0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}

void Sha256::update(std::span<const std::byte> data) {
    if (finished_) {
        throw std::logic_error("SHA-256 已结束");
    }
    total_bytes_ += data.size();
    std::size_t offset = 0;
    if (buffered_bytes_ > 0) {
        const auto count = std::min(buffer_.size() - buffered_bytes_, data.size());
        std::copy_n(data.begin(), count, buffer_.begin() +
                    static_cast<std::ptrdiff_t>(buffered_bytes_));
        buffered_bytes_ += count;
        offset += count;
        if (buffered_bytes_ == buffer_.size()) {
            transform(buffer_.data());
            buffered_bytes_ = 0;
        }
    }
    while (data.size() - offset >= buffer_.size()) {
        transform(data.data() + offset);
        offset += buffer_.size();
    }
    if (offset < data.size()) {
        buffered_bytes_ = data.size() - offset;
        std::copy(data.begin() + static_cast<std::ptrdiff_t>(offset), data.end(), buffer_.begin());
    }
}

std::array<std::byte, 32> Sha256::finish() {
    if (finished_) {
        throw std::logic_error("SHA-256 finish 不能重复调用");
    }
    const auto bit_length = total_bytes_ * 8U;
    buffer_[buffered_bytes_++] = std::byte{0x80};
    if (buffered_bytes_ > 56) {
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_bytes_),
                  buffer_.end(), std::byte{0});
        transform(buffer_.data());
        buffered_bytes_ = 0;
    }
    std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_bytes_),
              buffer_.begin() + 56, std::byte{0});
    for (std::size_t index = 0; index < 8; ++index) {
        buffer_[63 - index] = static_cast<std::byte>((bit_length >> (index * 8U)) & 0xffU);
    }
    transform(buffer_.data());
    finished_ = true;

    std::array<std::byte, 32> digest{};
    for (std::size_t index = 0; index < state_.size(); ++index) {
        for (std::size_t byte_index = 0; byte_index < 4; ++byte_index) {
            digest[index * 4 + byte_index] = static_cast<std::byte>(
                (state_[index] >> ((3U - static_cast<unsigned int>(byte_index)) * 8U)) & 0xffU);
        }
    }
    return digest;
}

std::string Sha256::to_hex(const std::array<std::byte, 32>& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(64);
    for (const auto byte : digest) {
        const auto value = std::to_integer<unsigned char>(byte);
        text.push_back(digits[value >> 4U]);
        text.push_back(digits[value & 0x0fU]);
    }
    return text;
}

void Sha256::transform(const std::byte* block) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
        words[index] = load_be32(block + index * 4);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
        const auto x = words[index - 15];
        const auto y = words[index - 2];
        const auto sigma0 = std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3U);
        const auto sigma1 = std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10U);
        words[index] = words[index - 16] + sigma0 + words[index - 7] + sigma1;
    }

    auto a = state_[0];
    auto b = state_[1];
    auto c = state_[2];
    auto d = state_[3];
    auto e = state_[4];
    auto f = state_[5];
    auto g = state_[6];
    auto h = state_[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
        const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const auto choice = (e & f) ^ (~e & g);
        const auto temp1 = h + sum1 + choice + round_constants[index] + words[index];
        const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto temp2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

} // namespace uife
