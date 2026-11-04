#pragma once

#include "uife/VFSNode.h"
#include "uife/drivers/image/IImageDriver.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace uife {

class IFSParser {
public:
    virtual ~IFSParser() = default;

    virtual void mount(std::shared_ptr<IImageDriver> image,
                       std::uint64_t byte_offset,
                       std::uint64_t byte_length) = 0;
    [[nodiscard]] virtual std::vector<VFSNode> list_dir(const std::string& path) const = 0;
    [[nodiscard]] virtual VFSNode get_metadata(const std::string& path) const = 0;
    virtual std::size_t read_file(const std::string& path,
                                  std::uint64_t offset,
                                  std::span<std::byte> destination) const = 0;

    // 读取命名备用数据流（NTFS ADS、HFS+/APFS 资源分支等）。
    // 默认后端不支持；支持的后端需同时通过 VFSMetadata::alternate_streams 报告可用流。
    virtual std::size_t read_stream(const std::string& path,
                                    const std::string& stream,
                                    std::uint64_t offset,
                                    std::span<std::byte> destination) const {
        (void)path;
        (void)stream;
        (void)offset;
        (void)destination;
        throw std::runtime_error("该文件系统后端不支持命名数据流读取");
    }

    // 后端能否读取文件内容。默认可以；libfsrefs 这类只有目录枚举接口的后端返回 false，
    // 提取器据此跳过内容导出并说明原因，而不是逐个文件报错。
    [[nodiscard]] virtual bool can_read_content() const noexcept { return true; }

    [[nodiscard]] virtual std::string_view filesystem_name() const noexcept = 0;
};

} // namespace uife
