#include "uife/drivers/image/DmgImageDriver.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>

#ifdef UIFE_WITH_ZLIB
#include <zlib.h>
#endif

namespace uife {
namespace {

constexpr std::uint64_t sector_bytes = 512;
constexpr std::size_t udif_footer_size = 512;
constexpr std::size_t blkx_header_size = 204;
constexpr std::uint64_t maximum_plist_size = 64U * 1024U * 1024U;
constexpr std::uint64_t maximum_plist_objects = 1U * 1024U * 1024U;
constexpr std::uint64_t maximum_run_size = 64U * 1024U * 1024U;
constexpr std::uint32_t block_zero = 0;
constexpr std::uint32_t block_raw = 1;
constexpr std::uint32_t block_ignore = 2;
constexpr std::uint32_t block_zlib = 0x80000005U;
constexpr std::uint32_t block_comment = 0x7ffffffeU;
constexpr std::uint32_t block_terminator = 0xffffffffU;

std::uint32_t read_be32(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned int index = 0; index < 4; ++index) {
        value = (value << 8U) |
            std::to_integer<unsigned char>(bytes[offset + index]);
    }
    return value;
}

std::uint64_t read_be64(std::span<const std::byte> bytes, std::size_t offset) {
    return (static_cast<std::uint64_t>(read_be32(bytes, offset)) << 32U) |
           read_be32(bytes, offset + 4);
}

std::uint64_t read_be_integer(std::span<const std::byte> bytes,
                              std::size_t offset, std::size_t width) {
    if (width == 0 || width > 8 || offset > bytes.size() ||
        width > bytes.size() - offset) {
        throw std::runtime_error("DMG binary plist 整数范围无效");
    }
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < width; ++index) {
        value = (value << 8U) |
                std::to_integer<unsigned char>(bytes[offset + index]);
    }
    return value;
}

void append_utf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint <= 0x7fU) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else if (codepoint <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
}

class BinaryPlistReader {
public:
    explicit BinaryPlistReader(std::span<const std::byte> bytes)
        : bytes_(bytes) {
        constexpr std::size_t trailer_size = 32;
        if (bytes_.size() < 8 + trailer_size ||
            std::to_integer<unsigned char>(bytes_[0]) != 'b' ||
            std::to_integer<unsigned char>(bytes_[1]) != 'p' ||
            std::to_integer<unsigned char>(bytes_[2]) != 'l' ||
            std::to_integer<unsigned char>(bytes_[3]) != 'i' ||
            std::to_integer<unsigned char>(bytes_[4]) != 's' ||
            std::to_integer<unsigned char>(bytes_[5]) != 't' ||
            std::to_integer<unsigned char>(bytes_[6]) != '0' ||
            std::to_integer<unsigned char>(bytes_[7]) != '0') {
            throw std::runtime_error("DMG binary plist 签名无效");
        }
        const auto trailer = bytes_.size() - trailer_size;
        offset_size_ = std::to_integer<unsigned char>(bytes_[trailer + 6]);
        reference_size_ = std::to_integer<unsigned char>(bytes_[trailer + 7]);
        object_count_ = read_be64(bytes_, trailer + 8);
        top_object_ = read_be64(bytes_, trailer + 16);
        offset_table_offset_ = read_be64(bytes_, trailer + 24);
        if (offset_size_ == 0 || offset_size_ > 8 ||
            reference_size_ == 0 || reference_size_ > 8 ||
            object_count_ == 0 || object_count_ > maximum_plist_objects ||
            top_object_ >= object_count_ ||
            offset_table_offset_ < 8 ||
            offset_table_offset_ > trailer ||
            object_count_ >
                (trailer - offset_table_offset_) / offset_size_) {
            throw std::runtime_error("DMG binary plist trailer 无效");
        }
        objects_end_ = static_cast<std::size_t>(offset_table_offset_);
        offsets_.reserve(static_cast<std::size_t>(object_count_));
        for (std::uint64_t index = 0; index < object_count_; ++index) {
            const auto entry_offset =
                static_cast<std::size_t>(offset_table_offset_) +
                static_cast<std::size_t>(index) * offset_size_;
            const auto object_offset = read_be_integer(
                bytes_, entry_offset, offset_size_);
            if (object_offset < 8 || object_offset >= offset_table_offset_) {
                throw std::runtime_error("DMG binary plist 对象偏移无效");
            }
            offsets_.push_back(static_cast<std::size_t>(object_offset));
        }
        objects_.resize(static_cast<std::size_t>(object_count_));
        states_.resize(static_cast<std::size_t>(object_count_));
    }

    [[nodiscard]] std::vector<std::vector<std::byte>> read_blkx_tables() {
        const auto& root = object(top_object_);
        if (root.kind != Kind::dictionary) {
            throw std::runtime_error("DMG binary plist 根对象不是字典");
        }
        auto resource_fork = dictionary_value(root, "resource-fork");
        if (!resource_fork) {
            resource_fork = top_object_;
        }
        const auto& resource = object(*resource_fork);
        if (resource.kind != Kind::dictionary) {
            throw std::runtime_error("DMG binary plist resource-fork 不是字典");
        }
        const auto blkx_index = dictionary_value(resource, "blkx");
        if (!blkx_index) {
            throw std::runtime_error("DMG binary plist 不包含 blkx 表");
        }
        const auto& blkx = object(*blkx_index);
        if (blkx.kind != Kind::array || blkx.references.size() > 4096) {
            throw std::runtime_error("DMG binary plist blkx 数组无效");
        }
        std::vector<std::vector<std::byte>> tables;
        tables.reserve(blkx.references.size());
        for (const auto reference : blkx.references) {
            const auto& item = object(reference);
            std::optional<std::uint64_t> data_index;
            if (item.kind == Kind::data) {
                tables.push_back(item.data);
                continue;
            }
            if (item.kind == Kind::dictionary) {
                data_index = dictionary_value(item, "Data");
            }
            if (!data_index || object(*data_index).kind != Kind::data) {
                throw std::runtime_error(
                    "DMG binary plist blkx 项缺少 Data 数据");
            }
            tables.push_back(object(*data_index).data);
        }
        if (tables.empty()) {
            throw std::runtime_error("DMG binary plist 不包含 blkx 数据表");
        }
        for (const auto& table : tables) {
            if (table.size() < blkx_header_size) {
                throw std::runtime_error("DMG blkx 表头不完整");
            }
        }
        return tables;
    }

private:
    enum class Kind : std::uint8_t {
        other,
        string,
        data,
        array,
        dictionary
    };

    struct Object {
        Kind kind{Kind::other};
        std::string string;
        std::vector<std::byte> data;
        std::vector<std::uint64_t> references;
        std::size_t dictionary_size{};
    };

    [[nodiscard]] std::uint64_t read_reference(std::size_t offset) const {
        const auto reference = read_be_integer(bytes_, offset, reference_size_);
        if (reference >= object_count_) {
            throw std::runtime_error("DMG binary plist 对象引用越界");
        }
        return reference;
    }

    [[nodiscard]] std::size_t read_length(std::uint8_t nibble,
                                          std::size_t& cursor) const {
        if (nibble < 15) {
            return nibble;
        }
        if (cursor >= objects_end_) {
            throw std::runtime_error("DMG binary plist 扩展长度缺失");
        }
        const auto marker = std::to_integer<unsigned char>(bytes_[cursor++]);
        if ((marker >> 4U) != 1U || (marker & 0x0fU) > 3U) {
            throw std::runtime_error(
                "DMG binary plist 扩展长度整数格式无效");
        }
        const auto width = static_cast<std::size_t>(
            std::size_t{1} << (marker & 0x0fU));
        if (cursor > objects_end_ || width > objects_end_ - cursor) {
            throw std::runtime_error(
                "DMG binary plist 扩展长度整数越界");
        }
        const auto length = read_be_integer(bytes_, cursor, width);
        cursor += width;
        if (length > objects_end_) {
            throw std::runtime_error("DMG binary plist 对象长度过大");
        }
        return static_cast<std::size_t>(length);
    }

    [[nodiscard]] std::string read_utf16be(std::size_t offset,
                                           std::size_t units) const {
        if (units > (objects_end_ - offset) / 2U) {
            throw std::runtime_error("DMG binary plist UTF-16 字符串越界");
        }
        std::string result;
        for (std::size_t index = 0; index < units; ++index) {
            auto codepoint = static_cast<std::uint32_t>(
                read_be_integer(bytes_, offset + index * 2U, 2));
            if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                if (index + 1 >= units) {
                    throw std::runtime_error(
                        "DMG binary plist UTF-16 代理项无效");
                }
                const auto low = static_cast<std::uint32_t>(
                    read_be_integer(bytes_, offset + (index + 1U) * 2U, 2));
                if (low < 0xdc00U || low > 0xdfffU) {
                    throw std::runtime_error(
                        "DMG binary plist UTF-16 代理项无效");
                }
                codepoint = 0x10000U + ((codepoint - 0xd800U) << 10U) +
                            (low - 0xdc00U);
                ++index;
            } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
                throw std::runtime_error(
                    "DMG binary plist UTF-16 代理项无效");
            }
            append_utf8(result, codepoint);
        }
        return result;
    }

    [[nodiscard]] const Object& object(std::uint64_t index) {
        if (index >= object_count_) {
            throw std::runtime_error("DMG binary plist 对象编号越界");
        }
        const auto object_index = static_cast<std::size_t>(index);
        if (states_[object_index] == 2) {
            return objects_[object_index];
        }
        if (states_[object_index] == 1) {
            throw std::runtime_error("DMG binary plist 对象引用循环");
        }
        states_[object_index] = 1;
        auto& parsed = objects_[object_index];
        auto cursor = offsets_[object_index];
        const auto marker = std::to_integer<unsigned char>(bytes_[cursor++]);
        const auto type = marker >> 4U;
        const auto nibble = static_cast<std::uint8_t>(marker & 0x0fU);
        if (type == 4U || type == 5U || type == 6U) {
            const auto length = read_length(nibble, cursor);
            const auto unit_size = type == 6U ? 2U : 1U;
            if (length > (objects_end_ - cursor) / unit_size) {
                throw std::runtime_error("DMG binary plist 字符串或数据越界");
            }
            if (type == 4U) {
                parsed.kind = Kind::data;
                parsed.data.assign(bytes_.begin() +
                                       static_cast<std::ptrdiff_t>(cursor),
                                   bytes_.begin() + static_cast<std::ptrdiff_t>(
                                       cursor + length));
            } else if (type == 5U) {
                parsed.kind = Kind::string;
                parsed.string.assign(
                    reinterpret_cast<const char*>(bytes_.data() + cursor),
                    length);
            } else {
                parsed.kind = Kind::string;
                parsed.string = read_utf16be(cursor, length);
            }
        } else if (type == 10U || type == 13U) {
            const auto count = read_length(nibble, cursor);
            const auto reference_count =
                type == 13U ? count * 2U : count;
            if (count > maximum_plist_objects ||
                reference_count >
                    (objects_end_ - cursor) / reference_size_) {
                throw std::runtime_error("DMG binary plist 引用列表越界");
            }
            parsed.kind = type == 10U ? Kind::array : Kind::dictionary;
            parsed.dictionary_size =
                type == 13U ? count : static_cast<std::size_t>(0);
            parsed.references.reserve(static_cast<std::size_t>(reference_count));
            for (std::uint64_t ref = 0; ref < reference_count; ++ref) {
                parsed.references.push_back(read_reference(cursor));
                cursor += reference_size_;
            }
        }
        states_[object_index] = 2;
        return parsed;
    }

    [[nodiscard]] std::optional<std::uint64_t> dictionary_value(
        const Object& dictionary, std::string_view key) {
        if (dictionary.kind != Kind::dictionary ||
            dictionary.references.size() != dictionary.dictionary_size * 2U) {
            throw std::runtime_error("DMG binary plist 字典结构无效");
        }
        for (std::size_t index = 0; index < dictionary.dictionary_size; ++index) {
            const auto& key_object = object(dictionary.references[index]);
            if (key_object.kind != Kind::string) {
                throw std::runtime_error(
                    "DMG binary plist 字典键不是字符串");
            }
            if (key_object.string == key) {
                return dictionary.references[dictionary.dictionary_size + index];
            }
        }
        return std::nullopt;
    }

    std::span<const std::byte> bytes_;
    std::size_t objects_end_{};
    std::uint64_t object_count_{};
    std::uint64_t top_object_{};
    std::uint64_t offset_table_offset_{};
    std::uint8_t offset_size_{};
    std::uint8_t reference_size_{};
    std::vector<std::size_t> offsets_;
    std::vector<Object> objects_;
    std::vector<std::uint8_t> states_;
};

std::vector<std::byte> decode_base64(std::string_view encoded) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<std::byte> decoded;
    decoded.reserve(encoded.size() / 4U * 3U);
    std::uint32_t accumulator = 0;
    unsigned int bits = 0;
    unsigned int padding_count = 0;
    std::size_t symbol_count = 0;
    bool padding_seen = false;
    for (const unsigned char character : encoded) {
        if (character == ' ' || character == '\t' ||
            character == '\r' || character == '\n') {
            continue;
        }
        if (character == '=') {
            padding_seen = true;
            ++padding_count;
            continue;
        }
        const auto digit = alphabet.find(static_cast<char>(character));
        if (digit == std::string_view::npos || padding_seen) {
            throw std::runtime_error("DMG XML 中的 blkx Base64 数据无效");
        }
        ++symbol_count;
        accumulator = (accumulator << 6U) |
                      static_cast<std::uint32_t>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<std::byte>(
                (accumulator >> bits) & 0xffU));
        }
    }
    if (padding_count > 2 || (symbol_count + padding_count) % 4 != 0 ||
        bits == 6 || (bits != 0 &&
                      (accumulator & ((1U << bits) - 1U)) != 0)) {
        throw std::runtime_error("DMG XML 中的 blkx Base64 尾部无效");
    }
    return decoded;
}

std::vector<std::vector<std::byte>> read_blkx_tables(
    std::string_view xml) {
    constexpr std::string_view key_open = "<key>";
    constexpr std::string_view key_close = "</key>";
    auto key = std::string_view::npos;
    std::size_t search_from = 0;
    while ((search_from = xml.find(key_open, search_from)) !=
           std::string_view::npos) {
        const auto key_end = xml.find(key_close, search_from + key_open.size());
        if (key_end == std::string_view::npos) {
            break;
        }
        auto value = xml.substr(search_from + key_open.size(),
                                key_end - search_from - key_open.size());
        while (!value.empty() &&
               (value.front() == ' ' || value.front() == '\t' ||
                value.front() == '\r' || value.front() == '\n')) {
            value.remove_prefix(1);
        }
        while (!value.empty() &&
               (value.back() == ' ' || value.back() == '\t' ||
                value.back() == '\r' || value.back() == '\n')) {
            value.remove_suffix(1);
        }
        search_from = key_end + key_close.size();
        if (value == "blkx") {
            key = search_from;
            break;
        }
    }
    if (key == std::string_view::npos) {
        throw std::runtime_error("DMG XML property list 不包含 blkx 表");
    }
    const auto array_start = xml.find("<array>", key);
    const auto array_end = array_start == std::string_view::npos
        ? std::string_view::npos : xml.find("</array>", array_start);
    if (array_start == std::string_view::npos ||
        array_end == std::string_view::npos) {
        throw std::runtime_error("DMG blkx property list 数组无效");
    }

    std::vector<std::vector<std::byte>> tables;
    auto cursor = array_start + 7;
    while (cursor < array_end) {
        const auto data_start = xml.find("<data>", cursor);
        if (data_start == std::string_view::npos || data_start >= array_end) {
            break;
        }
        const auto data_end = xml.find("</data>", data_start + 6);
        if (data_end == std::string_view::npos || data_end > array_end) {
            throw std::runtime_error("DMG blkx Base64 数据没有结束标签");
        }
        auto table = decode_base64(
            xml.substr(data_start + 6, data_end - data_start - 6));
        if (table.size() < blkx_header_size) {
            throw std::runtime_error("DMG blkx 表头不完整");
        }
        tables.push_back(std::move(table));
        cursor = data_end + 7;
    }
    if (tables.empty()) {
        throw std::runtime_error("DMG XML property list 不包含 blkx 数据表");
    }
    return tables;
}

} // namespace

DmgImageDriver::~DmgImageDriver() {
    close();
}

void DmgImageDriver::open(const std::filesystem::path& image_path) {
    close();
    std::error_code error;
    const auto file_size = std::filesystem::file_size(image_path, error);
    if (error || file_size < udif_footer_size ||
        file_size > static_cast<std::uint64_t>(
                        std::numeric_limits<std::streamoff>::max())) {
        throw std::runtime_error("DMG 文件大小异常或 footer 不完整");
    }

    std::ifstream candidate(image_path, std::ios::binary);
    if (!candidate) {
        throw std::runtime_error("无法以只读方式打开 DMG 镜像");
    }
    std::array<std::byte, udif_footer_size> footer{};
    candidate.seekg(static_cast<std::streamoff>(file_size - udif_footer_size));
    candidate.read(reinterpret_cast<char*>(footer.data()),
                   static_cast<std::streamsize>(footer.size()));
    if (candidate.gcount() != static_cast<std::streamsize>(footer.size()) ||
        read_be32(footer, 0) != 0x6b6f6c79U ||
        read_be32(footer, 8) != udif_footer_size) {
        throw std::runtime_error("DMG UDIF koly footer 无效");
    }
    if (read_be32(footer, 56) > 1 || read_be32(footer, 60) > 1) {
        throw std::runtime_error("当前 DMG 驱动不支持分段 UDIF 镜像");
    }

    const auto footer_offset = file_size - udif_footer_size;
    const auto data_offset = read_be64(footer, 24);
    const auto data_length = read_be64(footer, 32);
    const auto xml_offset = read_be64(footer, 216);
    const auto xml_length = read_be64(footer, 224);
    const auto sector_count = read_be64(footer, 492);
    if (sector_count == 0 ||
        sector_count > std::numeric_limits<std::uint64_t>::max() /
                           sector_bytes ||
        data_offset > file_size || data_length > file_size - data_offset ||
        data_offset + data_length > footer_offset ||
        xml_length == 0 || xml_length > maximum_plist_size ||
        xml_offset > file_size || xml_length > file_size - xml_offset ||
        xml_offset + xml_length > footer_offset ||
        (xml_offset < data_offset + data_length &&
         data_offset < xml_offset + xml_length)) {
        throw std::runtime_error("DMG UDIF footer 中的区域范围无效");
    }

    std::string xml(static_cast<std::size_t>(xml_length), '\0');
    candidate.seekg(static_cast<std::streamoff>(xml_offset));
    candidate.read(xml.data(), static_cast<std::streamsize>(xml.size()));
    if (candidate.gcount() != static_cast<std::streamsize>(xml.size())) {
        throw std::runtime_error("读取 DMG property list 失败");
    }
    std::vector<std::vector<std::byte>> tables;
    if (xml.starts_with("bplist00")) {
        const auto plist_bytes = std::span(
            reinterpret_cast<const std::byte*>(xml.data()), xml.size());
        tables = BinaryPlistReader(plist_bytes).read_blkx_tables();
    } else {
        tables = read_blkx_tables(xml);
    }

    std::vector<Run> runs;
    const auto logical_size = sector_count * sector_bytes;
    for (const auto& table : tables) {
        if (read_be32(table, 0) != 0x6d697368U) {
            throw std::runtime_error("DMG blkx 表签名无效");
        }
        if (read_be32(table, 4) != 1) {
            throw std::runtime_error("不支持的 DMG blkx 表版本");
        }
        const auto first_sector = read_be64(table, 8);
        const auto table_sectors = read_be64(table, 16);
        const auto table_data_start = read_be64(table, 24);
        const auto run_count = read_be32(table, 200);
        if (table_sectors == 0 ||
            first_sector > sector_count ||
            table_sectors > sector_count - first_sector ||
            run_count > (table.size() - blkx_header_size) / 40U) {
            throw std::runtime_error("DMG blkx 表的扇区或 run 数量无效");
        }
        const auto table_end_sector = first_sector + table_sectors;
        std::uint64_t prior_end_sector = first_sector;
        bool terminated = false;
        for (std::uint32_t index = 0; index < run_count; ++index) {
            const auto entry = blkx_header_size +
                               static_cast<std::size_t>(index) * 40U;
            const auto type = read_be32(table, entry);
            const auto run_start = read_be64(table, entry + 8);
            const auto run_sectors = read_be64(table, entry + 16);
            const auto compressed_offset = read_be64(table, entry + 24);
            const auto compressed_size = read_be64(table, entry + 32);
            if (type == block_terminator) {
                terminated = true;
                break;
            }
            if (type == block_comment) {
                continue;
            }
            if (run_sectors == 0 || run_start > table_sectors ||
                run_sectors > table_sectors - run_start) {
                throw std::runtime_error("DMG blkx run 超出表范围");
            }
            const auto logical_sector = first_sector + run_start;
            if (logical_sector < prior_end_sector ||
                logical_sector + run_sectors > table_end_sector) {
                throw std::runtime_error("DMG blkx run 重叠或未按逻辑顺序排列");
            }
            const auto run_size = run_sectors * sector_bytes;
            Run run;
            run.logical_offset = logical_sector * sector_bytes;
            run.logical_size = run_size;
            run.type = type;
            if (type == block_raw) {
                if (compressed_size != run_size ||
                    table_data_start > data_length ||
                    compressed_offset > data_length - table_data_start ||
                    compressed_size >
                        data_length - table_data_start - compressed_offset) {
                    throw std::runtime_error(
                        "DMG 原始 blkx run 数据范围无效");
                }
                run.source_offset =
                    data_offset + table_data_start + compressed_offset;
                run.source_size = compressed_size;
            } else if (type == block_zlib) {
                if (run_size > maximum_run_size ||
                    table_data_start > data_length ||
                    compressed_offset > data_length - table_data_start ||
                    compressed_size >
                        data_length - table_data_start - compressed_offset) {
                    throw std::runtime_error(
                        "DMG zlib blkx run 超出允许范围");
                }
                run.source_offset =
                    data_offset + table_data_start + compressed_offset;
                run.source_size = compressed_size;
#ifndef UIFE_WITH_ZLIB
                throw std::runtime_error(
                    "该 DMG 含 zlib 压缩块，但当前构建未启用 zlib");
#endif
            } else if (type != block_zero && type != block_ignore) {
                throw std::runtime_error(
                    "DMG 使用当前不支持的 blkx 压缩或加密类型: " +
                    std::to_string(type));
            } else if (compressed_size != 0) {
                throw std::runtime_error("DMG 零填充 blkx run 不应包含压缩数据");
            }
            runs.push_back(run);
            prior_end_sector = logical_sector + run_sectors;
        }
        if (!terminated) {
            throw std::runtime_error("DMG blkx 表缺少 terminator run");
        }
    }
    std::sort(runs.begin(), runs.end(),
              [](const Run& left, const Run& right) {
                  return left.logical_offset < right.logical_offset;
              });
    for (std::size_t index = 1; index < runs.size(); ++index) {
        if (runs[index - 1].logical_offset +
                runs[index - 1].logical_size > runs[index].logical_offset) {
            throw std::runtime_error("DMG blkx 表之间存在重叠逻辑范围");
        }
    }

    input_ = std::move(candidate);
    source_path_ = image_path;
    size_ = logical_size;
    runs_ = std::move(runs);
}

void DmgImageDriver::load_run(std::size_t index) const {
    if (cached_run_ == index) {
        return;
    }
    const auto& run = runs_[index];
    if (run.logical_size > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("DMG 解压块超出当前平台的内存范围");
    }
    cached_data_.assign(static_cast<std::size_t>(run.logical_size), std::byte{0});
    if (run.type == block_raw) {
        input_.clear();
        input_.seekg(static_cast<std::streamoff>(run.source_offset));
        input_.read(reinterpret_cast<char*>(cached_data_.data()),
                    static_cast<std::streamsize>(run.source_size));
        if (input_.gcount() != static_cast<std::streamsize>(run.source_size)) {
            throw std::runtime_error("读取 DMG 原始 blkx 数据失败");
        }
    } else if (run.type == block_zlib) {
#ifdef UIFE_WITH_ZLIB
        if (run.source_size > std::numeric_limits<uInt>::max() ||
            run.logical_size > std::numeric_limits<uInt>::max()) {
            throw std::runtime_error("DMG zlib 块超出 zlib 单次解压上限");
        }
        std::vector<std::byte> compressed(
            static_cast<std::size_t>(run.source_size));
        input_.clear();
        input_.seekg(static_cast<std::streamoff>(run.source_offset));
        input_.read(reinterpret_cast<char*>(compressed.data()),
                    static_cast<std::streamsize>(compressed.size()));
        if (input_.gcount() != static_cast<std::streamsize>(compressed.size())) {
            throw std::runtime_error("读取 DMG zlib 压缩块失败");
        }
        uLongf output_size = static_cast<uLongf>(cached_data_.size());
        const auto result = uncompress(
            reinterpret_cast<Bytef*>(cached_data_.data()), &output_size,
            reinterpret_cast<const Bytef*>(compressed.data()),
            static_cast<uLong>(compressed.size()));
        if (result != Z_OK || output_size != cached_data_.size()) {
            throw std::runtime_error("DMG zlib 块解压失败或长度不匹配");
        }
#else
        throw std::runtime_error("当前构建未启用 zlib");
#endif
    }
    cached_run_ = index;
}

std::size_t DmgImageDriver::read_sector(
    std::uint64_t sector_number,
    std::span<std::byte> destination) const {
    if (destination.size() != sector_bytes) {
        throw std::invalid_argument("DMG read_sector 要求 512 字节缓冲区");
    }
    if (sector_number > std::numeric_limits<std::uint64_t>::max() /
                            sector_bytes) {
        throw std::out_of_range("DMG sector offset overflow");
    }
    return read_at(sector_number * sector_bytes, destination);
}

std::size_t DmgImageDriver::read_at(
    std::uint64_t offset, std::span<std::byte> destination) const {
    std::scoped_lock lock(mutex_);
    if (!input_.is_open()) {
        throw std::runtime_error("DMG 镜像尚未打开");
    }
    if (offset > size_) {
        throw std::out_of_range("DMG 读取偏移超出逻辑镜像范围");
    }
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(destination.size(), size_ - offset));
    std::fill(destination.begin(),
              destination.begin() + static_cast<std::ptrdiff_t>(count),
              std::byte{0});
    const auto end = offset + count;
    auto cursor = offset;
    while (cursor < end) {
        const auto found = std::upper_bound(
            runs_.begin(), runs_.end(), cursor,
            [](std::uint64_t position, const Run& run) {
                return position < run.logical_offset;
            });
        if (found == runs_.begin()) {
            const auto next = found == runs_.end()
                ? end : std::min(end, found->logical_offset);
            cursor = next;
            continue;
        }
        const auto run_index =
            static_cast<std::size_t>(std::distance(runs_.begin(), found - 1));
        const auto& run = runs_[run_index];
        const auto run_end = run.logical_offset + run.logical_size;
        if (cursor >= run_end) {
            const auto next = found == runs_.end()
                ? end : std::min(end, found->logical_offset);
            cursor = next;
            continue;
        }
        const auto chunk_end = std::min(end, run_end);
        if (run.type == block_raw) {
            const auto source_offset =
                run.source_offset + cursor - run.logical_offset;
            const auto destination_offset =
                static_cast<std::size_t>(cursor - offset);
            const auto chunk_size =
                static_cast<std::size_t>(chunk_end - cursor);
            input_.clear();
            input_.seekg(static_cast<std::streamoff>(source_offset));
            input_.read(reinterpret_cast<char*>(
                            destination.data() + destination_offset),
                        static_cast<std::streamsize>(chunk_size));
            if (input_.gcount() != static_cast<std::streamsize>(chunk_size)) {
                throw std::runtime_error("读取 DMG 原始 blkx 数据失败");
            }
        } else if (run.type == block_zlib) {
            load_run(run_index);
            const auto source_offset =
                static_cast<std::size_t>(cursor - run.logical_offset);
            const auto destination_offset =
                static_cast<std::size_t>(cursor - offset);
            const auto chunk_size =
                static_cast<std::size_t>(chunk_end - cursor);
            std::copy_n(cached_data_.begin() +
                            static_cast<std::ptrdiff_t>(source_offset),
                        chunk_size,
                        destination.begin() +
                            static_cast<std::ptrdiff_t>(destination_offset));
        }
        cursor = chunk_end;
    }
    return count;
}

std::uint64_t DmgImageDriver::get_size() const noexcept {
    return size_;
}

std::uint32_t DmgImageDriver::sector_size() const noexcept {
    return static_cast<std::uint32_t>(sector_bytes);
}

std::filesystem::path DmgImageDriver::source_path() const {
    return source_path_;
}

std::string_view DmgImageDriver::format_name() const noexcept {
    return "Apple DMG";
}

void DmgImageDriver::close() noexcept {
    std::scoped_lock lock(mutex_);
    if (input_.is_open()) {
        input_.close();
    }
    source_path_.clear();
    size_ = 0;
    runs_.clear();
    cached_data_.clear();
    cached_run_.reset();
}

} // namespace uife
