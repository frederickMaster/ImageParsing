#pragma once

#include "uife/drivers/image/IImageDriver.h"

#include <memory>
#include <string>

namespace uife {

class EwfImageDriver final : public IImageDriver {
public:
    EwfImageDriver();
    ~EwfImageDriver() override;

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
    [[nodiscard]] std::string integrity_note() const override;

private:
    struct State;

    // 二分查找最早读不到的字节位置，用于把“缺后续分卷”量化成可读前缀长度。
    [[nodiscard]] std::uint64_t readable_prefix() const noexcept;

    std::unique_ptr<State> state_;
    std::filesystem::path source_path_;
    std::uint64_t size_{};
    std::uint32_t sector_size_{512};
    std::string integrity_note_;
};

} // namespace uife
