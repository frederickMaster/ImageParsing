#pragma once

#include "uife/drivers/image/IImageDriver.h"

#include <cstdint>
#include <string>
#include <vector>

namespace uife {

struct Partition {
    std::uint64_t byte_offset{};
    std::uint64_t byte_length{};
    std::string scheme;
    std::string type;
};

[[nodiscard]] std::vector<Partition> scan_partitions(const IImageDriver& image);

} // namespace uife
