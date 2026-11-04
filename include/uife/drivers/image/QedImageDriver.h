#pragma once

#include "uife/drivers/image/IImageDriver.h"

#include <fstream>
#include <mutex>
#include <vector>

namespace uife {

class QedImageDriver final : public IImageDriver {
public:
    ~QedImageDriver() override;

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
    [[nodiscard]] std::uint64_t read_u64_at(std::uint64_t offset) const;
    void read_backing(std::uint64_t offset,
                      std::span<std::byte> destination) const;

    mutable std::ifstream input_;
    mutable std::mutex mutex_;
    std::filesystem::path source_path_;
    std::uint64_t backing_size_{};
    std::uint64_t virtual_size_{};
    std::uint64_t entries_per_table_{};
    std::uint32_t cluster_size_{};
    std::vector<std::uint64_t> l1_table_;
};

} // namespace uife
