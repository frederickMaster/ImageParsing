#include "uife/DriverFactory.h"

#include "uife/drivers/image/CueBinImageDriver.h"
#include "uife/drivers/image/DmgImageDriver.h"
#include "uife/drivers/image/EwfImageDriver.h"
#include "uife/drivers/image/GhoImageDriver.h"
#include "uife/drivers/image/Qcow2ImageDriver.h"
#include "uife/drivers/image/QedImageDriver.h"
#include "uife/drivers/image/RawImageDriver.h"
#include "uife/drivers/image/VdiImageDriver.h"
#include "uife/drivers/image/VhdxImageDriver.h"
#include "uife/drivers/image/VmdkSparseImageDriver.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>

namespace uife {
namespace {

bool matches(std::span<const std::byte> bytes, std::size_t offset, std::string_view signature) {
    if (offset > bytes.size() || signature.size() > bytes.size() - offset) {
        return false;
    }
    return std::equal(signature.begin(), signature.end(), bytes.begin() +
        static_cast<std::ptrdiff_t>(offset),
        [](char left, std::byte right) {
            return static_cast<unsigned char>(left) == std::to_integer<unsigned char>(right);
        });
}

bool contains_text(std::span<const std::byte> bytes, std::string_view text) {
    if (text.size() > bytes.size()) {
        return false;
    }
    return std::search(bytes.begin(), bytes.end(), text.begin(), text.end(),
        [](std::byte left, char right) {
            return std::to_integer<unsigned char>(left) ==
                   static_cast<unsigned char>(right);
        }) != bytes.end();
}

bool has_vdi_signature(std::span<const std::byte> bytes) {
    constexpr std::size_t signature_offset = 64;
    constexpr std::array<std::byte, 4> signature{
        std::byte{0x7f}, std::byte{0x10}, std::byte{0xda}, std::byte{0xbe}};
    return bytes.size() >= signature_offset + signature.size() &&
        std::equal(signature.begin(), signature.end(),
                   bytes.begin() + static_cast<std::ptrdiff_t>(signature_offset));
}

std::uint32_t read_le32(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned int index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(
            std::to_integer<unsigned char>(bytes[offset + index])) << (index * 8U);
    }
    return value;
}

std::uint32_t read_be32(std::span<const std::byte> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(bytes[offset])) << 24U) |
           (static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(bytes[offset + 1])) << 16U) |
           (static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(bytes[offset + 2])) << 8U) |
           static_cast<std::uint32_t>(
               std::to_integer<unsigned char>(bytes[offset + 3]));
}

std::uint64_t read_be64(std::span<const std::byte> bytes, std::size_t offset) {
    return (static_cast<std::uint64_t>(read_be32(bytes, offset)) << 32U) |
           read_be32(bytes, offset + 4);
}

std::uint64_t read_le64(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (unsigned int index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(
            std::to_integer<unsigned char>(bytes[offset + index])) << (index * 8U);
    }
    return value;
}

std::vector<std::byte> read_header(const std::filesystem::path& path) {
    constexpr std::size_t header_size = 64U * 1024U;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("无法以只读方式打开镜像");
    }
    std::vector<std::byte> bytes(header_size);
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    return bytes;
}

bool has_extension(const std::filesystem::path& path, std::string_view extension) {
    auto actual = path.extension().string();
    std::transform(actual.begin(), actual.end(), actual.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return actual == extension;
}

} // namespace

DriverFactory& DriverFactory::instance() {
    static DriverFactory factory;
    return factory;
}

DriverFactory::DriverFactory() {
    const auto is_qcow2 = [](std::span<const std::byte> bytes) {
        return matches(bytes, 0, "QFI\xfb");
    };
    const auto is_supported_qcow2 = [](std::span<const std::byte> bytes) {
        if (bytes.size() < 72 || !matches(bytes, 0, "QFI\xfb")) {
            return false;
        }
        const auto version = read_be32(bytes, 4);
        if ((version != 2 && version != 3) ||
            read_be64(bytes, 8) != 0 ||
            read_be32(bytes, 16) != 0 ||
            read_be32(bytes, 32) != 0 ||
            read_be32(bytes, 60) != 0 ||
            read_be64(bytes, 64) != 0) {
            return false;
        }
        if (version == 3) {
            return bytes.size() >= 104 &&
                   read_be64(bytes, 72) == 0 &&
                   read_be64(bytes, 80) == 0 &&
                   read_be64(bytes, 88) == 0 &&
                   read_be32(bytes, 96) == 4 &&
                   read_be32(bytes, 100) >= 104;
        }
        return true;
    };
    register_driver("QCOW2", is_supported_qcow2,
                    [] { return std::make_unique<Qcow2ImageDriver>(); }, true);
    register_driver("QCOW2 (unsupported variant)", is_qcow2,
                    [] { return std::unique_ptr<IImageDriver>{}; }, false);
    register_driver("VMDK", [](auto bytes) {
        return matches(bytes, 0, "KDMV") ||
               (contains_text(bytes, "# Disk DescriptorFile") &&
                (contains_text(bytes, "\"createType\" = \"monolithicFlat\"") ||
                 contains_text(bytes,
                               "\"createType\" = \"twoGbMaxExtentFlat\"")));
    }, [] { return std::make_unique<VmdkSparseImageDriver>(); }, true);
    register_driver("VMDK (legacy COW)", [](auto bytes) {
        return matches(bytes, 0, "COWD");
    }, [] { return std::unique_ptr<IImageDriver>{}; }, false);
    register_driver("VMDK (descriptor/flat)", [](auto) { return false; },
                    [] { return std::unique_ptr<IImageDriver>{}; }, false);
    register_driver("VDI", [](auto bytes) {
        if (!has_vdi_signature(bytes) || bytes.size() < 80) {
            return false;
        }
        const auto version = static_cast<std::uint32_t>(
            std::to_integer<unsigned char>(bytes[68])) |
            (static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(bytes[69])) << 8U) |
            (static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(bytes[70])) << 16U) |
            (static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(bytes[71])) << 24U);
        const auto type = static_cast<std::uint32_t>(
            std::to_integer<unsigned char>(bytes[76])) |
            (static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(bytes[77])) << 8U) |
            (static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(bytes[78])) << 16U) |
            (static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(bytes[79])) << 24U);
        return version == 0x00010001U && (type == 1U || type == 2U);
    }, [] { return std::make_unique<VdiImageDriver>(); }, true);
    register_driver("VDI (unsupported variant)",
                    [](auto bytes) { return has_vdi_signature(bytes); },
                    [] { return std::unique_ptr<IImageDriver>{}; }, false);
    const auto is_supported_qed = [](std::span<const std::byte> bytes) {
        constexpr std::array<std::byte, 4> signature{
            std::byte{'Q'}, std::byte{'E'}, std::byte{'D'}, std::byte{0}};
        return bytes.size() >= 64 &&
               std::equal(signature.begin(), signature.end(), bytes.begin()) &&
               read_le64(bytes, 16) == 0 &&
               read_le64(bytes, 24) == 0 &&
               read_le64(bytes, 32) == 0 &&
               read_le32(bytes, 56) == 0 &&
               read_le32(bytes, 60) == 0;
    };
    const auto is_qed = [](std::span<const std::byte> bytes) {
        constexpr std::array<std::byte, 4> signature{
            std::byte{'Q'}, std::byte{'E'}, std::byte{'D'}, std::byte{0}};
        return bytes.size() >= signature.size() &&
               std::equal(signature.begin(), signature.end(), bytes.begin());
    };
    register_driver("QED", is_supported_qed,
                    [] { return std::make_unique<QedImageDriver>(); }, true);
    register_driver("QED (unsupported variant)", is_qed,
                    [] { return std::unique_ptr<IImageDriver>{}; }, false);
    register_driver("VHDX", [](auto bytes) { return matches(bytes, 0, "vhdxfile"); },
                    [] { return std::make_unique<VhdxImageDriver>(); }, true);
    register_driver("EWF/E01", [](auto bytes) {
        constexpr std::array<std::byte, 8> signature{
            std::byte{'E'}, std::byte{'V'}, std::byte{'F'}, std::byte{0x09},
            std::byte{0x0d}, std::byte{0x0a}, std::byte{0xff}, std::byte{0x00}
        };
        return bytes.size() >= signature.size() &&
               std::equal(signature.begin(), signature.end(), bytes.begin());
    },
#ifdef UIFE_WITH_LIBEWF
                    [] { return std::make_unique<EwfImageDriver>(); }, true);
#else
                    [] { return std::unique_ptr<IImageDriver>{}; }, false);
#endif
    register_driver("WIM/ESD", [](auto bytes) { return matches(bytes, 0, "MSWIM"); },
#ifdef UIFE_WITH_LIBWIM
                    [] { return std::make_unique<RawImageDriver>(); }, true);
#else
                    [] { return std::unique_ptr<IImageDriver>{}; }, false);
#endif
    register_driver("ISO9660", [](auto bytes) {
        return matches(bytes, 16U * 2048U, "\x01" "CD001\x01");
    }, [] { return std::make_unique<RawImageDriver>(); }, true);
    register_driver("Apple DMG", [](auto bytes) {
        return matches(bytes, 0, "koly");
    }, [] { return std::make_unique<DmgImageDriver>(); }, true);
    const auto is_gho = [](std::span<const std::byte> bytes) {
        return bytes.size() >= 512 &&
               bytes[0] == std::byte{0xfe} &&
               bytes[1] == std::byte{0xef};
    };
    const auto is_supported_gho = [](std::span<const std::byte> bytes) {
        if (bytes.size() < 512 ||
            bytes[0] != std::byte{0xfe} ||
            bytes[1] != std::byte{0xef} ||
            std::to_integer<unsigned char>(bytes[2]) != 1 ||
            (std::to_integer<unsigned char>(bytes[12]) & 0x02U) != 0) {
            return false;
        }
        const auto compression = std::to_integer<unsigned char>(bytes[3]);
        return compression == 0 || compression == 2 ||
               (compression >= 3 && compression <= 9);
    };
    register_driver("Norton Ghost GHO (multi-partition)",
                    is_supported_gho,
                    [] { return std::make_unique<GhoImageDriver>(); }, true);
    register_driver("Norton Ghost GHO (unsupported variant)", is_gho,
                    [] { return std::unique_ptr<IImageDriver>{}; }, false);
    register_driver("BIN+CUE", [](auto) { return false; },
                    [] { return std::make_unique<CueBinImageDriver>(); }, true);
    register_driver("RAW", [](auto) { return true; },
                    [] { return std::make_unique<RawImageDriver>(); }, true);
}

void DriverFactory::register_driver(std::string name, Probe probe,
                                    Creator creator, bool supported) {
    registrations_.push_back({std::move(name), std::move(probe), std::move(creator), supported});
}

ImageDriverInfo DriverFactory::identify(const std::filesystem::path& path) const {
    const auto header = read_header(path);
    for (const auto& registration : registrations_) {
        if (registration.name == "Apple DMG") {
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (!error && size >= 512) {
                std::ifstream input(path, std::ios::binary);
                input.seekg(static_cast<std::streamoff>(size - 512));
                std::array<char, 4> footer{};
                input.read(footer.data(), static_cast<std::streamsize>(footer.size()));
                if (input.gcount() == static_cast<std::streamsize>(footer.size()) &&
                    std::string_view(footer.data(), footer.size()) == "koly") {
                    return {registration.name, registration.supported};
                }
            }
        }
        if (registration.name == "BIN+CUE" && has_extension(path, ".cue")) {
            return {registration.name, registration.supported};
        }
        if (registration.name == "VMDK (descriptor/flat)" &&
            has_extension(path, ".vmdk")) {
            return {registration.name, registration.supported};
        }
        if (registration.probe(header)) {
            return {registration.name, registration.supported};
        }
    }
    return {"未知", false};
}

std::vector<std::string> DriverFactory::supported_formats() const {
    std::vector<std::string> formats;
    for (const auto& registration : registrations_) {
        if (registration.supported &&
            std::find(formats.begin(), formats.end(), registration.name) ==
                formats.end()) {
            formats.push_back(registration.name);
        }
    }
    return formats;
}

std::vector<std::string> DriverFactory::recognized_only_formats() const {
    std::vector<std::string> formats;
    for (const auto& registration : registrations_) {
        if (!registration.supported &&
            registration.name.find(" (") == std::string::npos &&
            std::find(formats.begin(), formats.end(), registration.name) ==
                formats.end()) {
            formats.push_back(registration.name);
        }
    }
    return formats;
}

std::unique_ptr<IImageDriver> DriverFactory::create(
    const std::filesystem::path& path) const {
    const auto info = identify(path);
    const auto found = std::find_if(registrations_.begin(), registrations_.end(),
        [&info](const Registration& registration) { return registration.name == info.format; });
    if (found == registrations_.end()) {
        throw std::runtime_error("无法识别镜像格式");
    }
    if (!found->supported) {
        throw std::runtime_error("已识别 " + found->name +
                                 "，但该镜像驱动尚未实现");
    }
    auto driver = found->creator();
    driver->open(path);
    return driver;
}

} // namespace uife
