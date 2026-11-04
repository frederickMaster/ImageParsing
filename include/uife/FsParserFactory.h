#pragma once

#include "uife/drivers/fs/IFSParser.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uife {

class FsParserFactory {
public:
    using Probe = std::function<bool(const IImageDriver&, std::uint64_t)>;
    using Creator = std::function<std::unique_ptr<IFSParser>(std::string_view)>;

    static FsParserFactory& instance();
    void register_parser(std::string name, Probe probe, Creator creator);
    [[nodiscard]] bool supports(const IImageDriver& image,
                               std::uint64_t byte_offset) const;
    [[nodiscard]] std::string identify(const IImageDriver& image,
                                       std::uint64_t byte_offset) const;
    [[nodiscard]] std::unique_ptr<IFSParser> create(
        const IImageDriver& image, std::uint64_t byte_offset,
        std::string_view password = {}) const;
    [[nodiscard]] std::vector<std::string> supported_names() const;

private:
    struct Registration {
        std::string name;
        Probe probe;
        Creator creator;
    };
    FsParserFactory();
    std::vector<Registration> registrations_;
};

} // namespace uife
