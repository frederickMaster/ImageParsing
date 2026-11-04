#pragma once

#include "uife/drivers/fs/IFSParser.h"

#include <memory>

namespace uife {

// ReFS 解析器：基于 libfsrefs（libyal）。
// 注意能力边界：libfsrefs 目前只提供目录枚举与元数据接口，没有文件内容读取入口，
// 因此这里可以 list/stat，但 read_file 会明确报错，而不是返回空数据。
class RefsFsParser final : public IFSParser {
public:
    RefsFsParser();
    ~RefsFsParser() override;

    RefsFsParser(const RefsFsParser&) = delete;
    RefsFsParser& operator=(const RefsFsParser&) = delete;

    void mount(std::shared_ptr<IImageDriver> image,
               std::uint64_t byte_offset,
               std::uint64_t byte_length) override;
    [[nodiscard]] std::vector<VFSNode> list_dir(const std::string& path) const override;
    [[nodiscard]] VFSNode get_metadata(const std::string& path) const override;
    std::size_t read_file(const std::string& path,
                          std::uint64_t offset,
                          std::span<std::byte> destination) const override;
    [[nodiscard]] std::string_view filesystem_name() const noexcept override;

    // 后端不支持读取文件内容（libfsrefs 未提供读取接口）。
    [[nodiscard]] bool can_read_content() const noexcept override { return false; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace uife
