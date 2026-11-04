#pragma once

#include "uife/drivers/image/IImageDriver.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uife {

struct ImageDriverInfo {
    std::string format;
    bool supported{};
};

class DriverFactory {
public:
    using Probe = std::function<bool(std::span<const std::byte>)>;
    using Creator = std::function<std::unique_ptr<IImageDriver>()>;

    static DriverFactory& instance();
    void register_driver(std::string name, Probe probe, Creator creator, bool supported);
    [[nodiscard]] std::unique_ptr<IImageDriver> create(
        const std::filesystem::path& path) const;
    [[nodiscard]] ImageDriverInfo identify(const std::filesystem::path& path) const;
    [[nodiscard]] std::vector<std::string> supported_formats() const;
    [[nodiscard]] std::vector<std::string> recognized_only_formats() const;

private:
    struct Registration {
        std::string name;
        Probe probe;
        Creator creator;
        bool supported{};
    };
    DriverFactory();
    std::vector<Registration> registrations_;
};

} // namespace uife
