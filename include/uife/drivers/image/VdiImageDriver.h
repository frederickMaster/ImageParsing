#pragma once

#include "uife/drivers/image/IImageDriver.h"

#include <fstream>
#include <mutex>
#include <vector>

namespace uife {

class VdiImageDriver final : public IImageDriver {
public:
    ~VdiImageDriver() override;

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
    void read_backing(std::uint64_t offset,
                      std::span<std::byte> destination) const;

    mutable std::ifstream input_;
    mutable std::mutex mutex_;
    std::filesystem::path source_path_;
    std::uint64_t backing_size_{};
    std::uint64_t virtual_size_{};
    std::uint64_t data_offset_{};
    std::uint32_t block_size_{};
    std::vector<std::uint32_t> block_map_;
};

} // namespace uife
