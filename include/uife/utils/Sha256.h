#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace uife {

class Sha256 {
public:
    Sha256();
    void update(std::span<const std::byte> data);
    [[nodiscard]] std::array<std::byte, 32> finish();
    [[nodiscard]] static std::string to_hex(const std::array<std::byte, 32>& digest);

private:
    void transform(const std::byte* block);
    std::array<std::uint32_t, 8> state_;
    std::array<std::byte, 64> buffer_{};
    std::uint64_t total_bytes_{};
    std::size_t buffered_bytes_{};
    bool finished_{};
};

} // namespace uife
