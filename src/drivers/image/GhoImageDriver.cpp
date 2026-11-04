#include "uife/drivers/image/GhoImageDriver.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <stdexcept>

#ifdef UIFE_WITH_ZLIB
#include <zlib.h>
#endif

namespace uife {
namespace {

constexpr std::uint64_t sector_bytes = 512;
constexpr std::uint64_t file_header_size = 512;
constexpr std::uint64_t record_header_size = 10;
constexpr std::uint64_t partition_header_size = 512;
constexpr std::uint32_t record_magic = 0x012f18d8U;
constexpr std::uint16_t record_track0 = 0x0006;
constexpr std::uint16_t record_partition = 0x0603;
constexpr std::uint16_t record_continuation = 0x0703;
constexpr std::uint16_t record_end = 0x0023;
constexpr std::uint16_t file_magic = 0xeffe;
constexpr std::uint32_t maximum_block_size = 32U * 1024U;
constexpr std::uint32_t maximum_stored_block_size = 33002U;

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

bool natural_filename_less(std::string left, std::string right) {
    left = lowercase(std::move(left));
    right = lowercase(std::move(right));
    std::size_t left_index = 0;
    std::size_t right_index = 0;
    while (left_index < left.size() && right_index < right.size()) {
        const auto left_digit =
            std::isdigit(static_cast<unsigned char>(left[left_index])) != 0;
        const auto right_digit =
            std::isdigit(static_cast<unsigned char>(right[right_index])) != 0;
        if (left_digit && right_digit) {
            const auto left_digits = left_index;
            const auto right_digits = right_index;
            while (left_index < left.size() &&
                   std::isdigit(static_cast<unsigned char>(left[left_index])) != 0) {
                ++left_index;
            }
            while (right_index < right.size() &&
                   std::isdigit(static_cast<unsigned char>(right[right_index])) !=
                       0) {
                ++right_index;
            }
            auto left_significant = left_digits;
            auto right_significant = right_digits;
            while (left_significant + 1U < left_index &&
                   left[left_significant] == '0') {
                ++left_significant;
            }
            while (right_significant + 1U < right_index &&
                   right[right_significant] == '0') {
                ++right_significant;
            }
            const auto left_length = left_index - left_significant;
            const auto right_length = right_index - right_significant;
            if (left_length != right_length) {
                return left_length < right_length;
            }
            const auto left_number =
                left.compare(left_significant, left_length,
                             right, right_significant, right_length);
            if (left_number != 0) {
                return left_number < 0;
            }
            const auto left_run_length = left_index - left_digits;
            const auto right_run_length = right_index - right_digits;
            if (left_run_length != right_run_length) {
                return left_run_length < right_run_length;
            }
            continue;
        }
        if (left[left_index] != right[right_index]) {
            return left[left_index] < right[right_index];
        }
        ++left_index;
        ++right_index;
    }
    return left.size() - left_index < right.size() - right_index;
}

std::uint16_t read_le16(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
        std::to_integer<unsigned char>(bytes[offset]) |
        (static_cast<std::uint16_t>(
             std::to_integer<unsigned char>(bytes[offset + 1])) << 8U));
}

std::uint32_t read_le32(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(
               std::to_integer<unsigned char>(bytes[offset])) |
           (static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(bytes[offset + 1])) << 8U) |
           (static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(bytes[offset + 2])) << 16U) |
           (static_cast<std::uint32_t>(
                std::to_integer<unsigned char>(bytes[offset + 3])) << 24U);
}

bool is_supported_compression(std::uint8_t compression) {
    return compression == 0 || compression == 2 ||
           (compression >= 3 && compression <= 9);
}

std::size_t fast_lz_hash(std::byte first, std::byte second,
                         std::byte third) {
    const auto value =
        static_cast<std::int32_t>(std::to_integer<unsigned char>(third)) ^
        (16 * (static_cast<std::int32_t>(
                   std::to_integer<unsigned char>(second)) ^
               (16 * static_cast<std::int32_t>(
                         std::to_integer<unsigned char>(first)))));
    const auto product = static_cast<std::uint32_t>(value) *
                         static_cast<std::uint32_t>(-24993);
    return static_cast<std::size_t>((product >> 4U) & 0x0fffU);
}

std::vector<std::byte> decode_fast_lz(std::span<const std::byte> encoded) {
    if (encoded.size() < 5) {
        throw std::runtime_error("GHO Fast LZ 数据块被截断");
    }
    if (encoded[0] == std::byte{1}) {
        if (encoded.size() <= 4 ||
            encoded.size() - 4U > maximum_block_size) {
            throw std::runtime_error("GHO Fast LZ 原始数据块长度无效");
        }
        return {encoded.begin() + 4, encoded.end()};
    }

    constexpr std::array<std::byte, 18> initial_dictionary{
        std::byte{'1'}, std::byte{'2'}, std::byte{'3'}, std::byte{'4'},
        std::byte{'5'}, std::byte{'6'}, std::byte{'7'}, std::byte{'8'},
        std::byte{'9'}, std::byte{'0'}, std::byte{'1'}, std::byte{'2'},
        std::byte{'3'}, std::byte{'4'}, std::byte{'5'}, std::byte{'6'},
        std::byte{'7'}, std::byte{'8'}};
    constexpr auto missing_position = std::numeric_limits<std::size_t>::max();
    std::array<std::size_t, 4096> hash_table{};
    hash_table.fill(missing_position);
    std::vector<std::byte> output(maximum_block_size, std::byte{0});
    std::size_t output_size = 0;
    std::size_t source = 4;
    std::uint32_t control = 1;
    std::uint16_t literal_run = 0;
    std::uint16_t previous_literal_run = 0;

    while (source < encoded.size()) {
        if (control == 1) {
            if (source + 2U > encoded.size()) {
                throw std::runtime_error("GHO Fast LZ control word 被截断");
            }
            control = static_cast<std::uint32_t>(
                          std::to_integer<unsigned char>(encoded[source])) |
                      (static_cast<std::uint32_t>(
                           std::to_integer<unsigned char>(encoded[source + 1U]))
                       << 8U) |
                      0x10000U;
            source += 2U;
        }

        const auto token_limit =
            encoded.size() - source < 32U ? 1U : 16U;
        for (std::size_t token = 0; token < token_limit; ++token) {
            if (source >= encoded.size()) {
                break;
            }
            if ((control & 1U) != 0) {
                if (source + 2U > encoded.size()) {
                    throw std::runtime_error("GHO Fast LZ match token 被截断");
                }
                const auto first = std::to_integer<unsigned char>(
                    encoded[source]);
                const auto hash_index =
                    (static_cast<std::size_t>(first & 0xf0U) << 4U) |
                    std::to_integer<unsigned char>(encoded[source + 1U]);
                const auto match_length =
                    3U + static_cast<std::size_t>(first & 0x0fU);
                const auto match_start = output_size;
                if (match_length > output.size() - output_size) {
                    throw std::runtime_error(
                        "GHO Fast LZ 输出超过 32 KiB block 上限");
                }
                const auto match_position = hash_table[hash_index];
                for (std::size_t index = 0; index < match_length; ++index) {
                    if (match_position == missing_position) {
                        output[output_size] =
                            index < initial_dictionary.size()
                                ? initial_dictionary[index]
                                : std::byte{0};
                    } else {
                        const auto source_position = match_position + index;
                        output[output_size] =
                            source_position < output.size()
                                ? output[source_position] : std::byte{0};
                    }
                    ++output_size;
                }
                source += 2U;
                if (literal_run > 0) {
                    const auto literal_start =
                        match_start - static_cast<std::size_t>(literal_run);
                    if (literal_start + 2U < output_size) {
                        hash_table[fast_lz_hash(
                            output[literal_start], output[literal_start + 1U],
                            output[literal_start + 2U])] = literal_start;
                        if (previous_literal_run == 2 &&
                            literal_start + 3U < output_size) {
                            hash_table[fast_lz_hash(
                                output[literal_start + 1U],
                                output[literal_start + 2U],
                                output[literal_start + 3U])] =
                                literal_start + 1U;
                        }
                    }
                    literal_run = 0;
                    previous_literal_run = 0;
                }
                hash_table[hash_index] = match_start;
            } else {
                if (output_size == output.size()) {
                    throw std::runtime_error(
                        "GHO Fast LZ 输出超过 32 KiB block 上限");
                }
                output[output_size] = encoded[source++];
                ++output_size;
                ++literal_run;
                previous_literal_run = literal_run;
                if (literal_run == 3) {
                    const auto literal_start = output_size - 3U;
                    hash_table[fast_lz_hash(
                        output[literal_start], output[literal_start + 1U],
                        output[literal_start + 2U])] = literal_start;
                    literal_run = 2;
                    previous_literal_run = 2;
                }
            }
            control >>= 1U;
            if (control == 1) {
                break;
            }
        }
    }
    if (output_size == 0) {
        throw std::runtime_error("GHO Fast LZ 数据块没有输出数据");
    }
    output.resize(output_size);
    return output;
}

std::vector<std::byte> decode_block(std::span<const std::byte> encoded,
                                    std::uint8_t compression) {
    if (compression == 0) {
        return {encoded.begin(), encoded.end()};
    }
    if (encoded.size() < 4) {
        throw std::runtime_error("GHO 压缩数据块缺少 block header");
    }
    if (compression == 2) {
        return decode_fast_lz(encoded);
    }
    if (encoded[0] == std::byte{1}) {
        if (encoded.size() <= 4 ||
            encoded.size() - 4U > maximum_block_size) {
            throw std::runtime_error("GHO 内嵌原始数据块长度无效");
        }
        return {encoded.begin() + 4, encoded.end()};
    }
#ifdef UIFE_WITH_ZLIB
    std::vector<std::byte> decoded(maximum_block_size);
    uLongf decoded_size = static_cast<uLongf>(decoded.size());
    const auto result = uncompress(
        reinterpret_cast<Bytef*>(decoded.data()), &decoded_size,
        reinterpret_cast<const Bytef*>(encoded.data() + 4),
        static_cast<uLong>(encoded.size() - 4U));
    if (result != Z_OK || decoded_size == 0 ||
        decoded_size > maximum_block_size) {
        throw std::runtime_error("GHO zlib 数据块解压失败或长度无效");
    }
    decoded.resize(static_cast<std::size_t>(decoded_size));
    return decoded;
#else
    throw std::runtime_error("当前构建未启用 zlib，无法读取压缩 GHO");
#endif
}

bool is_record(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || record_header_size > bytes.size() - offset ||
        read_le32(bytes, offset + 4) != record_magic) {
        return false;
    }
    const auto type = read_le16(bytes, offset);
    const auto body_size = read_le16(bytes, offset + 8);
    return (type == record_track0 && body_size >= 6) ||
           (type == record_partition && body_size == 20) ||
           (type == record_continuation && body_size == 20) ||
           (type == record_end && body_size == 24);
}

} // namespace

GhoImageDriver::~GhoImageDriver() {
    close();
}

void GhoImageDriver::open(const std::filesystem::path& image_path) {
    close();
    std::error_code error;
    const auto file_size = std::filesystem::file_size(image_path, error);
    if (error || file_size < file_header_size ||
        file_size > static_cast<std::uint64_t>(
                        std::numeric_limits<std::streamoff>::max())) {
        throw std::runtime_error("GHO 文件大小异常或文件头不完整");
    }

    std::ifstream candidate(image_path, std::ios::binary);
    if (!candidate) {
        throw std::runtime_error("无法以只读方式打开 GHO 镜像");
    }
    std::array<std::byte, file_header_size> file_header{};
    candidate.read(reinterpret_cast<char*>(file_header.data()),
                   static_cast<std::streamsize>(file_header.size()));
    if (candidate.gcount() != static_cast<std::streamsize>(file_header.size()) ||
        read_le16(file_header, 0) != file_magic) {
        throw std::runtime_error("GHO 文件头签名无效");
    }
    if (std::to_integer<unsigned char>(file_header[2]) != 1) {
        throw std::runtime_error("GHO 主文件类型无效或当前不支持");
    }
    if (std::to_integer<unsigned char>(file_header[12]) & 0x02U) {
        throw std::runtime_error("当前 GHO 驱动不支持加密镜像");
    }
    const auto compression =
        std::to_integer<std::uint8_t>(file_header[3]);
    const auto image_id = read_le32(file_header, 4);
    if (!is_supported_compression(compression)) {
        throw std::runtime_error(
            "当前 GHO 驱动不支持该压缩类型；支持未压缩与 zlib");
    }
    const bool compressed = compression != 0;
#ifndef UIFE_WITH_ZLIB
    if (compressed && compression != 2) {
        throw std::runtime_error("当前构建未启用 zlib，无法读取压缩 GHO");
    }
#endif

    std::vector<std::filesystem::path> span_paths;
    const auto directory = image_path.has_parent_path()
        ? image_path.parent_path() : std::filesystem::path(".");
    const auto primary_stem = lowercase(image_path.stem().string());
    std::error_code directory_error;
    for (std::filesystem::directory_iterator iterator(
             directory,
             std::filesystem::directory_options::skip_permission_denied,
             directory_error),
         end;
         !directory_error && iterator != end;) {
        const auto path = iterator->path();
        if (lowercase(path.extension().string()) == ".ghs") {
            const auto candidate_stem =
                lowercase(path.stem().string());
            const bool matches_primary =
                candidate_stem == primary_stem ||
                (candidate_stem.starts_with(primary_stem) &&
                 candidate_stem.size() > primary_stem.size() &&
                 (candidate_stem[primary_stem.size()] == '.' ||
                  candidate_stem[primary_stem.size()] == '_' ||
                  candidate_stem[primary_stem.size()] == '-' ||
                  std::isdigit(static_cast<unsigned char>(
                      candidate_stem[primary_stem.size()])) != 0));
            if (matches_primary) {
                std::error_code entry_error;
                const auto status = iterator->symlink_status(entry_error);
                if (entry_error) {
                    throw std::runtime_error(
                        "无法验证 GHO 分卷文件 " + path.string() + ": " +
                        entry_error.message());
                }
                if (std::filesystem::is_regular_file(status)) {
                    span_paths.push_back(path);
                } else {
                    throw std::runtime_error(
                        "匹配的 GHO 分卷不是普通文件: " + path.string());
                }
            }
        }
        iterator.increment(directory_error);
    }
    if (directory_error) {
        throw std::runtime_error(
            "无法枚举 GHO 分卷目录 " + directory.string() + ": " +
            directory_error.message());
    }
    std::sort(span_paths.begin(), span_paths.end(),
              [](const auto& left, const auto& right) {
                  if (natural_filename_less(left.filename().string(),
                                            right.filename().string())) {
                      return true;
                  }
                  if (natural_filename_less(right.filename().string(),
                                            left.filename().string())) {
                      return false;
                  }
                  return left.filename().string() < right.filename().string();
              });

    std::vector<SourceFile> source_files;
    source_files.reserve(span_paths.size() + 1U);
    source_files.push_back(
        {std::move(candidate), 0, 0, file_size});
    std::uint64_t virtual_file_size = file_size;
    for (const auto& span_path : span_paths) {
        std::error_code span_error;
        const auto span_size = std::filesystem::file_size(span_path, span_error);
        if (span_error || span_size <= file_header_size ||
            span_size > static_cast<std::uint64_t>(
                            std::numeric_limits<std::streamoff>::max())) {
            throw std::runtime_error("GHO 分卷大小异常或 header 不完整: " +
                                     span_path.string());
        }
        std::ifstream span_input(span_path, std::ios::binary);
        std::array<std::byte, file_header_size> span_header{};
        span_input.read(reinterpret_cast<char*>(span_header.data()),
                        static_cast<std::streamsize>(span_header.size()));
        if (!span_input ||
            read_le16(span_header, 0) != file_magic ||
            std::to_integer<unsigned char>(span_header[2]) != 9 ||
            std::to_integer<std::uint8_t>(span_header[3]) != compression ||
            read_le32(span_header, 4) != image_id ||
            (std::to_integer<unsigned char>(span_header[12]) & 0x02U) != 0) {
            throw std::runtime_error("GHO 分卷 header 无效或参数不匹配: " +
                                     span_path.string());
        }
        const auto span_data_size = span_size - file_header_size;
        if (virtual_file_size >
            std::numeric_limits<std::uint64_t>::max() - span_data_size) {
            throw std::runtime_error("GHO 分卷总长度溢出");
        }
        source_files.push_back({std::move(span_input), virtual_file_size,
                                file_header_size, span_data_size});
        virtual_file_size += span_data_size;
    }

    std::vector<Block> blocks;
    std::vector<Partition> partitions;
    std::vector<std::byte> track0_data;
    std::uint64_t logical_size = 0;
    std::size_t active_partition = std::numeric_limits<std::size_t>::max();
    std::uint64_t cursor = file_header_size;
    bool partition_header_seen = false;
    bool end_seen = false;
    std::array<std::byte, record_header_size> record_header{};
    std::array<std::byte, partition_header_size> partition_header{};
    std::array<std::byte, 2> block_length_bytes{};

    const auto read_exact = [&source_files](std::uint64_t offset,
                                            std::span<std::byte> buffer,
                                            const char* message) {
        std::size_t copied = 0;
        while (copied < buffer.size()) {
            const auto logical_offset = offset + copied;
            const auto source = std::find_if(
                source_files.begin(), source_files.end(),
                [logical_offset](const SourceFile& file) {
                    return logical_offset >= file.logical_offset &&
                           logical_offset - file.logical_offset < file.size;
                });
            if (source == source_files.end()) {
                throw std::runtime_error(message);
            }
            const auto source_position =
                source->source_offset + logical_offset - source->logical_offset;
            const auto available = static_cast<std::size_t>(
                std::min<std::uint64_t>(
                    source->size -
                        (logical_offset - source->logical_offset),
                    buffer.size() - copied));
            source->input.clear();
            source->input.seekg(static_cast<std::streamoff>(source_position));
            source->input.read(
                reinterpret_cast<char*>(buffer.data() + copied),
                static_cast<std::streamsize>(available));
            if (source->input.gcount() !=
                static_cast<std::streamsize>(available)) {
                throw std::runtime_error(message);
            }
            copied += available;
        }
    };

    while (cursor < virtual_file_size) {
        if (virtual_file_size - cursor >= record_header_size) {
            read_exact(cursor, record_header, "读取 GHO record header 失败");
        }
        const auto record_span = std::span<const std::byte>(
            record_header.data(), record_header.size());
        const bool record_candidate =
            virtual_file_size - cursor >= record_header_size &&
            is_record(record_span, 0);
        const auto candidate_type = record_candidate
            ? read_le16(record_span, 0) : 0;
        const auto candidate_body_size = record_candidate
            ? read_le16(record_span, 8) : 0;
        const bool valid_end_record =
            record_candidate && candidate_type == record_end &&
            cursor + record_header_size + candidate_body_size ==
                virtual_file_size;
        std::array<std::byte, 4> following_partition_header{};
        const auto following_header_offset =
            cursor + record_header_size + candidate_body_size;
        bool valid_partition_header = false;
        if (record_candidate &&
            (candidate_type == record_partition ||
             candidate_type == record_continuation) &&
            candidate_body_size == 20 &&
            following_header_offset <= virtual_file_size &&
            following_partition_header.size() <=
                virtual_file_size - following_header_offset) {
            read_exact(following_header_offset, following_partition_header,
                       "读取 GHO 后续分区 header 失败");
            valid_partition_header =
                read_le16(following_partition_header, 0) == file_magic &&
                std::to_integer<unsigned char>(
                    following_partition_header[2]) == 2 &&
                std::to_integer<unsigned char>(
                    following_partition_header[3]) == compression;
        }
        const bool record_at_boundary =
            record_candidate &&
            (!partition_header_seen ||
             ((candidate_type == record_partition ||
               candidate_type == record_continuation) &&
              valid_partition_header) ||
             valid_end_record);
        if (record_at_boundary) {
            const auto type = read_le16(record_span, 0);
            const auto body_size = read_le16(record_span, 8);
            cursor += record_header_size;
            if (body_size > virtual_file_size - cursor) {
                throw std::runtime_error("GHO record body 超出文件范围");
            }
            if (type == record_track0) {
                if (!partitions.empty()) {
                    throw std::runtime_error(
                        "GHO Track 0 record 出现在分区数据之后");
                }
                if (body_size < 6 || !track0_data.empty()) {
                    throw std::runtime_error("GHO Track 0 record body 不完整");
                }
                track0_data.resize(body_size - 6U);
                if (!track0_data.empty()) {
                    read_exact(cursor + 6U, track0_data,
                               "读取 GHO Track 0 数据失败");
                }
                cursor += body_size;
                continue;
            }
            if (type == record_partition) {
                if (body_size != 20 || partitions.size() >= 4) {
                    throw std::runtime_error(
                        "GHO 分区描述无效或数量超过 MBR 上限");
                }
                cursor += body_size;
                if (partition_header_size > virtual_file_size - cursor) {
                    throw std::runtime_error("GHO 分区 FEEF header 不完整");
                }
                read_exact(cursor, partition_header,
                           "读取 GHO 分区 FEEF header 失败");
                if (read_le16(partition_header, 0) != file_magic ||
                    std::to_integer<unsigned char>(partition_header[2]) != 2 ||
                    std::to_integer<unsigned char>(partition_header[3]) !=
                        compression) {
                    throw std::runtime_error(
                        "GHO 分区 FEEF header 类型或压缩方式不匹配");
                }
                cursor += partition_header_size;
                partitions.push_back({0, 0, blocks.size(), 0});
                active_partition = partitions.size() - 1U;
                logical_size = 0;
                partition_header_seen = true;
                continue;
            }
            if (type == record_continuation) {
                if (!partition_header_seen || body_size != 20) {
                    throw std::runtime_error(
                        "GHO continuation record 结构无效");
                }
                cursor += body_size;
                if (partition_header_size <= virtual_file_size - cursor) {
                    read_exact(cursor, std::span<std::byte>(partition_header)
                                           .first(4),
                               "读取 GHO continuation header 失败");
                    if (read_le16(partition_header, 0) == file_magic) {
                        if (std::to_integer<unsigned char>(partition_header[2]) != 2 ||
                            std::to_integer<unsigned char>(partition_header[3]) !=
                                compression) {
                            throw std::runtime_error(
                                "GHO continuation header 类型或压缩方式无效");
                        }
                        cursor += partition_header_size;
                    }
                }
                continue;
            }
            if (type == record_end) {
                if (partitions.empty() || !partition_header_seen ||
                    partitions.back().block_count == 0 ||
                    body_size != 24 ||
                    cursor + body_size != virtual_file_size) {
                    throw std::runtime_error(
                        "GHO end record 或分区数据结构无效");
                }
                end_seen = true;
                break;
            }
        }

        if (!partition_header_seen) {
            throw std::runtime_error("GHO 分区数据 record 结构无效");
        }
        if (file_size - cursor < block_length_bytes.size()) {
            throw std::runtime_error("GHO 分区数据块长度字段被截断");
        }
        read_exact(cursor, block_length_bytes,
                   "读取 GHO 分区数据块长度失败");
        const auto stored_size = read_le16(block_length_bytes, 0);
        if (stored_size == 0) {
            cursor += block_length_bytes.size();
            if (file_size - cursor < record_header_size) {
                throw std::runtime_error(
                    "GHO 零长度块后缺少 record header");
            }
            read_exact(cursor, record_header,
                       "读取 GHO 零长度块后的 record header 失败");
            if (!is_record(record_header, 0)) {
                throw std::runtime_error(
                    "GHO 零长度块后未紧跟有效 record");
            }
            continue;
        }
        if (stored_size < 3 || stored_size > maximum_stored_block_size ||
            stored_size > virtual_file_size - cursor) {
            throw std::runtime_error("GHO 数据块长度无效");
        }
        const auto encoded_size = static_cast<std::uint32_t>(
            stored_size - block_length_bytes.size());
        std::uint32_t block_size = encoded_size;
        if (compressed) {
            if (encoded_size > maximum_stored_block_size - 2U) {
                throw std::runtime_error("GHO 压缩数据块超过最大长度");
            }
            std::vector<std::byte> encoded(encoded_size);
            read_exact(cursor + block_length_bytes.size(), encoded,
                       "读取 GHO 压缩数据块失败");
            const auto decoded = decode_block(encoded, compression);
            block_size = static_cast<std::uint32_t>(decoded.size());
        } else if (block_size > maximum_block_size) {
            throw std::runtime_error("GHO 未压缩数据块超过 32 KiB");
        }
        if (active_partition >= partitions.size()) {
            throw std::runtime_error("GHO 数据块没有对应的分区");
        }
        blocks.push_back({logical_size, cursor + block_length_bytes.size(),
                          block_size, encoded_size});
        if (logical_size > std::numeric_limits<std::uint64_t>::max() - block_size) {
            throw std::runtime_error("GHO 分区逻辑长度溢出");
        }
        logical_size += block_size;
        partitions[active_partition].size = logical_size;
        ++partitions[active_partition].block_count;
        cursor += stored_size;
    }
    if (!end_seen || partitions.empty()) {
        throw std::runtime_error("GHO 镜像缺少有效 end record");
    }

    std::uint64_t logical_disk_size = 0;
    bool composed_disk = false;
    if (track0_data.size() >= 512 &&
        track0_data[510] == std::byte{0x55} &&
        track0_data[511] == std::byte{0xaa}) {
        struct MbrPartition {
            std::uint64_t start_sector{};
            std::uint64_t size{};
        };
        std::vector<MbrPartition> mbr_partitions;
        bool valid_primary_partitions = true;
        for (std::size_t index = 0; index < 4; ++index) {
            const auto entry = 446U + index * 16U;
            const auto type = std::to_integer<unsigned char>(
                track0_data[entry + 4U]);
            if (type == 0) {
                continue;
            }
            const auto status = std::to_integer<unsigned char>(
                track0_data[entry]);
            if ((status != 0 && status != 0x80U) ||
                type == 0x05U || type == 0x0fU || type == 0x85U) {
                valid_primary_partitions = false;
                break;
            }
            const auto start = static_cast<std::uint64_t>(
                std::to_integer<unsigned char>(track0_data[entry + 8U])) |
                (static_cast<std::uint64_t>(
                     std::to_integer<unsigned char>(track0_data[entry + 9U]))
                 << 8U) |
                (static_cast<std::uint64_t>(
                     std::to_integer<unsigned char>(track0_data[entry + 10U]))
                 << 16U) |
                (static_cast<std::uint64_t>(
                     std::to_integer<unsigned char>(track0_data[entry + 11U]))
                 << 24U);
            const auto sectors = static_cast<std::uint64_t>(
                std::to_integer<unsigned char>(track0_data[entry + 12U])) |
                (static_cast<std::uint64_t>(
                     std::to_integer<unsigned char>(track0_data[entry + 13U]))
                 << 8U) |
                (static_cast<std::uint64_t>(
                     std::to_integer<unsigned char>(track0_data[entry + 14U]))
                 << 16U) |
                (static_cast<std::uint64_t>(
                     std::to_integer<unsigned char>(track0_data[entry + 15U]))
                 << 24U);
            if (start == 0 || sectors == 0 ||
                sectors > std::numeric_limits<std::uint64_t>::max() /
                              sector_bytes) {
                valid_primary_partitions = false;
                break;
            }
            mbr_partitions.push_back({start, sectors * sector_bytes});
        }
        if (valid_primary_partitions &&
            mbr_partitions.size() == partitions.size()) {
            std::uint64_t disk_size = track0_data.size();
            for (std::size_t index = 0; index < partitions.size(); ++index) {
                auto& partition = partitions[index];
                const auto& mbr = mbr_partitions[index];
                if (mbr.size != partition.size ||
                    mbr.start_sector >
                        std::numeric_limits<std::uint64_t>::max() /
                            sector_bytes) {
                    valid_primary_partitions = false;
                    break;
                }
                const auto start_offset = mbr.start_sector * sector_bytes;
                if (start_offset < track0_data.size() ||
                    partition.size >
                        std::numeric_limits<std::uint64_t>::max() -
                            start_offset) {
                    valid_primary_partitions = false;
                    break;
                }
                for (std::size_t prior = 0; prior < index; ++prior) {
                    const auto prior_start = partitions[prior].logical_offset;
                    const auto prior_end = prior_start + partitions[prior].size;
                    if (start_offset < prior_end &&
                        prior_start < start_offset + partition.size) {
                        valid_primary_partitions = false;
                        break;
                    }
                }
                if (!valid_primary_partitions) {
                    break;
                }
                partition.logical_offset = start_offset;
                disk_size = std::max(disk_size, start_offset + partition.size);
            }
            if (valid_primary_partitions) {
                logical_disk_size = disk_size;
                composed_disk = true;
            }
        }
    }
    if (!composed_disk) {
        if (partitions.size() != 1) {
            throw std::runtime_error(
                "无法将多个 GHO 分区与 Track 0 MBR 安全对应");
        }
        partitions.front().logical_offset = 0;
        logical_disk_size = partitions.front().size;
    }
    source_files_ = std::move(source_files);
    source_path_ = image_path;
    size_ = logical_disk_size;
    track0_data_ = composed_disk ? std::move(track0_data)
                                 : std::vector<std::byte>{};
    partitions_ = std::move(partitions);
    blocks_ = std::move(blocks);
    compression_ = compression;
    cached_block_.reset();
    cached_data_.clear();
}

void GhoImageDriver::load_block(std::size_t index) const {
    if (cached_block_ == index) {
        return;
    }
    const auto& block = blocks_.at(index);
    if (block.source_size > maximum_stored_block_size - 2U) {
        throw std::runtime_error("GHO 压缩块源长度超限");
    }
    std::vector<std::byte> encoded(block.source_size);
    read_source_at(block.source_offset, encoded);
    cached_data_ = decode_block(encoded, compression_);
    if (cached_data_.size() != block.size) {
        throw std::runtime_error("GHO 压缩块长度与索引不匹配");
    }
    cached_block_ = index;
}

void GhoImageDriver::read_source_at(
    std::uint64_t offset, std::span<std::byte> destination) const {
    std::size_t copied = 0;
    while (copied < destination.size()) {
        const auto logical_offset = offset + copied;
        const auto source = std::find_if(
            source_files_.begin(), source_files_.end(),
            [logical_offset](const SourceFile& file) {
                return logical_offset >= file.logical_offset &&
                       logical_offset - file.logical_offset < file.size;
            });
        if (source == source_files_.end()) {
            throw std::runtime_error("GHO 读取偏移超出分卷范围");
        }
        const auto source_position =
            source->source_offset + logical_offset - source->logical_offset;
        if (source_position > static_cast<std::uint64_t>(
                                  std::numeric_limits<std::streamoff>::max())) {
            throw std::out_of_range("GHO 读取偏移超出平台文件流范围");
        }
        const auto available = static_cast<std::size_t>(
            std::min<std::uint64_t>(
                source->size - (logical_offset - source->logical_offset),
                destination.size() - copied));
        source->input.clear();
        source->input.seekg(static_cast<std::streamoff>(source_position));
        source->input.read(
            reinterpret_cast<char*>(destination.data() + copied),
            static_cast<std::streamsize>(available));
        if (source->input.gcount() !=
            static_cast<std::streamsize>(available)) {
            throw std::runtime_error("读取 GHO 分卷数据失败");
        }
        copied += available;
    }
}

std::size_t GhoImageDriver::read_sector(
    std::uint64_t sector_number,
    std::span<std::byte> destination) const {
    if (destination.size() != sector_bytes) {
        throw std::invalid_argument("GHO read_sector 要求 512 字节缓冲区");
    }
    if (sector_number > std::numeric_limits<std::uint64_t>::max() /
                            sector_bytes) {
        throw std::out_of_range("GHO sector offset overflow");
    }
    return read_at(sector_number * sector_bytes, destination);
}

std::size_t GhoImageDriver::read_at(
    std::uint64_t offset, std::span<std::byte> destination) const {
    std::scoped_lock lock(mutex_);
    if (source_files_.empty()) {
        throw std::runtime_error("GHO 镜像尚未打开");
    }
    if (offset > size_) {
        throw std::out_of_range("GHO 读取偏移超出逻辑分区范围");
    }
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(destination.size(), size_ - offset));
    std::fill_n(destination.begin(), count, std::byte{0});
    const auto end = offset + count;
    if (offset < track0_data_.size()) {
        const auto track0_end =
            std::min<std::uint64_t>(end, track0_data_.size());
        const auto track0_count =
            static_cast<std::size_t>(track0_end - offset);
        std::copy_n(track0_data_.begin() +
                        static_cast<std::ptrdiff_t>(offset),
                    track0_count, destination.begin());
    }
    for (const auto& partition : partitions_) {
        const auto partition_end = partition.logical_offset + partition.size;
        const auto requested_start = std::max(offset, partition.logical_offset);
        const auto requested_end = std::min(end, partition_end);
        if (requested_start >= requested_end) {
            continue;
        }
        std::uint64_t block_logical_offset = 0;
        for (std::size_t block_index = partition.first_block;
             block_index < partition.first_block + partition.block_count;
             ++block_index) {
            const auto& block = blocks_[block_index];
            const auto block_start =
                partition.logical_offset + block_logical_offset;
            const auto block_end = block_start + block.size;
            if (requested_start < block_end && block_start < requested_end) {
                const auto chunk_start = std::max(requested_start, block_start);
                const auto chunk_end = std::min(requested_end, block_end);
                const auto chunk_size =
                    static_cast<std::size_t>(chunk_end - chunk_start);
                const auto destination_offset =
                    static_cast<std::size_t>(chunk_start - offset);
                const auto block_offset =
                    static_cast<std::size_t>(chunk_start - block_start);
                if (compression_ != 0) {
                    load_block(block_index);
                    std::copy_n(
                        cached_data_.begin() +
                            static_cast<std::ptrdiff_t>(block_offset),
                        chunk_size,
                        destination.begin() +
                            static_cast<std::ptrdiff_t>(destination_offset));
                } else {
                    const auto source_offset =
                        block.source_offset + block_offset;
                    read_source_at(
                        source_offset,
                        destination.subspan(destination_offset, chunk_size));
                }
            }
            block_logical_offset += block.size;
            if (block_start >= requested_end) {
                break;
            }
        }
    }
    return count;
}

std::uint64_t GhoImageDriver::get_size() const noexcept {
    return size_;
}

std::uint32_t GhoImageDriver::sector_size() const noexcept {
    return static_cast<std::uint32_t>(sector_bytes);
}

std::filesystem::path GhoImageDriver::source_path() const {
    return source_path_;
}

std::string_view GhoImageDriver::format_name() const noexcept {
    return "Norton Ghost GHO (multi-partition)";
}

void GhoImageDriver::close() noexcept {
    std::scoped_lock lock(mutex_);
    for (auto& source : source_files_) {
        if (source.input.is_open()) {
            source.input.close();
        }
    }
    source_files_.clear();
    source_path_.clear();
    size_ = 0;
    track0_data_.clear();
    partitions_.clear();
    blocks_.clear();
    compression_ = 0;
    cached_block_.reset();
    cached_data_.clear();
}

} // namespace uife
