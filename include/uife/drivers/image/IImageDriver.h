#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

namespace uife {

class IImageDriver {
public:
    virtual ~IImageDriver() = default;

    IImageDriver() = default;
    IImageDriver(const IImageDriver&) = delete;
    IImageDriver& operator=(const IImageDriver&) = delete;

    virtual void open(const std::filesystem::path& image_path) = 0;
    virtual std::size_t read_sector(std::uint64_t sector_number,
                                    std::span<std::byte> destination) const = 0;
    virtual std::size_t read_at(std::uint64_t offset,
                                std::span<std::byte> destination) const = 0;
    [[nodiscard]] virtual std::uint64_t get_size() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t sector_size() const noexcept = 0;
    [[nodiscard]] virtual std::filesystem::path source_path() const = 0;
    [[nodiscard]] virtual std::string_view format_name() const noexcept = 0;
    virtual void close() noexcept = 0;
};

} // namespace uife
