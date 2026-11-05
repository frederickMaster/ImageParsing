#include "uife/drivers/fs/Iso9660.h"
#include "uife/DriverFactory.h"
#include "uife/Extractor.h"
#include "uife/FsParserFactory.h"
#include "uife/PartitionScanner.h"
#include "uife/utils/Sha256.h"
#include "uife/vfs/VFS.h"

#include "test_support.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef UIFE_WITH_LIBWIM
#include <wimlib.h>
#endif

#ifdef UIFE_WITH_ZLIB
#include <zlib.h>
#endif

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void remove_test_output(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        for (std::filesystem::recursive_directory_iterator item(path, error), end;
             !error && item != end; item.increment(error)) {
            std::filesystem::permissions(
                item->path(), std::filesystem::perms::owner_all,
                std::filesystem::perm_options::add, error);
            error.clear();
        }
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, error);
        error.clear();
    }
    std::filesystem::remove_all(path, error);
    if (error) {
        throw std::filesystem::filesystem_error(
            "failed to remove UIFE test output", path, error);
    }
}

class TemporaryTestDirectory {
public:
    TemporaryTestDirectory() {
        const auto root = std::filesystem::temp_directory_path();
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        for (std::uint64_t attempt = 0; attempt < 100; ++attempt) {
            path_ = root / ("uife-integration-" +
                            std::to_string(stamp) + "-" +
                            std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::filesystem::filesystem_error(
                    "failed to create UIFE integration test directory",
                    path_, error);
            }
        }
        throw std::runtime_error(
            "could not allocate a unique UIFE integration test directory");
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

void write_le16(std::vector<std::byte>& bytes, std::size_t offset,
                std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void write_le32(std::vector<std::byte>& bytes, std::size_t offset,
                std::uint32_t value) {
    for (unsigned int index = 0; index < 4; ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

void write_le64(std::vector<std::byte>& bytes, std::size_t offset,
                std::uint64_t value) {
    for (unsigned int index = 0; index < 8; ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

std::uint32_t crc32c(std::span<const std::byte> bytes,
                     std::size_t checksum_offset) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto value = index >= checksum_offset &&
                                   index < checksum_offset + sizeof(std::uint32_t)
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

void write_dual32(char* output, std::uint32_t value) {
    for (unsigned int index = 0; index < 4; ++index) {
        output[index] = static_cast<char>((value >> (index * 8U)) & 0xffU);
        output[index + 4] = static_cast<char>((value >> ((3U - index) * 8U)) & 0xffU);
    }
}

void write_dual16(char* output, std::uint16_t value) {
    output[0] = static_cast<char>(value & 0xffU);
    output[1] = static_cast<char>((value >> 8U) & 0xffU);
    output[2] = static_cast<char>((value >> 8U) & 0xffU);
    output[3] = static_cast<char>(value & 0xffU);
}

std::vector<char> read_fixture(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "failed to open ISO fixture");
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

void write_fixture(const std::filesystem::path& path,
                   const std::vector<char>& bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "failed to write ISO integrity fixture");
}

std::string hash_bytes(const std::vector<char>& bytes) {
    uife::Sha256 hash;
    hash.update(std::as_bytes(std::span<const char>(bytes)));
    return uife::Sha256::to_hex(hash.finish());
}

void append_le16(std::vector<std::byte>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::byte>(value & 0xffU));
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void append_le32(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (unsigned int index = 0; index < 4; ++index) {
        bytes.push_back(
            static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
    }
}

void append_le64(std::vector<std::byte>& bytes, std::uint64_t value) {
    for (unsigned int index = 0; index < 8; ++index) {
        bytes.push_back(
            static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
    }
}

void write_be32(std::vector<std::byte>& bytes, std::size_t offset,
                std::uint32_t value) {
    for (unsigned int index = 0; index < 4; ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> ((3U - index) * 8U)) & 0xffU);
    }
}

void write_be64(std::vector<std::byte>& bytes, std::size_t offset,
                std::uint64_t value) {
    for (unsigned int index = 0; index < 8; ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> ((7U - index) * 8U)) & 0xffU);
    }
}

std::string base64_encode(std::span<const std::byte> bytes) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((bytes.size() + 2U) / 3U) * 4U);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const auto first = std::to_integer<unsigned char>(bytes[index]);
        const auto second = index + 1 < bytes.size()
            ? std::to_integer<unsigned char>(bytes[index + 1]) : 0U;
        const auto third = index + 2 < bytes.size()
            ? std::to_integer<unsigned char>(bytes[index + 2]) : 0U;
        result.push_back(alphabet[first >> 2U]);
        result.push_back(alphabet[((first & 0x03U) << 4U) | (second >> 4U)]);
        result.push_back(index + 1 < bytes.size()
            ? alphabet[((second & 0x0fU) << 2U) | (third >> 6U)] : '=');
        result.push_back(index + 2 < bytes.size()
            ? alphabet[third & 0x3fU] : '=');
    }
    return result;
}

std::vector<std::byte> make_dmg_fixture(
    bool compress_raw_run = false,
    std::uint32_t raw_run_type = 0,
    bool binary_plist = false) {
    constexpr std::uint64_t sectors = 2;
    std::vector<std::byte> raw_payload(512);
    for (std::size_t index = 0; index < raw_payload.size(); ++index) {
        raw_payload[index] =
            static_cast<std::byte>((index * 37U + 11U) & 0xffU);
    }
    auto stored_payload = raw_payload;
    if (compress_raw_run) {
#ifdef UIFE_WITH_ZLIB
        uLongf compressed_size = compressBound(
            static_cast<uLong>(raw_payload.size()));
        stored_payload.resize(static_cast<std::size_t>(compressed_size));
        const auto result = compress2(
            reinterpret_cast<Bytef*>(stored_payload.data()), &compressed_size,
            reinterpret_cast<const Bytef*>(raw_payload.data()),
            static_cast<uLong>(raw_payload.size()), Z_DEFAULT_COMPRESSION);
        require(result == Z_OK, "failed to compress DMG zlib fixture");
        stored_payload.resize(static_cast<std::size_t>(compressed_size));
#else
        throw std::runtime_error("zlib test support was not built");
#endif
    }
    std::vector<std::byte> table(324, std::byte{0});
    write_be32(table, 0, 0x6d697368U);
    write_be32(table, 4, 1);
    write_be64(table, 8, 0);
    write_be64(table, 16, sectors);
    write_be64(table, 24, 0);
    write_be32(table, 200, 3);
    write_be32(table, 204, raw_run_type != 0
        ? raw_run_type : compress_raw_run ? 0x80000005U : 1U);
    write_be64(table, 212, 0);
    write_be64(table, 220, 1);
    write_be64(table, 228, 0);
    write_be64(table, 236, stored_payload.size());
    write_be32(table, 244, 0);
    write_be64(table, 252, 1);
    write_be64(table, 260, 1);
    write_be64(table, 268, 0);
    write_be64(table, 276, 0);
    write_be32(table, 284, 0xffffffffU);

    std::vector<std::byte> plist;
    if (binary_plist) {
        plist = {
            std::byte{'b'}, std::byte{'p'}, std::byte{'l'}, std::byte{'i'},
            std::byte{'s'}, std::byte{'t'}, std::byte{'0'}, std::byte{'0'}};
        std::vector<std::vector<std::byte>> objects(8);
        objects[0] = {std::byte{0xd1}, std::byte{1}, std::byte{2}};
        const auto append_ascii = [](std::string_view text) {
            std::vector<std::byte> object;
            object.push_back(static_cast<std::byte>(0x50U + text.size()));
            for (const auto character : text) {
                object.push_back(static_cast<std::byte>(
                    static_cast<unsigned char>(character)));
            }
            return object;
        };
        objects[1] = append_ascii("resource-fork");
        objects[2] = {std::byte{0xd1}, std::byte{3}, std::byte{4}};
        objects[3] = append_ascii("blkx");
        objects[4] = {std::byte{0xa1}, std::byte{5}};
        objects[5] = {std::byte{0xd1}, std::byte{6}, std::byte{7}};
        objects[6] = append_ascii("Data");
        objects[7] = {std::byte{0x4f}, std::byte{0x11},
                      static_cast<std::byte>((table.size() >> 8U) & 0xffU),
                      static_cast<std::byte>(table.size() & 0xffU)};
        objects[7].insert(objects[7].end(), table.begin(), table.end());

        std::vector<std::uint16_t> offsets;
        offsets.reserve(objects.size());
        for (const auto& object : objects) {
            require(plist.size() <= std::numeric_limits<std::uint16_t>::max(),
                    "binary DMG plist fixture exceeds offset capacity");
            offsets.push_back(static_cast<std::uint16_t>(plist.size()));
            plist.insert(plist.end(), object.begin(), object.end());
        }
        require(plist.size() <= std::numeric_limits<std::uint16_t>::max(),
                "binary DMG plist fixture offset table is too large");
        const auto offset_table = static_cast<std::uint64_t>(plist.size());
        for (const auto offset : offsets) {
            plist.push_back(static_cast<std::byte>((offset >> 8U) & 0xffU));
            plist.push_back(static_cast<std::byte>(offset & 0xffU));
        }
        plist.resize(plist.size() + 6U, std::byte{0});
        plist.push_back(std::byte{2});
        plist.push_back(std::byte{1});
        const auto append_be64 = [&plist](std::uint64_t value) {
            for (int shift = 56; shift >= 0; shift -= 8) {
                plist.push_back(static_cast<std::byte>(
                    (value >> static_cast<unsigned int>(shift)) & 0xffU));
            }
        };
        append_be64(objects.size());
        append_be64(0);
        append_be64(offset_table);
    } else {
        const auto encoded_table = base64_encode(table);
        const auto xml = std::string(
            "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
            "<key>blkx</key><array><dict><key>Data</key><data>") +
            encoded_table +
            "</data></dict></array></dict></plist>";
        plist.reserve(xml.size());
        for (const auto character : xml) {
            plist.push_back(static_cast<std::byte>(
                static_cast<unsigned char>(character)));
        }
    }

    std::vector<std::byte> image(
        stored_payload.size() + plist.size() + 512, std::byte{0});
    std::copy(stored_payload.begin(), stored_payload.end(), image.begin());
    std::copy(plist.begin(), plist.end(),
              image.begin() + static_cast<std::ptrdiff_t>(stored_payload.size()));
    const auto footer_offset = stored_payload.size() + plist.size();
    write_be32(image, footer_offset, 0x6b6f6c79U);
    write_be32(image, footer_offset + 4, 4);
    write_be32(image, footer_offset + 8, 512);
    write_be32(image, footer_offset + 56, 1);
    write_be32(image, footer_offset + 60, 1);
    write_be64(image, footer_offset + 24, 0);
    write_be64(image, footer_offset + 32, stored_payload.size());
    write_be64(image, footer_offset + 216, stored_payload.size());
    write_be64(image, footer_offset + 224, plist.size());
    write_be64(image, footer_offset + 492, sectors);
    return image;
}

std::vector<std::byte> make_dmg_raw_image(
    std::span<const std::byte> logical_image) {
    require(!logical_image.empty() && logical_image.size() % 512U == 0,
            "DMG raw fixture requires whole 512-byte sectors");
    const auto sectors = logical_image.size() / 512U;
    std::vector<std::byte> table(284, std::byte{0});
    write_be32(table, 0, 0x6d697368U);
    write_be32(table, 4, 1);
    write_be64(table, 8, 0);
    write_be64(table, 16, sectors);
    write_be64(table, 24, 0);
    write_be32(table, 200, 2);
    write_be32(table, 204, 1);
    write_be64(table, 212, 0);
    write_be64(table, 220, sectors);
    write_be64(table, 228, 0);
    write_be64(table, 236, logical_image.size());
    write_be32(table, 244, 0xffffffffU);

    const auto encoded_table = base64_encode(table);
    const auto xml = std::string(
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
        "<key>blkx</key><array><dict><key>Data</key><data>") +
        encoded_table +
        "</data></dict></array></dict></plist>";

    std::vector<std::byte> image(
        logical_image.size() + xml.size() + 512U, std::byte{0});
    std::copy(logical_image.begin(), logical_image.end(), image.begin());
    std::copy(xml.begin(), xml.end(),
              reinterpret_cast<char*>(image.data() + logical_image.size()));
    const auto footer_offset = logical_image.size() + xml.size();
    write_be32(image, footer_offset, 0x6b6f6c79U);
    write_be32(image, footer_offset + 4, 4);
    write_be32(image, footer_offset + 8, 512);
    write_be32(image, footer_offset + 56, 1);
    write_be32(image, footer_offset + 60, 1);
    write_be64(image, footer_offset + 32, logical_image.size());
    write_be64(image, footer_offset + 216, logical_image.size());
    write_be64(image, footer_offset + 224, xml.size());
    write_be64(image, footer_offset + 492, sectors);
    return image;
}

std::vector<std::byte> make_gho_fixture(
    std::span<const std::byte> partition_data,
    std::uint8_t compression = 0,
    bool store_zlib_blocks_uncompressed = false,
    bool include_track0_mbr = false,
    bool zero_length_block_terminator = false,
    bool include_continuation = false,
    std::span<const std::byte> fast_lz_block = {}) {
    require(!partition_data.empty(),
            "GHO fixture requires non-empty partition data");
    std::vector<std::byte> image(512, std::byte{0});
    write_le16(image, 0, 0xeffe);
    image[2] = std::byte{1};
    image[3] = static_cast<std::byte>(compression);
    const auto append_record = [&image](std::uint32_t type,
                                        std::uint16_t body_size) {
        const auto offset = image.size();
        image.resize(offset + 10U + body_size, std::byte{0});
        write_le32(image, offset, type);
        write_le32(image, offset + 4U, 0x012f18d8U);
        write_le16(image, offset + 8U, body_size);
        return offset + 10U;
    };
    if (include_track0_mbr) {
        require(partition_data.size() % 512U == 0 &&
                    partition_data.size() / 512U <=
                        std::numeric_limits<std::uint32_t>::max(),
                "GHO MBR fixture partition geometry is invalid");
        const auto track0_body = append_record(0x0006U, 518);
        image[track0_body] = std::byte{6};
        image[track0_body + 1U] = std::byte{63};
        const auto mbr = track0_body + 6U;
        image[mbr + 510U] = std::byte{0x55};
        image[mbr + 511U] = std::byte{0xaa};
        image[mbr + 446U + 4U] = std::byte{0x83};
        write_le32(image, mbr + 446U + 8U, 1);
        write_le32(image, mbr + 446U + 12U,
                   static_cast<std::uint32_t>(partition_data.size() / 512U));
    }
    append_record(0x0603U, 20);
    const auto partition_header_offset = image.size();
    image.resize(partition_header_offset + 512U, std::byte{0});
    write_le16(image, partition_header_offset, 0xeffe);
    image[partition_header_offset + 2U] = std::byte{2};
    image[partition_header_offset + 3U] = static_cast<std::byte>(compression);
    constexpr std::size_t block_size = 32U * 1024U;
    for (std::size_t offset = 0; offset < partition_data.size();) {
        const auto length = std::min(block_size, partition_data.size() - offset);
        if (compression == 0) {
            append_le16(image, static_cast<std::uint16_t>(length + 2U));
            image.insert(
                image.end(),
                partition_data.begin() + static_cast<std::ptrdiff_t>(offset),
                partition_data.begin() +
                    static_cast<std::ptrdiff_t>(offset + length));
        } else if (compression == 2) {
            std::vector<std::byte> encoded;
            if (!fast_lz_block.empty()) {
                require(offset == 0 && length == partition_data.size(),
                        "custom Fast LZ fixture must contain one block");
                encoded.assign(fast_lz_block.begin(), fast_lz_block.end());
            } else {
                encoded.assign(4U, std::byte{0});
                encoded[0] = std::byte{1};
                encoded.insert(
                    encoded.end(),
                    partition_data.begin() + static_cast<std::ptrdiff_t>(offset),
                    partition_data.begin() +
                        static_cast<std::ptrdiff_t>(offset + length));
            }
            require(encoded.size() + 2U <=
                        std::numeric_limits<std::uint16_t>::max(),
                    "Fast LZ fixture block is too large");
            append_le16(image,
                        static_cast<std::uint16_t>(encoded.size() + 2U));
            image.insert(image.end(), encoded.begin(), encoded.end());
        } else {
#ifdef UIFE_WITH_ZLIB
            std::vector<std::byte> encoded(4U, std::byte{0});
            if (store_zlib_blocks_uncompressed) {
                encoded[0] = std::byte{1};
                encoded.insert(
                    encoded.end(),
                    partition_data.begin() + static_cast<std::ptrdiff_t>(offset),
                    partition_data.begin() +
                        static_cast<std::ptrdiff_t>(offset + length));
            } else {
                encoded.resize(4U + static_cast<std::size_t>(
                    compressBound(static_cast<uLong>(length))));
                uLongf compressed_size =
                    static_cast<uLongf>(encoded.size() - 4U);
                const auto result = compress2(
                    reinterpret_cast<Bytef*>(encoded.data() + 4U),
                    &compressed_size,
                    reinterpret_cast<const Bytef*>(
                        partition_data.data() + offset),
                    static_cast<uLong>(length), static_cast<int>(compression));
                require(result == Z_OK, "failed to compress GHO zlib fixture");
                encoded.resize(4U + static_cast<std::size_t>(compressed_size));
            }
            require(encoded.size() + 2U <=
                        std::numeric_limits<std::uint16_t>::max(),
                    "compressed GHO fixture block is too large");
            append_le16(image,
                        static_cast<std::uint16_t>(encoded.size() + 2U));
            image.insert(image.end(), encoded.begin(), encoded.end());
#else
            throw std::runtime_error("zlib test support was not built");
#endif
        }
        offset += length;
        if (include_continuation && offset == block_size &&
            offset < partition_data.size()) {
            append_record(0x0703U, 20);
            const auto continuation_header_offset = image.size();
            image.resize(continuation_header_offset + 512U, std::byte{0});
            write_le16(image, continuation_header_offset, 0xeffe);
            image[continuation_header_offset + 2U] = std::byte{2};
            image[continuation_header_offset + 3U] =
                static_cast<std::byte>(compression);
        }
    }
    if (zero_length_block_terminator) {
        append_le16(image, 0);
    }
    append_record(0x0023U, 24);
    return image;
}

std::vector<std::byte> make_gho_multi_partition_fixture(
    std::span<const std::byte> first_partition,
    std::span<const std::byte> second_partition,
    bool mismatch_mbr = false) {
    require(first_partition.size() % 512U == 0 &&
                second_partition.size() % 512U == 0,
            "multi-partition GHO fixture requires sector-aligned partitions");
    std::vector<std::byte> image(512, std::byte{0});
    write_le16(image, 0, 0xeffe);
    image[2] = std::byte{1};
    image[3] = std::byte{0};
    const auto append_record = [&image](std::uint32_t type,
                                        std::uint16_t body_size) {
        const auto offset = image.size();
        image.resize(offset + 10U + body_size, std::byte{0});
        write_le32(image, offset, type);
        write_le32(image, offset + 4U, 0x012f18d8U);
        write_le16(image, offset + 8U, body_size);
        return offset + 10U;
    };
    const auto track0 = append_record(0x0006U, 518);
    image[track0] = std::byte{6};
    image[track0 + 1U] = std::byte{63};
    const auto mbr = track0 + 6U;
    image[mbr + 510U] = std::byte{0x55};
    image[mbr + 511U] = std::byte{0xaa};
    const auto first_sectors =
        static_cast<std::uint32_t>(first_partition.size() / 512U);
    const auto second_sectors =
        static_cast<std::uint32_t>(second_partition.size() / 512U);
    const auto second_start = first_sectors + 2U;
    const auto set_mbr_entry = [&image, mbr](
                                   std::size_t index, std::uint32_t start,
                                   std::uint32_t sectors) {
        const auto entry = mbr + 446U + index * 16U;
        image[entry + 4U] = std::byte{0x83};
        write_le32(image, entry + 8U, start);
        write_le32(image, entry + 12U, sectors);
    };
    set_mbr_entry(0, 1, mismatch_mbr ? first_sectors - 1U : first_sectors);
    set_mbr_entry(1, second_start, second_sectors);

    const auto append_partition = [&image, &append_record](
                                      std::span<const std::byte> data) {
        append_record(0x0603U, 20);
        const auto header = image.size();
        image.resize(header + 512U, std::byte{0});
        write_le16(image, header, 0xeffe);
        image[header + 2U] = std::byte{2};
        const auto block_size = 32U * 1024U;
        for (std::size_t offset = 0; offset < data.size();) {
            const auto length = std::min(
                static_cast<std::size_t>(block_size), data.size() - offset);
            append_le16(image, static_cast<std::uint16_t>(length + 2U));
            image.insert(image.end(),
                         data.begin() + static_cast<std::ptrdiff_t>(offset),
                         data.begin() +
                             static_cast<std::ptrdiff_t>(offset + length));
            offset += length;
        }
    };
    append_partition(first_partition);
    append_partition(second_partition);
    append_record(0x0023U, 24);
    return image;
}

void test_gho_driver(const std::filesystem::path& image_path,
                     std::uint8_t compression = 0,
                     bool store_zlib_blocks_uncompressed = false,
                     bool zero_length_block_terminator = false,
                     bool include_continuation = false) {
    std::vector<std::byte> payload(32U * 1024U + 37U);
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] =
            static_cast<std::byte>((index * 29U + 7U) & 0xffU);
    }
    const auto fixture = make_gho_fixture(
        payload, compression, store_zlib_blocks_uncompressed, false,
        zero_length_block_terminator, include_continuation);
    std::vector<char> chars(
        reinterpret_cast<const char*>(fixture.data()),
        reinterpret_cast<const char*>(fixture.data() + fixture.size()));
    write_fixture(image_path, chars);
    const auto original_hash = hash_bytes(chars);

    const auto info = uife::DriverFactory::instance().identify(image_path);
    require(info.format ==
                "Norton Ghost GHO (multi-partition)" &&
                info.supported,
            "valid GHO was not recognized as readable");
    auto driver = uife::DriverFactory::instance().create(image_path);
    require(driver->get_size() == payload.size() &&
                driver->sector_size() == 512,
            "GHO logical partition geometry is incorrect");
    std::array<std::byte, 50> sample{};
    const auto source_offset = 32U * 1024U - 31U;
    require(driver->read_at(source_offset, sample) == sample.size() &&
                std::equal(sample.begin(), sample.end(),
                           payload.begin() +
                               static_cast<std::ptrdiff_t>(source_offset)),
            "GHO random read across block boundary returned wrong bytes");
    require(hash_bytes(read_fixture(image_path)) == original_hash,
            "GHO reading modified the source image");
}

void test_gho_multi_partition(const std::filesystem::path& image_path) {
    std::vector<std::byte> first(2048, std::byte{0x31});
    std::vector<std::byte> second(1536, std::byte{0xa7});
    const auto fixture =
        make_gho_multi_partition_fixture(first, second);
    std::vector<char> chars(
        reinterpret_cast<const char*>(fixture.data()),
        reinterpret_cast<const char*>(fixture.data() + fixture.size()));
    write_fixture(image_path, chars);
    const auto original_hash = hash_bytes(chars);

    auto driver = uife::DriverFactory::instance().create(image_path);
    require(driver->get_size() == 512U + first.size() + 512U + second.size(),
            "multi-partition GHO disk geometry is incorrect");
    const auto partitions = uife::scan_partitions(*driver);
    require(partitions.size() == 2 &&
                partitions[0].byte_offset == 512U &&
                partitions[0].byte_length == first.size() &&
                partitions[1].byte_offset == 512U + first.size() + 512U &&
                partitions[1].byte_length == second.size(),
            "GHO MBR partitions were not mapped to decoded partition images");
    std::array<std::byte, 12> boundary{};
    const auto boundary_offset =
        partitions[1].byte_offset - 4U;
    require(driver->read_at(boundary_offset, boundary) == boundary.size() &&
                std::all_of(boundary.begin(), boundary.begin() + 4,
                            [](std::byte value) {
                                return value == std::byte{0};
                            }) &&
                std::all_of(boundary.begin() + 4, boundary.end(),
                            [](std::byte value) {
                                return value == std::byte{0xa7};
                            }),
            "GHO inter-partition gap or second partition data is incorrect");
    require(hash_bytes(read_fixture(image_path)) == original_hash,
            "reading multi-partition GHO modified the source image");
}

void test_gho_rejects_mismatched_multi_partition_mbr(
    const std::filesystem::path& image_path) {
    std::vector<std::byte> first(2048, std::byte{0x31});
    std::vector<std::byte> second(1536, std::byte{0xa7});
    const auto valid_fixture = make_gho_fixture(first);
    std::vector<char> valid_chars(
        reinterpret_cast<const char*>(valid_fixture.data()),
        reinterpret_cast<const char*>(valid_fixture.data() + valid_fixture.size()));
    write_fixture(image_path, valid_chars);
    auto driver = uife::DriverFactory::instance().create(image_path);

    const auto fixture =
        make_gho_multi_partition_fixture(first, second, true);
    std::vector<char> chars(
        reinterpret_cast<const char*>(fixture.data()),
        reinterpret_cast<const char*>(fixture.data() + fixture.size()));
    write_fixture(image_path, chars);
    const auto original_hash = hash_bytes(chars);
    bool rejected = false;
    try {
        driver->open(image_path);
    } catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).find("多个 GHO 分区") !=
                   std::string::npos;
    }
    require(rejected,
            "GHO with mismatched MBR and partition data was not rejected");
    bool closed_after_failure = false;
    try {
        std::array<std::byte, 1> byte{};
        (void)driver->read_at(0, byte);
    } catch (const std::runtime_error& error) {
        closed_after_failure =
            std::string(error.what()).find("尚未打开") != std::string::npos;
    }
    require(driver->get_size() == 0 && closed_after_failure,
            "failed GHO open left stale source handles or image state");
    require(hash_bytes(read_fixture(image_path)) == original_hash,
            "rejecting mismatched GHO partitions modified the source image");
}

void test_gho_rejects_unsupported_variants(
    const std::filesystem::path& image_path) {
    const std::array<std::byte, 512> payload{};
    const auto fixture = make_gho_fixture(payload);
    for (const auto variant : {0, 1}) {
        auto unsupported = fixture;
        if (variant == 0) {
            unsupported[3] = std::byte{1};
        } else {
            unsupported[12] = std::byte{2};
        }
        std::vector<char> chars(
            reinterpret_cast<const char*>(unsupported.data()),
            reinterpret_cast<const char*>(unsupported.data() +
                                          unsupported.size()));
        write_fixture(image_path, chars);
        const auto original_hash = hash_bytes(chars);
        const auto info = uife::DriverFactory::instance().identify(image_path);
        require(info.format == "Norton Ghost GHO (unsupported variant)" &&
                    !info.supported,
                "compressed or encrypted GHO was incorrectly advertised readable");
        bool rejected = false;
        try {
            (void)uife::DriverFactory::instance().create(image_path);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "unsupported GHO variant was not refused");
        require(hash_bytes(read_fixture(image_path)) == original_hash,
                "rejecting an unsupported GHO variant modified its source");
    }
}

void test_gho_fast_lz_matches(const std::filesystem::path& image_path) {
    const std::array<std::byte, 6> expected{
        std::byte{'1'}, std::byte{'2'}, std::byte{'3'},
        std::byte{'1'}, std::byte{'2'}, std::byte{'3'}};
    const std::array<std::byte, 10> sentinel_match{
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{3}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}};
    const auto verify = [&image_path](std::span<const std::byte> source,
                                      std::span<const std::byte> encoded,
                                      std::string_view failure) {
        const auto fixture = make_gho_fixture(
            source, 2, false, false, false, false, encoded);
        std::vector<char> chars(
            reinterpret_cast<const char*>(fixture.data()),
            reinterpret_cast<const char*>(fixture.data() + fixture.size()));
        write_fixture(image_path, chars);
        const auto original_hash = hash_bytes(chars);
        const auto info = uife::DriverFactory::instance().identify(image_path);
        require(info.supported &&
                    info.format == "Norton Ghost GHO (multi-partition)",
                "Fast LZ GHO was not recognized as a supported variant");
        auto driver = uife::DriverFactory::instance().create(image_path);
        std::vector<std::byte> decoded(source.size());
        require(driver->read_at(0, decoded) == decoded.size() &&
                    std::equal(decoded.begin(), decoded.end(), source.begin()),
                std::string(failure).c_str());
        require(hash_bytes(read_fixture(image_path)) == original_hash,
                "GHO Fast LZ read modified the source image");
    };
    verify(expected, sentinel_match,
           "GHO Fast LZ initial-dictionary match returned wrong bytes");

    const std::array<std::byte, 6> repeated{
        std::byte{'a'}, std::byte{'b'}, std::byte{'c'},
        std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    const std::array<std::byte, 11> literal_then_match{
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{8}, std::byte{0}, std::byte{'a'}, std::byte{'b'},
        std::byte{'c'}, std::byte{0xa0}, std::byte{0xbd}};
    verify(repeated, literal_then_match,
           "GHO Fast LZ literal/hash back-reference returned wrong bytes");
}

void test_gho_rejects_corrupt_zlib_block(
    const std::filesystem::path& image_path) {
    std::vector<std::byte> payload(4096, std::byte{0x41});
    auto fixture = make_gho_fixture(payload, 3);
    constexpr std::size_t first_compressed_byte = 512U + 10U + 20U + 512U + 2U + 4U;
    require(first_compressed_byte < fixture.size() - 34U,
            "GHO zlib fixture layout is too small");
    fixture[first_compressed_byte] = std::byte{0xff};
    std::vector<char> chars(
        reinterpret_cast<const char*>(fixture.data()),
        reinterpret_cast<const char*>(fixture.data() + fixture.size()));
    write_fixture(image_path, chars);
    const auto original_hash = hash_bytes(chars);

    bool rejected = false;
    try {
        (void)uife::DriverFactory::instance().create(image_path);
    } catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).find("GHO") != std::string::npos;
    }
    require(rejected, "corrupt GHO zlib block was not explicitly rejected");
    require(hash_bytes(read_fixture(image_path)) == original_hash,
            "corrupt GHO zlib validation modified the source image");
}

void test_gho_record_signature_inside_block(
    const std::filesystem::path& image_path) {
    std::vector<std::byte> payload(1537, std::byte{0x5a});
    payload[2] = std::byte{0xd8};
    payload[3] = std::byte{0x18};
    payload[4] = std::byte{0x2f};
    payload[5] = std::byte{0x01};
    payload[6] = std::byte{20};
    payload[7] = std::byte{0};
    const auto fixture = make_gho_fixture(payload);
    std::vector<char> chars(
        reinterpret_cast<const char*>(fixture.data()),
        reinterpret_cast<const char*>(fixture.data() + fixture.size()));
    write_fixture(image_path, chars);
    const auto original_hash = hash_bytes(chars);

    auto driver = uife::DriverFactory::instance().create(image_path);
    std::vector<std::byte> decoded(payload.size());
    require(driver->read_at(0, decoded) == decoded.size() &&
                decoded == payload,
            "GHO record-like bytes in an uncompressed block were misparsed");
    require(hash_bytes(read_fixture(image_path)) == original_hash,
            "GHO block-signature check modified the source image");
}

void test_gho_spanned_image(
    const std::filesystem::path& image_path,
    const std::filesystem::path& first_span_path,
    const std::filesystem::path& middle_span_path,
    const std::filesystem::path& last_span_path,
    const std::filesystem::path& unrelated_span_path) {
    std::vector<std::byte> payload(4096);
    std::uint32_t state = 0x12345678U;
    for (std::size_t index = 0; index < payload.size(); ++index) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        payload[index] = static_cast<std::byte>(state & 0xffU);
    }
    const auto fixture = make_gho_fixture(payload, 3);
    constexpr std::size_t partition_data_offset = 1056;
    constexpr std::size_t first_split_offset = partition_data_offset + 400U;
    constexpr std::size_t second_split_offset = partition_data_offset + 800U;
    constexpr std::size_t third_split_offset = partition_data_offset + 1200U;
    require(third_split_offset < fixture.size(),
            "GHO span split point is outside the fixture");
    const std::vector<std::byte> primary(
        fixture.begin(),
        fixture.begin() + static_cast<std::ptrdiff_t>(first_split_offset));
    const auto make_span = [&fixture](std::size_t begin, std::size_t end) {
        std::vector<std::byte> span(512, std::byte{0});
        std::copy_n(fixture.begin(), 512, span.begin());
        span[2] = std::byte{9};
        span.insert(span.end(),
                    fixture.begin() + static_cast<std::ptrdiff_t>(begin),
                    fixture.begin() + static_cast<std::ptrdiff_t>(end));
        return span;
    };
    const auto first_span =
        make_span(first_split_offset, second_split_offset);
    const auto middle_span =
        make_span(second_split_offset, third_split_offset);
    const auto last_span = make_span(third_split_offset, fixture.size());

    const auto to_chars = [](std::span<const std::byte> bytes) {
        return std::vector<char>(
            reinterpret_cast<const char*>(bytes.data()),
            reinterpret_cast<const char*>(bytes.data() + bytes.size()));
    };
    const auto primary_chars = to_chars(primary);
    const auto first_span_chars = to_chars(first_span);
    const auto middle_span_chars = to_chars(middle_span);
    const auto last_span_chars = to_chars(last_span);
    write_fixture(image_path, primary_chars);
    write_fixture(first_span_path, first_span_chars);
    write_fixture(middle_span_path, middle_span_chars);
    write_fixture(last_span_path, last_span_chars);
    const std::array<std::byte, 12> unrelated_bytes{};
    const auto unrelated_chars = [&unrelated_bytes] {
        return std::vector<char>(
            reinterpret_cast<const char*>(unrelated_bytes.data()),
            reinterpret_cast<const char*>(unrelated_bytes.data() +
                                           unrelated_bytes.size()));
    }();
    write_fixture(unrelated_span_path, unrelated_chars);
    const auto primary_hash = hash_bytes(primary_chars);
    const auto first_span_hash = hash_bytes(first_span_chars);
    const auto middle_span_hash = hash_bytes(middle_span_chars);
    const auto last_span_hash = hash_bytes(last_span_chars);

    auto driver = uife::DriverFactory::instance().create(image_path);
    std::array<std::byte, 48> sample{};
    const auto sample_offset = 380U;
    require(driver->get_size() == payload.size() &&
                driver->read_at(sample_offset, sample) == sample.size() &&
                std::equal(sample.begin(), sample.end(),
                           payload.begin() +
                               static_cast<std::ptrdiff_t>(sample_offset)),
            "GHO split across primary and first span returned wrong bytes");
    const auto middle_sample_offset = 780U;
    require(driver->read_at(middle_sample_offset, sample) == sample.size() &&
                std::equal(sample.begin(), sample.end(),
                           payload.begin() +
                               static_cast<std::ptrdiff_t>(middle_sample_offset)),
            "GHO span number ordering returned wrong bytes");
    const auto last_sample_offset = 1180U;
    require(driver->read_at(last_sample_offset, sample) == sample.size() &&
                std::equal(sample.begin(), sample.end(),
                           payload.begin() +
                               static_cast<std::ptrdiff_t>(last_sample_offset)),
            "GHO split across final numbered span returned wrong bytes");
    require(hash_bytes(read_fixture(image_path)) == primary_hash &&
                hash_bytes(read_fixture(first_span_path)) == first_span_hash &&
                hash_bytes(read_fixture(middle_span_path)) == middle_span_hash &&
                hash_bytes(read_fixture(last_span_path)) == last_span_hash,
            "reading spanned GHO modified a source file");
}

void test_gho_rejects_invalid_span_header(
    const std::filesystem::path& image_path,
    const std::filesystem::path& first_span_path,
    const std::filesystem::path& middle_span_path,
    const std::filesystem::path& last_span_path) {
    std::vector<std::byte> payload(2048, std::byte{0x4d});
    const auto fixture = make_gho_fixture(payload);
    auto invalid_span = std::vector<std::byte>(512, std::byte{0});
    std::copy_n(fixture.begin(), invalid_span.size(), invalid_span.begin());
    invalid_span[2] = std::byte{9};
    invalid_span[4] ^= std::byte{0x01};
    invalid_span.insert(invalid_span.end(),
                        fixture.begin() + 512,
                        fixture.end());
    const auto chars = [](std::span<const std::byte> bytes) {
        return std::vector<char>(
            reinterpret_cast<const char*>(bytes.data()),
            reinterpret_cast<const char*>(bytes.data() + bytes.size()));
    };
    const auto primary_chars = chars(fixture);
    const auto invalid_span_chars = chars(invalid_span);
    write_fixture(image_path, primary_chars);
    write_fixture(first_span_path, invalid_span_chars);
    write_fixture(middle_span_path, invalid_span_chars);
    write_fixture(last_span_path, invalid_span_chars);
    const auto primary_hash = hash_bytes(primary_chars);
    const auto span_hash = hash_bytes(invalid_span_chars);
    std::string rejection_message;
    try {
        (void)uife::DriverFactory::instance().create(image_path);
    } catch (const std::runtime_error& error) {
        rejection_message = error.what();
    }
    if (rejection_message.find("GHO 分卷 header") == std::string::npos) {
        throw std::runtime_error(
            "GHO mismatched span ID was not rejected by header validation: " +
            rejection_message);
    }
    require(hash_bytes(read_fixture(image_path)) == primary_hash &&
                hash_bytes(read_fixture(first_span_path)) == span_hash &&
                hash_bytes(read_fixture(middle_span_path)) == span_hash &&
                hash_bytes(read_fixture(last_span_path)) == span_hash,
            "rejecting an invalid GHO span modified a source file");
}

void test_gho_iso_browsing(const std::filesystem::path& iso_path,
                           const std::filesystem::path& gho_path,
                           std::uint8_t compression = 0) {
    const auto iso_chars = read_fixture(iso_path);
    const auto iso_data = std::as_bytes(
        std::span<const char>(iso_chars.data(), iso_chars.size()));
    const auto gho_bytes = make_gho_fixture(iso_data, compression, false, true);
    std::vector<char> chars(
        reinterpret_cast<const char*>(gho_bytes.data()),
        reinterpret_cast<const char*>(gho_bytes.data() + gho_bytes.size()));
    write_fixture(gho_path, chars);
    const auto original_hash = hash_bytes(chars);

    auto image = uife::DriverFactory::instance().create(gho_path);
    const auto partitions = uife::scan_partitions(*image);
    require(partitions.size() == 1 &&
                partitions.front().byte_offset == 512 &&
                partitions.front().byte_length == iso_data.size(),
            "GHO track0 MBR was not composed with its partition image");
    require(uife::FsParserFactory::instance().supports(
                *image, partitions.front().byte_offset),
            "ISO filesystem parser was not available through GHO");
    auto parser = uife::FsParserFactory::instance().create(
        *image, partitions.front().byte_offset);
    std::shared_ptr<uife::IImageDriver> shared_image(std::move(image));
    parser->mount(shared_image, partitions.front().byte_offset,
                  partitions.front().byte_length);
    const uife::VFS vfs(*parser);
    const auto entries = vfs.list_dir("/");
    require(entries.size() == 1 && entries.front().name == "HELLO.TXT",
            "could not list ISO filesystem stored in GHO partition");
    std::array<std::byte, 12> contents{};
    require(vfs.read("/HELLO.TXT", 0, contents) == contents.size() &&
                std::string(reinterpret_cast<const char*>(contents.data()),
                            contents.size()) == "fixture-data",
            "could not read file from ISO filesystem stored in GHO partition");
    require(hash_bytes(read_fixture(gho_path)) == original_hash,
            "browsing GHO modified the source image");
}

void test_dmg_driver(const std::filesystem::path& image_path,
                     bool compressed = false,
                     bool binary_plist = false) {
    const auto fixture = make_dmg_fixture(compressed, 0, binary_plist);
    std::vector<char> chars(
        reinterpret_cast<const char*>(fixture.data()),
        reinterpret_cast<const char*>(fixture.data() + fixture.size()));
    write_fixture(image_path, chars);
    const auto original_hash = hash_bytes(chars);

    const auto detected = uife::DriverFactory::instance().identify(image_path);
    require(detected.format == "Apple DMG" && detected.supported,
            "valid UDIF image was not recognized by the DMG reader");
    auto driver = uife::DriverFactory::instance().create(image_path);
    require(driver->format_name() == "Apple DMG" &&
                driver->get_size() == 1024 &&
                driver->sector_size() == 512,
            "DMG logical image geometry was incorrect");
    std::array<std::byte, 24> boundary{};
    require(driver->read_at(500, boundary) == boundary.size(),
            "DMG boundary read returned an incorrect byte count");
    for (std::size_t index = 0; index < 12; ++index) {
        require(std::to_integer<unsigned char>(boundary[index]) ==
                    ((500U + index) * 37U + 11U) % 256U,
                "DMG raw blkx data did not match source bytes");
    }
    for (std::size_t index = 12; index < boundary.size(); ++index) {
        require(boundary[index] == std::byte{0},
                "DMG zero-fill blkx run did not return zero bytes");
    }
    std::array<std::byte, 512> sector{};
    require(driver->read_sector(1, sector) == sector.size(),
            "DMG read_sector failed");
    for (std::size_t index = 0; index < sector.size(); ++index) {
        require(sector[index] == std::byte{0},
                "DMG zero-fill sector returned nonzero data");
    }
    require(hash_bytes(read_fixture(image_path)) == original_hash,
            "DMG reading modified the source image");
}

void test_dmg_rejects_unsupported_run(
    const std::filesystem::path& image_path) {
    const auto fixture = make_dmg_fixture(false, 0x80000004U);
    std::vector<char> chars(
        reinterpret_cast<const char*>(fixture.data()),
        reinterpret_cast<const char*>(fixture.data() + fixture.size()));
    write_fixture(image_path, chars);
    const auto original_hash = hash_bytes(chars);

    const auto info = uife::DriverFactory::instance().identify(image_path);
    require(info.format == "Apple DMG" && info.supported,
            "DMG with an unsupported run type was not recognized");
    bool rejected_explicitly = false;
    try {
        (void)uife::DriverFactory::instance().create(image_path);
    } catch (const std::runtime_error& error) {
        rejected_explicitly =
            std::string(error.what()).find("blkx") != std::string::npos;
    }
    require(rejected_explicitly,
            "DMG unsupported compression run was silently accepted");
    require(hash_bytes(read_fixture(image_path)) == original_hash,
            "DMG unsupported-run detection modified the source image");
}

void test_dmg_rejects_malformed_binary_plist(
    const std::filesystem::path& image_path) {
    auto fixture = make_dmg_fixture(false, 0, true);
    const auto footer_offset = fixture.size() - 512U;
    fixture[footer_offset - 25U] = std::byte{0xff};
    std::vector<char> chars(
        reinterpret_cast<const char*>(fixture.data()),
        reinterpret_cast<const char*>(fixture.data() + fixture.size()));
    write_fixture(image_path, chars);
    const auto original_hash = hash_bytes(chars);

    bool rejected_explicitly = false;
    try {
        (void)uife::DriverFactory::instance().create(image_path);
    } catch (const std::runtime_error& error) {
        rejected_explicitly =
            std::string(error.what()).find("trailer") != std::string::npos;
    }
    require(rejected_explicitly,
            "malformed binary DMG plist was not rejected by trailer validation");
    require(hash_bytes(read_fixture(image_path)) == original_hash,
            "malformed binary DMG plist validation modified the source image");
}

void test_dmg_rejects_segmented_udif(
    const std::filesystem::path& image_path) {
    auto fixture = make_dmg_fixture();
    const auto footer_offset = fixture.size() - 512U;
    write_be32(fixture, footer_offset + 56, 2);
    write_be32(fixture, footer_offset + 60, 2);
    std::vector<char> chars(
        reinterpret_cast<const char*>(fixture.data()),
        reinterpret_cast<const char*>(fixture.data() + fixture.size()));
    write_fixture(image_path, chars);

    bool rejected_explicitly = false;
    try {
        (void)uife::DriverFactory::instance().create(image_path);
    } catch (const std::runtime_error& error) {
        rejected_explicitly =
            std::string(error.what()).find("分段 UDIF") != std::string::npos;
    }
    require(rejected_explicitly,
            "segmented DMG was not rejected with an explicit limitation");
}

void test_dmg_iso_browsing(const std::filesystem::path& iso_path,
                           const std::filesystem::path& dmg_path) {
    const auto iso_chars = read_fixture(iso_path);
    const auto iso_bytes = std::as_bytes(
        std::span<const char>(iso_chars.data(), iso_chars.size()));
    const auto dmg_bytes = make_dmg_raw_image(iso_bytes);
    const auto dmg_chars = std::vector<char>(
        reinterpret_cast<const char*>(dmg_bytes.data()),
        reinterpret_cast<const char*>(dmg_bytes.data() + dmg_bytes.size()));
    write_fixture(dmg_path, dmg_chars);
    const auto source_hash = hash_bytes(dmg_chars);

    auto image = uife::DriverFactory::instance().create(dmg_path);
    std::shared_ptr<uife::IImageDriver> shared_image(std::move(image));
    const auto& factory = uife::FsParserFactory::instance();
    require(factory.identify(*shared_image, 0) == "ISO9660" &&
                factory.supports(*shared_image, 0),
            "ISO filesystem inside DMG was not recognized");
    auto parser = factory.create(*shared_image, 0);
    parser->mount(shared_image, 0, shared_image->get_size());
    const auto entries = parser->list_dir("/");
    require(entries.size() == 1 && entries.front().name == "HELLO.TXT",
            "ISO file tree inside DMG was not browsable");
    std::array<std::byte, 12> payload{};
    require(parser->read_file("/HELLO.TXT", 0, payload) == payload.size() &&
                std::string(reinterpret_cast<const char*>(payload.data()),
                            payload.size()) == "fixture-data",
            "ISO file content inside DMG was not readable");
    require(hash_bytes(read_fixture(dmg_path)) == source_hash,
            "DMG filesystem browsing modified the source image");
}

void append_btrfs_attribute(std::vector<std::byte>& payload,
                            std::uint16_t id,
                            std::span<const std::byte> value) {
    require(value.size() <= std::numeric_limits<std::uint16_t>::max(),
            "Btrfs fixture attribute exceeds protocol limit");
    append_le16(payload, id);
    append_le16(payload, static_cast<std::uint16_t>(value.size()));
    payload.insert(payload.end(), value.begin(), value.end());
}

void append_btrfs_string_attribute(std::vector<std::byte>& payload,
                                   std::uint16_t id,
                                   std::string_view value) {
    append_btrfs_attribute(
        payload, id,
        std::as_bytes(std::span<const char>(value.data(), value.size())));
}

std::uint32_t btrfs_crc32c(std::span<const std::byte> bytes) {
    std::uint32_t crc = 0;
    for (const auto byte : bytes) {
        crc ^= std::to_integer<unsigned char>(byte);
        for (unsigned int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(
                -static_cast<std::int32_t>(crc & 1U));
            crc = (crc >> 1U) ^ (0x82f63b78U & mask);
        }
    }
    return crc;
}

void append_btrfs_command(std::vector<std::byte>& stream,
                          std::uint16_t command,
                          std::span<const std::byte> payload) {
    const auto command_start = stream.size();
    append_le32(stream, static_cast<std::uint32_t>(payload.size()));
    append_le16(stream, command);
    append_le32(stream, 0);
    stream.insert(stream.end(), payload.begin(), payload.end());
    const auto crc = btrfs_crc32c(
        std::span<const std::byte>(stream).subspan(command_start));
    for (unsigned int index = 0; index < 4; ++index) {
        stream[command_start + 6 + index] =
            static_cast<std::byte>((crc >> (index * 8U)) & 0xffU);
    }
}

std::vector<std::byte> make_btrfs_send_fixture(std::uint32_t version = 2) {
    constexpr std::string_view magic("btrfs-stream", 13);
    std::vector<std::byte> stream;
    const auto magic_bytes =
        std::as_bytes(std::span<const char>(magic.data(), magic.size()));
    stream.insert(stream.end(), magic_bytes.begin(), magic_bytes.end());
    append_le32(stream, version);

    std::vector<std::byte> payload;
    append_btrfs_string_attribute(payload, 15, "fixture-volume");
    append_btrfs_command(stream, 1, payload);

    payload.clear();
    append_btrfs_string_attribute(payload, 15, "docs");
    std::vector<std::byte> integer;
    append_le64(integer, 0040755);
    append_btrfs_attribute(payload, 5, integer);
    integer.clear();
    append_le64(integer, 256);
    append_btrfs_attribute(payload, 3, integer);
    append_btrfs_command(stream, 4, payload);

    payload.clear();
    append_btrfs_string_attribute(payload, 15, "docs/data.bin");
    integer.clear();
    append_le64(integer, 0100644);
    append_btrfs_attribute(payload, 5, integer);
    integer.clear();
    append_le64(integer, 257);
    append_btrfs_attribute(payload, 3, integer);
    append_btrfs_command(stream, 3, payload);

    payload.clear();
    append_btrfs_string_attribute(payload, 15, "docs/data.bin");
    integer.clear();
    append_le64(integer, 0);
    append_btrfs_attribute(payload, 18, integer);
    constexpr std::string_view first_data = "abcdef";
    const auto first_data_bytes =
        std::as_bytes(std::span<const char>(first_data.data(), first_data.size()));
    if (version == 1) {
        append_btrfs_attribute(payload, 19, first_data_bytes);
    } else {
        append_le16(payload, 19);
        payload.insert(payload.end(), first_data_bytes.begin(),
                       first_data_bytes.end());
    }
    append_btrfs_command(stream, 15, payload);

    payload.clear();
    append_btrfs_string_attribute(payload, 15, "docs/data.bin");
    integer.clear();
    append_le64(integer, 2);
    append_btrfs_attribute(payload, 18, integer);
    constexpr std::string_view overwrite = "XY";
    const auto overwrite_bytes =
        std::as_bytes(std::span<const char>(overwrite.data(), overwrite.size()));
    if (version == 1) {
        append_btrfs_attribute(payload, 19, overwrite_bytes);
    } else {
        append_le16(payload, 19);
        payload.insert(payload.end(), overwrite_bytes.begin(),
                       overwrite_bytes.end());
    }
    append_btrfs_command(stream, 15, payload);

    payload.clear();
    append_btrfs_string_attribute(payload, 15, "docs/data.bin");
    integer.clear();
    append_le64(integer, 10);
    append_btrfs_attribute(payload, 4, integer);
    append_btrfs_command(stream, 17, payload);

    payload.clear();
    append_btrfs_string_attribute(payload, 15, "docs/data.bin");
    append_btrfs_string_attribute(payload, 16, "docs/renamed.bin");
    append_btrfs_command(stream, 9, payload);

    payload.clear();
    append_btrfs_string_attribute(payload, 15, "docs/copy.bin");
    append_btrfs_string_attribute(payload, 17, "docs/renamed.bin");
    append_btrfs_command(stream, 10, payload);

    payload.clear();
    append_btrfs_string_attribute(payload, 15, "docs/renamed.bin");
    integer.clear();
    append_le64(integer, 0100600);
    append_btrfs_attribute(payload, 5, integer);
    append_btrfs_command(stream, 18, payload);

    append_btrfs_command(stream, 21, {});
    return stream;
}

void test_btrfs_send_parser(const std::filesystem::path& image_path,
                            const std::filesystem::path& corrupt_path,
                            const std::filesystem::path& output_path) {
    const auto fixture = make_btrfs_send_fixture();
    const auto fixture_as_chars = std::vector<char>(
        reinterpret_cast<const char*>(fixture.data()),
        reinterpret_cast<const char*>(fixture.data() + fixture.size()));
    write_fixture(image_path, fixture_as_chars);
    const auto source_hash = hash_bytes(fixture_as_chars);

    const auto detected = uife::DriverFactory::instance().identify(image_path);
    require(detected.format == "RAW" && detected.supported,
            "Btrfs send stream did not open through the RAW image driver");
    auto image = uife::DriverFactory::instance().create(image_path);
    std::shared_ptr<uife::IImageDriver> shared_image(std::move(image));
    require(uife::FsParserFactory::instance().identify(*shared_image, 0) ==
                "Btrfs send stream" &&
                uife::FsParserFactory::instance().supports(*shared_image, 0),
            "Btrfs send stream parser was not detected and registered");
    auto parser =
        uife::FsParserFactory::instance().create(*shared_image, 0);
    parser->mount(shared_image, 0, shared_image->get_size());
    const auto root_entries = parser->list_dir("/");
    require(root_entries.size() == 1 && root_entries.front().name == "docs" &&
                root_entries.front().type == uife::NodeType::directory,
            "Btrfs send root directory listing failed");
    const auto file_entries = parser->list_dir("/docs");
    require(file_entries.size() == 2,
            "Btrfs send hard link was not reconstructed in the file tree");
    const auto renamed_file = std::find_if(
        file_entries.begin(), file_entries.end(),
        [](const uife::VFSNode& node) { return node.name == "renamed.bin"; });
    const auto linked_file = std::find_if(
        file_entries.begin(), file_entries.end(),
        [](const uife::VFSNode& node) { return node.name == "copy.bin"; });
    require(renamed_file != file_entries.end() &&
                linked_file != file_entries.end() &&
                renamed_file->metadata.size == 10 &&
                linked_file->metadata.size == 10 &&
                renamed_file->metadata.link_count == 2 &&
                linked_file->metadata.link_count == 2 &&
                renamed_file->metadata.mode == 0100600 &&
                linked_file->metadata.mode == 0100600,
            "Btrfs send rename, hard-link data, or inode metadata was not reconstructed");
    std::array<std::byte, 10> data{};
    require(parser->read_file("/docs/renamed.bin", 0, data) == data.size(),
            "Btrfs send file data read returned the wrong length");
    constexpr std::array<unsigned char, 10> expected{
        'a', 'b', 'X', 'Y', 'e', 'f', 0, 0, 0, 0};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        require(std::to_integer<unsigned char>(data[index]) == expected[index],
                "Btrfs send overlapping writes or sparse range were incorrect");
    }
    std::array<std::byte, 10> linked_data{};
    require(parser->read_file("/docs/copy.bin", 0, linked_data) ==
                    linked_data.size() &&
                linked_data == data,
            "Btrfs send hard-link data did not share the original extents");

    remove_test_output(output_path);
    uife::ExtractOptions options;
    options.output_directory = output_path;
    const auto extracted =
        uife::Extractor::extract(uife::VFS(*parser), "/docs/renamed.bin", options);
    const auto output_file = output_path / "renamed.bin";
    require(extracted.size() == 1 &&
                extracted.front().sha256 == hash_bytes(
                    std::vector<char>(reinterpret_cast<const char*>(expected.data()),
                                      reinterpret_cast<const char*>(expected.data() +
                                                                    expected.size()))) &&
                std::filesystem::file_size(output_file) == expected.size(),
            "Btrfs send extracted payload hash or size was incorrect");
    require(hash_bytes(read_fixture(image_path)) == source_hash,
            "Btrfs send browsing or extraction modified the source stream");

    auto corrupt = fixture_as_chars;
    corrupt[17 + 6] ^= 1;
    write_fixture(corrupt_path, corrupt);
    auto corrupt_image = uife::DriverFactory::instance().create(corrupt_path);
    std::shared_ptr<uife::IImageDriver> shared_corrupt(
        std::move(corrupt_image));
    auto corrupt_parser =
        uife::FsParserFactory::instance().create(*shared_corrupt, 0);
    bool rejected_crc = false;
    try {
        corrupt_parser->mount(shared_corrupt, 0, shared_corrupt->get_size());
    } catch (const std::runtime_error& error) {
        rejected_crc =
            std::string(error.what()).find("CRC32C") != std::string::npos;
    }
    require(rejected_crc,
            "Btrfs send stream with a corrupted command CRC was accepted");
}

void test_btrfs_versioned_send_parser(
    const std::filesystem::path& image_path, std::uint32_t version) {
    const auto fixture = make_btrfs_send_fixture(version);
    const auto fixture_as_chars = std::vector<char>(
        reinterpret_cast<const char*>(fixture.data()),
        reinterpret_cast<const char*>(fixture.data() + fixture.size()));
    write_fixture(image_path, fixture_as_chars);

    auto image = uife::DriverFactory::instance().create(image_path);
    std::shared_ptr<uife::IImageDriver> shared_image(std::move(image));
    auto parser =
        uife::FsParserFactory::instance().create(*shared_image, 0);
    parser->mount(shared_image, 0, shared_image->get_size());

    std::array<std::byte, 10> data{};
    require(parser->read_file("/docs/copy.bin", 0, data) == data.size(),
            "versioned Btrfs DATA attributes were not readable");
    constexpr std::array<unsigned char, 10> expected{
        'a', 'b', 'X', 'Y', 'e', 'f', 0, 0, 0, 0};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        require(std::to_integer<unsigned char>(data[index]) == expected[index],
                "versioned Btrfs DATA attribute decoding produced incorrect bytes");
    }
}

void test_refs_detection(const std::filesystem::path& image_path) {
    std::vector<char> image(4096, '\0');
    image[3] = 'R';
    image[4] = 'e';
    image[5] = 'F';
    image[6] = 'S';
    image[12] = 2;
    write_fixture(image_path, image);

    auto driver = uife::DriverFactory::instance().create(image_path);
    std::shared_ptr<uife::IImageDriver> shared_driver(std::move(driver));
    const auto& factory = uife::FsParserFactory::instance();
    const auto identification = factory.identify(*shared_driver, 0);
    require(identification.starts_with("ReFS"),
            "ReFS boot-sector signature was not recognized");
#if defined(UIFE_WITH_LIBFSREFS)
    require(factory.supports(*shared_driver, 0),
            "ReFS parser was not registered although libfsrefs is available");
    auto refs_parser = factory.create(*shared_driver, 0);
    require(!refs_parser->can_read_content(),
            "ReFS backend must declare that file content cannot be read");
    bool rejected_invalid_volume = false;
    try {
        // 只有启动签名、没有有效卷结构的镜像必须被拒绝，而不是当成空卷。
        refs_parser->mount(shared_driver, 0, shared_driver->get_size());
    } catch (const std::runtime_error&) {
        rejected_invalid_volume = true;
    }
    require(rejected_invalid_volume,
            "ReFS parser accepted an image that only carries a boot signature");
#else
    require(identification.starts_with("ReFS（已识别") &&
                !factory.supports(*shared_driver, 0),
            "ReFS should stay detection-only without libfsrefs");
    bool rejected_without_backend = false;
    try {
        (void)factory.create(*shared_driver, 0);
    } catch (const std::runtime_error& error) {
        rejected_without_backend =
            std::string(error.what()).find("ReFS") != std::string::npos;
    }
    require(rejected_without_backend,
            "ReFS without a parser backend did not fail with an explicit error");
#endif
}

#ifdef UIFE_WITH_LIBWIM
void check_wimlib_result(int result, const char* operation) {
    if (result != 0) {
        throw std::runtime_error(
            std::string(operation) + " failed with wimlib error " +
            std::to_string(result));
    }
}

void make_split_wim_fixture(const std::filesystem::path& image_path,
                            const std::filesystem::path& split_path) {
    WIMStruct* wim = nullptr;
    check_wimlib_result(wimlib_open_wim(image_path.c_str(), 0, &wim),
                        "wimlib_open_wim(split fixture)");
    try {
        check_wimlib_result(
            wimlib_split(wim, split_path.c_str(), 1, 0),
            "wimlib_split");
        wimlib_free(wim);
    } catch (...) {
        wimlib_free(wim);
        throw;
    }

    WIMStruct* split = nullptr;
    check_wimlib_result(wimlib_open_wim(split_path.c_str(), 0, &split),
                        "wimlib_open_wim(split verification)");
    wimlib_wim_info info{};
    const auto info_result = wimlib_get_wim_info(split, &info);
    wimlib_free(split);
    check_wimlib_result(info_result, "wimlib_get_wim_info(split fixture)");
    require(info.part_number == 1 && info.total_parts > 1,
            "wimlib did not create a multi-part split WIM fixture");
}

void make_wim_fixture(const std::filesystem::path& image_path,
                      const std::filesystem::path& first_source,
                      const std::filesystem::path& second_source) {
    std::filesystem::create_directories(first_source / "docs");
    std::filesystem::create_directories(second_source / "docs");
    {
        std::ofstream first(first_source / "docs" / "hello.txt",
                            std::ios::binary);
        first << "wim image one payload";
        require(static_cast<bool>(first), "failed to create first WIM payload");
        std::ofstream second(second_source / "docs" / "hello.txt",
                             std::ios::binary);
        second << "wim image two payload";
        require(static_cast<bool>(second), "failed to create second WIM payload");
        const auto unicode_name = std::filesystem::path(u8"中文.txt");
        std::ofstream unicode_file(first_source / unicode_name,
                                   std::ios::binary);
        unicode_file << "unicode filename payload";
        require(static_cast<bool>(unicode_file),
                "failed to create Unicode WIM filename fixture");
    }

    check_wimlib_result(wimlib_global_init(0), "wimlib_global_init");
    WIMStruct* wim = nullptr;
    check_wimlib_result(
        wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_LZX, &wim),
        "wimlib_create_new_wim");
    const auto free_wim = [&wim] {
        if (wim != nullptr) {
            wimlib_free(wim);
            wim = nullptr;
        }
    };
    try {
#ifdef _WIN32
        constexpr wimlib_tchar first_name[] = L"First Image";
        constexpr wimlib_tchar second_name[] = L"Second Image";
#else
        constexpr wimlib_tchar first_name[] = "First Image";
        constexpr wimlib_tchar second_name[] = "Second Image";
#endif
        check_wimlib_result(
            wimlib_add_image(wim, first_source.c_str(), first_name, nullptr,
                             WIMLIB_ADD_FLAG_NO_ACLS),
            "wimlib_add_image(first)");
        check_wimlib_result(
            wimlib_add_image(wim, second_source.c_str(), second_name, nullptr,
                             WIMLIB_ADD_FLAG_NO_ACLS),
            "wimlib_add_image(second)");
        check_wimlib_result(
            wimlib_write(wim, image_path.c_str(), WIMLIB_ALL_IMAGES, 0, 1),
            "wimlib_write");
        free_wim();
    } catch (...) {
        free_wim();
        throw;
    }
}

std::string read_wim_payload(const uife::IFSParser& parser,
                             const std::string& path,
                             std::size_t expected_size) {
    std::vector<std::byte> bytes(expected_size);
    require(parser.read_file(path, 0, bytes) == bytes.size(),
            "WIM parser returned an unexpected file length");
    return std::string(reinterpret_cast<const char*>(bytes.data()),
                       bytes.size());
}

void test_wim_parser(const std::filesystem::path& image_path) {
    const auto source_hash = hash_bytes(read_fixture(image_path));
    const auto image_info = uife::DriverFactory::instance().identify(image_path);
    require(image_info.format == "WIM/ESD" && image_info.supported,
            "WIM/ESD was not identified as a readable image");
    auto image = uife::DriverFactory::instance().create(image_path);
    std::shared_ptr<uife::IImageDriver> shared_image(std::move(image));
    require(uife::FsParserFactory::instance().identify(*shared_image, 0) ==
                "WIM/ESD" &&
                uife::FsParserFactory::instance().supports(*shared_image, 0),
            "WIM parser was not registered");
    auto parser =
        uife::FsParserFactory::instance().create(*shared_image, 0);
    parser->mount(shared_image, 0, shared_image->get_size());

    const auto images = parser->list_dir("/images");
    require(images.size() == 2 && images[0].path == "/images/1" &&
                images[1].path == "/images/2",
            "WIM image indices were not exposed in the VFS");
    const auto first_files = parser->list_dir("/images/1/docs");
    const auto second_files = parser->list_dir("/images/2/docs");
    require(first_files.size() == 1 && first_files.front().name == "hello.txt" &&
                second_files.size() == 1 &&
                second_files.front().name == "hello.txt",
            "WIM directory tree listing failed");
    require(read_wim_payload(*parser, "/images/1/docs/hello.txt", 21) ==
                "wim image one payload" &&
                read_wim_payload(*parser, "/images/2/docs/hello.txt", 21) ==
                "wim image two payload",
            "WIM file data was read from the wrong image");
    require(read_wim_payload(*parser, "/images/1/中文.txt", 24) ==
                "unicode filename payload",
            "WIM Unicode filename or file data was not read correctly");

    parser.reset();
    shared_image.reset();
    require(hash_bytes(read_fixture(image_path)) == source_hash,
            "WIM source changed during browsing and extraction");
}

void test_split_wim_missing_part_rejected(
    const std::filesystem::path& first_part,
    const std::filesystem::path& missing_part) {
    auto displaced_part = missing_part;
    displaced_part += ".temporarily-missing";
    std::filesystem::rename(missing_part, displaced_part);
    bool rejected_incomplete_set = false;
    try {
        auto image = uife::DriverFactory::instance().create(first_part);
        std::shared_ptr<uife::IImageDriver> shared_image(std::move(image));
        auto parser =
            uife::FsParserFactory::instance().create(*shared_image, 0);
        try {
            parser->mount(shared_image, 0, shared_image->get_size());
        } catch (const std::runtime_error& error) {
            rejected_incomplete_set =
                std::string(error.what()).find("WIM 分卷不完整") !=
                std::string::npos;
        }
    } catch (...) {
        std::filesystem::rename(displaced_part, missing_part);
        throw;
    }
    std::filesystem::rename(displaced_part, missing_part);
    require(rejected_incomplete_set,
            "WIM parser did not explicitly reject a missing split part");
}

void test_split_wim_parser(const std::filesystem::path& first_part) {
    WIMStruct* wim = nullptr;
    check_wimlib_result(wimlib_open_wim(first_part.c_str(), 0, &wim),
                        "wimlib_open_wim(split test)");
    wimlib_wim_info info{};
    const auto info_result = wimlib_get_wim_info(wim, &info);
    wimlib_free(wim);
    check_wimlib_result(info_result, "wimlib_get_wim_info(split test)");

    std::vector<std::pair<std::filesystem::path, std::string>> source_hashes;
    const auto stem = first_part.stem().string();
    for (std::uint32_t index = 1; index <= info.total_parts; ++index) {
        auto part_name = stem;
        if (index > 1) {
            part_name += std::to_string(index);
        }
        const auto part_path =
            first_part.parent_path() / (part_name + first_part.extension().string());
        source_hashes.emplace_back(
            part_path, hash_bytes(read_fixture(part_path)));
    }

    require(source_hashes.size() > 1,
            "split WIM fixture contains no additional parts");
    test_split_wim_missing_part_rejected(first_part,
                                         source_hashes[1].first);
    test_wim_parser(first_part);
    for (const auto& [path, expected_hash] : source_hashes) {
        require(hash_bytes(read_fixture(path)) == expected_hash,
                "split WIM source changed during browsing");
    }
}
#endif

void test_iso_integrity(const std::filesystem::path& valid_image,
                        const std::filesystem::path& corrupt_descriptor,
                        const std::filesystem::path& corrupt_record,
                        const std::filesystem::path& truncated_volume) {
    constexpr std::size_t sector_size = 2048;
    const auto source = read_fixture(valid_image);

    auto invalid_descriptor = source;
    invalid_descriptor[16 * sector_size + 84] ^= 1;
    write_fixture(corrupt_descriptor, invalid_descriptor);
    bool rejected_descriptor = false;
    try {
        (void)uife::iso9660::Iso9660(corrupt_descriptor);
    } catch (const std::runtime_error&) {
        rejected_descriptor = true;
    }
    require(rejected_descriptor,
            "ISO parser accepted inconsistent dual-endian volume geometry");

    auto invalid_record = source;
    invalid_record[20 * sector_size + 34 + 34 + 6] ^= 1;
    write_fixture(corrupt_record, invalid_record);
    bool rejected_record = false;
    try {
        const uife::iso9660::Iso9660 image(corrupt_record);
        (void)image.list("/");
    } catch (const std::runtime_error&) {
        rejected_record = true;
    }
    require(rejected_record,
            "ISO parser accepted an inconsistent dual-endian directory extent");

    auto undersized_volume = source;
    write_dual32(undersized_volume.data() + 16 * sector_size + 80, 20);
    write_fixture(truncated_volume, undersized_volume);
    bool rejected_extent = false;
    try {
        (void)uife::iso9660::Iso9660(truncated_volume);
    } catch (const std::runtime_error&) {
        rejected_extent = true;
    }
    require(rejected_extent,
            "ISO parser accepted a root directory outside declared volume bounds");
}

void test_rock_ridge_iso_parser(const std::filesystem::path& image_path) {
    auto image = uife::DriverFactory::instance().create(image_path);
    std::shared_ptr<uife::IImageDriver> shared_image(std::move(image));
    auto parser =
        uife::FsParserFactory::instance().create(*shared_image, 0);
    parser->mount(shared_image, 0, shared_image->get_size());

    const auto entries = parser->list_dir("/");
    require(entries.size() == 3 &&
                std::any_of(entries.begin(), entries.end(),
                    [](const auto& entry) {
                        return entry.name == "rock ridge.txt";
                    }),
            "Rock Ridge NM name was not used for the ISO entry");
    const auto metadata = parser->get_metadata("/rock ridge.txt");
    require(metadata.type == uife::NodeType::regular_file &&
                metadata.metadata.mode == 0640 &&
                metadata.metadata.has_owner &&
                metadata.metadata.uid == 1001 &&
                metadata.metadata.gid == 1002 &&
                metadata.metadata.link_count == 2 &&
                metadata.metadata.hard_link_group == 77,
            "Rock Ridge PX POSIX metadata was not preserved");
    const auto alias = parser->get_metadata("/ALIAS");
    require(alias.metadata.hard_link_group == 77,
            "Rock Ridge PX file serial was not used for hard-link identity");
    const auto link = parser->get_metadata("/LINK");
    require(link.type == uife::NodeType::symbolic_link &&
                link.metadata.symlink_target == "target.txt",
            "Rock Ridge SL symbolic link target was not reconstructed");
    std::array<std::byte, 12> payload{};
    require(parser->read_file("/rock ridge.txt", 0, payload) ==
                payload.size() &&
                std::string(reinterpret_cast<const char*>(payload.data()),
                            payload.size()) == "fixture-data",
            "Rock Ridge named file data could not be read");
}

void test_rock_ridge_ce_rejects_out_of_bounds(
    const std::filesystem::path& image_path) {
    bool rejected = false;
    try {
        const uife::iso9660::Iso9660 image(image_path);
        (void)image.list("/");
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected,
            "Rock Ridge CE accepted an extent outside the declared volume");
}

std::vector<char> make_directory_record(std::uint32_t extent,
                                        std::uint32_t size,
                                        const std::vector<unsigned char>& name,
                                        bool is_directory) {
    const auto record_size = 33U + name.size() + (name.size() % 2U == 0 ? 1U : 0U);
    std::vector<char> record(record_size);
    record[0] = static_cast<char>(record_size);
    write_dual32(record.data() + 2, extent);
    write_dual32(record.data() + 10, size);
    record[18] = 126;
    record[19] = 10;
    record[20] = 26;
    record[21] = 12;
    record[24] = 4;
    record[25] = is_directory ? 2 : 0;
    record[28] = 1;
    record[30] = 0;
    record[31] = 1;
    record[32] = static_cast<char>(name.size());
    std::transform(name.begin(), name.end(), record.begin() + 33,
                   [](unsigned char value) { return static_cast<char>(value); });
    return record;
}

void make_iso_fixture(const std::filesystem::path& path) {
    constexpr std::size_t block_size = 2048;
    constexpr std::uint32_t sector_count = 22;
    std::vector<char> image(static_cast<std::size_t>(sector_count) * block_size);
    auto* primary = image.data() + 16 * block_size;
    primary[0] = 1;
    std::copy_n("CD001", 5, primary + 1);
    primary[6] = 1;
    write_dual32(primary + 80, sector_count);
    write_dual16(primary + 128, static_cast<std::uint16_t>(block_size));

    const auto root = make_directory_record(20, block_size, {0}, true);
    std::copy(root.begin(), root.end(), primary + 156);
    auto* terminator = image.data() + 17 * block_size;
    terminator[0] = static_cast<char>(255);
    std::copy_n("CD001", 5, terminator + 1);
    terminator[6] = 1;

    auto* directory = image.data() + 20 * block_size;
    std::size_t position = 0;
    for (const auto& record : {
             make_directory_record(20, block_size, {0}, true),
             make_directory_record(20, block_size, {1}, true),
             make_directory_record(21, 12,
                 {'H','E','L','L','O','.','T','X','T',';','1'}, false)}) {
        std::copy(record.begin(), record.end(), directory + position);
        position += record.size();
    }
    const std::string payload = "fixture-data";
    std::copy(payload.begin(), payload.end(), image.begin() + 21 * block_size);
    std::ofstream output(path, std::ios::binary);
    output.write(image.data(), static_cast<std::streamsize>(image.size()));
    if (!output) {
        throw std::runtime_error("failed to create ISO fixture");
    }
}

void make_rock_ridge_iso_fixture(const std::filesystem::path& path,
                                 bool corrupt_ce_extent = false) {
    constexpr std::size_t block_size = 2048;
    constexpr std::uint32_t sector_count = 23;
    std::vector<char> image(
        static_cast<std::size_t>(sector_count) * block_size);
    auto* primary = image.data() + 16 * block_size;
    primary[0] = 1;
    std::copy_n("CD001", 5, primary + 1);
    primary[6] = 1;
    write_dual32(primary + 80, sector_count);
    write_dual16(primary + 128, static_cast<std::uint16_t>(block_size));
    const auto root = make_directory_record(20, block_size, {0}, true);
    std::copy(root.begin(), root.end(), primary + 156);
    auto* terminator = image.data() + 17 * block_size;
    terminator[0] = static_cast<char>(255);
    std::copy_n("CD001", 5, terminator + 1);
    terminator[6] = 1;

    auto dot = make_directory_record(20, block_size, {0}, true);
    const std::array<char, 7> susp_sp{'S', 'P', 7, 1,
                                      static_cast<char>(0xbe),
                                      static_cast<char>(0xef), 0};
    dot.insert(dot.end(), susp_sp.begin(), susp_sp.end());
    dot[0] = static_cast<char>(dot.size());
    auto dotdot = make_directory_record(20, block_size, {1}, true);
    auto file = make_directory_record(21, 12, {'X', ';', '1'}, false);
    constexpr std::string_view rock_ridge_name = "rock ridge.txt";
    std::vector<char> continuation_nm{
        'N', 'M', static_cast<char>(5 + rock_ridge_name.size()), 1, 0};
    continuation_nm.insert(continuation_nm.end(), rock_ridge_name.begin(),
                   rock_ridge_name.end());
    continuation_nm.insert(continuation_nm.end(), {'S', 'T', 4, 1});
    std::array<char, 28> susp_ce{};
    susp_ce[0] = 'C';
    susp_ce[1] = 'E';
    susp_ce[2] = static_cast<char>(susp_ce.size());
    susp_ce[3] = 1;
    write_dual32(susp_ce.data() + 4, corrupt_ce_extent ? 99U : 22U);
    write_dual32(susp_ce.data() + 12, 0);
    write_dual32(susp_ce.data() + 20,
                 static_cast<std::uint32_t>(continuation_nm.size()));
    file.insert(file.end(), susp_ce.begin(), susp_ce.end());
    std::array<char, 44> susp_px{};
    susp_px[0] = 'P';
    susp_px[1] = 'X';
    susp_px[2] = static_cast<char>(susp_px.size());
    susp_px[3] = 1;
    write_dual32(susp_px.data() + 4, 0100640);
    write_dual32(susp_px.data() + 12, 2);
    write_dual32(susp_px.data() + 20, 1001);
    write_dual32(susp_px.data() + 28, 1002);
    write_dual32(susp_px.data() + 36, 77);
    file.insert(file.end(), susp_px.begin(), susp_px.end());
    file[0] = static_cast<char>(file.size());
    auto alias = make_directory_record(
        21, 12, {'A', 'L', 'I', 'A', 'S', ';', '1'}, false);
    alias.insert(alias.end(), susp_px.begin(), susp_px.end());
    alias[0] = static_cast<char>(alias.size());
    auto link = make_directory_record(
        21, 0, {'L', 'I', 'N', 'K', ';', '1'}, false);
    std::array<char, 17> susp_sl{
        'S', 'L', 17, 1, 0, 0, 10,
        't', 'a', 'r', 'g', 'e', 't', '.', 't', 'x', 't'};
    link.insert(link.end(), susp_sl.begin(), susp_sl.end());
    std::array<char, 36> susp_link_px{};
    susp_link_px[0] = 'P';
    susp_link_px[1] = 'X';
    susp_link_px[2] = static_cast<char>(susp_link_px.size());
    susp_link_px[3] = 1;
    write_dual32(susp_link_px.data() + 4, 0120777);
    write_dual32(susp_link_px.data() + 12, 1);
    write_dual32(susp_link_px.data() + 20, 1001);
    write_dual32(susp_link_px.data() + 28, 1002);
    link.insert(link.end(), susp_link_px.begin(), susp_link_px.end());
    link[0] = static_cast<char>(link.size());

    auto* directory = image.data() + 20 * block_size;
    std::size_t position = 0;
    for (const auto* record : {&dot, &dotdot, &file, &alias, &link}) {
        std::copy(record->begin(), record->end(), directory + position);
        position += record->size();
    }
    const std::string payload = "fixture-data";
    std::copy(payload.begin(), payload.end(), image.begin() + 21 * block_size);
    std::copy(continuation_nm.begin(), continuation_nm.end(),
              image.begin() + 22 * block_size);
    std::ofstream output(path, std::ios::binary);
    output.write(image.data(), static_cast<std::streamsize>(image.size()));
    require(static_cast<bool>(output),
            "failed to create Rock Ridge ISO fixture");
}

void make_mode1_cue_fixture(const std::filesystem::path& iso_path,
                            const std::filesystem::path& bin_path,
                            const std::filesystem::path& second_bin_path,
                            const std::filesystem::path& cue_path) {
    constexpr std::size_t payload_size = 2048;
    constexpr std::size_t raw_sector_size = 2352;
    std::ifstream input(iso_path, std::ios::binary);
    require(static_cast<bool>(input), "failed to open ISO for BIN+CUE fixture");
    std::vector<char> iso(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    require(!iso.empty() && iso.size() % payload_size == 0,
            "ISO for BIN+CUE fixture has invalid sector geometry");

    const std::array<char, raw_sector_size> framing{};
    constexpr std::size_t split_sector = 17;
    const auto write_tracks = [&](const std::filesystem::path& path,
                                  std::size_t first_sector,
                                  std::size_t end_sector) {
        std::ofstream binary(path, std::ios::binary);
        require(static_cast<bool>(binary),
                "failed to create BIN+CUE binary fixture");
        for (std::size_t sector = first_sector; sector < end_sector; ++sector) {
            const auto offset = sector * payload_size;
            binary.write(framing.data(), 16);
            binary.write(iso.data() + offset, payload_size);
            binary.write(framing.data() + 16,
                         raw_sector_size - 16 - payload_size);
        }
        require(static_cast<bool>(binary),
                "failed to write BIN+CUE binary fixture");
    };
    const auto total_sectors = iso.size() / payload_size;
    require(total_sectors > split_sector,
            "ISO fixture is too small for multi-file BIN+CUE test");
    write_tracks(bin_path, 0, split_sector);
    write_tracks(second_bin_path, split_sector, total_sectors);

    std::ofstream cue(cue_path, std::ios::binary);
    cue << "FILE \"" << bin_path.filename().string() << "\" BINARY\n"
           "  TRACK 01 MODE1/2352\n"
           "    INDEX 01 00:00:00\n"
           "FILE \"" << second_bin_path.filename().string() << "\" BINARY\n"
           "  TRACK 02 MODE1/2352\n"
           "    INDEX 01 00:00:00\n";
    require(static_cast<bool>(cue), "failed to create BIN+CUE descriptor fixture");
}

void make_vmdk_sparse_fixture(const std::filesystem::path& source_path,
                              const std::filesystem::path& vmdk_path) {
    constexpr std::uint64_t sector_size = 512;
    constexpr std::uint64_t grain_size_sectors = 4;
    constexpr std::uint32_t entries_per_table = 512;
    std::ifstream source(source_path, std::ios::binary);
    require(static_cast<bool>(source), "failed to open ISO for VMDK fixture");
    std::vector<char> virtual_disk(
        (std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    require(!virtual_disk.empty(), "ISO fixture for VMDK is empty");

    const auto capacity_sectors =
        (virtual_disk.size() + sector_size - 1) / sector_size;
    const auto grain_count =
        (capacity_sectors + grain_size_sectors - 1) / grain_size_sectors;
    const auto grain_directory_entries =
        (grain_count + entries_per_table - 1) / entries_per_table;
    const auto data_start_sector = 3 + grain_directory_entries +
        (grain_directory_entries * entries_per_table * sizeof(std::uint32_t) +
         sector_size - 1) / sector_size;
    const auto grain_bytes = grain_size_sectors * sector_size;
    std::vector<char> vmdk(static_cast<std::size_t>(
        (data_start_sector + grain_count * grain_size_sectors) * sector_size));
    const auto put_u32 = [&vmdk](std::size_t offset, std::uint32_t value) {
        for (unsigned int index = 0; index < 4; ++index) {
            vmdk[offset + index] =
                static_cast<char>((value >> (index * 8U)) & 0xffU);
        }
    };
    const auto put_u64 = [&vmdk, &put_u32](std::size_t offset, std::uint64_t value) {
        put_u32(offset, static_cast<std::uint32_t>(value));
        put_u32(offset + 4, static_cast<std::uint32_t>(value >> 32U));
    };
    put_u32(0, 0x564d444bU);
    put_u32(4, 1);
    put_u64(12, capacity_sectors);
    put_u64(20, grain_size_sectors);
    put_u32(44, entries_per_table);
    put_u64(56, 1);
    put_u64(64, data_start_sector);
    put_u32(static_cast<std::size_t>(sector_size), 2);
    for (std::uint64_t grain = 0; grain < grain_count; ++grain) {
        const auto grain_sector = data_start_sector + grain * grain_size_sectors;
        put_u32(static_cast<std::size_t>(2 * sector_size + grain * sizeof(std::uint32_t)),
                static_cast<std::uint32_t>(grain_sector));
        const auto source_offset = static_cast<std::size_t>(grain * grain_bytes);
        const auto bytes_to_copy = std::min<std::size_t>(
            static_cast<std::size_t>(grain_bytes),
            virtual_disk.size() - source_offset);
        std::copy_n(virtual_disk.begin() + static_cast<std::ptrdiff_t>(source_offset),
                    bytes_to_copy,
                    vmdk.begin() + static_cast<std::ptrdiff_t>(grain_sector * sector_size));
    }
    std::ofstream output(vmdk_path, std::ios::binary);
    output.write(vmdk.data(), static_cast<std::streamsize>(vmdk.size()));
    require(static_cast<bool>(output), "failed to create sparse VMDK fixture");
}

void make_vmdk_multi_flat_fixture(
    const std::filesystem::path& iso_path,
    const std::filesystem::path& descriptor_path,
    const std::filesystem::path& first_extent_path,
    const std::filesystem::path& second_extent_path) {
    constexpr std::size_t sector_size = 2048;
    constexpr std::size_t split_sector = 21;
    std::ifstream input(iso_path, std::ios::binary);
    require(static_cast<bool>(input), "failed to open ISO for VMDK FLAT fixture");
    std::vector<char> image(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    require(image.size() > split_sector * sector_size &&
                image.size() % 512 == 0,
            "ISO for VMDK FLAT fixture has invalid sector geometry");
    {
        std::ofstream first(first_extent_path, std::ios::binary);
        first.write(image.data(),
                    static_cast<std::streamsize>(split_sector * sector_size));
        require(static_cast<bool>(first),
                "failed to create first VMDK FLAT extent fixture");
    }
    {
        std::ofstream second(second_extent_path, std::ios::binary);
        second.write(image.data() + split_sector * sector_size,
                     static_cast<std::streamsize>(
                         image.size() - split_sector * sector_size));
        require(static_cast<bool>(second),
                "failed to create second VMDK FLAT extent fixture");
    }
    std::ofstream descriptor(descriptor_path, std::ios::binary);
    constexpr std::size_t vmdk_sector_size = 512;
    const auto first_extent_sectors =
        split_sector * sector_size / vmdk_sector_size;
    const auto total_vmdk_sectors = image.size() / vmdk_sector_size;
    descriptor << "# Disk DescriptorFile\n"
                  "version=1\n"
                  "CID=fffffffe\n"
                  "parentCID=ffffffff\n"
                  "\"createType\" = \"twoGbMaxExtentFlat\"\n"
                  "RW " << first_extent_sectors << " FLAT \""
               << first_extent_path.filename().string() << "\" 0\n"
                  "RW " << total_vmdk_sectors - first_extent_sectors
               << " FLAT \"" << second_extent_path.filename().string()
               << "\" 0\n"
                  "ddb.virtualHWVersion = \"4\"\n";
    require(static_cast<bool>(descriptor),
            "failed to create multi-extent VMDK descriptor fixture");
}

void make_partitioned_iso_fixture(const std::filesystem::path& iso_path,
                                  const std::filesystem::path& output_path) {
    constexpr std::size_t sector_size = 512;
    std::ifstream input(iso_path, std::ios::binary);
    require(static_cast<bool>(input),
            "failed to open ISO for partitioned virtual-disk fixture");
    const std::vector<char> iso(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    require(!iso.empty() && iso.size() % sector_size == 0 &&
                iso.size() / sector_size <=
                    std::numeric_limits<std::uint32_t>::max(),
            "ISO size is invalid for MBR partition fixture");
    std::vector<std::byte> disk(sector_size + iso.size());
    disk[510] = std::byte{0x55};
    disk[511] = std::byte{0xaa};
    disk[446 + 4] = std::byte{0x83};
    write_le32(disk, 446 + 8, 1);
    write_le32(disk, 446 + 12,
               static_cast<std::uint32_t>(iso.size() / sector_size));
    std::transform(iso.begin(), iso.end(), disk.begin() + sector_size,
                   [](char value) {
                       return static_cast<std::byte>(
                           static_cast<unsigned char>(value));
                   });
    std::ofstream output(output_path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(disk.data()),
                 static_cast<std::streamsize>(disk.size()));
    require(static_cast<bool>(output),
            "failed to create partitioned virtual-disk fixture");
}

void make_vdi_fixture(const std::filesystem::path& source_path,
                      const std::filesystem::path& vdi_path) {
    constexpr std::uint64_t sector_size = 512;
    constexpr std::uint32_t block_size = 2048;
    std::ifstream source(source_path, std::ios::binary);
    require(static_cast<bool>(source), "failed to open ISO for VDI fixture");
    const std::vector<char> virtual_disk(
        (std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    require(!virtual_disk.empty() && virtual_disk.size() % block_size == 0,
            "ISO fixture size is invalid for VDI block mapping");
    const auto block_count = virtual_disk.size() / block_size;
    constexpr std::uint32_t block_map_offset = 512;
    constexpr std::uint32_t data_offset = 1024;
    std::vector<char> vdi(
        data_offset + virtual_disk.size(), '\0');
    constexpr std::string_view preheader =
        "<<< Oracle VM VirtualBox Disk Image >>>\n";
    std::copy(preheader.begin(), preheader.end(), vdi.begin());
    const auto put_u32 = [&vdi](std::size_t offset, std::uint32_t value) {
        for (unsigned int index = 0; index < 4; ++index) {
            vdi[offset + index] =
                static_cast<char>((value >> (index * 8U)) & 0xffU);
        }
    };
    const auto put_u64 = [&put_u32](std::size_t offset, std::uint64_t value) {
        put_u32(offset, static_cast<std::uint32_t>(value));
        put_u32(offset + 4, static_cast<std::uint32_t>(value >> 32U));
    };
    put_u32(64, 0xbeda107fU);
    put_u32(68, 0x00010001U);
    put_u32(72, 400);
    put_u32(76, 1);
    put_u32(340, block_map_offset);
    put_u32(344, data_offset);
    put_u32(360, static_cast<std::uint32_t>(sector_size));
    put_u64(368, virtual_disk.size());
    put_u32(376, block_size);
    put_u32(384, static_cast<std::uint32_t>(block_count));
    put_u32(388, static_cast<std::uint32_t>(block_count));
    for (std::size_t index = 0; index < block_count; ++index) {
        put_u32(block_map_offset + index * sizeof(std::uint32_t),
                static_cast<std::uint32_t>(index));
    }
    std::copy(virtual_disk.begin(), virtual_disk.end(),
              vdi.begin() + data_offset);
    std::ofstream output(vdi_path, std::ios::binary);
    output.write(vdi.data(), static_cast<std::streamsize>(vdi.size()));
    require(static_cast<bool>(output), "failed to create VDI fixture");
}

void make_qed_fixture(const std::filesystem::path& source_path,
                      const std::filesystem::path& qed_path) {
    constexpr std::uint32_t cluster_size = 4096;
    constexpr std::uint64_t l1_offset = cluster_size;
    constexpr std::uint64_t l2_offset = 2ULL * cluster_size;
    constexpr std::uint64_t data_offset = 3ULL * cluster_size;
    std::ifstream source(source_path, std::ios::binary);
    require(static_cast<bool>(source), "failed to open ISO for QED fixture");
    const std::vector<char> virtual_disk(
        (std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    require(!virtual_disk.empty(), "ISO fixture for QED is empty");
    const auto cluster_count =
        (virtual_disk.size() + cluster_size - 1) / cluster_size;
    std::vector<char> qed(
        static_cast<std::size_t>(data_offset) + cluster_count * cluster_size);
    const auto put_u32 = [&qed](std::size_t offset, std::uint32_t value) {
        for (unsigned int index = 0; index < 4; ++index) {
            qed[offset + index] =
                static_cast<char>((value >> (index * 8U)) & 0xffU);
        }
    };
    const auto put_u64 = [&put_u32](std::size_t offset, std::uint64_t value) {
        put_u32(offset, static_cast<std::uint32_t>(value));
        put_u32(offset + 4, static_cast<std::uint32_t>(value >> 32U));
    };
    put_u32(0, 0x00444551U);
    put_u32(4, cluster_size);
    put_u32(8, 1);
    put_u32(12, 1);
    put_u64(40, l1_offset);
    put_u64(48, virtual_disk.size());
    put_u64(static_cast<std::size_t>(l1_offset), l2_offset);
    for (std::size_t cluster = 0; cluster < cluster_count; ++cluster) {
        const auto extent_offset = cluster * cluster_size;
        const auto copy_size = std::min<std::size_t>(
            cluster_size, virtual_disk.size() - extent_offset);
        const auto physical_offset =
            static_cast<std::size_t>(data_offset) + cluster * cluster_size;
        put_u64(static_cast<std::size_t>(l2_offset) + cluster * 8,
                physical_offset);
        std::copy_n(virtual_disk.begin() + static_cast<std::ptrdiff_t>(extent_offset),
                    copy_size,
                    qed.begin() + static_cast<std::ptrdiff_t>(physical_offset));
    }
    std::ofstream output(qed_path, std::ios::binary);
    output.write(qed.data(), static_cast<std::streamsize>(qed.size()));
    require(static_cast<bool>(output), "failed to create QED fixture");
}

void make_qcow2_fixture(const std::filesystem::path& source_path,
                        const std::filesystem::path& qcow2_path) {
    constexpr std::uint64_t cluster_size = 4096;
    constexpr std::uint64_t refcount_table_offset = cluster_size;
    constexpr std::uint64_t l1_table_offset = 2 * cluster_size;
    constexpr std::uint64_t l2_table_offset = 3 * cluster_size;
    constexpr std::uint64_t refcount_block_offset = 4 * cluster_size;
    constexpr std::uint64_t first_data_offset = 5 * cluster_size;
    std::ifstream source(source_path, std::ios::binary);
    require(static_cast<bool>(source), "failed to open ISO for QCOW2 fixture");
    const std::vector<char> virtual_disk(
        (std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    require(!virtual_disk.empty(), "ISO fixture for QCOW2 is empty");
    const auto cluster_count =
        (virtual_disk.size() + cluster_size - 1) / cluster_size;
    const auto file_cluster_count = first_data_offset / cluster_size + cluster_count;
    std::vector<char> image(file_cluster_count * cluster_size);
    const auto put_be32 = [&image](std::size_t offset, std::uint32_t value) {
        for (unsigned int index = 0; index < 4; ++index) {
            image[offset + index] =
                static_cast<char>((value >> ((3U - index) * 8U)) & 0xffU);
        }
    };
    const auto put_be64 = [&put_be32](std::size_t offset, std::uint64_t value) {
        put_be32(offset, static_cast<std::uint32_t>(value >> 32U));
        put_be32(offset + 4, static_cast<std::uint32_t>(value));
    };
    put_be32(0, 0x514649fbU);
    put_be32(4, 2);
    put_be32(20, 12);
    put_be64(24, virtual_disk.size());
    put_be32(36, 1);
    put_be64(40, l1_table_offset);
    put_be64(48, refcount_table_offset);
    put_be32(56, 1);
    put_be64(static_cast<std::size_t>(refcount_table_offset),
             refcount_block_offset);
    put_be64(static_cast<std::size_t>(l1_table_offset), l2_table_offset);
    for (std::size_t cluster = 0; cluster < cluster_count; ++cluster) {
        const auto source_offset = cluster * cluster_size;
        const auto physical_offset =
            static_cast<std::size_t>(first_data_offset + cluster * cluster_size);
        put_be64(static_cast<std::size_t>(l2_table_offset) + cluster * 8,
                 physical_offset);
        const auto copy_size = std::min<std::size_t>(
            static_cast<std::size_t>(cluster_size),
            virtual_disk.size() - source_offset);
        std::copy_n(virtual_disk.begin() + static_cast<std::ptrdiff_t>(source_offset),
                    copy_size,
                    image.begin() + static_cast<std::ptrdiff_t>(physical_offset));
    }
    for (std::size_t cluster = 0; cluster < file_cluster_count; ++cluster) {
        const auto offset =
            static_cast<std::size_t>(refcount_block_offset) + cluster * 2;
        image[offset] = 0;
        image[offset + 1] = 1;
    }
    std::ofstream output(qcow2_path, std::ios::binary);
    output.write(image.data(), static_cast<std::streamsize>(image.size()));
    require(static_cast<bool>(output), "failed to create QCOW2 fixture");
}

void make_vhdx_fixture(const std::filesystem::path& source_path,
                       const std::filesystem::path& vhdx_path) {
    constexpr std::uint64_t mib = 1024U * 1024U;
    constexpr std::uint64_t virtual_size = 2 * mib;
    constexpr std::uint64_t bat_offset = 2 * mib;
    constexpr std::uint64_t metadata_offset = 3 * mib;
    constexpr std::uint64_t data_offset = 4 * mib;
    std::ifstream source(source_path, std::ios::binary);
    require(static_cast<bool>(source), "failed to open ISO for VHDX fixture");
    const std::vector<char> virtual_disk(
        (std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    require(!virtual_disk.empty() && virtual_disk.size() <= mib,
            "ISO fixture size is invalid for VHDX");
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
        write_le32(image, offset + 4, crc32c(
            std::span<const std::byte>(image).subspan(offset, 4096), 4));
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
               static_cast<std::uint32_t>(mib));
    write_le64(image, metadata_base + 64U * 1024U + 8, virtual_size);
    write_le32(image, metadata_base + 64U * 1024U + 16, 512);
    write_le64(image, static_cast<std::size_t>(bat_offset), data_offset | 6U);
    std::transform(virtual_disk.begin(), virtual_disk.end(),
                   image.begin() + static_cast<std::ptrdiff_t>(data_offset),
                   [](char value) { return static_cast<std::byte>(value); });
    std::ofstream output(vhdx_path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(image.data()),
                 static_cast<std::streamsize>(image.size()));
    require(static_cast<bool>(output), "failed to create VHDX fixture");
}

void make_joliet_fixture(const std::filesystem::path& path) {
    constexpr std::size_t block_size = 2048;
    constexpr std::uint32_t sector_count = 22;
    std::vector<char> image(static_cast<std::size_t>(sector_count) * block_size);
    auto* primary = image.data() + 16 * block_size;
    primary[0] = 1;
    std::copy_n("CD001", 5, primary + 1);
    primary[6] = 1;
    write_dual32(primary + 80, sector_count);
    write_dual16(primary + 128, static_cast<std::uint16_t>(block_size));
    const auto primary_root = make_directory_record(20, block_size, {0}, true);
    std::copy(primary_root.begin(), primary_root.end(), primary + 156);

    auto* supplementary = image.data() + 17 * block_size;
    supplementary[0] = 2;
    std::copy_n("CD001", 5, supplementary + 1);
    supplementary[6] = 1;
    supplementary[88] = '%';
    supplementary[89] = '/';
    supplementary[90] = 'E';
    write_dual32(supplementary + 80, sector_count);
    write_dual16(supplementary + 128, static_cast<std::uint16_t>(block_size));
    std::copy(primary_root.begin(), primary_root.end(), supplementary + 156);

    auto* terminator = image.data() + 18 * block_size;
    terminator[0] = static_cast<char>(255);
    std::copy_n("CD001", 5, terminator + 1);
    terminator[6] = 1;

    auto* directory = image.data() + 20 * block_size;
    const std::vector<unsigned char> joliet_filename{
        0x4f, 0x60, 0x59, 0x7d, 0x00, 0x2e, 0x00, 0x54,
        0x00, 0x58, 0x00, 0x54, 0x00, 0x3b, 0x00, 0x31
    };
    std::size_t position = 0;
    for (const auto& record : {
             make_directory_record(20, block_size, {0}, true),
             make_directory_record(20, block_size, {1}, true),
             make_directory_record(21, 12, joliet_filename, false)}) {
        std::copy(record.begin(), record.end(), directory + position);
        position += record.size();
    }
    const std::string payload = "joliet-data!";
    std::copy(payload.begin(), payload.end(), image.begin() + 21 * block_size);
    std::ofstream output(path, std::ios::binary);
    output.write(image.data(), static_cast<std::streamsize>(image.size()));
    if (!output) {
        throw std::runtime_error("failed to create Joliet ISO fixture");
    }
}

void make_multi_extent_fixture(const std::filesystem::path& path) {
    constexpr std::size_t block_size = 2048;
    constexpr std::uint32_t sector_count = 24;
    std::vector<char> image(static_cast<std::size_t>(sector_count) * block_size);
    auto* primary = image.data() + 16 * block_size;
    primary[0] = 1;
    std::copy_n("CD001", 5, primary + 1);
    primary[6] = 1;
    write_dual32(primary + 80, sector_count);
    write_dual16(primary + 128, static_cast<std::uint16_t>(block_size));
    const auto root = make_directory_record(20, block_size, {0}, true);
    std::copy(root.begin(), root.end(), primary + 156);

    auto* terminator = image.data() + 17 * block_size;
    terminator[0] = static_cast<char>(255);
    std::copy_n("CD001", 5, terminator + 1);
    terminator[6] = 1;

    auto* directory = image.data() + 20 * block_size;
    std::size_t position = 0;
    const auto first_split_extent =
        make_directory_record(22, 5, {'S','P','L','I','T','.','B','I','N',';','1'}, false);
    auto first_with_more = first_split_extent;
    first_with_more[25] = static_cast<char>(
        static_cast<unsigned char>(first_with_more[25]) | 0x80U);
    for (const auto& record : {
             make_directory_record(20, block_size, {0}, true),
             make_directory_record(20, block_size, {1}, true),
             make_directory_record(21, 12,
                 {'H','E','L','L','O','.','T','X','T',';','1'}, false),
             first_with_more,
             make_directory_record(23, 6,
                 {'S','P','L','I','T','.','B','I','N',';','1'}, false)}) {
        std::copy(record.begin(), record.end(), directory + position);
        position += record.size();
    }
    std::copy_n("fixture-data", 12, image.begin() + 21 * block_size);
    std::copy_n("alpha", 5, image.begin() + 22 * block_size);
    std::copy_n("omega!", 6, image.begin() + 23 * block_size);
    std::ofstream output(path, std::ios::binary);
    output.write(image.data(), static_cast<std::streamsize>(image.size()));
    if (!output) {
        throw std::runtime_error("failed to create multi-extent ISO fixture");
    }
}

} // namespace

void run_all_integration_tests() {
    const TemporaryTestDirectory test_directory;
    const auto gho_path =
        test_directory.path() / "uife-test-partition.gho";
    const auto gho_span_primary_path =
        test_directory.path() / "uife-test-span-case.gho";
    const auto gho_span_path =
        test_directory.path() / "uife-test-span-case1.ghs";
    const auto gho_second_span_path =
        test_directory.path() / "uife-test-span-case10.ghs";
    const auto gho_middle_span_path =
        test_directory.path() / "uife-test-span-case2.ghs";
    const auto gho_unrelated_span_path =
        test_directory.path() / "uife-test-span-caseunrelated.ghs";
    const auto gho_iso_path =
        test_directory.path() / "uife-test-iso.gho";
    const auto dmg_path =
        test_directory.path() / "uife-test-valid.dmg";
    const auto dmg_zlib_path =
        test_directory.path() / "uife-test-zlib.dmg";
    const auto dmg_unsupported_path =
        test_directory.path() / "uife-test-unsupported.dmg";
    const auto dmg_segmented_path =
        test_directory.path() / "uife-test-segmented.dmg";
    const auto dmg_iso_path =
        test_directory.path() / "uife-test-iso.dmg";
    const auto btrfs_send_path =
        test_directory.path() / "uife-test-btrfs-send.stream";
    const auto btrfs_send_corrupt_path =
        test_directory.path() /
        "uife-test-btrfs-send-corrupt.stream";
    const auto btrfs_send_output_path =
        test_directory.path() / "uife-test-btrfs-send-output";
    const auto btrfs_v1_path =
        test_directory.path() / "uife-test-btrfs-send-v1.stream";
    const auto btrfs_v3_path =
        test_directory.path() / "uife-test-btrfs-send-v3.stream";
    const auto refs_path =
        test_directory.path() / "uife-test-refs.img";
#ifdef UIFE_WITH_LIBWIM
    const auto wim_path =
        test_directory.path() / "uife-test-two-images.wim";
    const auto split_wim_path =
        test_directory.path() / "uife-test-two-images.swm";
    const auto wim_first_source =
        test_directory.path() / "uife-test-wim-first";
    const auto wim_second_source =
        test_directory.path() / "uife-test-wim-second";
#endif
    const auto iso_path = test_directory.path() / "uife-test.iso";
    const auto rock_ridge_iso_path =
        test_directory.path() / "uife-test-rock-ridge.iso";
    const auto rock_ridge_bad_ce_path =
        test_directory.path() / "uife-test-rock-ridge-bad-ce.iso";
    const auto vdi_path =
        test_directory.path() / "uife-test.vdi";
    const auto vmdk_path =
        test_directory.path() / "uife-test-sparse.vmdk";
    const auto flat_vmdk_path =
        test_directory.path() / "uife-test-multi-flat.vmdk";
    const auto flat_vmdk_first =
        test_directory.path() / "uife-test-flat-first.bin";
    const auto flat_vmdk_second =
        test_directory.path() / "uife-test-flat-second.bin";
    const auto qcow2_path =
        test_directory.path() / "uife-test.qcow2";
    const auto qed_path =
        test_directory.path() / "uife-test.qed";
    const auto vhdx_path =
        test_directory.path() / "uife-test.vhdx";
    const auto joliet_path =
        test_directory.path() / "uife-test-joliet.iso";
    const auto multi_extent_path =
        test_directory.path() / "uife-test-multi-extent.iso";
    const auto partitioned_iso_path =
        test_directory.path() / "uife-test-partitioned.raw";
    const auto partitioned_vmdk_path =
        test_directory.path() / "uife-test-partitioned.vmdk";
    const auto partitioned_vmdk_first =
        test_directory.path() / "uife-test-partitioned-first.bin";
    const auto partitioned_vmdk_second =
        test_directory.path() / "uife-test-partitioned-second.bin";
    const auto cue_bin_path =
        test_directory.path() / "uife-test-disc.bin";
    const auto cue_second_bin_path =
        test_directory.path() / "uife-test-disc-2.bin";
    const auto cue_path =
        test_directory.path() / "uife-test-disc.cue";
    const auto timestamp_output_path =
        test_directory.path() / "uife-iso-timestamp-output";
    const auto corrupt_iso_descriptor_path =
        test_directory.path() / "uife-test-corrupt-iso-descriptor.iso";
    const auto corrupt_iso_record_path =
        test_directory.path() / "uife-test-corrupt-iso-record.iso";
    const auto truncated_iso_volume_path =
        test_directory.path() / "uife-test-truncated-iso-volume.iso";
    try {
        test_gho_driver(gho_path);
        test_gho_driver(gho_path, 2);
        test_gho_driver(gho_path, 3);
        test_gho_driver(gho_path, 3, true);
        test_gho_driver(gho_path, 0, false, true);
        test_gho_driver(gho_path, 0, false, false, true);
        test_gho_driver(gho_path, 3, false, false, true);
        test_gho_rejects_unsupported_variants(gho_path);
        test_gho_fast_lz_matches(gho_path);
        test_gho_rejects_corrupt_zlib_block(gho_path);
        test_gho_record_signature_inside_block(gho_path);
        test_gho_multi_partition(gho_path);
        test_gho_rejects_mismatched_multi_partition_mbr(gho_path);
        test_gho_spanned_image(gho_span_primary_path, gho_span_path,
                               gho_middle_span_path, gho_second_span_path,
                               gho_unrelated_span_path);
        test_gho_rejects_invalid_span_header(
            gho_span_primary_path, gho_span_path, gho_middle_span_path,
            gho_second_span_path);
        test_dmg_driver(dmg_path);
        test_dmg_driver(dmg_zlib_path, true);
        test_dmg_driver(dmg_path, false, true);
        test_dmg_rejects_malformed_binary_plist(dmg_unsupported_path);
        test_dmg_rejects_unsupported_run(dmg_unsupported_path);
        test_dmg_rejects_segmented_udif(dmg_segmented_path);
        test_btrfs_send_parser(btrfs_send_path, btrfs_send_corrupt_path,
                               btrfs_send_output_path);
        test_btrfs_versioned_send_parser(btrfs_v1_path, 1);
        test_btrfs_versioned_send_parser(btrfs_v3_path, 3);
        test_refs_detection(refs_path);
#ifdef UIFE_WITH_LIBWIM
        remove_test_output(wim_first_source);
        remove_test_output(wim_second_source);
        std::filesystem::remove(wim_path);
        make_wim_fixture(wim_path, wim_first_source, wim_second_source);
        test_wim_parser(wim_path);
        make_split_wim_fixture(wim_path, split_wim_path);
        test_split_wim_parser(split_wim_path);
        std::filesystem::remove(wim_path);
        std::filesystem::remove(split_wim_path);
        remove_test_output(wim_first_source);
        remove_test_output(wim_second_source);
#endif
        make_iso_fixture(iso_path);
        make_rock_ridge_iso_fixture(rock_ridge_iso_path);
        test_rock_ridge_iso_parser(rock_ridge_iso_path);
        make_rock_ridge_iso_fixture(rock_ridge_bad_ce_path, true);
        test_rock_ridge_ce_rejects_out_of_bounds(rock_ridge_bad_ce_path);
        test_dmg_iso_browsing(iso_path, dmg_iso_path);
        test_gho_iso_browsing(iso_path, gho_iso_path);
        test_gho_iso_browsing(iso_path, gho_iso_path, 3);
        test_iso_integrity(iso_path, corrupt_iso_descriptor_path,
                           corrupt_iso_record_path, truncated_iso_volume_path);

        make_joliet_fixture(joliet_path);
        {
            const uife::iso9660::Iso9660 joliet(joliet_path);
            const auto entries = joliet.list("/");
            require(entries.size() == 1 && entries.front().name == "你好.TXT" &&
                        entries.front().size == 12,
                    "Joliet UTF-16 filename was not decoded correctly");
            std::array<char, 12> contents{};
            require(joliet.read_file("/你好.txt", 0, contents.data(), contents.size()) ==
                        contents.size() &&
                        std::string(contents.data(), contents.size()) == "joliet-data!",
                    "Joliet case-insensitive file read failed");
        }

        make_multi_extent_fixture(multi_extent_path);
        {
            const uife::iso9660::Iso9660 multi_extent(multi_extent_path);
            const auto entries = multi_extent.list("/");
            const auto split_file = std::find_if(entries.begin(), entries.end(),
                [](const uife::iso9660::IsoEntry& entry) {
                    return entry.name == "SPLIT.BIN";
                });
            require(split_file != entries.end() && split_file->size == 11 &&
                        split_file->extents.size() == 2,
                    "ISO multi-extent file records were not combined");
            std::array<char, 11> contents{};
            require(multi_extent.read_file("/split.bin", 0,
                        contents.data(), contents.size()) == contents.size() &&
                        std::string(contents.data(), contents.size()) == "alphaomega!",
                    "ISO multi-extent file data was not read in extent order");
        }
        {
            auto image_driver =
                uife::DriverFactory::instance().create(multi_extent_path);
            std::shared_ptr<uife::IImageDriver> shared_driver(std::move(image_driver));
            auto parser = uife::FsParserFactory::instance().create(*shared_driver, 0);
            parser->mount(shared_driver, 0, shared_driver->get_size());
            const uife::VFS vfs(*parser);
            std::array<std::byte, 11> contents{};
            require(vfs.read("/SPLIT.BIN", 0, contents) == contents.size() &&
                        std::string(reinterpret_cast<const char*>(contents.data()),
                                    contents.size()) == "alphaomega!",
                    "UIFE VFS did not read ISO multi-extent file data");
        }

        {
            const auto driver_info = uife::DriverFactory::instance().identify(iso_path);
            require(driver_info.format == "ISO9660" && driver_info.supported,
                    "UIFE did not register ISO9660 as a readable image");
            const auto original_image_hash = hash_bytes(read_fixture(iso_path));
            auto image_driver = uife::DriverFactory::instance().create(iso_path);
            std::shared_ptr<uife::IImageDriver> shared_driver(std::move(image_driver));
            auto parser = uife::FsParserFactory::instance().create(*shared_driver, 0);
            parser->mount(shared_driver, 0, shared_driver->get_size());
            const uife::VFS vfs(*parser);
            const auto vfs_entries = vfs.list_dir("/");
            require(vfs_entries.size() == 1 &&
                        vfs_entries.front().name == "HELLO.TXT" &&
                        vfs_entries.front().metadata.size == 12 &&
                        vfs_entries.front().metadata.has_modified_time &&
                        vfs_entries.front().metadata.modified ==
                            std::chrono::system_clock::time_point{
                                std::chrono::sys_days{
                                    std::chrono::year{2026} /
                                    std::chrono::October / 26} +
                                std::chrono::hours{11}},
                    "UIFE ISO9660 VFS directory listing failed");
            const auto file_metadata = vfs.stat("/HELLO.TXT");
            require(file_metadata.metadata.has_modified_time &&
                        file_metadata.metadata.modified ==
                            vfs_entries.front().metadata.modified,
                    "UIFE ISO9660 metadata lookup lost the recording timestamp");
            std::array<std::byte, 12> vfs_contents{};
            require(vfs.read("/HELLO.TXT", 0, vfs_contents) == vfs_contents.size() &&
                        std::string(reinterpret_cast<const char*>(vfs_contents.data()),
                                    vfs_contents.size()) == "fixture-data",
                    "UIFE ISO9660 VFS file read failed");

            remove_test_output(timestamp_output_path);
            uife::ExtractOptions extract_options;
            extract_options.output_directory = timestamp_output_path;
            const auto extracted =
                uife::Extractor::extract(vfs, "/HELLO.TXT", extract_options);
            const auto extracted_file = timestamp_output_path / "HELLO.TXT";
            require(extracted.size() == 1 && extracted.front().size == 12 &&
                        std::filesystem::file_size(extracted_file) == 12,
                    "UIFE did not extract the ISO9660 file with its expected size");
            const auto expected_system_time =
                vfs_entries.front().metadata.modified;
            // MSVC 的 file_time_type::clock 不提供 from_sys（libstdc++ 提供），
            // 因此用"两个时钟当前值之差"换算，不依赖具体实现的转换接口。
            const auto now_system = std::chrono::system_clock::now();
            const auto now_file =
                std::filesystem::file_time_type::clock::now();
            const auto expected_file_time =
                now_file + (expected_system_time - now_system);
            const auto actual_file_time =
                std::filesystem::last_write_time(extracted_file);
            const auto timestamp_delta = actual_file_time > expected_file_time
                ? actual_file_time - expected_file_time
                : expected_file_time - actual_file_time;
            require(timestamp_delta <= std::chrono::seconds{2},
                    "UIFE extraction did not preserve the ISO9660 modification timestamp");
            require(hash_bytes(read_fixture(iso_path)) == original_image_hash,
                    "UIFE modified the source ISO while browsing or extracting it");
            std::filesystem::permissions(
                extracted_file, std::filesystem::perms::owner_all,
                std::filesystem::perm_options::add);
            remove_test_output(timestamp_output_path);
        }

        make_mode1_cue_fixture(iso_path, cue_bin_path, cue_second_bin_path,
                               cue_path);
        {
            const auto image_info = uife::DriverFactory::instance().identify(cue_path);
            require(image_info.format == "BIN+CUE" && image_info.supported,
                    "UIFE did not identify BIN+CUE as a readable image");
            auto image_driver = uife::DriverFactory::instance().create(cue_path);
            std::shared_ptr<uife::IImageDriver> shared_driver(
                std::move(image_driver));
            auto parser =
                uife::FsParserFactory::instance().create(*shared_driver, 0);
            parser->mount(shared_driver, 0, shared_driver->get_size());
            const uife::VFS vfs(*parser);
            const auto entries = vfs.list_dir("/");
            require(entries.size() == 1 && entries.front().name == "HELLO.TXT",
                    "UIFE could not list ISO9660 files through BIN+CUE");
            std::array<std::byte, 12> contents{};
            require(vfs.read("/HELLO.TXT", 0, contents) == contents.size() &&
                        std::string(reinterpret_cast<const char*>(contents.data()),
                                    contents.size()) == "fixture-data",
                    "UIFE could not read ISO9660 file data through BIN+CUE");
        }

        make_vmdk_sparse_fixture(iso_path, vmdk_path);
        {
            const auto image_info =
                uife::DriverFactory::instance().identify(vmdk_path);
            require(image_info.format == "VMDK" && image_info.supported,
                    "UIFE did not identify sparse VMDK as readable");
            auto image_driver =
                uife::DriverFactory::instance().create(vmdk_path);
            std::shared_ptr<uife::IImageDriver> shared_driver(
                std::move(image_driver));
            auto parser =
                uife::FsParserFactory::instance().create(*shared_driver, 0);
            parser->mount(shared_driver, 0, shared_driver->get_size());
            const uife::VFS vfs(*parser);
            const auto entries = vfs.list_dir("/");
            require(entries.size() == 1 && entries.front().name == "HELLO.TXT",
                    "UIFE could not list ISO files through the VMDK image driver");
            std::array<std::byte, 12> contents{};
            require(vfs.read("/HELLO.TXT", 0, contents) == contents.size() &&
                        std::string(reinterpret_cast<const char*>(contents.data()),
                                    contents.size()) == "fixture-data",
                    "UIFE could not read ISO file data through the VMDK image driver");
        }

        make_vmdk_multi_flat_fixture(iso_path, flat_vmdk_path,
                                     flat_vmdk_first, flat_vmdk_second);
        {
            const auto image_info =
                uife::DriverFactory::instance().identify(flat_vmdk_path);
            require(image_info.format == "VMDK" && image_info.supported,
                    "UIFE did not identify multi-extent FLAT VMDK as readable");
            auto image_driver =
                uife::DriverFactory::instance().create(flat_vmdk_path);
            std::shared_ptr<uife::IImageDriver> shared_driver(
                std::move(image_driver));
            auto parser =
                uife::FsParserFactory::instance().create(*shared_driver, 0);
            parser->mount(shared_driver, 0, shared_driver->get_size());
            const uife::VFS vfs(*parser);
            const auto entries = vfs.list_dir("/");
            require(entries.size() == 1 && entries.front().name == "HELLO.TXT",
                    "UIFE could not list ISO9660 files through multi-extent VMDK");
            std::array<std::byte, 12> contents{};
            require(vfs.read("/HELLO.TXT", 0, contents) == contents.size() &&
                        std::string(reinterpret_cast<const char*>(contents.data()),
                                    contents.size()) == "fixture-data",
                    "UIFE could not read ISO data from the second VMDK FLAT extent");
        }

        make_vdi_fixture(iso_path, vdi_path);
        {
            const auto image_info = uife::DriverFactory::instance().identify(vdi_path);
            require(image_info.format == "VDI" && image_info.supported,
                    "UIFE did not identify VDI as a readable image");
            auto image_driver = uife::DriverFactory::instance().create(vdi_path);
            std::shared_ptr<uife::IImageDriver> shared_driver(
                std::move(image_driver));
            auto parser =
                uife::FsParserFactory::instance().create(*shared_driver, 0);
            parser->mount(shared_driver, 0, shared_driver->get_size());
            const uife::VFS vfs(*parser);
            const auto entries = vfs.list_dir("/");
            require(entries.size() == 1 && entries.front().name == "HELLO.TXT",
                    "UIFE could not list ISO files through the VDI image driver");
            std::array<std::byte, 12> contents{};
            require(vfs.read("/HELLO.TXT", 0, contents) == contents.size() &&
                        std::string(reinterpret_cast<const char*>(contents.data()),
                                    contents.size()) == "fixture-data",
                    "UIFE could not read ISO file data through the VDI image driver");
        }

        make_qcow2_fixture(iso_path, qcow2_path);
        {
            const auto image_info =
                uife::DriverFactory::instance().identify(qcow2_path);
            require(image_info.format == "QCOW2" && image_info.supported,
                    "UIFE did not identify QCOW2 as a readable image");
            auto image_driver =
                uife::DriverFactory::instance().create(qcow2_path);
            std::shared_ptr<uife::IImageDriver> shared_driver(
                std::move(image_driver));
            auto parser =
                uife::FsParserFactory::instance().create(*shared_driver, 0);
            parser->mount(shared_driver, 0, shared_driver->get_size());
            const uife::VFS vfs(*parser);
            const auto entries = vfs.list_dir("/");
            require(entries.size() == 1 && entries.front().name == "HELLO.TXT",
                    "UIFE could not list ISO files through QCOW2");
            std::array<std::byte, 12> contents{};
            require(vfs.read("/HELLO.TXT", 0, contents) == contents.size() &&
                        std::string(reinterpret_cast<const char*>(contents.data()),
                                    contents.size()) == "fixture-data",
                    "UIFE could not read ISO file data through QCOW2");
        }

        make_vhdx_fixture(iso_path, vhdx_path);
        {
            const auto image_info =
                uife::DriverFactory::instance().identify(vhdx_path);
            require(image_info.format == "VHDX" && image_info.supported,
                    "UIFE did not identify VHDX as a readable image");
            auto image_driver =
                uife::DriverFactory::instance().create(vhdx_path);
            std::shared_ptr<uife::IImageDriver> shared_driver(
                std::move(image_driver));
            auto parser =
                uife::FsParserFactory::instance().create(*shared_driver, 0);
            parser->mount(shared_driver, 0, shared_driver->get_size());
            const uife::VFS vfs(*parser);
            const auto entries = vfs.list_dir("/");
            require(entries.size() == 1 && entries.front().name == "HELLO.TXT",
                    "UIFE could not list ISO files through VHDX");
            std::array<std::byte, 12> contents{};
            require(vfs.read("/HELLO.TXT", 0, contents) == contents.size() &&
                        std::string(reinterpret_cast<const char*>(contents.data()),
                                    contents.size()) == "fixture-data",
                    "UIFE could not read ISO file data through VHDX");
        }

        make_qed_fixture(iso_path, qed_path);
        {
            const auto image_info = uife::DriverFactory::instance().identify(qed_path);
            require(image_info.format == "QED" && image_info.supported,
                    "UIFE did not identify QED as a readable image");
            auto image_driver = uife::DriverFactory::instance().create(qed_path);
            std::shared_ptr<uife::IImageDriver> shared_driver(
                std::move(image_driver));
            auto parser =
                uife::FsParserFactory::instance().create(*shared_driver, 0);
            parser->mount(shared_driver, 0, shared_driver->get_size());
            const uife::VFS vfs(*parser);
            const auto entries = vfs.list_dir("/");
            require(entries.size() == 1 && entries.front().name == "HELLO.TXT",
                    "UIFE could not list ISO files through the QED image driver");
            std::array<std::byte, 12> contents{};
            require(vfs.read("/HELLO.TXT", 0, contents) == contents.size() &&
                        std::string(reinterpret_cast<const char*>(contents.data()),
                                    contents.size()) == "fixture-data",
                    "UIFE could not read ISO file data through the QED image driver");
        }

        make_partitioned_iso_fixture(iso_path, partitioned_iso_path);
        {
            make_vmdk_multi_flat_fixture(
                partitioned_iso_path, partitioned_vmdk_path,
                partitioned_vmdk_first, partitioned_vmdk_second);
            auto image_driver = uife::DriverFactory::instance().create(
                partitioned_vmdk_path);
            std::shared_ptr<uife::IImageDriver> shared_driver(std::move(image_driver));
            const auto partitions = uife::scan_partitions(*shared_driver);
            require(partitions.size() == 1 &&
                        partitions.front().byte_offset == 512 &&
                        partitions.front().byte_length ==
                            shared_driver->get_size() - 512,
                    "UIFE did not scan MBR partitions through multi-extent VMDK");
            require(uife::FsParserFactory::instance().supports(
                        *shared_driver, partitions.front().byte_offset),
                    "UIFE did not detect ISO9660 inside a VMDK partition");
            auto parser = uife::FsParserFactory::instance().create(
                *shared_driver, partitions.front().byte_offset);
            parser->mount(shared_driver, partitions.front().byte_offset,
                          partitions.front().byte_length);
            const uife::VFS vfs(*parser);
            const auto entries = vfs.list_dir("/");
            require(entries.size() == 1 && entries.front().name == "HELLO.TXT",
                    "UIFE could not list files inside a partitioned VMDK");
            std::array<std::byte, 12> contents{};
            require(vfs.read("/HELLO.TXT", 0, contents) == contents.size() &&
                        std::string(reinterpret_cast<const char*>(contents.data()),
                                    contents.size()) == "fixture-data",
                    "UIFE could not read file data inside a partitioned VMDK");
        }

        // 收尾前对所有 fixture 做一次只读不变性清扫：每张镜像都跑
        // 打开→读扇区→扫分区→挂载→列目录→读文件→整卷导出，然后比对整个
        // 工作目录的字节快照。任何驱动偷偷写源文件都会在这里被抓出来。
        {
            std::vector<uife_test::FixtureTarget> immutability_targets{
                {gho_path, "gho"},
                {gho_iso_path, "gho-iso"},
                {gho_span_primary_path, "gho-span"},
                {dmg_path, "dmg"},
                {dmg_zlib_path, "dmg-zlib"},
                {dmg_iso_path, "dmg-iso"},
                {btrfs_send_path, "btrfs-send"},
                {refs_path, "refs"},
                {rock_ridge_iso_path, "rock-ridge-iso"},
                {vdi_path, "vdi"},
                {vmdk_path, "vmdk-sparse"},
                {flat_vmdk_path, "vmdk-flat"},
                {qcow2_path, "qcow2"},
                {qed_path, "qed"},
                {vhdx_path, "vhdx"},
                {joliet_path, "joliet-iso"},
                {multi_extent_path, "multi-extent-iso"},
                {partitioned_iso_path, "partitioned-raw"},
                {partitioned_vmdk_path, "partitioned-vmdk"},
                {cue_bin_path, "cue-bin"},
            };
#ifdef UIFE_WITH_LIBWIM
            immutability_targets.push_back({wim_path, "wim"});
            immutability_targets.push_back({split_wim_path, "split-wim"});
#endif
            uife_test::check_source_immutability(
                test_directory.path(),
                test_directory.path() / "uife-immutability-output",
                immutability_targets);
            uife_test::check_truncated_inputs(test_directory.path(),
                                              immutability_targets);
            uife_test::check_malformed_inputs(test_directory.path(),
                                              immutability_targets);
        }

        std::filesystem::remove(iso_path);
        std::filesystem::remove(vdi_path);
        std::filesystem::remove(vmdk_path);
        std::filesystem::remove(flat_vmdk_path);
        std::filesystem::remove(flat_vmdk_first);
        std::filesystem::remove(flat_vmdk_second);
        std::filesystem::remove(qcow2_path);
        std::filesystem::remove(vhdx_path);
        std::filesystem::remove(qed_path);
        std::filesystem::remove(joliet_path);
        std::filesystem::remove(multi_extent_path);
        std::filesystem::remove(partitioned_iso_path);
        std::filesystem::remove(partitioned_vmdk_path);
        std::filesystem::remove(partitioned_vmdk_first);
        std::filesystem::remove(partitioned_vmdk_second);
        std::filesystem::remove(cue_bin_path);
        std::filesystem::remove(cue_second_bin_path);
        std::filesystem::remove(cue_path);
        std::filesystem::remove(btrfs_send_path);
        std::filesystem::remove(btrfs_send_corrupt_path);
        std::filesystem::remove(btrfs_v1_path);
        std::filesystem::remove(btrfs_v3_path);
        std::filesystem::remove(gho_path);
        std::filesystem::remove(gho_span_primary_path);
        std::filesystem::remove(gho_span_path);
        std::filesystem::remove(gho_second_span_path);
        std::filesystem::remove(gho_middle_span_path);
        std::filesystem::remove(gho_unrelated_span_path);
        std::filesystem::remove(gho_iso_path);
        std::filesystem::remove(dmg_path);
        std::filesystem::remove(dmg_zlib_path);
        std::filesystem::remove(dmg_unsupported_path);
        std::filesystem::remove(dmg_segmented_path);
        std::filesystem::remove(dmg_iso_path);
        std::filesystem::remove(refs_path);
        std::filesystem::remove(rock_ridge_iso_path);
        std::filesystem::remove(rock_ridge_bad_ce_path);
        remove_test_output(btrfs_send_output_path);
#ifdef UIFE_WITH_LIBWIM
        std::filesystem::remove(wim_path);
        std::filesystem::remove(split_wim_path);
        remove_test_output(wim_first_source);
        remove_test_output(wim_second_source);
#endif
        remove_test_output(timestamp_output_path);
        std::filesystem::remove(corrupt_iso_descriptor_path);
        std::filesystem::remove(corrupt_iso_record_path);
        std::filesystem::remove(truncated_iso_volume_path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(gho_path, ignored);
        std::filesystem::remove(gho_span_primary_path, ignored);
        std::filesystem::remove(gho_span_path, ignored);
        std::filesystem::remove(gho_second_span_path, ignored);
        std::filesystem::remove(gho_middle_span_path, ignored);
        std::filesystem::remove(gho_unrelated_span_path, ignored);
        std::filesystem::remove(gho_iso_path, ignored);
        std::filesystem::remove(iso_path, ignored);
        std::filesystem::remove(rock_ridge_iso_path, ignored);
        std::filesystem::remove(rock_ridge_bad_ce_path, ignored);
        std::filesystem::remove(vdi_path, ignored);
        std::filesystem::remove(vmdk_path, ignored);
        std::filesystem::remove(flat_vmdk_path, ignored);
        std::filesystem::remove(flat_vmdk_first, ignored);
        std::filesystem::remove(flat_vmdk_second, ignored);
        std::filesystem::remove(qcow2_path, ignored);
        std::filesystem::remove(vhdx_path, ignored);
        std::filesystem::remove(qed_path, ignored);
        std::filesystem::remove(joliet_path, ignored);
        std::filesystem::remove(multi_extent_path, ignored);
        std::filesystem::remove(partitioned_iso_path, ignored);
        std::filesystem::remove(partitioned_vmdk_path, ignored);
        std::filesystem::remove(partitioned_vmdk_first, ignored);
        std::filesystem::remove(partitioned_vmdk_second, ignored);
        std::filesystem::remove(cue_bin_path, ignored);
        std::filesystem::remove(cue_second_bin_path, ignored);
        std::filesystem::remove(cue_path, ignored);
        std::filesystem::remove(btrfs_send_path, ignored);
        std::filesystem::remove(btrfs_send_corrupt_path, ignored);
        std::filesystem::remove(btrfs_v1_path, ignored);
        std::filesystem::remove(btrfs_v3_path, ignored);
        std::filesystem::remove(dmg_path, ignored);
        std::filesystem::remove(dmg_zlib_path, ignored);
        std::filesystem::remove(dmg_unsupported_path, ignored);
        std::filesystem::remove(dmg_segmented_path, ignored);
        std::filesystem::remove(dmg_iso_path, ignored);
        std::filesystem::remove(refs_path, ignored);
        std::error_code btrfs_cleanup_error;
        std::filesystem::remove_all(btrfs_send_output_path,
                                    btrfs_cleanup_error);
#ifdef UIFE_WITH_LIBWIM
        std::filesystem::remove(wim_path, ignored);
        std::filesystem::remove(split_wim_path, ignored);
        std::filesystem::remove_all(wim_first_source, ignored);
        std::filesystem::remove_all(wim_second_source, ignored);
#endif
        std::error_code cleanup_error;
        if (std::filesystem::exists(timestamp_output_path, cleanup_error)) {
            for (std::filesystem::recursive_directory_iterator item(
                     timestamp_output_path, cleanup_error), end;
                 !cleanup_error && item != end; item.increment(cleanup_error)) {
                std::filesystem::permissions(
                    item->path(), std::filesystem::perms::owner_all,
                    std::filesystem::perm_options::add, cleanup_error);
                cleanup_error.clear();
            }
            std::filesystem::permissions(
                timestamp_output_path, std::filesystem::perms::owner_all,
                std::filesystem::perm_options::add, cleanup_error);
            cleanup_error.clear();
        }
        std::filesystem::remove_all(timestamp_output_path, cleanup_error);
        std::filesystem::remove(corrupt_iso_descriptor_path, ignored);
        std::filesystem::remove(corrupt_iso_record_path, ignored);
        std::filesystem::remove(truncated_iso_volume_path, ignored);
        throw;
    }
    std::cout << "UIFE image integration tests passed\n";
}

int main() {
    // 与 uife_tests 一致：把未捕获异常转成可读信息 + 常规退出码，
    // 避免 MSVC 下 abort 成 0xC0000409 后 CTest 只说 "Exception" 而无原因。
    try {
        run_all_integration_tests();
    } catch (const std::exception& error) {
        std::cerr << "\nTEST FAILURE: " << error.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\nTEST FAILURE: unknown exception" << std::endl;
        return 1;
    }
    return 0;
}
