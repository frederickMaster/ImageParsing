#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
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

    // 镜像层面的完整性提示，例如 EWF 缺少后续分卷、容器只拿到前几个分段。
    // 返回空字符串表示打开时没有发现问题。这不阻止打开：能读到的部分仍然可用，
    // 上层据此在探测和导出前提示用户，而不是等到读到中途才失败。
    [[nodiscard]] virtual std::string integrity_note() const { return {}; }
};

} // namespace uife
