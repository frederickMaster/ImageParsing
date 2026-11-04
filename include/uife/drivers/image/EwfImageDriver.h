#pragma once

#include "uife/drivers/image/IImageDriver.h"

#include <memory>

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

private:
    struct State;

    std::unique_ptr<State> state_;
    std::filesystem::path source_path_;
    std::uint64_t size_{};
    std::uint32_t sector_size_{512};
};

} // namespace uife
