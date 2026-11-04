#pragma once

#include "uife/drivers/fs/IFSParser.h"

#include <memory>

namespace uife {

class BtrfsSendFsParser final : public IFSParser {
public:
    BtrfsSendFsParser();
    ~BtrfsSendFsParser() override;

    void mount(std::shared_ptr<IImageDriver> image,
               std::uint64_t byte_offset,
               std::uint64_t byte_length) override;
    [[nodiscard]] std::vector<VFSNode> list_dir(
        const std::string& path) const override;
    [[nodiscard]] VFSNode get_metadata(const std::string& path) const override;
    std::size_t read_file(const std::string& path,
                          std::uint64_t offset,
                          std::span<std::byte> destination) const override;
    [[nodiscard]] std::string_view filesystem_name() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace uife
