#pragma once

#include "uife/drivers/image/IImageDriver.h"

#include <fstream>
#include <mutex>
#include <optional>
#include <vector>

namespace uife {

class DmgImageDriver final : public IImageDriver {
public:
    ~DmgImageDriver() override;

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
    struct Run {
        std::uint64_t logical_offset{};
        std::uint64_t logical_size{};
        std::uint64_t source_offset{};
        std::uint64_t source_size{};
        std::uint32_t type{};
    };

    void load_run(std::size_t index) const;

    mutable std::ifstream input_;
    mutable std::mutex mutex_;
    std::filesystem::path source_path_;
    std::uint64_t size_{};
    std::vector<Run> runs_;
    mutable std::optional<std::size_t> cached_run_;
    mutable std::vector<std::byte> cached_data_;
};

} // namespace uife
