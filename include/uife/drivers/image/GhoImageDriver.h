#pragma once

#include "uife/drivers/image/IImageDriver.h"

#include <fstream>
#include <mutex>
#include <optional>
#include <vector>

namespace uife {

class GhoImageDriver final : public IImageDriver {
public:
    ~GhoImageDriver() override;

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
    struct Block {
        std::uint64_t logical_offset{};
        std::uint64_t source_offset{};
        std::uint32_t size{};
        std::uint32_t source_size{};
    };

    struct Partition {
        std::uint64_t logical_offset{};
        std::uint64_t size{};
        std::size_t first_block{};
        std::size_t block_count{};
    };

    struct SourceFile {
        std::ifstream input;
        std::uint64_t logical_offset{};
        std::uint64_t source_offset{};
        std::uint64_t size{};
    };

    void load_block(std::size_t index) const;
    void read_source_at(std::uint64_t offset,
                        std::span<std::byte> destination) const;

    mutable std::mutex mutex_;
    std::filesystem::path source_path_;
    std::uint64_t size_{};
    std::vector<std::byte> track0_data_;
    std::vector<Partition> partitions_;
    std::vector<Block> blocks_;
    mutable std::vector<SourceFile> source_files_;
    std::uint8_t compression_{};
    mutable std::optional<std::size_t> cached_block_;
    mutable std::vector<std::byte> cached_data_;
};

} // namespace uife
