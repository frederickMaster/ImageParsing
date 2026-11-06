#include "uife/DriverFactory.h"
#include "uife/Extractor.h"
#include "uife/FsParserFactory.h"
#include "uife/PartitionScanner.h"
#include "uife/drivers/fs/NtfsSecurity.h"
#include "uife/drivers/fs/TskCliFsParser.h"
#include "uife/drivers/fs/TskFsParser.h"
#include "uife/drivers/image/RawImageDriver.h"
#include "uife/utils/Sha256.h"
#include "uife/vfs/VFS.h"

#include "test_support.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <winioctl.h>
#endif

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write_le32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned int index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

void write_le16(std::vector<std::byte>& bytes, std::size_t offset,
                std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void write_le64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned int index = 0; index < 8; ++index) {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

void write_be32(std::span<std::byte> bytes, std::size_t offset,
                std::uint32_t value) {
    for (unsigned int index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> ((3U - index) * 8U)) & 0xffU);
    }
}

void write_be16(std::span<std::byte> bytes, std::size_t offset,
                std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[offset + 1] = static_cast<std::byte>(value & 0xffU);
}

void write_be64(std::span<std::byte> bytes, std::size_t offset,
                std::uint64_t value) {
    write_be32(bytes, offset, static_cast<std::uint32_t>(value >> 32U));
    write_be32(bytes, offset + 4, static_cast<std::uint32_t>(value));
}

void write_be32(std::vector<std::byte>& bytes, std::size_t offset,
                std::uint32_t value) {
    for (unsigned int index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> ((3U - index) * 8U)) & 0xffU);
    }
}

void write_be64(std::vector<std::byte>& bytes, std::size_t offset,
                std::uint64_t value) {
    write_be32(bytes, offset, static_cast<std::uint32_t>(value >> 32U));
    write_be32(bytes, offset + 4, static_cast<std::uint32_t>(value));
}

std::uint32_t crc32c(std::span<const std::byte> bytes, std::size_t zero_offset) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto value = index >= zero_offset &&
                                   index < zero_offset + sizeof(std::uint32_t)
                               ? 0U
                               : std::to_integer<unsigned char>(bytes[index]);
        crc ^= value;
        for (unsigned int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(
                -static_cast<std::int32_t>(crc & 1U));
            crc = (crc >> 1U) ^ (0x82f63b78U & mask);
        }
    }
    return ~crc;
}

std::vector<std::byte> make_vhdx_fixture(bool fixed = false,
                                         bool has_parent = false) {
    constexpr std::uint64_t mib = 1024U * 1024U;
    constexpr std::uint64_t virtual_size = 2 * mib;
    constexpr std::uint64_t block_size = mib;
    constexpr std::uint64_t bat_offset = 2 * mib;
    constexpr std::uint64_t metadata_offset = 3 * mib;
    constexpr std::uint64_t first_data_offset = 4 * mib;
    constexpr std::uint64_t second_data_offset = 5 * mib;
    std::vector<std::byte> image(6 * mib);
    const auto copy_text = [&image](std::size_t offset, std::string_view text) {
        std::transform(text.begin(), text.end(),
                       image.begin() + static_cast<std::ptrdiff_t>(offset),
                       [](char value) { return static_cast<std::byte>(value); });
    };
    copy_text(0, "vhdxfile");

    const auto write_header = [&](std::size_t offset, std::uint64_t sequence) {
        copy_text(offset, "head");
        write_le64(image, offset + 8, sequence);
        write_le16(image, offset + 66, 1);
        write_le32(image, offset + 68, static_cast<std::uint32_t>(mib));
        write_le64(image, offset + 72, mib);
        const auto checksum = crc32c(
            std::span<const std::byte>(image).subspan(offset, 4096), 4);
        write_le32(image, offset + 4, checksum);
    };
    write_header(64U * 1024U, 1);
    write_header(128U * 1024U, 2);

    constexpr std::array<std::byte, 16> bat_guid{
        std::byte{0x66}, std::byte{0x77}, std::byte{0xc2}, std::byte{0x2d},
        std::byte{0x23}, std::byte{0xf6}, std::byte{0x00}, std::byte{0x42},
        std::byte{0x9d}, std::byte{0x64}, std::byte{0x11}, std::byte{0x5e},
        std::byte{0x9b}, std::byte{0xfd}, std::byte{0x4a}, std::byte{0x08}};
    constexpr std::array<std::byte, 16> metadata_guid{
        std::byte{0x06}, std::byte{0xa2}, std::byte{0x7c}, std::byte{0x8b},
        std::byte{0x90}, std::byte{0x47}, std::byte{0x9a}, std::byte{0x4b},
        std::byte{0xb8}, std::byte{0xfe}, std::byte{0x57}, std::byte{0x5f},
        std::byte{0x05}, std::byte{0x0f}, std::byte{0x88}, std::byte{0x6e}};
    const auto write_region_table = [&](std::size_t offset) {
        copy_text(offset, "regi");
        write_le32(image, offset + 8, 2);
        std::copy(bat_guid.begin(), bat_guid.end(),
                  image.begin() + static_cast<std::ptrdiff_t>(offset + 16));
        write_le64(image, offset + 32, bat_offset);
        write_le32(image, offset + 40, static_cast<std::uint32_t>(mib));
        write_le32(image, offset + 44, 1);
        std::copy(metadata_guid.begin(), metadata_guid.end(),
                  image.begin() + static_cast<std::ptrdiff_t>(offset + 48));
        write_le64(image, offset + 64, metadata_offset);
        write_le32(image, offset + 72, static_cast<std::uint32_t>(mib));
        write_le32(image, offset + 76, 1);
        write_le32(image, offset + 4, crc32c(
            std::span<const std::byte>(image).subspan(offset, 64U * 1024U), 4));
    };
    write_region_table(192U * 1024U);
    write_region_table(256U * 1024U);

    const auto metadata_base = static_cast<std::size_t>(metadata_offset);
    copy_text(metadata_base, "metadata");
    write_le16(image, metadata_base + 10, 3);
    constexpr std::array<std::byte, 16> parameters_guid{
        std::byte{0x37}, std::byte{0x67}, std::byte{0xa1}, std::byte{0xca},
        std::byte{0x36}, std::byte{0xfa}, std::byte{0x43}, std::byte{0x4d},
        std::byte{0xb3}, std::byte{0xb6}, std::byte{0x33}, std::byte{0xf0},
        std::byte{0xaa}, std::byte{0x44}, std::byte{0xe7}, std::byte{0x6b}};
    constexpr std::array<std::byte, 16> size_guid{
        std::byte{0x24}, std::byte{0x42}, std::byte{0xa5}, std::byte{0x2f},
        std::byte{0x1b}, std::byte{0xcd}, std::byte{0x76}, std::byte{0x48},
        std::byte{0xb2}, std::byte{0x11}, std::byte{0x5d}, std::byte{0xbe},
        std::byte{0xd8}, std::byte{0x3b}, std::byte{0xf4}, std::byte{0xb8}};
    constexpr std::array<std::byte, 16> sector_guid{
        std::byte{0x1d}, std::byte{0xbf}, std::byte{0x41}, std::byte{0x81},
        std::byte{0x96}, std::byte{0xa9}, std::byte{0x09}, std::byte{0x47},
        std::byte{0xba}, std::byte{0x47}, std::byte{0xf2}, std::byte{0x33},
        std::byte{0xa8}, std::byte{0xfa}, std::byte{0xab}, std::byte{0x5f}};
    const auto write_metadata_entry = [&](std::size_t index, const auto& guid,
                                          std::uint32_t item_offset,
                                          std::uint32_t item_length) {
        const auto offset = metadata_base + 32 + index * 32;
        std::copy(guid.begin(), guid.end(),
                  image.begin() + static_cast<std::ptrdiff_t>(offset));
        write_le32(image, offset + 16, item_offset);
        write_le32(image, offset + 20, item_length);
        write_le32(image, offset + 24, 4);
    };
    write_metadata_entry(0, parameters_guid, 64U * 1024U, 8);
    write_metadata_entry(1, size_guid, 64U * 1024U + 8, 8);
    write_metadata_entry(2, sector_guid, 64U * 1024U + 16, 4);
    write_le32(image, metadata_base + 64U * 1024U,
               static_cast<std::uint32_t>(block_size));
    write_le32(image, metadata_base + 64U * 1024U + 4,
               (fixed ? 1U : 0U) | (has_parent ? 2U : 0U));
    write_le64(image, metadata_base + 64U * 1024U + 8, virtual_size);
    write_le32(image, metadata_base + 64U * 1024U + 16, 512);

    write_le64(image, static_cast<std::size_t>(bat_offset),
               first_data_offset | 6U);
    if (fixed) {
        write_le64(image, static_cast<std::size_t>(bat_offset) + 8,
                   second_data_offset | 6U);
    }
    std::fill(image.begin() + static_cast<std::ptrdiff_t>(first_data_offset),
              image.begin() + static_cast<std::ptrdiff_t>(
                  first_data_offset + block_size), std::byte{0x41});
    std::fill(image.begin() + static_cast<std::ptrdiff_t>(second_data_offset),
              image.end(), std::byte{0x72});
    return image;
}

std::uint32_t crc32(std::span<const std::byte> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes) {
        crc ^= std::to_integer<unsigned char>(byte);
        for (unsigned int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(
                -static_cast<std::int32_t>(crc & 1U));
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

class FakeFsParser final : public uife::IFSParser {
public:
    explicit FakeFsParser(bool include_unsafe_name = false,
                          bool include_invalid_size = false,
                          bool include_streams = false,
                          bool include_security = false)
        : include_unsafe_name_(include_unsafe_name),
          include_invalid_size_(include_invalid_size),
          include_streams_(include_streams),
          include_security_(include_security) {}

    void mount(std::shared_ptr<uife::IImageDriver>, std::uint64_t, std::uint64_t) override {}

    std::vector<uife::VFSNode> list_dir(const std::string& path) const override {
        if (path == "/") {
            std::vector<uife::VFSNode> entries{
                {"folder", "/folder", uife::NodeType::directory, {}},
                {"hello.txt", "/hello.txt", uife::NodeType::regular_file, file_metadata()}
            };
            if (include_unsafe_name_) {
                entries.push_back({"..", "/..", uife::NodeType::regular_file,
                                   file_metadata()});
            }
            if (include_invalid_size_) {
                entries.push_back({"corrupt.bin", "/corrupt.bin",
                                   uife::NodeType::regular_file,
                                   invalid_size_metadata()});
            }
            return entries;
        }
        if (path == "/folder") {
            return {{"nested.bin", "/folder/nested.bin",
                     uife::NodeType::regular_file, nested_metadata()}};
        }
        throw std::runtime_error("unexpected test directory");
    }

    uife::VFSNode get_metadata(const std::string& path) const override {
        if (path == "/") {
            return {"/", "/", uife::NodeType::directory, {}};
        }
        if (path == "/folder") {
            return {"folder", path, uife::NodeType::directory, {}};
        }
        if (path == "/folder/nested.bin") {
            return {"nested.bin", path, uife::NodeType::regular_file, nested_metadata()};
        }
        if (path == "/hello.txt") {
            return {"hello.txt", path, uife::NodeType::regular_file, file_metadata()};
        }
        throw std::runtime_error("test path not found");
    }

    std::size_t read_file(const std::string& path, std::uint64_t offset,
                          std::span<std::byte> destination) const override {
        const auto content = path == "/hello.txt" ? std::string("abc")
                            : path == "/folder/nested.bin" ? std::string("nested-data")
                            : std::string{};
        if (offset >= content.size()) {
            return 0;
        }
        const auto count = std::min(destination.size(),
            content.size() - static_cast<std::size_t>(offset));
        std::transform(content.begin() + static_cast<std::ptrdiff_t>(offset),
                       content.begin() + static_cast<std::ptrdiff_t>(offset + count),
                       destination.begin(),
                       [](char character) { return static_cast<std::byte>(character); });
        return count;
    }

    std::string_view filesystem_name() const noexcept override { return "test"; }

    std::size_t read_stream(const std::string& path, const std::string& stream,
                            std::uint64_t offset,
                            std::span<std::byte> destination) const override {
        if (path != "/hello.txt") {
            throw std::runtime_error("unexpected alternate stream path");
        }
        const auto content = stream == "Zone.Identifier" ? std::string("stream")
                            : stream == "evil name.txt" ? std::string("x")
                            : std::string{};
        if (content.empty()) {
            throw std::runtime_error("unknown test stream");
        }
        if (offset >= content.size()) {
            return 0;
        }
        const auto count = std::min(destination.size(),
            content.size() - static_cast<std::size_t>(offset));
        std::transform(content.begin() + static_cast<std::ptrdiff_t>(offset),
                       content.begin() + static_cast<std::ptrdiff_t>(offset + count),
                       destination.begin(),
                       [](char character) { return static_cast<std::byte>(character); });
        return count;
    }

private:
    bool include_unsafe_name_{};
    bool include_invalid_size_{};
    bool include_streams_{};
    bool include_security_{};

    uife::VFSMetadata file_metadata() const {
        uife::VFSMetadata value;
        value.size = 3;
        value.mode = 0644;
        if (include_streams_) {
            value.alternate_streams = {
                {"Zone.Identifier", 6, true},
                {"evil name.txt", 1, true},
            };
        }
        if (include_security_) {
            value.has_security = true;
            value.security.valid = true;
            value.security.owner_sid = "S-1-5-32-544";
            value.security.group_sid = "S-1-5-32-545";
            value.security.dacl_present = true;
            value.security.dacl.push_back(
                {"allow", 0, false, 0x001F01FFU, "S-1-1-0"});
        }
        return value;
    }

    static uife::VFSMetadata nested_metadata() {
        uife::VFSMetadata value;
        value.size = 11;
        value.mode = 0644;
        return value;
    }

    static uife::VFSMetadata invalid_size_metadata() {
        uife::VFSMetadata value;
        value.size = std::numeric_limits<std::uint64_t>::max();
        value.size_valid = false;
        return value;
    }
};

// 只暴露一个重解析点节点的解析器，用于验证“junction 必须重建成 junction”。
class ReparsePointFsParser final : public uife::IFSParser {
public:
    ReparsePointFsParser(std::string target, std::uint32_t tag)
        : target_(std::move(target)), tag_(tag) {}

    void mount(std::shared_ptr<uife::IImageDriver>, std::uint64_t,
               std::uint64_t) override {}

    std::vector<uife::VFSNode> list_dir(const std::string& path) const override {
        if (path != "/") {
            throw std::runtime_error("unexpected test directory");
        }
        return {link_node()};
    }

    uife::VFSNode get_metadata(const std::string& path) const override {
        if (path != "/linkdir") {
            throw std::runtime_error("test path not found");
        }
        return link_node();
    }

    std::size_t read_file(const std::string&, std::uint64_t,
                          std::span<std::byte>) const override {
        throw std::runtime_error("reparse point has no default data stream");
    }

    std::string_view filesystem_name() const noexcept override { return "test"; }

private:
    uife::VFSNode link_node() const {
        uife::VFSNode node;
        node.name = "linkdir";
        node.path = "/linkdir";
        node.type = uife::NodeType::symbolic_link;
        node.metadata.reparse_point = true;
        node.metadata.has_reparse_tag = true;
        node.metadata.reparse_tag = tag_;
        node.metadata.symlink_target = target_;
        return node;
    }

    std::string target_;
    std::uint32_t tag_;
};

void test_sha256() {
    constexpr std::string_view source = "abc";
    uife::Sha256 hash;
    hash.update(std::as_bytes(std::span(source.data(), source.size())));
    require(uife::Sha256::to_hex(hash.finish()) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "SHA-256 known-answer test failed");
    constexpr std::string_view multi_block =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    uife::Sha256 multi_hash;
    multi_hash.update(std::as_bytes(std::span(multi_block.data(), 17)));
    multi_hash.update(std::as_bytes(std::span(multi_block.data() + 17,
                                               multi_block.size() - 17)));
    require(uife::Sha256::to_hex(multi_hash.finish()) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        "SHA-256 multi-block test failed");
}

void test_raw_and_partitions(const std::filesystem::path& image_path) {
    constexpr std::size_t image_size = 1024U * 1024U;
    std::vector<std::byte> image(image_size);
    image[510] = std::byte{0x55};
    image[511] = std::byte{0xaa};
    const std::size_t partition_entry = 446;
    image[partition_entry + 4] = std::byte{0x07};
    write_le32(image, partition_entry + 8, 1);
    write_le32(image, partition_entry + 12, 100);
    {
        std::ofstream output(image_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(image.data()),
                     static_cast<std::streamsize>(image.size()));
        require(static_cast<bool>(output), "failed to create RAW test image");
    }

    const auto info = uife::DriverFactory::instance().identify(image_path);
    require(info.format == "RAW" && info.supported, "RAW driver factory detection failed");
    auto driver = uife::DriverFactory::instance().create(image_path);
    std::array<std::byte, 512> sector{};
    require(driver->read_sector(0, sector) == sector.size(), "RAW sector read failed");
    const auto partitions = uife::scan_partitions(*driver);
    require(partitions.size() == 1 && partitions[0].byte_offset == 512 &&
                partitions[0].byte_length == 51200,
            "MBR partition scanner returned incorrect bounds");

    std::array<std::byte, 512> ntfs_boot{};
    std::copy_n(reinterpret_cast<const std::byte*>("NTFS    "), 8, ntfs_boot.begin() + 3);
    ntfs_boot[11] = std::byte{0};
    ntfs_boot[12] = std::byte{2};
    {
        std::fstream output(image_path, std::ios::binary | std::ios::in | std::ios::out);
        output.seekp(512);
        output.write(reinterpret_cast<const char*>(ntfs_boot.data()),
                     static_cast<std::streamsize>(ntfs_boot.size()));
    }
    auto raw = uife::DriverFactory::instance().create(image_path);
    require(uife::FsParserFactory::instance().identify(*raw, 512).starts_with("NTFS"),
            "NTFS boot sector probe failed");

    std::array<std::byte, 512> fat_boot{};
    fat_boot[11] = std::byte{0x00};
    fat_boot[12] = std::byte{0x02};
    fat_boot[13] = std::byte{0x01};
    fat_boot[14] = std::byte{0x01};
    fat_boot[16] = std::byte{0x02};
    fat_boot[17] = std::byte{0x00};
    fat_boot[18] = std::byte{0x02};
    fat_boot[19] = std::byte{0x64};
    fat_boot[21] = std::byte{0xf8};
    fat_boot[22] = std::byte{0x01};
    fat_boot[510] = std::byte{0x55};
    fat_boot[511] = std::byte{0xaa};
    {
        std::fstream output(image_path, std::ios::binary | std::ios::in | std::ios::out);
        output.seekp(512);
        output.write(reinterpret_cast<const char*>(fat_boot.data()),
                     static_cast<std::streamsize>(fat_boot.size()));
    }
    auto fat_image = uife::DriverFactory::instance().create(image_path);
    require(uife::FsParserFactory::instance().identify(*fat_image, 512).starts_with("FAT"),
            "FAT boot sector probe failed");
#ifdef UIFE_WITH_LIBTSK
    require(uife::FsParserFactory::instance().supports(*fat_image, 512),
            "FAT parser was not registered with libtsk enabled");
#elif defined(_WIN32)
    if (uife::TskCliFsParser::available()) {
        require(uife::FsParserFactory::instance().supports(*fat_image, 512),
                "FAT parser was not registered with Sleuth Kit CLI available");
    }
#endif
    {
        std::fstream output(image_path,
                            std::ios::binary | std::ios::in | std::ios::out);
        output.seekp(2048);
        output.write(reinterpret_cast<const char*>(fat_boot.data()),
                     static_cast<std::streamsize>(fat_boot.size()));
        require(static_cast<bool>(output),
                "failed to write nested FAT filesystem fixture");
    }
    auto cue_path = image_path;
    cue_path.replace_extension(".cue");
    {
        std::ofstream output(cue_path, std::ios::binary);
        output << "FILE \"" << image_path.filename().string() << "\" BINARY\n"
                  "  TRACK 01 MODE1/2048\n"
                  "    INDEX 01 00:00:00\n";
        require(static_cast<bool>(output),
                "failed to write nested filesystem CUE fixture");
    }
    auto cue_image = uife::DriverFactory::instance().create(cue_path);
    const auto nested_fat_name =
        uife::FsParserFactory::instance().identify(*cue_image, 2048);
    require(nested_fat_name.starts_with("FAT"),
            "FAT filesystem was not recognized through BIN+CUE");
#ifdef UIFE_WITH_LIBTSK
    require(uife::FsParserFactory::instance().supports(*cue_image, 2048),
            "libtsk backend did not register FAT through BIN+CUE");
#else
    require(!uife::FsParserFactory::instance().supports(*cue_image, 2048),
            "Sleuth Kit CLI incorrectly claimed it could read a virtual image");
    require(nested_fat_name.find("仅支持 RAW") != std::string::npos,
            "CLI fallback did not explain its RAW-only limitation");
#endif

    std::array<std::byte, 512> exfat_boot{};
    std::copy_n(reinterpret_cast<const std::byte*>("EXFAT   "), 8,
                exfat_boot.begin() + 3);
    exfat_boot[108] = std::byte{9};
    exfat_boot[109] = std::byte{0};
    exfat_boot[510] = std::byte{0x55};
    exfat_boot[511] = std::byte{0xaa};
    {
        std::fstream output(image_path, std::ios::binary | std::ios::in | std::ios::out);
        output.seekp(512);
        output.write(reinterpret_cast<const char*>(exfat_boot.data()),
                     static_cast<std::streamsize>(exfat_boot.size()));
    }
    auto exfat_image = uife::DriverFactory::instance().create(image_path);
    require(uife::FsParserFactory::instance().identify(*exfat_image, 512)
                .starts_with("exFAT"),
            "exFAT boot sector probe failed");
#ifdef UIFE_WITH_LIBTSK
    require(uife::FsParserFactory::instance().supports(*exfat_image, 512),
            "exFAT parser was not registered with libtsk enabled");
#elif defined(_WIN32)
    if (uife::TskCliFsParser::available()) {
        require(uife::FsParserFactory::instance().supports(*exfat_image, 512),
                "exFAT parser was not registered with Sleuth Kit CLI available");
    }
#endif

    std::array<std::byte, 512> hfs_volume_header{};
    hfs_volume_header[0] = std::byte{0x48};
    hfs_volume_header[1] = std::byte{0x2b};
    hfs_volume_header[2] = std::byte{0x00};
    hfs_volume_header[3] = std::byte{0x04};
    hfs_volume_header[42] = std::byte{0x02};
    hfs_volume_header[47] = std::byte{0x08};
    hfs_volume_header[51] = std::byte{0x04};
    {
        std::fstream output(image_path, std::ios::binary | std::ios::in | std::ios::out);
        output.seekp(1024);
        output.write(reinterpret_cast<const char*>(hfs_volume_header.data()),
                     static_cast<std::streamsize>(hfs_volume_header.size()));
    }
    auto hfs_image = uife::DriverFactory::instance().create(image_path);
    require(uife::FsParserFactory::instance().identify(*hfs_image, 0).starts_with("HFS+"),
            "HFS+ volume header probe failed");
#ifdef UIFE_WITH_LIBTSK
    require(uife::FsParserFactory::instance().supports(*hfs_image, 0),
            "HFS+ parser was not registered with libtsk enabled");
#elif defined(_WIN32)
    if (uife::TskCliFsParser::available()) {
        require(uife::FsParserFactory::instance().supports(*hfs_image, 0),
                "HFS+ parser was not registered with Sleuth Kit CLI available");
    }
#endif
    hfs_volume_header[42] = std::byte{0x00};
    {
        std::fstream output(image_path, std::ios::binary | std::ios::in | std::ios::out);
        output.seekp(1024);
        output.write(reinterpret_cast<const char*>(hfs_volume_header.data()),
                     static_cast<std::streamsize>(hfs_volume_header.size()));
    }
    auto invalid_hfs_image = uife::DriverFactory::instance().create(image_path);
    require(uife::FsParserFactory::instance().identify(*invalid_hfs_image, 0) == "未知",
            "invalid HFS+ volume block size was incorrectly accepted");

    std::array<std::byte, 1024> ext_superblock{};
    ext_superblock[24] = std::byte{0};
    ext_superblock[56] = std::byte{0x53};
    ext_superblock[57] = std::byte{0xef};
    ext_superblock[96] = std::byte{0x40};
    {
        std::fstream output(image_path, std::ios::binary | std::ios::in | std::ios::out);
        output.seekp(1024);
        output.write(reinterpret_cast<const char*>(ext_superblock.data()),
                     static_cast<std::streamsize>(ext_superblock.size()));
    }
    auto ext_image = uife::DriverFactory::instance().create(image_path);
    const auto ext_identification =
        uife::FsParserFactory::instance().identify(*ext_image, 0);
    require(ext_identification.starts_with("ext4"),
            "ext4 superblock probe failed");
#ifdef UIFE_WITH_LIBTSK
    require(uife::FsParserFactory::instance().supports(*ext_image, 0),
            "ext4 parser was not registered with libtsk enabled");
    auto ext_parser = uife::FsParserFactory::instance().create(*ext_image, 0);
    std::shared_ptr<uife::IImageDriver> shared_ext_image(std::move(ext_image));
    bool reached_filesystem_validation = false;
    try {
        ext_parser->mount(shared_ext_image, 0, shared_ext_image->get_size());
    } catch (const std::runtime_error& error) {
        reached_filesystem_validation =
            std::string(error.what()).find("libtsk 无法挂载该 ext 文件系统") !=
            std::string::npos;
    }
    require(reached_filesystem_validation,
            "libtsk did not read the image through the read-only image driver");
#else
#ifdef _WIN32
    const bool has_tsk_cli = uife::TskCliFsParser::available();
    if (has_tsk_cli) {
        require(uife::FsParserFactory::instance().supports(*ext_image, 0),
                "ext4 parser was not registered with Sleuth Kit CLI available");
    } else {
        require(!uife::FsParserFactory::instance().supports(*ext_image, 0),
                "ext4 must not be reported as readable without a parser backend");
    }
#else
    require(!uife::FsParserFactory::instance().supports(*ext_image, 0),
            "ext4 must not be reported as readable without a parser backend");
#endif
#ifdef _WIN32
    if (!uife::TskCliFsParser::available()) {
#endif
    bool refused_ext_without_backend = false;
    try {
        (void)uife::FsParserFactory::instance().create(*ext_image, 0);
    } catch (const std::runtime_error&) {
        refused_ext_without_backend = true;
    }
    require(refused_ext_without_backend,
            "ext4 was incorrectly advertised as readable without a parser backend");
#ifdef _WIN32
    }
#endif
#endif

    std::array<std::byte, 512> xfs_superblock{};
    xfs_superblock[0] = std::byte{'X'};
    xfs_superblock[1] = std::byte{'F'};
    xfs_superblock[2] = std::byte{'S'};
    xfs_superblock[3] = std::byte{'B'};
    write_be32(xfs_superblock, 4, 4096);
    write_be64(xfs_superblock, 8, 256);
    write_be16(xfs_superblock, 102, 512);
    write_be16(xfs_superblock, 104, 512);
    {
        std::fstream output(image_path, std::ios::binary | std::ios::in | std::ios::out);
        output.seekp(0);
        output.write(reinterpret_cast<const char*>(xfs_superblock.data()),
                     static_cast<std::streamsize>(xfs_superblock.size()));
        std::array<std::byte, 1024> cleared_superblock{};
        output.seekp(1024);
        output.write(reinterpret_cast<const char*>(cleared_superblock.data()),
                     static_cast<std::streamsize>(cleared_superblock.size()));
    }
    auto xfs_image = uife::DriverFactory::instance().create(image_path);
    const auto xfs_identification =
        uife::FsParserFactory::instance().identify(*xfs_image, 0);
    require(xfs_identification.starts_with("XFS"),
            "XFS superblock probe failed");
#ifdef UIFE_WITH_TSK_XFS
    require(uife::FsParserFactory::instance().supports(*xfs_image, 0),
            "XFS parser was not registered with a compatible libtsk");
#else
    require(!uife::FsParserFactory::instance().supports(*xfs_image, 0),
            "XFS must not be reported readable without a compatible parser");
    bool refused_xfs_without_backend = false;
    try {
        (void)uife::FsParserFactory::instance().create(*xfs_image, 0);
    } catch (const std::runtime_error&) {
        refused_xfs_without_backend = true;
    }
    require(refused_xfs_without_backend,
            "XFS was incorrectly advertised without a compatible parser");
#endif

    std::array<std::byte, 48> apfs_nx_superblock{};
    apfs_nx_superblock[32] = std::byte{'N'};
    apfs_nx_superblock[33] = std::byte{'X'};
    apfs_nx_superblock[34] = std::byte{'S'};
    apfs_nx_superblock[35] = std::byte{'B'};
    apfs_nx_superblock[37] = std::byte{0x10};
    apfs_nx_superblock[41] = std::byte{0x01};
    {
        std::fstream output(image_path, std::ios::binary | std::ios::in | std::ios::out);
        output.seekp(0);
        output.write(reinterpret_cast<const char*>(apfs_nx_superblock.data()),
                     static_cast<std::streamsize>(apfs_nx_superblock.size()));
    }
    auto apfs_image = uife::DriverFactory::instance().create(image_path);
    const auto apfs_identification =
        uife::FsParserFactory::instance().identify(*apfs_image, 0);
    require(apfs_identification.starts_with("APFS"),
            "APFS container header probe failed");
#if defined(UIFE_WITH_LIBFSAPFS) || defined(UIFE_WITH_TSK_APFS)
    require(uife::FsParserFactory::instance().supports(*apfs_image, 0),
            "APFS parser was not registered although an APFS backend is available");
    auto apfs_parser =
        uife::FsParserFactory::instance().create(*apfs_image, 0);
    std::shared_ptr<uife::IImageDriver> shared_apfs_image(
        std::move(apfs_image));
    bool rejected_corrupt_apfs = false;
    try {
        // 这里只有一个伪造的 NXSB 头，校验和与容器结构都不成立，
        // 任何真实后端都必须拒绝挂载，而不能把它当成空卷。
        apfs_parser->mount(shared_apfs_image, 0,
                           shared_apfs_image->get_size());
    } catch (const std::runtime_error&) {
        rejected_corrupt_apfs = true;
    }
    require(rejected_corrupt_apfs,
            "APFS parser accepted a container with an invalid NXSB checksum");
#else
    require(!uife::FsParserFactory::instance().supports(*apfs_image, 0),
            "APFS must not be reported readable without an APFS backend");
    bool refused_apfs_without_backend = false;
    try {
        (void)uife::FsParserFactory::instance().create(*apfs_image, 0);
    } catch (const std::runtime_error&) {
        refused_apfs_without_backend = true;
    }
    require(refused_apfs_without_backend,
            "APFS was incorrectly advertised without a backend");
#endif
}

void test_container_recognition(const std::filesystem::path& image_path) {
    const auto supported_formats =
        uife::DriverFactory::instance().supported_formats();
    for (const std::string_view expected :
         {"RAW", "ISO9660", "VMDK", "VDI", "QED", "QCOW2", "VHDX",
          "BIN+CUE", "Norton Ghost GHO (multi-partition)"}) {
        require(std::find(supported_formats.begin(), supported_formats.end(),
                          expected) != supported_formats.end(),
                "readable image format is missing from the capability catalog");
    }
    const auto recognized_only =
        uife::DriverFactory::instance().recognized_only_formats();
#ifdef UIFE_WITH_LIBEWF
    require(std::find(supported_formats.begin(), supported_formats.end(),
                      "EWF/E01") != supported_formats.end(),
            "EWF reader was not registered when libewf is enabled");
    require(std::find(recognized_only.begin(), recognized_only.end(),
                      "EWF/E01") == recognized_only.end(),
            "EWF was incorrectly reported as detection-only with libewf");
#else
    require(std::find(recognized_only.begin(), recognized_only.end(),
                      "EWF/E01") != recognized_only.end(),
            "EWF must remain detection-only without libewf");
#endif
    const auto wim_position =
        std::find(supported_formats.begin(), supported_formats.end(), "WIM/ESD");
    const auto wim_detection_position =
        std::find(recognized_only.begin(), recognized_only.end(), "WIM/ESD");
#ifdef UIFE_WITH_LIBWIM
    require(wim_position != supported_formats.end() &&
                wim_detection_position == recognized_only.end(),
            "WIM/ESD was not registered with the enabled wimlib backend");
#else
    require(wim_position == supported_formats.end() &&
                wim_detection_position != recognized_only.end(),
            "WIM/ESD must remain detection-only without wimlib");
#endif
    require(std::find(supported_formats.begin(), supported_formats.end(),
                      "Apple DMG") != supported_formats.end() &&
                std::find(recognized_only.begin(), recognized_only.end(),
                          "Apple DMG") == recognized_only.end(),
            "DMG driver was not advertised as a readable image format");
    const auto filesystems =
        uife::FsParserFactory::instance().supported_names();
    require(std::find(filesystems.begin(), filesystems.end(), "ISO9660") !=
                filesystems.end(),
            "ISO9660 parser is missing from the capability catalog");

    {
        std::ofstream output(image_path, std::ios::binary);
        constexpr std::array<unsigned char, 8> signature{
            'E', 'V', 'F', 0x09, 0x0d, 0x0a, 0xff, 0x00};
        for (const auto value : signature) {
            output.put(static_cast<char>(value));
        }
        require(static_cast<bool>(output), "failed to create EWF signature test");
    }
    const auto ewf_info =
        uife::DriverFactory::instance().identify(image_path);
#ifdef UIFE_WITH_LIBEWF
    require(ewf_info.format == "EWF/E01" && ewf_info.supported,
            "EWF signature was not associated with the enabled reader");
#else
    require(ewf_info.format == "EWF/E01" && !ewf_info.supported,
            "EWF should remain detection-only without libewf");
#endif
    bool refused_invalid_ewf = false;
    try {
        (void)uife::DriverFactory::instance().create(image_path);
    } catch (const std::runtime_error&) {
        refused_invalid_ewf = true;
    }
    require(refused_invalid_ewf,
            "EWF reader accepted a signature-only truncated segment");

    {
        std::ofstream output(image_path, std::ios::binary);
        std::array<char, 512> footer{};
        footer[0] = 'k';
        footer[1] = 'o';
        footer[2] = 'l';
        footer[3] = 'y';
        output.write(footer.data(), static_cast<std::streamsize>(footer.size()));
        require(static_cast<bool>(output), "failed to create DMG footer test");
    }
    const auto dmg_info =
        uife::DriverFactory::instance().identify(image_path);
    require(dmg_info.format == "Apple DMG" && dmg_info.supported,
            "DMG footer signature was not recognized as a supported format");
    bool rejected_invalid_dmg = false;
    try {
        (void)uife::DriverFactory::instance().create(image_path);
    } catch (const std::runtime_error& error) {
        rejected_invalid_dmg =
            std::string(error.what()).find("DMG") != std::string::npos;
    }
    require(rejected_invalid_dmg,
            "DMG driver accepted a footer-only invalid image");

    {
        std::ofstream output(image_path, std::ios::binary);
        constexpr std::array<char, 4> signature{'Q', 'F', 'I', static_cast<char>(0xfb)};
        output.write(signature.data(), static_cast<std::streamsize>(signature.size()));
    }
    const auto detected = uife::DriverFactory::instance().identify(image_path);
    require(detected.format == "QCOW2 (unsupported variant)" &&
                !detected.supported,
            "QCOW2 should be recognized but not advertised as a working driver");
    bool refused_decode = false;
    try {
        (void)uife::DriverFactory::instance().create(image_path);
    } catch (const std::runtime_error&) {
        refused_decode = true;
    }
    require(refused_decode, "unsupported QCOW2 image was incorrectly opened as RAW");
}

void test_cue_bin_driver(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    const auto cue_path = directory / "disc.cue";
    const auto bin_path = directory / "disc.bin";
    constexpr std::size_t raw_sector_size = 2352;
    std::vector<std::byte> sectors(3 * raw_sector_size);
    std::fill(sectors.begin() + static_cast<std::ptrdiff_t>(raw_sector_size + 16),
              sectors.begin() + static_cast<std::ptrdiff_t>(2 * raw_sector_size),
              std::byte{0x31});
    std::fill(sectors.begin() + static_cast<std::ptrdiff_t>(2 * raw_sector_size + 16),
              sectors.end(), std::byte{0x32});
    {
        std::ofstream output(bin_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(sectors.data()),
                     static_cast<std::streamsize>(sectors.size()));
        require(static_cast<bool>(output), "failed to create BIN+CUE data fixture");
    }
    const auto write_cue = [&cue_path](std::string_view body) {
        std::ofstream output(cue_path, std::ios::binary);
        output << body;
        require(static_cast<bool>(output), "failed to create BIN+CUE descriptor fixture");
    };
    write_cue("FILE \"disc.bin\" BINARY\n"
              "  TRACK 01 MODE1/2352\n"
              "    INDEX 01 00:00:01\n");

    const auto image_info = uife::DriverFactory::instance().identify(cue_path);
    require(image_info.format == "BIN+CUE" && image_info.supported,
            "UIFE did not register the supported BIN+CUE track layout");
    auto driver = uife::DriverFactory::instance().create(cue_path);
    require(driver->format_name() == "BIN+CUE" &&
                driver->sector_size() == 2048 &&
                driver->get_size() == 2 * 2048,
            "BIN+CUE logical image geometry is incorrect");
    std::array<std::byte, 2048> first_sector{};
    require(driver->read_sector(0, first_sector) == first_sector.size() &&
                std::all_of(first_sector.begin(), first_sector.end(),
                            [](std::byte value) { return value == std::byte{0x31}; }),
            "BIN+CUE MODE1/2352 payload offset or INDEX 01 was not applied");
    std::array<std::byte, 8> cross_sector{};
    require(driver->read_at(2044, cross_sector) == cross_sector.size() &&
                std::all_of(cross_sector.begin(), cross_sector.begin() + 4,
                            [](std::byte value) { return value == std::byte{0x31}; }) &&
                std::all_of(cross_sector.begin() + 4, cross_sector.end(),
                            [](std::byte value) { return value == std::byte{0x32}; }),
            "BIN+CUE random reads across logical sectors failed");
    driver->close();

    std::vector<std::byte> cooked_sectors(2 * 2048, std::byte{0x42});
    {
        std::ofstream output(bin_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(cooked_sectors.data()),
                     static_cast<std::streamsize>(cooked_sectors.size()));
        require(static_cast<bool>(output),
                "failed to create 2048-byte BIN+CUE data fixture");
    }
    write_cue("FILE \"disc.bin\" BINARY\n"
              "TRACK 01 MODE1/2048\n"
              "INDEX 01 00:00:01\n");
    auto cooked_driver = uife::DriverFactory::instance().create(cue_path);
    std::array<std::byte, 2048> cooked_sector{};
    require(cooked_driver->get_size() == 2048 &&
                cooked_driver->read_sector(0, cooked_sector) ==
                    cooked_sector.size() &&
                std::all_of(cooked_sector.begin(), cooked_sector.end(),
                            [](std::byte value) { return value == std::byte{0x42}; }),
            "BIN+CUE MODE1/2048 track was not exposed correctly");
    cooked_driver->close();

    {
        std::ofstream output(bin_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(sectors.data()),
                     static_cast<std::streamsize>(sectors.size()));
        require(static_cast<bool>(output),
                "failed to restore MODE1/2352 multi-track fixture");
    }
    write_cue("FILE \"disc.bin\" BINARY\n"
              "TRACK 01 MODE1/2352\n"
              "INDEX 01 00:00:00\n"
              "TRACK 02 MODE1/2352\n"
              "INDEX 01 00:00:02\n");
    auto multi_track_driver =
        uife::DriverFactory::instance().create(cue_path);
    require(multi_track_driver->get_size() == 3 * 2048,
            "BIN+CUE MODE1 multi-track logical size is incorrect");
    std::array<std::byte, 8> second_track{};
    require(multi_track_driver->read_at(2 * 2048, second_track) ==
                second_track.size() &&
                std::all_of(second_track.begin(), second_track.end(),
                            [](std::byte value) { return value == std::byte{0x32}; }),
            "BIN+CUE reads did not follow the second track INDEX 01");
    multi_track_driver->close();

    const auto second_bin_path = directory / "disc-second.bin";
    std::vector<std::byte> first_file(raw_sector_size, std::byte{0});
    std::vector<std::byte> second_file(raw_sector_size, std::byte{0});
    std::fill(first_file.begin() + 16, first_file.end(),
              std::byte{0x51});
    std::fill(second_file.begin() + 16, second_file.end(),
              std::byte{0x52});
    {
        std::ofstream output(bin_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(first_file.data()),
                     static_cast<std::streamsize>(first_file.size()));
        require(static_cast<bool>(output),
                "failed to create first multi-file BIN+CUE fixture");
    }
    {
        std::ofstream output(second_bin_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(second_file.data()),
                     static_cast<std::streamsize>(second_file.size()));
        require(static_cast<bool>(output),
                "failed to create second multi-file BIN+CUE fixture");
    }
    write_cue("FILE \"disc.bin\" BINARY\n"
              "TRACK 01 MODE1/2352\n"
              "INDEX 01 00:00:00\n"
              "FILE \"disc-second.bin\" BINARY\n"
              "TRACK 02 MODE1/2352\n"
              "INDEX 01 00:00:00\n");
    auto multi_file_driver =
        uife::DriverFactory::instance().create(cue_path);
    std::array<std::byte, 8> across_file_boundary{};
    require(multi_file_driver->get_size() == 2 * 2048 &&
                multi_file_driver->read_at(2044, across_file_boundary) ==
                    across_file_boundary.size() &&
                std::all_of(across_file_boundary.begin(),
                            across_file_boundary.begin() + 4,
                            [](std::byte value) {
                                return value == std::byte{0x51};
                            }) &&
                std::all_of(across_file_boundary.begin() + 4,
                            across_file_boundary.end(),
                            [](std::byte value) {
                                return value == std::byte{0x52};
                            }),
            "BIN+CUE reads did not cross correctly between BINARY files");
    multi_file_driver->close();

    write_cue("FILE \"disc.bin\" BINARY\n"
              "TRACK 01 MODE1/2352\n"
              "INDEX 01 00:00:00\n"
              "TRACK 02 AUDIO\n"
              "INDEX 01 00:00:02\n");
    bool rejected_audio_track = false;
    try {
        (void)uife::DriverFactory::instance().create(cue_path);
    } catch (const std::runtime_error&) {
        rejected_audio_track = true;
    }
    require(rejected_audio_track,
            "BIN+CUE accepted an audio track outside the supported subset");

    write_cue("FILE \"..\\\\outside.bin\" BINARY\n"
              "TRACK 01 MODE1/2048\n"
              "INDEX 01 00:00:00\n");
    bool rejected_path_escape = false;
    try {
        (void)uife::DriverFactory::instance().create(cue_path);
    } catch (const std::runtime_error&) {
        rejected_path_escape = true;
    }
    require(rejected_path_escape,
            "BIN+CUE allowed its descriptor to reference data outside its directory");

    write_cue("FILE \"disc.bin\" BINARY\n"
              "TRACK 01 MODE1/2352\n"
              "INDEX 01 00:00:00\n"
              "FILE \"missing.bin\" BINARY\n"
              "TRACK 02 MODE1/2352\n"
              "INDEX 01 00:00:00\n");
    bool rejected_missing_file = false;
    try {
        (void)uife::DriverFactory::instance().create(cue_path);
    } catch (const std::runtime_error&) {
        rejected_missing_file = true;
    }
    require(rejected_missing_file,
            "BIN+CUE accepted a missing associated BINARY file");

    write_cue("FILE \"disc.bin\" BINARY\n"
              "TRACK 01 MODE1/2352\n"
              "INDEX 01 00:00:00\n"
              "FILE \"disc-second.bin\" BINARY\n"
              "TRACK 02 MODE1/2352\n"
              "INDEX 01 00:00:02\n");
    bool rejected_out_of_range_index = false;
    try {
        (void)uife::DriverFactory::instance().create(cue_path);
    } catch (const std::runtime_error&) {
        rejected_out_of_range_index = true;
    }
    require(rejected_out_of_range_index,
            "BIN+CUE accepted an INDEX 01 beyond its own BINARY file");
}

void test_qcow2_driver(const std::filesystem::path& image_path) {
    constexpr std::uint64_t cluster_size = 4096;
    constexpr std::uint64_t refcount_table_offset = cluster_size;
    constexpr std::uint64_t l1_table_offset = 2 * cluster_size;
    constexpr std::uint64_t l2_table_offset = 3 * cluster_size;
    constexpr std::uint64_t first_data_offset = 4 * cluster_size;
    constexpr std::uint64_t refcount_block_offset = 5 * cluster_size;
    constexpr std::uint64_t third_data_offset = 6 * cluster_size;

    const auto create_image = [&](std::uint32_t version,
                                  std::uint64_t incompatible_features) {
        std::vector<std::byte> image(7 * cluster_size);
        write_be32(image, 0, 0x514649fbU);
        write_be32(image, 4, version);
        write_be32(image, 20, 12);
        write_be64(image, 24, 3 * cluster_size);
        write_be32(image, 36, 1);
        write_be64(image, 40, l1_table_offset);
        write_be64(image, 48, refcount_table_offset);
        write_be32(image, 56, 1);
        write_be64(image, static_cast<std::size_t>(refcount_table_offset),
                   refcount_block_offset);
        write_be64(image, static_cast<std::size_t>(l1_table_offset),
                   l2_table_offset);
        write_be64(image, static_cast<std::size_t>(l2_table_offset),
                   first_data_offset);
        write_be64(image, static_cast<std::size_t>(l2_table_offset) + 16,
                   third_data_offset);
        if (version == 3) {
            write_be64(image, 72, incompatible_features);
            write_be32(image, 96, 4);
            write_be32(image, 100, 104);
            if (incompatible_features == 0) {
                write_be64(image, static_cast<std::size_t>(l2_table_offset) + 8, 1);
            }
        }
        for (std::uint64_t cluster = 0; cluster < 3; ++cluster) {
            const auto data_offset =
                cluster == 0 ? first_data_offset : third_data_offset;
            std::fill(image.begin() + static_cast<std::ptrdiff_t>(data_offset),
                      image.begin() + static_cast<std::ptrdiff_t>(
                          data_offset + cluster_size),
                      cluster == 0 ? std::byte{0x45} : std::byte{0x6a});
        }
        for (std::size_t cluster = 0; cluster < 7; ++cluster) {
            image[static_cast<std::size_t>(refcount_block_offset) + cluster * 2] =
                std::byte{0};
            image[static_cast<std::size_t>(refcount_block_offset) + cluster * 2 + 1] =
                std::byte{1};
        }
        return image;
    };
    const auto write_image = [&](const std::vector<std::byte>& image) {
        std::ofstream output(image_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(image.data()),
                     static_cast<std::streamsize>(image.size()));
        require(static_cast<bool>(output), "failed to create QCOW2 test image");
    };

    for (const auto version : {2U, 3U}) {
        auto image = create_image(version, 0);
        write_image(image);
        const auto info = uife::DriverFactory::instance().identify(image_path);
        require(info.format == "QCOW2" && info.supported,
                "supported QCOW2 image was not recognized");
        auto driver = uife::DriverFactory::instance().create(image_path);
        require(driver->get_size() == 3 * cluster_size,
                "QCOW2 virtual capacity is incorrect");
        std::array<std::byte, 512> first_cluster{};
        require(driver->read_at(0, first_cluster) == first_cluster.size() &&
                    std::all_of(first_cluster.begin(), first_cluster.end(),
                                [](std::byte value) {
                                    return value == std::byte{0x45};
                                }),
                "QCOW2 allocated cluster mapping failed");
        std::array<std::byte, 512> sparse_cluster{};
        std::fill(sparse_cluster.begin(), sparse_cluster.end(), std::byte{0xff});
        require(driver->read_at(cluster_size, sparse_cluster) ==
                    sparse_cluster.size() &&
                    std::all_of(sparse_cluster.begin(), sparse_cluster.end(),
                                [](std::byte value) {
                                    return value == std::byte{0};
                                }),
                "QCOW2 sparse/zero cluster was not zero-filled");
        std::array<std::byte, 8> across_clusters{};
        require(driver->read_at(cluster_size - 4, across_clusters) ==
                    across_clusters.size() &&
                    std::all_of(across_clusters.begin(), across_clusters.begin() + 4,
                                [](std::byte value) {
                                    return value == std::byte{0x45};
                                }) &&
                    std::all_of(across_clusters.begin() + 4, across_clusters.end(),
                                [](std::byte value) {
                                    return value == std::byte{0};
                                }),
                "QCOW2 random read across cluster boundary failed");
        std::array<std::byte, 512> last_cluster{};
        require(driver->read_at(2 * cluster_size, last_cluster) ==
                    last_cluster.size() &&
                    std::all_of(last_cluster.begin(), last_cluster.end(),
                                [](std::byte value) {
                                    return value == std::byte{0x6a};
                                }),
                "QCOW2 later allocated cluster read failed");
        std::array<std::byte, 16> partial{};
        require(driver->read_at(3 * cluster_size - 5, partial) == 5,
                "QCOW2 end-of-image read was not bounded");
    }

    auto unsupported_image = create_image(3, 1);
    write_image(unsupported_image);
    const auto unsupported_info =
        uife::DriverFactory::instance().identify(image_path);
    require(unsupported_info.format == "QCOW2 (unsupported variant)" &&
                !unsupported_info.supported,
            "QCOW2 feature-bearing image was incorrectly advertised as readable");
    bool rejected_feature = false;
    try {
        (void)uife::DriverFactory::instance().create(image_path);
    } catch (const std::runtime_error&) {
        rejected_feature = true;
    }
    require(rejected_feature, "unsupported QCOW2 feature image was opened");

    auto malformed_image = create_image(2, 0);
    write_be64(malformed_image, static_cast<std::size_t>(l2_table_offset),
               malformed_image.size() + cluster_size);
    write_image(malformed_image);
    auto malformed_driver =
        uife::DriverFactory::instance().create(image_path);
    bool rejected_bad_cluster = false;
    try {
        std::array<std::byte, 16> data{};
        (void)malformed_driver->read_at(0, data);
    } catch (const std::runtime_error&) {
        rejected_bad_cluster = true;
    }
    require(rejected_bad_cluster,
            "QCOW2 out-of-bounds cluster mapping was read");
}

void test_vhdx_driver(const std::filesystem::path& image_path) {
    const auto write_image = [&](const std::vector<std::byte>& image) {
        std::ofstream output(image_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(image.data()),
                     static_cast<std::streamsize>(image.size()));
        require(static_cast<bool>(output), "failed to create VHDX test image");
    };
    auto image = make_vhdx_fixture();
    write_image(image);
    const auto info = uife::DriverFactory::instance().identify(image_path);
    require(info.format == "VHDX" && info.supported,
            "VHDX image was not registered as readable");
    auto driver = uife::DriverFactory::instance().create(image_path);
    require(driver->format_name() == "VHDX" &&
                driver->get_size() == 2ULL * 1024U * 1024U &&
                driver->sector_size() == 512,
            "VHDX virtual geometry is incorrect");
    std::array<std::byte, 512> sector{};
    require(driver->read_sector(0, sector) == sector.size() &&
                std::all_of(sector.begin(), sector.end(),
                            [](std::byte value) {
                                return value == std::byte{0x41};
                            }),
            "VHDX logical-sector read failed");
    std::array<std::byte, 512> allocated{};
    require(driver->read_at(0, allocated) == allocated.size() &&
                std::all_of(allocated.begin(), allocated.end(),
                            [](std::byte value) {
                                return value == std::byte{0x41};
                            }),
            "VHDX allocated payload block read failed");
    std::array<std::byte, 512> sparse{};
    std::fill(sparse.begin(), sparse.end(), std::byte{0xff});
    require(driver->read_at(1024U * 1024U, sparse) == sparse.size() &&
                std::all_of(sparse.begin(), sparse.end(),
                            [](std::byte value) {
                                return value == std::byte{0};
                            }),
            "VHDX not-present payload block was not zero-filled");
    std::array<std::byte, 8> across_blocks{};
    require(driver->read_at(1024U * 1024U - 4, across_blocks) ==
                across_blocks.size() &&
                std::all_of(across_blocks.begin(), across_blocks.begin() + 4,
                            [](std::byte value) {
                                return value == std::byte{0x41};
                            }) &&
                std::all_of(across_blocks.begin() + 4, across_blocks.end(),
                            [](std::byte value) {
                                return value == std::byte{0};
                            }),
            "VHDX read across payload block boundary failed");
    std::array<std::byte, 16> partial{};
    require(driver->read_at(2ULL * 1024U * 1024U - 5, partial) == 5,
            "VHDX end-of-image read was not bounded");

    image = make_vhdx_fixture();
    image[64U * 1024U + 4] ^= std::byte{0x01};
    image[192U * 1024U + 4] ^= std::byte{0x01};
    write_image(image);
    auto recovered_driver = uife::DriverFactory::instance().create(image_path);
    require(recovered_driver->get_size() == 2ULL * 1024U * 1024U,
            "VHDX valid alternate header/region table was not selected");

    image = make_vhdx_fixture(true);
    write_image(image);
    auto fixed_driver = uife::DriverFactory::instance().create(image_path);
    std::array<std::byte, 512> fixed_block{};
    require(fixed_driver->read_at(1024U * 1024U, fixed_block) ==
                fixed_block.size() &&
                std::all_of(fixed_block.begin(), fixed_block.end(),
                            [](std::byte value) {
                                return value == std::byte{0x72};
                            }),
            "VHDX fixed payload block read failed");

    image = make_vhdx_fixture(false, true);
    write_image(image);
    bool rejected_parent = false;
    try {
        (void)uife::DriverFactory::instance().create(image_path);
    } catch (const std::runtime_error&) {
        rejected_parent = true;
    }
    require(rejected_parent, "VHDX differencing image was accepted");

    image = make_vhdx_fixture();
    write_le64(image, 2U * 1024U * 1024U,
               static_cast<std::uint64_t>(image.size()) + 1024U * 1024U | 6U);
    write_image(image);
    bool rejected_bad_payload = false;
    try {
        (void)uife::DriverFactory::instance().create(image_path);
    } catch (const std::runtime_error&) {
        rejected_bad_payload = true;
    }
    require(rejected_bad_payload, "VHDX out-of-bounds payload block was accepted");

    image = make_vhdx_fixture();
    image[64U * 1024U + 4] ^= std::byte{0x01};
    image[128U * 1024U + 4] ^= std::byte{0x01};
    write_image(image);
    bool rejected_bad_headers = false;
    try {
        (void)uife::DriverFactory::instance().create(image_path);
    } catch (const std::runtime_error&) {
        rejected_bad_headers = true;
    }
    require(rejected_bad_headers, "VHDX invalid header checksums were accepted");
}

void test_vmdk_sparse_driver(const std::filesystem::path& image_path) {
    constexpr std::size_t sector_size = 512;
    constexpr std::size_t grain_sectors = 4;
    constexpr std::size_t grain_bytes = sector_size * grain_sectors;
    constexpr std::size_t backing_sectors = 3 + grain_sectors;
    std::vector<std::byte> image(sector_size * backing_sectors);
    write_le32(image, 0, 0x564d444bU);
    write_le32(image, 4, 1);
    write_le64(image, 12, 8);
    write_le64(image, 20, grain_sectors);
    write_le32(image, 44, 4);
    write_le64(image, 56, 1);
    write_le32(image, sector_size, 2);
    write_le32(image, 2 * sector_size, 3);
    std::fill(image.begin() + static_cast<std::ptrdiff_t>(3 * sector_size),
              image.begin() + static_cast<std::ptrdiff_t>(3 * sector_size + grain_bytes),
              std::byte{0x5a});
    {
        std::ofstream output(image_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(image.data()),
                     static_cast<std::streamsize>(image.size()));
        require(static_cast<bool>(output), "failed to create sparse VMDK test image");
    }

    const auto detected = uife::DriverFactory::instance().identify(image_path);
    require(detected.format == "VMDK" && detected.supported,
            "sparse VMDK driver was not advertised as readable");
    auto driver = uife::DriverFactory::instance().create(image_path);
    require(driver->format_name() == "VMDK" &&
                driver->get_size() == 8 * sector_size,
            "sparse VMDK virtual geometry is incorrect");

    std::array<std::byte, sector_size> allocated{};
    require(driver->read_sector(0, allocated) == allocated.size() &&
                std::all_of(allocated.begin(), allocated.end(),
                            [](std::byte value) { return value == std::byte{0x5a}; }),
            "sparse VMDK allocated grain read failed");
    std::array<std::byte, sector_size> sparse{};
    require(driver->read_sector(4, sparse) == sparse.size() &&
                std::all_of(sparse.begin(), sparse.end(),
                            [](std::byte value) { return value == std::byte{0}; }),
            "sparse VMDK unallocated grain was not zero-filled");

    std::array<std::byte, 8> cross_grain{};
    require(driver->read_at(grain_bytes - 2, cross_grain) == cross_grain.size() &&
                cross_grain[0] == std::byte{0x5a} &&
                cross_grain[1] == std::byte{0x5a} &&
                std::all_of(cross_grain.begin() + 2, cross_grain.end(),
                            [](std::byte value) { return value == std::byte{0}; }),
            "sparse VMDK random read across grain boundaries failed");

    write_le32(image, 8, 0x4U);
    write_le32(image, 2 * sector_size + sizeof(std::uint32_t), 1);
    {
        std::ofstream output(image_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(image.data()),
                     static_cast<std::streamsize>(image.size()));
        require(static_cast<bool>(output), "failed to create zeroed-grain VMDK test");
    }
    auto zeroed_grain_driver =
        uife::DriverFactory::instance().create(image_path);
    std::fill(sparse.begin(), sparse.end(), std::byte{0xff});
    require(zeroed_grain_driver->read_sector(4, sparse) == sparse.size() &&
                std::all_of(sparse.begin(), sparse.end(),
                            [](std::byte value) { return value == std::byte{0}; }),
            "sparse VMDK zeroed-grain marker was not honored");

    write_le32(image, 8, 0x10000U);
    {
        std::ofstream output(image_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(image.data()),
                     static_cast<std::streamsize>(image.size()));
        require(static_cast<bool>(output), "failed to create compressed VMDK test image");
    }
    bool rejected_compression = false;
    try {
        (void)uife::DriverFactory::instance().create(image_path);
    } catch (const std::runtime_error&) {
        rejected_compression = true;
    }
    require(rejected_compression,
            "unsupported compressed VMDK was not explicitly rejected");

    const auto flat_extent_path =
        image_path.parent_path() / "uife-vmdk-flat extent.bin";
    const auto flat_descriptor_path =
        image_path.parent_path() / "uife-test-flat.vmdk";
    std::vector<std::byte> flat_extent(5 * sector_size, std::byte{0});
    std::fill(flat_extent.begin() + static_cast<std::ptrdiff_t>(sector_size),
              flat_extent.end(), std::byte{0x3c});
    {
        std::ofstream output(flat_extent_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(flat_extent.data()),
                     static_cast<std::streamsize>(flat_extent.size()));
        require(static_cast<bool>(output), "failed to create VMDK FLAT extent test file");
    }
    const auto write_flat_descriptor = [&flat_descriptor_path](std::uint64_t sectors) {
        std::ofstream output(flat_descriptor_path, std::ios::binary);
        output << "# Disk DescriptorFile\n"
                  "version=1\n"
                  "CID=fffffffe\n"
                  "parentCID=ffffffff\n"
                  "\"createType\" = \"monolithicFlat\"\n"
                  "RW " << sectors << " FLAT \"uife-vmdk-flat extent.bin\" 1\n"
                  "ddb.virtualHWVersion = \"4\"\n";
        require(static_cast<bool>(output), "failed to create VMDK FLAT descriptor");
    };
    write_flat_descriptor(4);
    const auto flat_info =
        uife::DriverFactory::instance().identify(flat_descriptor_path);
    require(flat_info.format == "VMDK" && flat_info.supported,
            "single-extent VMDK FLAT descriptor was not recognized");
    auto flat_driver =
        uife::DriverFactory::instance().create(flat_descriptor_path);
    require(flat_driver->get_size() == 4 * sector_size &&
                flat_driver->format_name() == "VMDK",
            "VMDK FLAT virtual capacity is incorrect");
    std::array<std::byte, sector_size> flat_sector{};
    require(flat_driver->read_sector(0, flat_sector) == flat_sector.size() &&
                std::all_of(flat_sector.begin(), flat_sector.end(),
                            [](std::byte value) { return value == std::byte{0x3c}; }),
            "VMDK FLAT extent offset was not applied to reads");
    std::array<std::byte, 4> flat_tail{};
    require(flat_driver->read_at(4 * sector_size - flat_tail.size(), flat_tail) ==
                flat_tail.size() &&
                std::all_of(flat_tail.begin(), flat_tail.end(),
                            [](std::byte value) { return value == std::byte{0x3c}; }),
            "VMDK FLAT random read near virtual end failed");
    {
        std::ofstream output(flat_descriptor_path, std::ios::binary);
        output << "# Disk DescriptorFile\n"
                  "version=1\n"
                  "CID=fffffffe\n"
                  "parentCID=ffffffff\n"
                  "\"createType\" = \"monolithicFlat\"\n"
                  "RDONLY 4 FLAT \"uife-vmdk-flat extent.bin\" 1\n";
        require(static_cast<bool>(output),
                "failed to create read-only VMDK descriptor");
    }
    auto readonly_flat_driver =
        uife::DriverFactory::instance().create(flat_descriptor_path);
    require(readonly_flat_driver->get_size() == 4 * sector_size &&
                readonly_flat_driver->read_sector(0, flat_sector) ==
                    flat_sector.size() &&
                std::all_of(flat_sector.begin(), flat_sector.end(),
                            [](std::byte value) {
                                return value == std::byte{0x3c};
                            }),
            "VMDK RDONLY FLAT extent could not be read");
    readonly_flat_driver->close();
    const auto second_flat_extent_path =
        image_path.parent_path() / "uife-vmdk-flat-second.bin";
    std::vector<std::byte> second_flat_extent(
        2 * sector_size, std::byte{0x6a});
    {
        std::ofstream output(second_flat_extent_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(second_flat_extent.data()),
                     static_cast<std::streamsize>(second_flat_extent.size()));
        require(static_cast<bool>(output),
                "failed to create second VMDK FLAT extent test file");
    }
    {
        std::ofstream output(flat_descriptor_path, std::ios::binary);
        output << "# Disk DescriptorFile\n"
                  "version=1\n"
                  "CID=fffffffe\n"
                  "parentCID=ffffffff\n"
                  "\"createType\" = \"twoGbMaxExtentFlat\"\n"
                  "RW 2 FLAT \"uife-vmdk-flat extent.bin\" 1\n"
                  "RW 2 FLAT \"uife-vmdk-flat-second.bin\" 0\n"
                  "ddb.virtualHWVersion = \"4\"\n";
        require(static_cast<bool>(output),
                "failed to create multi-extent VMDK descriptor");
    }
    auto multi_flat_driver =
        uife::DriverFactory::instance().create(flat_descriptor_path);
    std::array<std::byte, 4> across_flat_extents{};
    require(multi_flat_driver->get_size() == 4 * sector_size &&
                multi_flat_driver->read_at(2 * sector_size - 2,
                                           across_flat_extents) ==
                    across_flat_extents.size() &&
                across_flat_extents[0] == std::byte{0x3c} &&
                across_flat_extents[1] == std::byte{0x3c} &&
                across_flat_extents[2] == std::byte{0x6a} &&
                across_flat_extents[3] == std::byte{0x6a},
            "VMDK FLAT reads did not cross extent boundaries correctly");
    multi_flat_driver->close();
    {
        std::ofstream output(flat_descriptor_path, std::ios::binary);
        output << "# Disk DescriptorFile\n"
                  "version=1\n"
                  "CID=fffffffe\n"
                  "parentCID=ffffffff\n"
                  "\"createType\" = \"monolithicFlat\"\n"
                  "RW 2 FLAT \"uife-vmdk-flat extent.bin\" 1\n"
                  "RW 2 FLAT \"uife-vmdk-flat-second.bin\" 0\n";
        require(static_cast<bool>(output),
                "failed to create invalid monolithicFlat descriptor");
    }
    bool rejected_multiple_monolithic_extents = false;
    try {
        (void)uife::DriverFactory::instance().create(flat_descriptor_path);
    } catch (const std::runtime_error&) {
        rejected_multiple_monolithic_extents = true;
    }
    require(rejected_multiple_monolithic_extents,
            "VMDK accepted multiple FLAT extents as monolithicFlat");
    {
        std::ofstream output(flat_descriptor_path, std::ios::binary);
        output << "# Disk DescriptorFile\n"
                  "version=1\n"
                  "CID=fffffffe\n"
                  "parentCID=12345678\n"
                  "\"createType\" = \"twoGbMaxExtentFlat\"\n"
                  "RW 2 FLAT \"uife-vmdk-flat extent.bin\" 1\n"
                  "\"parentFileNameHint\" = \"parent.vmdk\"\n";
        require(static_cast<bool>(output),
                "failed to create parent-linked VMDK descriptor");
    }
    bool rejected_parent_link = false;
    try {
        (void)uife::DriverFactory::instance().create(flat_descriptor_path);
    } catch (const std::runtime_error&) {
        rejected_parent_link = true;
    }
    require(rejected_parent_link,
            "VMDK accepted a parent-linked FLAT descriptor as standalone");
    {
        std::ofstream output(flat_descriptor_path, std::ios::binary);
        output << "# Disk DescriptorFile\n"
                  "version=1\n"
                  "CID=fffffffe\n"
                  "\"createType\" = \"monolithicFlat\"\n"
                  "RW 2 FLAT \"uife-vmdk-flat extent.bin\" 1\n";
        require(static_cast<bool>(output),
                "failed to create VMDK descriptor missing parentCID");
    }
    bool rejected_missing_parent_cid = false;
    try {
        (void)uife::DriverFactory::instance().create(flat_descriptor_path);
    } catch (const std::runtime_error&) {
        rejected_missing_parent_cid = true;
    }
    require(rejected_missing_parent_cid,
            "VMDK accepted a descriptor without a parentCID field");
    {
        std::ofstream output(flat_descriptor_path, std::ios::binary);
        output << "# Disk DescriptorFile\n"
                  "version=1\n"
                  "parentCID=ffffffff\n"
                  "\"createType\" = \"monolithicFlat\"\n"
                  "RW 2 FLAT \"uife-vmdk-flat extent.bin\" 1\n";
        require(static_cast<bool>(output),
                "failed to create VMDK descriptor missing CID");
    }
    bool rejected_missing_cid = false;
    try {
        (void)uife::DriverFactory::instance().create(flat_descriptor_path);
    } catch (const std::runtime_error&) {
        rejected_missing_cid = true;
    }
    require(rejected_missing_cid,
            "VMDK accepted a descriptor without a CID field");
    {
        std::ofstream output(flat_descriptor_path, std::ios::binary);
        output << "# Disk DescriptorFile\n"
                  "version=1\n"
                  "CID=fffffffe\n"
                  "parentCID=ffffffff\n"
                  "\"createType\" = \"monolithicFlat\"\n"
                  "NOACCESS 2 FLAT \"uife-vmdk-flat extent.bin\" 1\n";
        require(static_cast<bool>(output),
                "failed to create VMDK NOACCESS descriptor");
    }
    bool rejected_noaccess_extent = false;
    try {
        (void)uife::DriverFactory::instance().create(flat_descriptor_path);
    } catch (const std::runtime_error&) {
        rejected_noaccess_extent = true;
    }
    require(rejected_noaccess_extent,
            "VMDK ignored a NOACCESS extent declaration");
    {
        std::ofstream output(flat_descriptor_path, std::ios::binary);
        output << "# Disk DescriptorFile\n"
                  "version=1\n"
                  "CID=fffffffe\n"
                  "parentCID=ffffffff\n"
                  "\"createType\" = \"monolithicFlat\"\n"
                  "RW 2 FLAT \"uife-vmdk-flat extent.bin\" 1\n"
                  "\"parentFileNameHint\" = \"unterminated\n";
        require(static_cast<bool>(output),
                "failed to create malformed VMDK parent-file setting");
    }
    bool rejected_malformed_parent_setting = false;
    try {
        (void)uife::DriverFactory::instance().create(flat_descriptor_path);
    } catch (const std::runtime_error&) {
        rejected_malformed_parent_setting = true;
    }
    require(rejected_malformed_parent_setting,
            "VMDK ignored a malformed quoted parent-file setting");
    {
        std::ofstream output(flat_descriptor_path, std::ios::binary);
        output << "# Disk DescriptorFile\n"
                  "version=1\n"
                  "CID=fffffffe\n"
                  "parentCID=ffffffff\n"
                  "\"createType\" = \"monolithicFlat\"\n"
                  "RW 2 FLAT \"..\\\\outside-flat.bin\" 0\n";
        require(static_cast<bool>(output),
                "failed to create VMDK path traversal descriptor");
    }
    bool rejected_extent_path_escape = false;
    try {
        (void)uife::DriverFactory::instance().create(flat_descriptor_path);
    } catch (const std::runtime_error&) {
        rejected_extent_path_escape = true;
    }
    require(rejected_extent_path_escape,
            "VMDK descriptor was allowed to reference an extent outside its directory");
    {
        std::ofstream output(flat_descriptor_path, std::ios::binary);
        output << "# Disk DescriptorFile\n"
                  "version=1\n"
                  "CID=fffffffe\n"
                  "parentCID=ffffffff\n"
                  "\"createType\" = \"monolithicFlat\"\n"
                  "RW 2 FLAT \"" << flat_extent_path.string() << "\" 0\n";
        require(static_cast<bool>(output),
                "failed to create absolute-path VMDK descriptor");
    }
    bool rejected_absolute_extent_path = false;
    try {
        (void)uife::DriverFactory::instance().create(flat_descriptor_path);
    } catch (const std::runtime_error&) {
        rejected_absolute_extent_path = true;
    }
    require(rejected_absolute_extent_path,
            "VMDK descriptor was allowed to reference an absolute extent path");
    write_flat_descriptor(6);
    bool rejected_flat_overflow = false;
    try {
        (void)uife::DriverFactory::instance().create(flat_descriptor_path);
    } catch (const std::runtime_error&) {
        rejected_flat_overflow = true;
    }
    require(rejected_flat_overflow,
            "VMDK FLAT extent beyond its backing file was accepted");
    flat_driver->close();
    std::filesystem::remove(flat_extent_path);
    std::filesystem::remove(second_flat_extent_path);
    std::filesystem::remove(flat_descriptor_path);

    constexpr std::string_view descriptor =
        "# Disk DescriptorFile\nversion=1\n";
    {
        std::ofstream output(image_path, std::ios::binary);
        output.write(descriptor.data(),
                     static_cast<std::streamsize>(descriptor.size()));
        require(static_cast<bool>(output), "failed to create VMDK descriptor test");
    }
    const auto descriptor_info =
        uife::DriverFactory::instance().identify(image_path);
    require(descriptor_info.format == "VMDK (descriptor/flat)" &&
                !descriptor_info.supported,
            "unsupported descriptor VMDK was not identified explicitly");
    bool refused_descriptor = false;
    try {
        (void)uife::DriverFactory::instance().create(image_path);
    } catch (const std::runtime_error&) {
        refused_descriptor = true;
    }
    require(refused_descriptor,
            "unsupported descriptor VMDK was incorrectly treated as a RAW image");
}

void test_vdi_driver(const std::filesystem::path& image_path) {
    constexpr std::size_t sector_size = 512;
    constexpr std::size_t block_size = sector_size;
    std::vector<std::byte> image(4 * sector_size);
    constexpr std::string_view preheader =
        "<<< Oracle VM VirtualBox Disk Image >>>\n";
    std::transform(preheader.begin(), preheader.end(), image.begin(),
        [](char value) { return static_cast<std::byte>(value); });
    write_le32(image, 64, 0xbeda107fU);
    write_le32(image, 68, 0x00010001U);
    write_le32(image, 72, 400);
    write_le32(image, 76, 1);
    write_le32(image, 340, 512);
    write_le32(image, 344, 1024);
    write_le32(image, 360, sector_size);
    write_le64(image, 368, 4 * block_size);
    write_le32(image, 376, block_size);
    write_le32(image, 380, 0);
    write_le32(image, 384, 4);
    write_le32(image, 388, 2);
    write_le32(image, 512, 1);
    write_le32(image, 512 + 4, 0xffffffffU);
    write_le32(image, 512 + 8, 0);
    write_le32(image, 512 + 12, 0xfffffffeU);
    std::fill(image.begin() + 2 * sector_size,
              image.begin() + 3 * sector_size, std::byte{0x2a});
    std::fill(image.begin() + 3 * sector_size,
              image.end(), std::byte{0x5a});
    {
        std::ofstream output(image_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(image.data()),
                     static_cast<std::streamsize>(image.size()));
        require(static_cast<bool>(output), "failed to create VDI test image");
    }
    const auto info = uife::DriverFactory::instance().identify(image_path);
    require(info.format == "VDI" && info.supported,
            "VDI signature was not registered as a readable image");
    auto driver = uife::DriverFactory::instance().create(image_path);
    require(driver->format_name() == "VDI" &&
                driver->get_size() == 4 * block_size,
            "VDI virtual disk geometry is incorrect");
    std::array<std::byte, sector_size> first_block{};
    require(driver->read_sector(0, first_block) == first_block.size() &&
                std::all_of(first_block.begin(), first_block.end(),
                            [](std::byte value) { return value == std::byte{0x5a}; }),
            "VDI physical block mapping failed");
    std::array<std::byte, sector_size> unallocated{};
    require(driver->read_sector(1, unallocated) == unallocated.size() &&
                std::all_of(unallocated.begin(), unallocated.end(),
                            [](std::byte value) { return value == std::byte{0}; }),
            "VDI unallocated block was not zero-filled");
    std::array<std::byte, sector_size> second_allocated{};
    require(driver->read_sector(2, second_allocated) == second_allocated.size() &&
                std::all_of(second_allocated.begin(), second_allocated.end(),
                            [](std::byte value) { return value == std::byte{0x2a}; }),
            "VDI non-sequential physical block mapping failed");
    std::array<std::byte, 8> across_blocks{};
    require(driver->read_at(block_size - 4, across_blocks) == across_blocks.size() &&
                across_blocks[0] == std::byte{0x5a} &&
                across_blocks[3] == std::byte{0x5a} &&
                std::all_of(across_blocks.begin() + 4, across_blocks.end(),
                            [](std::byte value) { return value == std::byte{0}; }),
            "VDI random read across block boundary failed");
    std::array<std::byte, sector_size> partial{};
    require(driver->read_at(4 * block_size - 10, partial) == 10,
            "VDI virtual end did not return a bounded partial read");

    write_le32(image, 76, 4);
    {
        std::ofstream output(image_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(image.data()),
                     static_cast<std::streamsize>(image.size()));
        require(static_cast<bool>(output), "failed to create differencing VDI test");
    }
    bool rejected_differencing = false;
    const auto differencing_info =
        uife::DriverFactory::instance().identify(image_path);
    require(differencing_info.format == "VDI (unsupported variant)" &&
                !differencing_info.supported,
            "differencing VDI was reported as a readable image");
    try {
        (void)uife::DriverFactory::instance().create(image_path);
    } catch (const std::runtime_error&) {
        rejected_differencing = true;
    }
    require(rejected_differencing,
            "unsupported differencing VDI was not explicitly rejected");
}

void test_qed_driver(const std::filesystem::path& image_path) {
    constexpr std::uint32_t cluster_size = 4096;
    constexpr std::uint32_t table_size_clusters = 1;
    constexpr std::uint64_t l1_offset = cluster_size;
    constexpr std::uint64_t l2_offset = 2ULL * cluster_size;
    constexpr std::uint64_t first_data_offset = 3ULL * cluster_size;
    constexpr std::uint64_t second_data_offset = 4ULL * cluster_size;
    std::vector<std::byte> image(5ULL * cluster_size);
    write_le32(image, 0, 0x00444551U);
    write_le32(image, 4, cluster_size);
    write_le32(image, 8, table_size_clusters);
    write_le32(image, 12, 1);
    write_le64(image, 40, l1_offset);
    write_le64(image, 48, 3ULL * cluster_size);
    write_le64(image, static_cast<std::size_t>(l1_offset), l2_offset);
    write_le64(image, static_cast<std::size_t>(l2_offset), first_data_offset);
    write_le64(image, static_cast<std::size_t>(l2_offset) + 8, 1);
    write_le64(image, static_cast<std::size_t>(l2_offset) + 16, second_data_offset);
    std::fill(image.begin() + static_cast<std::ptrdiff_t>(first_data_offset),
              image.begin() + static_cast<std::ptrdiff_t>(first_data_offset + cluster_size),
              std::byte{0x31});
    std::fill(image.begin() + static_cast<std::ptrdiff_t>(second_data_offset),
              image.end(), std::byte{0x73});
    {
        std::ofstream output(image_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(image.data()),
                     static_cast<std::streamsize>(image.size()));
        require(static_cast<bool>(output), "failed to create QED test image");
    }

    const auto info = uife::DriverFactory::instance().identify(image_path);
    require(info.format == "QED" && info.supported,
            "QED image was not registered as a readable format");
    auto driver = uife::DriverFactory::instance().create(image_path);
    require(driver->format_name() == "QED" &&
                driver->get_size() == 3ULL * cluster_size,
            "QED virtual capacity is incorrect");
    std::array<std::byte, 512> first_cluster{};
    require(driver->read_at(0, first_cluster) == first_cluster.size() &&
                std::all_of(first_cluster.begin(), first_cluster.end(),
                            [](std::byte value) { return value == std::byte{0x31}; }),
            "QED allocated cluster mapping failed");
    std::array<std::byte, 512> zero_cluster{};
    std::fill(zero_cluster.begin(), zero_cluster.end(), std::byte{0xff});
    require(driver->read_at(cluster_size, zero_cluster) == zero_cluster.size() &&
                std::all_of(zero_cluster.begin(), zero_cluster.end(),
                            [](std::byte value) { return value == std::byte{0}; }),
            "QED explicit zero cluster was not zero-filled");
    std::array<std::byte, 8> across_clusters{};
    require(driver->read_at(cluster_size - 4, across_clusters) ==
                across_clusters.size() &&
                std::all_of(across_clusters.begin(), across_clusters.begin() + 4,
                            [](std::byte value) { return value == std::byte{0x31}; }) &&
                std::all_of(across_clusters.begin() + 4, across_clusters.end(),
                            [](std::byte value) { return value == std::byte{0}; }),
            "QED random read across cluster boundary failed");
    std::array<std::byte, 512> last_cluster{};
    require(driver->read_at(2ULL * cluster_size, last_cluster) ==
                last_cluster.size() &&
                std::all_of(last_cluster.begin(), last_cluster.end(),
                            [](std::byte value) { return value == std::byte{0x73}; }),
            "QED second allocated cluster read failed");
    std::array<std::byte, 16> partial{};
    require(driver->read_at(3ULL * cluster_size - 5, partial) == 5,
            "QED image end did not return a bounded partial read");

    write_le64(image, 16, 0x2);
    {
        std::ofstream output(image_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(image.data()),
                     static_cast<std::streamsize>(image.size()));
        require(static_cast<bool>(output), "failed to create QED need-check test");
    }
    const auto need_check_info =
        uife::DriverFactory::instance().identify(image_path);
    require(need_check_info.format == "QED (unsupported variant)" &&
                !need_check_info.supported,
            "QED need-check image was incorrectly marked readable");
    bool rejected_need_check = false;
    try {
        (void)uife::DriverFactory::instance().create(image_path);
    } catch (const std::runtime_error&) {
        rejected_need_check = true;
    }
    require(rejected_need_check,
            "QED image requiring consistency check was accepted");

    write_le64(image, 16, 0);
    write_le64(image, static_cast<std::size_t>(l2_offset), image.size() + cluster_size);
    {
        std::ofstream output(image_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(image.data()),
                     static_cast<std::streamsize>(image.size()));
        require(static_cast<bool>(output), "failed to create QED bad-data test");
    }
    auto malformed_data_driver =
        uife::DriverFactory::instance().create(image_path);
    bool rejected_bad_data = false;
    try {
        std::array<std::byte, 16> invalid_data{};
        (void)malformed_data_driver->read_at(0, invalid_data);
    } catch (const std::runtime_error&) {
        rejected_bad_data = true;
    }
    require(rejected_bad_data, "QED out-of-bounds data cluster was read");

    write_le64(image, static_cast<std::size_t>(l1_offset), image.size() + cluster_size);
    {
        std::ofstream output(image_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(image.data()),
                     static_cast<std::streamsize>(image.size()));
        require(static_cast<bool>(output), "failed to create QED bad-table test");
    }
    bool rejected_bad_table = false;
    try {
        (void)uife::DriverFactory::instance().create(image_path);
    } catch (const std::runtime_error&) {
        rejected_bad_table = true;
    }
    require(rejected_bad_table, "QED out-of-bounds L2 table was accepted");
}

void test_gpt(const std::filesystem::path& image_path) {
    constexpr std::size_t sector_size = 512;
    constexpr std::uint32_t sector_count = 256;
    std::vector<std::byte> image(sector_size * sector_count);
    image[510] = std::byte{0x55};
    image[511] = std::byte{0xaa};
    image[446 + 4] = std::byte{0xee};
    write_le32(image, 446 + 8, 1);
    write_le32(image, 446 + 12, sector_count - 1);

    constexpr std::size_t header_offset = sector_size;
    constexpr std::size_t entries_offset = 2 * sector_size;
    constexpr std::size_t table_size = 4 * 128;
    std::copy_n(reinterpret_cast<const std::byte*>("EFI PART"), 8,
                image.begin() + static_cast<std::ptrdiff_t>(header_offset));
    write_le32(image, header_offset + 8, 0x00010000);
    write_le32(image, header_offset + 12, 92);
    write_le64(image, header_offset + 24, 1);
    write_le64(image, header_offset + 32, sector_count - 1);
    write_le64(image, header_offset + 40, 34);
    write_le64(image, header_offset + 48, sector_count - 34);
    write_le64(image, header_offset + 72, 2);
    write_le32(image, header_offset + 80, 4);
    write_le32(image, header_offset + 84, 128);

    image[entries_offset] = std::byte{0x28};
    image[entries_offset + 1] = std::byte{0x73};
    write_le64(image, entries_offset + 32, 34);
    write_le64(image, entries_offset + 40, 100);
    write_le32(image, header_offset + 88,
               crc32(std::span<const std::byte>(image).subspan(entries_offset, table_size)));
    std::array<std::byte, 92> header{};
    std::copy_n(image.begin() + static_cast<std::ptrdiff_t>(header_offset),
                header.size(), header.begin());
    header[16] = std::byte{0};
    header[17] = std::byte{0};
    header[18] = std::byte{0};
    header[19] = std::byte{0};
    const auto header_crc = crc32(header);
    write_le32(image, header_offset + 16, header_crc);
    {
        std::ofstream output(image_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(image.data()),
                     static_cast<std::streamsize>(image.size()));
        require(static_cast<bool>(output), "failed to create GPT test image");
    }
    auto driver = uife::DriverFactory::instance().create(image_path);
    const auto partitions = uife::scan_partitions(*driver);
    require(partitions.size() == 1 && partitions.front().scheme == "GPT" &&
                partitions.front().byte_offset == 34 * sector_size &&
                partitions.front().byte_length == 67 * sector_size,
            "GPT partition scanner returned incorrect bounds");
}

void test_mbr_logical_partitions(const std::filesystem::path& image_path) {
    constexpr std::size_t sector_size = 512;
    constexpr std::uint32_t sector_count = 6000;
    constexpr std::uint32_t extended_base = 100;
    constexpr std::uint32_t second_ebr_relative = 1000;
    std::vector<std::byte> image(sector_size * sector_count);
    image[510] = std::byte{0x55};
    image[511] = std::byte{0xaa};
    image[446 + 4] = std::byte{0x0f};
    write_le32(image, 446 + 8, extended_base);
    write_le32(image, 446 + 12, 5000);

    const auto first_ebr_offset = static_cast<std::size_t>(extended_base) * sector_size;
    const auto second_ebr_offset =
        static_cast<std::size_t>(extended_base + second_ebr_relative) * sector_size;
    for (const auto offset : {first_ebr_offset, second_ebr_offset}) {
        image[offset + 510] = std::byte{0x55};
        image[offset + 511] = std::byte{0xaa};
    }
    image[first_ebr_offset + 446 + 4] = std::byte{0x83};
    write_le32(image, first_ebr_offset + 446 + 8, 63);
    write_le32(image, first_ebr_offset + 446 + 12, 400);
    image[first_ebr_offset + 462 + 4] = std::byte{0x0f};
    write_le32(image, first_ebr_offset + 462 + 8, second_ebr_relative);
    write_le32(image, first_ebr_offset + 462 + 12, 4000);

    image[second_ebr_offset + 446 + 4] = std::byte{0x07};
    write_le32(image, second_ebr_offset + 446 + 8, 63);
    write_le32(image, second_ebr_offset + 446 + 12, 400);
    {
        std::ofstream output(image_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(image.data()),
                     static_cast<std::streamsize>(image.size()));
        require(static_cast<bool>(output), "failed to create MBR EBR test image");
    }
    auto driver = uife::DriverFactory::instance().create(image_path);
    auto partitions = uife::scan_partitions(*driver);
    require(partitions.size() == 2 &&
                partitions[0].byte_offset ==
                    static_cast<std::uint64_t>(extended_base + 63) * sector_size &&
                partitions[0].byte_length == 400ULL * sector_size &&
                partitions[1].byte_offset ==
                    static_cast<std::uint64_t>(extended_base + second_ebr_relative + 63) *
                        sector_size &&
                partitions[1].byte_length == 400ULL * sector_size,
            "MBR EBR scanner failed to find bounded logical partitions");

    image[second_ebr_offset + 462 + 4] = std::byte{0x0f};
    write_le32(image, second_ebr_offset + 462 + 8, 0);
    write_le32(image, second_ebr_offset + 462 + 12, 5000);
    {
        std::ofstream output(image_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(image.data()),
                     static_cast<std::streamsize>(image.size()));
        require(static_cast<bool>(output), "failed to write cyclic EBR test image");
    }
    driver = uife::DriverFactory::instance().create(image_path);
    bool rejected_cycle = false;
    try {
        (void)uife::scan_partitions(*driver);
    } catch (const std::runtime_error&) {
        rejected_cycle = true;
    }
    require(rejected_cycle, "MBR EBR scanner did not reject a cyclic chain");
}

void test_restore(const std::filesystem::path& output_path,
                  const std::filesystem::path& unsafe_output) {
    FakeFsParser parser;
    const uife::VFS filesystem(parser);
    std::uint64_t last_progress = 0;
    uife::ExtractOptions options;
    options.output_directory = output_path;
    options.progress = [&last_progress](std::uint64_t done, std::uint64_t total) {
        require(done <= total, "progress exceeded total bytes");
        last_progress = done;
    };
    const auto result = uife::Extractor::restore(filesystem, options);
    require(result.size() == 2, "restore did not export all regular files");
    require(last_progress == 14, "progress callback did not account for all file bytes");
    const auto root_file = output_path / "hello.txt";
    const auto nested_file = output_path / "folder" / "nested.bin";
    require(std::filesystem::exists(root_file) && std::filesystem::exists(nested_file),
            "restore output tree is incomplete");
    require(std::any_of(result.begin(), result.end(), [](const auto& file) {
        return file.sha256 ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    }), "exported file SHA-256 differs from the source");

    bool refused_overwrite = false;
    try {
    (void)uife::Extractor::restore(filesystem, options);
    } catch (const std::runtime_error&) {
    refused_overwrite = true;
    }
    require(refused_overwrite, "extractor overwrote an existing file");

    FakeFsParser unsafe_parser(true);
    const uife::VFS unsafe_vfs(unsafe_parser);
    options.output_directory = unsafe_output;
    bool refused_traversal = false;
    try {
    (void)uife::Extractor::restore(unsafe_vfs, options);
    } catch (const std::runtime_error&) {
    refused_traversal = true;
    }
    require(refused_traversal, "extractor accepted a path-traversal filename");
    require(!std::filesystem::exists(unsafe_output.parent_path() / "escape"),
        "path traversal created a file outside the export directory");

    FakeFsParser invalid_size_parser(false, true);
    const uife::VFS invalid_size_vfs(invalid_size_parser);
    const auto invalid_size_output = unsafe_output / "invalid-size";
    options.output_directory = invalid_size_output;
    bool refused_invalid_size = false;
    try {
        (void)uife::Extractor::restore(invalid_size_vfs, options);
    } catch (const std::runtime_error& error) {
        refused_invalid_size =
            std::string(error.what()).find("文件大小元数据异常") != std::string::npos;
    }
    require(refused_invalid_size,
            "extractor did not reject file entries with invalid size metadata");
    require(!std::filesystem::exists(invalid_size_output / "hello.txt") &&
                !std::filesystem::exists(invalid_size_output / "corrupt.bin"),
            "extractor wrote files before validating all file sizes");
}

// 目录联接（IO_REPARSE_TAG_MOUNT_POINT）必须重建成联接点：退化成文件符号链接
// 在 Windows 上根本指不到目录，导出结果看起来"成功"却不可用。
void test_reparse_point_restore(const std::filesystem::path& output_path) {
    const auto target = output_path.parent_path() / "uife-junction-target";
    std::filesystem::create_directories(target);
    {
        std::ofstream marker(target / "marker.txt", std::ios::binary);
        marker << "marker";
    }

    ReparsePointFsParser parser(target.generic_string(), 0xA0000003U);
    const uife::VFS filesystem(parser);
    uife::ExtractOptions options;
    options.output_directory = output_path;
    const auto result = uife::Extractor::restore(filesystem, options);
    require(result.empty(), "a junction must not be exported as a regular file");

    const auto link = output_path / "linkdir";
    require(std::filesystem::exists(link / "marker.txt"),
            "restored junction does not resolve to its target");
#ifdef _WIN32
    HANDLE handle = CreateFileW(link.c_str(), FILE_READ_EA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "cannot open the restored junction");
    std::array<std::byte, 16 * 1024> buffer{};
    DWORD returned = 0;
    const auto ok = DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, nullptr, 0,
        buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr);
    CloseHandle(handle);
    require(ok && returned >= 4, "cannot read the restored reparse point");
    std::uint32_t tag = 0;
    std::memcpy(&tag, buffer.data(), sizeof(tag));
    require(tag == 0xA0000003U,
            "restored reparse point is not a mount point (junction)");
#else
    require(std::filesystem::is_symlink(std::filesystem::symlink_status(link)),
            "restored junction is not a symlink on this platform");
    require(std::filesystem::read_symlink(link) == target,
            "restored junction points at the wrong target");
#endif
}


// ACL 重建：Windows 上把 $Secure:$SDS 解出的 DACL 写回导出文件；其他平台无法
// 用 POSIX 权限位等价表达，必须通过 metadata_note 明确报出来而不是静默丢弃。
void test_acl_restore(const std::filesystem::path& output_path) {
    FakeFsParser parser(false, false, false, true);
    const uife::VFS filesystem(parser);
    std::vector<std::string> notes;
    uife::ExtractOptions options;
    options.output_directory = output_path;
    options.restore_acl = true;
    options.metadata_note = [&notes](const std::string& path,
                                     const std::string& field,
                                     const std::string& reason) {
        notes.push_back(path + "|" + field + "|" + reason);
    };
    const auto result = uife::Extractor::restore(filesystem, options);
    require(result.size() == 2, "restore with ACL metadata lost files");
#ifdef _WIN32
    const auto file = output_path / "hello.txt";
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto status = GetNamedSecurityInfoW(file.c_str(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor);
    require(status == ERROR_SUCCESS && dacl != nullptr,
            "restored file has no DACL");
    auto found = false;
    for (WORD index = 0; index < dacl->AceCount; ++index) {
        LPVOID raw = nullptr;
        if (GetAce(dacl, index, &raw) == 0) {
            continue;
        }
        auto* header = static_cast<ACE_HEADER*>(raw);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            continue;
        }
        auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(raw);
        if (allowed->Mask != 0x001F01FFU) {
            continue;
        }
        char* sid_text = nullptr;
        if (ConvertSidToStringSidA(&allowed->SidStart, &sid_text) == 0) {
            continue;
        }
        found = std::string(sid_text) == "S-1-1-0";
        LocalFree(sid_text);
        if (found) {
            break;
        }
    }
    LocalFree(descriptor);
    require(found, "restored DACL lost the source ACE");
#else
    require(std::any_of(notes.begin(), notes.end(), [](const std::string& note) {
        return note.find("security_descriptor") != std::string::npos;
    }), "POSIX export did not report the unrestorable NTFS ACL");
#endif
}


// 一个 4 KiB 的稀疏文件：0..1K 与 2K..4K 有数据，中间 1K 是空洞。
// 解析器会记录是否被要求读取空洞——稀疏导出不应该去读那些零。
class SparseFsParser final : public uife::IFSParser {
public:
    void mount(std::shared_ptr<uife::IImageDriver>, std::uint64_t,
               std::uint64_t) override {}

    std::vector<uife::VFSNode> list_dir(const std::string& path) const override {
        if (path != "/") {
            throw std::runtime_error("unexpected test directory");
        }
        return {node()};
    }

    uife::VFSNode get_metadata(const std::string& path) const override {
        if (path != "/sparse.bin") {
            throw std::runtime_error("test path not found");
        }
        return node();
    }

    std::size_t read_file(const std::string& path, std::uint64_t offset,
                          std::span<std::byte> destination) const override {
        if (path != "/sparse.bin") {
            throw std::runtime_error("unexpected test path");
        }
        if (offset >= 1024 && offset < 2048) {
            hole_read_ = true;
        }
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(destination.size(), 4096 - offset));
        for (std::size_t index = 0; index < count; ++index) {
            const auto position = offset + index;
            destination[index] = static_cast<std::byte>(
                position < 1024 ? 'A' : (position < 2048 ? 0 : 'B'));
        }
        return count;
    }

    std::string_view filesystem_name() const noexcept override { return "test"; }

    [[nodiscard]] bool hole_read() const noexcept { return hole_read_; }

private:
    static uife::VFSNode node() {
        uife::VFSNode node;
        node.name = "sparse.bin";
        node.path = "/sparse.bin";
        node.type = uife::NodeType::regular_file;
        node.metadata.size = 4096;
        node.metadata.sparse = true;
        node.metadata.extents = {
            {0, 1024, true},
            {1024, 1024, false},
            {2048, 2048, true},
        };
        return node;
    }

    mutable bool hole_read_{};
};

// 稀疏文件导出：空洞不读也不写，但内容与哈希必须和"逐字节读出来的零"一致。
void test_sparse_restore(const std::filesystem::path& output_path) {
    SparseFsParser parser;
    const uife::VFS filesystem(parser);
    std::uint64_t completed = 0;
    std::uint64_t total = 0;
    uife::ExtractOptions options;
    options.output_directory = output_path;
    options.progress = [&](std::uint64_t done, std::uint64_t expected) {
        completed = done;
        total = expected;
    };
    const auto result = uife::Extractor::restore(filesystem, options);
    require(result.size() == 1 && completed == 4096 && total == 4096,
            "sparse export did not account for the hole in progress");
    require(!parser.hole_read(),
            "sparse export read the hole instead of skipping it");

    std::ifstream input(output_path / "sparse.bin", std::ios::binary);
    require(static_cast<bool>(input), "sparse export did not create the file");
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    require(content.size() == 4096, "sparse export has the wrong size");
    require(content.substr(0, 1024) == std::string(1024, 'A') &&
                content.substr(1024, 1024) == std::string(1024, '\0') &&
                content.substr(2048) == std::string(2048, 'B'),
            "sparse export produced the wrong byte layout");
    uife::Sha256 hash;
    hash.update(std::as_bytes(std::span(content.data(), content.size())));
    require(result[0].sha256 == uife::Sha256::to_hex(hash.finish()),
            "sparse export hash does not cover the hole as zeros");
}

void test_alternate_streams(const std::filesystem::path& output_path) {
    FakeFsParser parser(false, false, true);
    const uife::VFS filesystem(parser);
    std::uint64_t total_bytes = 0;
    uife::ExtractOptions options;
    options.output_directory = output_path;
    options.progress = [&total_bytes](std::uint64_t, std::uint64_t total) {
        total_bytes = total;
    };
    const auto result = uife::Extractor::restore(filesystem, options);
    require(result.size() == 4, "alternate data streams were not exported");
    require(total_bytes == 3 + 11 + 6 + 1,
            "progress total did not include alternate stream bytes");

    auto read_file = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        require(static_cast<bool>(input), "exported alternate stream file is missing");
        return std::string((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    };
    require(read_file(output_path / "hello.txt__ads__Zone.Identifier") == "stream",
            "alternate stream content differs from the source");
    require(read_file(output_path / "hello.txt__ads__evil%20name.txt") == "x",
            "unsafe alternate stream name was not encoded safely");
    bool rejected_unsafe_stream = false;
    try {
        std::array<std::byte, 4> buffer{};
        (void)filesystem.read_stream("/hello.txt", "../evil", 0, buffer);
    } catch (const std::invalid_argument&) {
        rejected_unsafe_stream = true;
    }
    require(rejected_unsafe_stream, "VFS accepted a path-like data stream name");

    bool saw_named_stream = false;
    for (const auto& file : result) {
        if (file.stream != "Zone.Identifier") {
            continue;
        }
        saw_named_stream = true;
        require(file.output == "hello.txt__ads__Zone.Identifier",
                "extracted stream did not report its output path");
        require(file.path == "/hello.txt", "extracted stream lost its source path");
        require(file.sha256 ==
            "dca83e717b1f64eb141057a7415a330ad1361f51703efa2e4776f40047898a04",
            "exported alternate stream SHA-256 differs from the source");
    }
    require(saw_named_stream, "extraction result omitted the named stream entry");
}

void test_extract_volume_root(const std::filesystem::path& output_path) {
    FakeFsParser parser;
    const uife::VFS filesystem(parser);
    uife::ExtractOptions options;
    options.output_directory = output_path;
    const auto result = uife::Extractor::extract(filesystem, "/", options);
    require(result.size() == 2, "extract / did not export the volume root");
    require(std::filesystem::exists(output_path / "hello.txt"),
            "extract / wrote outside the export root");
    require(std::filesystem::exists(output_path / "folder" / "nested.bin"),
            "extract / missed nested files");
}

class TemporaryTestDirectory {
public:
    TemporaryTestDirectory() {
        const auto root = std::filesystem::temp_directory_path();
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        for (std::uint64_t attempt = 0; attempt < 100; ++attempt) {
            path_ = root / ("uife-core-" + std::to_string(stamp) + "-" +
                            std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::filesystem::filesystem_error(
                    "failed to create UIFE core test directory", path_, error);
            }
        }
        throw std::runtime_error(
            "could not allocate a unique UIFE core test directory");
    }

    ~TemporaryTestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

} // namespace

// 合成 $SDS 数据流：一条完整条目 + 一条损坏条目，验证解码、对齐步进与容错。
// 手工构造：
//   条目 A @0x00: security_id=0x100，描述符 @0x20（owner=S-1-5-18 SYSTEM，
//                 group=S-1-5-32-544 Administrators，DACL 1 条 allow，SACL 1 条 audit）
//   条目 B @0x60: security_id=0x101，描述符 offset 指向流外（应终止且不影响 A）
void test_ntfs_security() {
    using uife::VFSAce;
    using uife::VFSSecurityDescriptor;
    using uife::ntfs_security::parse_sds;
    using uife::ntfs_security::sid_to_string;

    // SID: S-1-5-18（本地 SYSTEM）。identifier authority 是 6 字节大端，
    // 值 5 位于第 7 字节。
    std::vector<std::byte> system_sid(12);
    system_sid[0] = std::byte{1};
    system_sid[1] = std::byte{1};
    system_sid[7] = std::byte{5};
    write_le32(system_sid, 8, 18);
    // SID: S-1-5-32-544（BUILTIN\Administrators）
    std::vector<std::byte> admins_sid(16);
    admins_sid[0] = std::byte{1};
    admins_sid[1] = std::byte{2};
    admins_sid[7] = std::byte{5};
    write_le32(admins_sid, 8, 32);
    write_le32(admins_sid, 12, 544);
    // SID: S-1-1-0（Everyone）
    std::vector<std::byte> everyone_sid(12);
    everyone_sid[0] = std::byte{1};
    everyone_sid[1] = std::byte{1};
    everyone_sid[7] = std::byte{1};
    write_le32(everyone_sid, 8, 0);

    require(sid_to_string(system_sid) == "S-1-5-18",
            "SID decoding produced the wrong S-1-5-18 text form");
    require(sid_to_string(admins_sid) == "S-1-5-32-544",
            "SID decoding produced the wrong S-1-5-32-544 text form");
    require(sid_to_string(std::span<const std::byte>(system_sid).first(7)) ==
                std::string(),
            "SID decoding accepted a truncated buffer");

    // DACL: revision=1 count=1，一条 allow ACE（FILE_GENERIC_READ 近似值）
    std::vector<std::byte> dacl(8 + 8 + 12);
    dacl[0] = std::byte{2};  // ACL_REVISION_DS
    write_le16(dacl, 2, static_cast<std::uint16_t>(dacl.size()));
    write_le16(dacl, 4, 1);
    std::size_t ace = 8;
    dacl[ace] = std::byte{0};        // ACCESS_ALLOWED_ACE_TYPE
    dacl[ace + 1] = std::byte{0};    // 非 inherited
    write_le16(dacl, ace + 2, 8 + 12);
    write_le32(dacl, ace + 4, 0x00120089U);
    std::copy(system_sid.begin(), system_sid.end(), dacl.begin() + static_cast<long>(ace + 8));

    // SACL: 一条 audit ACE（inherited）
    std::vector<std::byte> sacl(8 + 8 + 12);
    sacl[0] = std::byte{2};
    write_le16(sacl, 2, static_cast<std::uint16_t>(sacl.size()));
    write_le16(sacl, 4, 1);
    ace = 8;
    sacl[ace] = std::byte{2};              // SYSTEM_AUDIT_ACE_TYPE
    sacl[ace + 1] = std::byte{0x10};       // INHERITED_ACE
    write_le16(sacl, ace + 2, 8 + 12);
    write_le32(sacl, ace + 4, 0x00000116U);
    std::copy(everyone_sid.begin(), everyone_sid.end(), sacl.begin() + static_cast<long>(ace + 8));

    // 自相对 SECURITY_DESCRIPTOR：owner@20 group@32 dacl@44 sacl@56+16=...
    const std::size_t owner_offset = 20;
    const std::size_t group_offset = owner_offset + system_sid.size();   // 32
    const std::size_t dacl_offset = group_offset + admins_sid.size();    // 48
    const std::size_t sacl_offset = dacl_offset + dacl.size();
    std::vector<std::byte> descriptor(sacl_offset + sacl.size());
    descriptor[0] = std::byte{1};  // revision 1
    write_le16(descriptor, 2, 0x0004U | 0x0010U | 0x8000U);  // DACL|SACL|self-relative
    write_le32(descriptor, 4, static_cast<std::uint32_t>(owner_offset));
    write_le32(descriptor, 8, static_cast<std::uint32_t>(group_offset));
    write_le32(descriptor, 12, static_cast<std::uint32_t>(sacl_offset));
    write_le32(descriptor, 16, static_cast<std::uint32_t>(dacl_offset));
    std::copy(system_sid.begin(), system_sid.end(),
              descriptor.begin() + static_cast<long>(owner_offset));
    std::copy(admins_sid.begin(), admins_sid.end(),
              descriptor.begin() + static_cast<long>(group_offset));
    std::copy(dacl.begin(), dacl.end(), descriptor.begin() + static_cast<long>(dacl_offset));
    std::copy(sacl.begin(), sacl.end(), descriptor.begin() + static_cast<long>(sacl_offset));

    // $SDS 条目（Windows 实测布局）：offset 字段 = 条目自身的流内偏移，
    // length = 20 字节头 + 描述符的总长，条目按 16 字节对齐。
    const std::size_t descriptor_length = descriptor.size();
    const std::size_t entry_stride =
        (20 + descriptor_length + 15U) & ~static_cast<std::size_t>(15U);
    std::vector<std::byte> sds(entry_stride + 20);
    write_le32(sds, 0, 0x11223344U);       // hash
    write_le32(sds, 4, 0x100U);            // security_id
    write_le64(sds, 8, 0);                 // 条目自身偏移
    write_le32(sds, 16,
        static_cast<std::uint32_t>(20 + descriptor_length));
    std::copy(descriptor.begin(), descriptor.end(), sds.begin() + 20);

    const auto index = parse_sds(sds);
    if (index.size() != 1) {
        // 诊断输出：失败时把实际数值与头部字节打出来。minGW 与 MSVC 的差异
        // 往往就在这里暴露（同样的输入应该得到同样的条目数）。
        const auto byte_at = [&sds](std::size_t i) {
            return static_cast<int>(std::to_integer<unsigned char>(sds[i]));
        };
        std::cerr << "\n[debug] $SDS entries=" << index.size()
                  << " sds_size=" << sds.size()
                  << " span=" << std::span<const std::byte>(sds).size()
                  << " sizeof(size_t)=" << sizeof(std::size_t)
                  << " stride=" << entry_stride
                  << " descriptor_length=" << descriptor_length << "\n[debug] header:";
        for (std::size_t i = 0; i < 20; ++i) {
            std::cerr << ' ' << std::hex << byte_at(i) << std::dec;
        }
        std::cerr << "\n[debug] descriptor[0..7] (revision/control/owner_off):";
        for (std::size_t i = 20; i < 28; ++i) {
            std::cerr << ' ' << std::hex << byte_at(i) << std::dec;
        }
        std::cerr << "\n[debug] tail 20 bytes:";
        for (std::size_t i = entry_stride; i < sds.size(); ++i) {
            std::cerr << ' ' << std::hex << byte_at(i) << std::dec;
        }
        // 绕过条目循环，直接把描述符喂给解码器：用于区分"循环没进去/布局判定
        // 失败"与"描述符本身解不出来"。
        const auto probe = uife::ntfs_security::parse_security_descriptor(
            std::span<const std::byte>(sds).subspan(20, descriptor_length));
        std::cerr << "\n[debug] direct descriptor valid=" << probe.valid
                  << " dacl=" << probe.dacl.size()
                  << " sacl=" << probe.sacl.size()
                  << " owner=" << probe.owner_sid
                  << " group=" << probe.group_sid;
        std::cerr << std::endl;
    }
    require(index.size() == 1, "single-entry $SDS stream produced wrong entry count");
    const auto found = index.find(0x100);
    require(found != index.end(), "$SDS entry with security_id 0x100 was not indexed");
    const auto& security = found->second;
    require(security.valid, "well-formed security descriptor was marked invalid");
    require(security.owner_sid == "S-1-5-18", "owner SID mismatch");
    require(security.group_sid == "S-1-5-32-544", "group SID mismatch");
    require(security.dacl_present && security.dacl.size() == 1,
            "DACL was not decoded with exactly one ACE");
    require(security.dacl[0].type == "allow" && !security.dacl[0].inherited &&
                security.dacl[0].mask == 0x00120089U &&
                security.dacl[0].sid == "S-1-5-18",
            "DACL ACE fields mismatch");
    require(security.sacl_present && security.sacl.size() == 1 &&
                security.sacl[0].type == "audit" && security.sacl[0].inherited &&
                security.sacl[0].sid == "S-1-1-0",
            "SACL ACE fields mismatch");

    // 损坏条目：length 合法但 revision 非 1（两种布局都不成立）→ 终止遍历，
    // 不影响已解码的条目。
    // 注意缓冲区要显式留出第二个条目头 + 描述符首字节的空间：sds 本身是按
    // "一个条目 + 20 字节尾"精确构造的，直接在上面写 entry_stride + 20 就会越界
    // （linux-sanitizers 上的 ASan 报的正是这一处）。
    std::vector<std::byte> corrupted(sds);
    corrupted.resize(entry_stride + 20 + 64, std::byte{0});
    write_le64(corrupted, entry_stride + 8, entry_stride);
    write_le32(corrupted, entry_stride + 4, 0x101U);
    write_le32(corrupted, entry_stride + 16, 60);
    corrupted[entry_stride + 20] = std::byte{2};   // 非 1 的 revision
    const auto partial = parse_sds(corrupted);
    require(partial.size() == 1 && partial.count(0x100) == 1,
            "corrupted trailing $SDS entry must not discard earlier entries");

    // revision 非 1 的描述符必须判为无效。
    std::vector<std::byte> bad_revision(sds);
    bad_revision[20] = std::byte{2};
    const auto invalid = parse_sds(bad_revision);
    require(invalid.empty(), "descriptor with unknown revision must be rejected");}

#ifdef UIFE_WITH_LIBTSK
namespace fat16_fixture {

constexpr std::uint32_t kSectorSize = 512;
constexpr std::uint16_t kRootEntries = 512;
constexpr std::uint16_t kFatSectors = 47;
constexpr std::uint32_t kReservedSectors = 1;
constexpr std::uint16_t kTotalSectors = 12032;   // 簇 11905 个，落在 FAT16 区间

// 构造一个只有 HELLO.TXT 的最小 FAT16 镜像。
// total_sectors 写进 BPB，与镜像实际长度解耦：真实证据里分区长度常比 BPB 声明的
// 总扇区数少一两个扇区，这里用它来复现那种几何偏差。
std::vector<std::byte> build(std::uint16_t total_sectors) {
    const std::uint32_t root_sectors = kRootEntries * 32U / kSectorSize;
    const std::uint32_t data_start =
        kReservedSectors + 2U * kFatSectors + root_sectors;
    std::vector<std::byte> image(std::size_t{kTotalSectors} * kSectorSize,
                                 std::byte{0});

    const std::array<std::byte, 3> jump{std::byte{0xeb}, std::byte{0x3c},
                                        std::byte{0x90}};
    std::copy(jump.begin(), jump.end(), image.begin());
    std::copy_n(reinterpret_cast<const std::byte*>("MSDOS5.0"), 8,
                image.begin() + 3);
    write_le16(image, 11, static_cast<std::uint16_t>(kSectorSize));
    image[13] = std::byte{1};   // 每簇 1 个扇区
    write_le16(image, 14, static_cast<std::uint16_t>(kReservedSectors));
    image[16] = std::byte{2};   // 两份 FAT
    write_le16(image, 17, kRootEntries);
    write_le16(image, 19, total_sectors);
    image[21] = std::byte{0xf8};
    write_le16(image, 22, kFatSectors);
    write_le16(image, 24, 32);  // 每磁道扇区
    write_le16(image, 26, 2);   // 磁头
    write_le32(image, 28, 0);   // 隐藏扇区
    write_le32(image, 32, 0);   // 32 位总扇区数
    image[36] = std::byte{0x80};
    image[38] = std::byte{0x29};
    write_le32(image, 39, 0x12345678U);
    std::copy_n(reinterpret_cast<const std::byte*>("NO NAME    "), 11,
                image.begin() + 43);
    std::copy_n(reinterpret_cast<const std::byte*>("FAT16   "), 8,
                image.begin() + 54);
    image[510] = std::byte{0x55};
    image[511] = std::byte{0xaa};

    // FAT[0]/FAT[1] 保留表项，FAT[2]=EOC 表示 HELLO.TXT 占用簇 2。
    const std::size_t fat_offset = std::size_t{kReservedSectors} * kSectorSize;
    write_le16(image, fat_offset, 0xfff8);
    write_le16(image, fat_offset + 2, 0xffff);
    write_le16(image, fat_offset + 4, 0xffff);
    std::copy_n(image.begin() + static_cast<std::ptrdiff_t>(fat_offset),
                std::size_t{kFatSectors} * kSectorSize,
                image.begin() + static_cast<std::ptrdiff_t>(
                    std::size_t{kReservedSectors + kFatSectors} * kSectorSize));

    const std::size_t root_offset =
        std::size_t{kReservedSectors + 2U * kFatSectors} * kSectorSize;
    std::copy_n(reinterpret_cast<const std::byte*>("HELLO   TXT"), 11,
                image.begin() + static_cast<std::ptrdiff_t>(root_offset));
    image[root_offset + 11] = std::byte{0x20};   // archive
    write_le16(image, root_offset + 26, 2);      // 起始簇
    write_le32(image, root_offset + 28, 5);      // 文件大小

    std::copy_n(reinterpret_cast<const std::byte*>("hello"), 5,
                image.begin() + static_cast<std::ptrdiff_t>(
                    std::size_t{data_start} * kSectorSize));
    return image;
}

} // namespace fat16_fixture
#endif

// 容量校验必须容忍真实镜像里“分区比 BPB 少一两个扇区”的几何偏差，但仍要拒绝
// 明显不可信的声明（多半是伪文件系统或偏移错误）。
static void test_fat_geometry_tolerance(const std::filesystem::path& image_path) {
#ifdef UIFE_WITH_LIBTSK
    const auto write_fixture = [&image_path](std::uint16_t total_sectors) {
        const auto bytes = fat16_fixture::build(total_sectors);
        std::ofstream output(image_path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        require(static_cast<bool>(output), "failed to write FAT16 geometry fixture");
    };
    const auto mount = [&image_path] {
        auto driver = uife::DriverFactory::instance().create(image_path);
        std::shared_ptr<uife::IImageDriver> image(std::move(driver));
        auto parser = std::make_unique<uife::TskFsParser>("FAT");
        parser->mount(image, 0, image->get_size());
        return parser;
    };
    const auto require_browsable = [](const uife::IFSParser& parser,
                                      const char* message) {
        const auto entries = parser.list_dir("/");
        require(std::any_of(entries.begin(), entries.end(),
                            [](const uife::VFSNode& node) {
                                return node.name == "HELLO.TXT";
                            }),
                message);
    };

    write_fixture(fat16_fixture::kTotalSectors);
    require_browsable(*mount(), "FAT16 fixture did not expose its file");

    // 声明比镜像多一个扇区：真实证据镜像的常见形态，必须仍可浏览。
    write_fixture(static_cast<std::uint16_t>(
        fat16_fixture::kTotalSectors + 1));
    require_browsable(*mount(),
        "FAT16 with a one-sector overshoot must stay browsable");

    // 多出 200 个扇区已超出扇区/簇级对齐误差，说明几何声明不可信。
    write_fixture(static_cast<std::uint16_t>(
        fat16_fixture::kTotalSectors + 200));
    std::string rejection;
    try {
        mount();
    } catch (const std::exception& error) {
        rejection = error.what();
    }
    require(rejection.find("容量超出") != std::string::npos,
            "FAT parser did not reject an implausible filesystem geometry");
#else
    (void)image_path;
#endif
}

static void run_all_tests() {
    const TemporaryTestDirectory test_directory;
    const auto& directory = test_directory.path();
    const auto image = directory / "uife-test-image.raw";
    const auto container = directory / "uife-test-container.qcow2";
    const auto vhdx_image = directory / "uife-test.vhdx";
    const auto qed_image = directory / "uife-test.qed";
    const auto vdi_image = directory / "uife-test.vdi";
    const auto vmdk_image = directory / "uife-test-sparse.vmdk";
    const auto gpt_image = directory / "uife-test-gpt.raw";
    const auto ebr_image = directory / "uife-test-ebr.raw";
    const auto output = directory / "uife-test-output";
    const auto unsafe_output = directory / "uife-test-unsafe-output";
    const auto cue_directory = directory / "uife-test-cue-bin";
    try {
        // 逐用例打印进度到 stderr（不带缓冲）：若某个用例直接使进程崩溃
        // （MSVC 下未捕获异常会 abort 成 0xC0000409），最后一行就指出位置。
        const auto run = [](const char* name, auto&& fn) {
            std::cerr << "[ test ] " << name << std::endl;
            fn();
        };
        run("sha256", [] { test_sha256(); });
        run("ntfs_security", [] { test_ntfs_security(); });
        run("raw_and_partitions", [&] { test_raw_and_partitions(image); });
        run("fat_geometry_tolerance", [&] {
            test_fat_geometry_tolerance(directory / "uife-test-fat16.raw"); });
        run("container_recognition", [&] { test_container_recognition(container); });
        run("cue_bin_driver", [&] { test_cue_bin_driver(cue_directory); });
        run("qcow2_driver", [&] { test_qcow2_driver(container); });
        run("vhdx_driver", [&] { test_vhdx_driver(vhdx_image); });
        run("qed_driver", [&] { test_qed_driver(qed_image); });
        run("vdi_driver", [&] { test_vdi_driver(vdi_image); });
        run("vmdk_sparse_driver", [&] { test_vmdk_sparse_driver(vmdk_image); });
        run("gpt", [&] { test_gpt(gpt_image); });
        run("mbr_logical_partitions", [&] { test_mbr_logical_partitions(ebr_image); });
        std::filesystem::remove_all(output);
        std::filesystem::remove_all(unsafe_output);
        run("restore", [&] { test_restore(output, unsafe_output); });
        const auto junction_output = directory / "uife-test-junction-output";
        std::filesystem::remove_all(junction_output);
        run("reparse_point", [&] { test_reparse_point_restore(junction_output); });
        std::filesystem::remove_all(junction_output);
        const auto acl_output = directory / "uife-test-acl-output";
        std::filesystem::remove_all(acl_output);
        run("acl_restore", [&] { test_acl_restore(acl_output); });
        std::filesystem::remove_all(acl_output);
        const auto sparse_output = directory / "uife-test-sparse-output";
        std::filesystem::remove_all(sparse_output);
        run("sparse_restore", [&] { test_sparse_restore(sparse_output); });
        std::filesystem::remove_all(sparse_output);
        const auto stream_output = directory / "uife-test-stream-output";
        std::filesystem::remove_all(stream_output);
        run("alternate_streams", [&] { test_alternate_streams(stream_output); });
        std::filesystem::remove_all(stream_output);
        const auto root_output = directory / "uife-test-root-output";
        std::filesystem::remove_all(root_output);
        run("extract_volume_root", [&] { test_extract_volume_root(root_output); });
        std::filesystem::remove_all(root_output);
        // 容器类 fixture 的只读不变性与截断输入检查（与集成测试共用同一套断言）。
        run("read_only_sweep", [&] {
            const std::vector<uife_test::FixtureTarget> targets{
                {image, "raw"},
                {container, "qcow2"},
                {vhdx_image, "vhdx"},
                {qed_image, "qed"},
                {vdi_image, "vdi"},
                {vmdk_image, "vmdk"},
                {gpt_image, "gpt"},
                {ebr_image, "ebr"},
            };
            uife_test::check_source_immutability(
                directory, directory / "uife-immutability-output", targets);
            uife_test::check_truncated_inputs(directory, targets);
            uife_test::check_malformed_inputs(directory, targets);
        });
        std::filesystem::remove(image);
        auto cue_descriptor = image;
        cue_descriptor.replace_extension(".cue");
        std::filesystem::remove(cue_descriptor);
        std::filesystem::remove(container);
        std::filesystem::remove(vhdx_image);
        std::filesystem::remove(qed_image);
        std::filesystem::remove(vdi_image);
        std::filesystem::remove(vmdk_image);
        std::filesystem::remove(gpt_image);
        std::filesystem::remove(ebr_image);
        std::filesystem::remove_all(output);
        std::filesystem::remove_all(unsafe_output);
        std::filesystem::remove_all(cue_directory);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(image, ignored);
        auto cue_descriptor = image;
        cue_descriptor.replace_extension(".cue");
        std::filesystem::remove(cue_descriptor, ignored);
        std::filesystem::remove(container, ignored);
        std::filesystem::remove(vhdx_image, ignored);
        std::filesystem::remove(qed_image, ignored);
        std::filesystem::remove(vdi_image, ignored);
        std::filesystem::remove(vmdk_image, ignored);
        std::filesystem::remove(gpt_image, ignored);
        std::filesystem::remove(ebr_image, ignored);
        std::filesystem::remove_all(output, ignored);
        std::filesystem::remove_all(cue_directory, ignored);
        throw;
    }
    std::cout << "UIFE core tests passed\n";
}

int main() {
    // 未捕获异常在 MSVC 下会直接 abort（退出码 0xC0000409），CTest 只能看到
    // "Exception" 而无任何原因。这里统一转成可读信息 + 常规退出码 1。
    try {
        run_all_tests();
    } catch (const std::exception& error) {
        std::cerr << "\nTEST FAILURE: " << error.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\nTEST FAILURE: unknown exception" << std::endl;
        return 1;
    }
    return 0;
}
