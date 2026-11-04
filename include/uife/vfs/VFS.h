#pragma once

#include "uife/drivers/fs/IFSParser.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace uife {

class VFS {
public:
    explicit VFS(const IFSParser& parser) noexcept;

    [[nodiscard]] std::vector<VFSNode> list_dir(const std::string& path) const;
    [[nodiscard]] VFSNode stat(const std::string& path) const;
    std::size_t read(const std::string& path, std::uint64_t offset,
                     std::span<std::byte> destination) const;
    // 读取命名数据流；stream 必须是 VFSNode::metadata.alternate_streams 中报告过的名称。
    std::size_t read_stream(const std::string& path, const std::string& stream,
                            std::uint64_t offset,
                            std::span<std::byte> destination) const;
    [[nodiscard]] std::string_view filesystem_name() const noexcept;
    [[nodiscard]] bool can_read_content() const noexcept;

    [[nodiscard]] static std::string normalize_path(const std::string& path);

private:
    const IFSParser& parser_;
};

} // namespace uife
