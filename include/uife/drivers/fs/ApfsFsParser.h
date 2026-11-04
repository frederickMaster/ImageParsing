#pragma once

#include "uife/drivers/fs/IFSParser.h"

#include <memory>

namespace uife {

// APFS 解析器：基于 libfsapfs（libyal）实现。
// 与 libtsk 不同，libfsapfs 支持容器级多卷、快照以及加密卷解锁，
// 因此这里承担 APFS 的全部读取职责（含命名数据流 = 扩展属性）。
class ApfsFsParser final : public IFSParser {
public:
    explicit ApfsFsParser(std::string password = {});
    ~ApfsFsParser() override;

    ApfsFsParser(const ApfsFsParser&) = delete;
    ApfsFsParser& operator=(const ApfsFsParser&) = delete;

    void mount(std::shared_ptr<IImageDriver> image,
               std::uint64_t byte_offset,
               std::uint64_t byte_length) override;
    [[nodiscard]] std::vector<VFSNode> list_dir(const std::string& path) const override;
    [[nodiscard]] VFSNode get_metadata(const std::string& path) const override;
    std::size_t read_file(const std::string& path,
                          std::uint64_t offset,
                          std::span<std::byte> destination) const override;
    std::size_t read_stream(const std::string& path,
                            const std::string& stream,
                            std::uint64_t offset,
                            std::span<std::byte> destination) const override;
    [[nodiscard]] std::string_view filesystem_name() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace uife
