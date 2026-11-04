#pragma once

#include "uife/drivers/image/IImageDriver.h"

#include <fstream>
#include <mutex>
#include <vector>

namespace uife {

class CueBinImageDriver final : public IImageDriver {
public:
    ~CueBinImageDriver() override;

    void open(const std::filesystem::path& image_path) override;
    std::size_t read_sector(std::uint64_t sector_number,
                            std::span<std::byte> destination) const override;
    std::size_t read_at(std::uint64_t offset,
                        std::span<std::byte> destination) const override;
    [[nodiscard]] std::uint64_t get_size() const noexcept override;
    [[nodiscard]] std::uint32_t sector_size() const noexcept override;
    [[nodiscard]] std::filesystem::path source_path() const override;
    [[nodiscard]] std::string_view format_name() const noexcept override;
    void close() noexcept override;

private:
    struct TrackExtent {
        std::uint64_t logical_offset{};
        std::uint64_t logical_length{};
        std::uint64_t file_sector_offset{};
        std::size_t file_index{};
        std::uint32_t stored_sector_size{};
        std::uint32_t payload_offset{};
    };

    mutable std::vector<std::ifstream> inputs_;
    mutable std::mutex mutex_;
    std::filesystem::path source_path_;
    std::uint64_t image_size_{};
    std::vector<TrackExtent> tracks_;
};

} // namespace uife
