#include "uife/drivers/image/CueBinImageDriver.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace uife {
namespace {

std::string trim(std::string value) {
    const auto is_space = [](unsigned char character) {
        return std::isspace(character) != 0;
    };
    const auto first = std::find_if_not(value.begin(), value.end(), is_space);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), is_space).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::string uppercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::toupper(character));
                   });
    return value;
}

std::uint64_t parse_index(std::string_view value) {
    const auto first_colon = value.find(':');
    const auto second_colon = first_colon == std::string_view::npos
        ? std::string_view::npos : value.find(':', first_colon + 1);
    if (first_colon == std::string_view::npos ||
        second_colon == std::string_view::npos ||
        value.find(':', second_colon + 1) != std::string_view::npos) {
        throw std::runtime_error("BIN+CUE INDEX 时间戳格式无效");
    }

    const auto parse_number = [](std::string_view number) {
        if (number.empty()) {
            throw std::runtime_error("BIN+CUE INDEX 时间戳字段为空");
        }
        std::uint64_t result = 0;
        for (const auto character : number) {
            if (character < '0' || character > '9') {
                throw std::runtime_error("BIN+CUE INDEX 时间戳字段不是数字");
            }
            const auto digit = static_cast<std::uint64_t>(character - '0');
            if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
                throw std::runtime_error("BIN+CUE INDEX 时间戳溢出");
            }
            result = result * 10 + digit;
        }
        return result;
    };

    const auto minutes = parse_number(value.substr(0, first_colon));
    const auto seconds = parse_number(
        value.substr(first_colon + 1, second_colon - first_colon - 1));
    const auto frames = parse_number(value.substr(second_colon + 1));
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (seconds >= 60 || frames >= 75 ||
        minutes > (maximum - seconds) / 60) {
        throw std::runtime_error("BIN+CUE INDEX 时间戳超出有效范围");
    }
    const auto total_seconds = minutes * 60 + seconds;
    if (total_seconds > (maximum - frames) / 75) {
        throw std::runtime_error("BIN+CUE INDEX 时间戳超出有效范围");
    }
    return total_seconds * 75 + frames;
}

std::filesystem::path resolve_data_file(const std::filesystem::path& cue_path,
                                        const std::string& filename) {
    const std::filesystem::path relative_path(filename);
    if (relative_path.empty() || relative_path.is_absolute() ||
        relative_path.has_root_name() || relative_path.has_root_directory()) {
        throw std::runtime_error("BIN+CUE 只允许引用同目录下的相对数据文件");
    }
    for (const auto& component : relative_path) {
        if (component == "." || component == "..") {
            throw std::runtime_error(
                "BIN+CUE 数据文件路径不能离开 CUE 文件所在目录");
        }
    }

    std::error_code error;
    const auto parent = cue_path.parent_path().empty()
        ? std::filesystem::current_path(error)
        : cue_path.parent_path();
    if (error) {
        throw std::runtime_error("无法读取当前目录: " + error.message());
    }
    const auto cue_directory = std::filesystem::weakly_canonical(parent, error);
    if (error) {
        throw std::runtime_error("无法解析 CUE 文件所在目录: " + error.message());
    }
    const auto data_path = std::filesystem::canonical(
        cue_directory / relative_path, error);
    if (error) {
        throw std::runtime_error("无法打开 BIN+CUE 数据文件: " + error.message());
    }
    const auto relative_data_path = data_path.lexically_relative(cue_directory);
    if (relative_data_path.empty() || relative_data_path.is_absolute() ||
        *relative_data_path.begin() == "..") {
        throw std::runtime_error(
            "BIN+CUE 数据文件不能通过链接离开 CUE 文件所在目录");
    }
    return data_path;
}

struct CueTrack {
    std::size_t file_index{};
    std::uint32_t stored_sector_size{};
    std::uint64_t start_sector{};
    bool has_index{};
};

} // namespace

CueBinImageDriver::~CueBinImageDriver() {
    close();
}

void CueBinImageDriver::open(const std::filesystem::path& image_path) {
    close();
    std::ifstream cue(image_path, std::ios::binary);
    if (!cue) {
        throw std::runtime_error("无法以只读方式打开 CUE 文件");
    }

    std::vector<std::string> data_filenames;
    std::vector<CueTrack> cue_tracks;
    std::size_t current_file = 0;
    bool have_file = false;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(cue, line)) {
        ++line_number;
        if (line_number == 1 && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xef &&
            static_cast<unsigned char>(line[1]) == 0xbb &&
            static_cast<unsigned char>(line[2]) == 0xbf) {
            line.erase(0, 3);
        }
        line = trim(std::move(line));
        if (line.empty()) {
            continue;
        }
        std::istringstream fields(line);
        std::string command;
        fields >> command;
        command = uppercase(std::move(command));
        if (command == "FILE") {
            std::string filename;
            fields >> std::ws;
            if (fields.peek() == '"') {
                fields.get();
                if (!std::getline(fields, filename, '"')) {
                    throw std::runtime_error("BIN+CUE FILE 文件名引号未闭合");
                }
            } else {
                fields >> filename;
            }
            std::string file_type;
            std::string extra;
            fields >> file_type;
            if (filename.empty() || uppercase(file_type) != "BINARY" ||
                (fields >> extra)) {
                throw std::runtime_error(
                    "BIN+CUE 当前只支持 BINARY 数据文件");
            }
            data_filenames.push_back(std::move(filename));
            current_file = data_filenames.size() - 1;
            have_file = true;
        } else if (command == "TRACK") {
            unsigned int track_number = 0;
            std::string mode;
            std::string extra;
            if (!have_file ||
                (!cue_tracks.empty() && !cue_tracks.back().has_index) ||
                cue_tracks.size() >= 99 ||
                !(fields >> track_number >> mode) ||
                track_number != cue_tracks.size() + 1 ||
                (fields >> extra)) {
                throw std::runtime_error(
                    "BIN+CUE 轨道号无效或上一轨缺少 INDEX 01");
            }
            mode = uppercase(std::move(mode));
            std::uint32_t track_sector_size = 0;
            if (mode == "MODE1/2048") {
                track_sector_size = 2048;
            } else if (mode == "MODE1/2352") {
                track_sector_size = 2352;
            } else {
                throw std::runtime_error(
                    "BIN+CUE 当前仅支持 MODE1/2048 和 MODE1/2352 轨道");
            }
            cue_tracks.push_back(
                {current_file, track_sector_size, 0, false});
        } else if (command == "INDEX") {
            unsigned int index_number = 0;
            std::string timestamp;
            std::string extra;
            if (cue_tracks.empty() || cue_tracks.back().has_index ||
                !(fields >> index_number >> timestamp) || index_number != 1 ||
                (fields >> extra)) {
                throw std::runtime_error(
                    "BIN+CUE 当前要求每条轨道具有唯一且有效的 INDEX 01");
            }
            auto& track = cue_tracks.back();
            track.start_sector = parse_index(timestamp);
            track.has_index = true;
        } else if (command == "REM" || command == "TITLE" ||
                   command == "PERFORMER" || command == "CATALOG" ||
                   command == "ISRC" || command == "FLAGS") {
            continue;
        } else {
            throw std::runtime_error("BIN+CUE 不支持的指令: " + command);
        }
    }
    if (cue.bad()) {
        throw std::runtime_error("读取 CUE 文件失败");
    }
    if (!have_file || cue_tracks.empty() ||
        std::any_of(cue_tracks.begin(), cue_tracks.end(),
                    [](const CueTrack& track) { return !track.has_index; })) {
        throw std::runtime_error(
            "BIN+CUE 文件缺少 FILE、TRACK 或 INDEX 01 声明");
    }

    std::vector<std::filesystem::path> data_paths;
    std::vector<std::uint64_t> file_sector_counts;
    data_paths.reserve(data_filenames.size());
    file_sector_counts.reserve(data_filenames.size());
    for (std::size_t index = 0; index < data_filenames.size(); ++index) {
        const auto first_track = std::find_if(
            cue_tracks.begin(), cue_tracks.end(),
            [index](const CueTrack& track) {
                return track.file_index == index;
            });
        if (first_track == cue_tracks.end()) {
            throw std::runtime_error(
                "BIN+CUE 声明了未使用的数据文件");
        }
        if (std::any_of(first_track + 1, cue_tracks.end(),
                        [index, sector_size = first_track->stored_sector_size](
                            const CueTrack& track) {
                            return track.file_index == index &&
                                   track.stored_sector_size != sector_size;
                        })) {
            throw std::runtime_error(
                "BIN+CUE 同一文件中的各轨道必须使用相同的物理扇区格式");
        }
        const auto data_path =
            resolve_data_file(image_path, data_filenames[index]);
        std::error_code error;
        const auto data_size = std::filesystem::file_size(data_path, error);
        if (error) {
            throw std::runtime_error("无法读取 BIN+CUE 数据文件大小: " +
                                     error.message());
        }
        const auto file_sector_size = first_track->stored_sector_size;
        if (data_size == 0 || data_size % file_sector_size != 0) {
            throw std::runtime_error(
                "BIN+CUE 数据为空或未按物理扇区对齐");
        }
        data_paths.push_back(data_path);
        file_sector_counts.push_back(data_size / file_sector_size);
    }
    std::vector<TrackExtent> tracks;
    tracks.reserve(cue_tracks.size());
    std::uint64_t logical_offset = 0;
    for (std::size_t index = 0; index < cue_tracks.size(); ++index) {
        const auto& cue_track = cue_tracks[index];
        const auto file_index = cue_track.file_index;
        const auto start = cue_track.start_sector;
        auto end = file_sector_counts[file_index];
        for (auto next = index + 1; next < cue_tracks.size(); ++next) {
            if (cue_tracks[next].file_index == file_index) {
                end = cue_tracks[next].start_sector;
                break;
            }
        }
        if (start >= file_sector_counts[file_index] || end <= start ||
            end > file_sector_counts[file_index]) {
            throw std::runtime_error(
                "BIN+CUE INDEX 01 顺序无效或超出数据文件范围");
        }
        const auto track_sector_count = end - start;
        if (track_sector_count >
                std::numeric_limits<std::uint64_t>::max() / 2048 ||
            track_sector_count * 2048 >
                std::numeric_limits<std::uint64_t>::max() - logical_offset) {
            throw std::runtime_error("BIN+CUE 逻辑镜像容量溢出");
        }
        const auto track_length = track_sector_count * 2048;
        const auto payload_offset =
            cue_track.stored_sector_size == 2352 ? 16U : 0U;
        tracks.push_back({logical_offset, track_length, start, file_index,
                          cue_track.stored_sector_size, payload_offset});
        logical_offset += track_length;
    }

    std::vector<std::ifstream> candidate_inputs;
    candidate_inputs.reserve(data_paths.size());
    for (const auto& data_path : data_paths) {
        std::ifstream candidate(data_path, std::ios::binary);
        if (!candidate) {
            throw std::runtime_error("无法以只读方式打开 BIN+CUE 数据文件");
        }
        candidate_inputs.push_back(std::move(candidate));
    }
    inputs_ = std::move(candidate_inputs);
    source_path_ = image_path;
    image_size_ = logical_offset;
    tracks_ = std::move(tracks);
}

std::size_t CueBinImageDriver::read_sector(
    std::uint64_t sector_number, std::span<std::byte> destination) const {
    if (destination.size() != sector_size()) {
        throw std::invalid_argument(
            "BIN+CUE read_sector 要求恰好一个 2048 字节逻辑扇区");
    }
    if (sector_number > std::numeric_limits<std::uint64_t>::max() / sector_size()) {
        throw std::out_of_range("BIN+CUE 扇区号溢出");
    }
    return read_at(sector_number * sector_size(), destination);
}

std::size_t CueBinImageDriver::read_at(
    std::uint64_t offset, std::span<std::byte> destination) const {
    std::scoped_lock lock(mutex_);
    if (inputs_.empty()) {
        throw std::runtime_error("BIN+CUE 镜像尚未打开");
    }
    if (offset > image_size_) {
        throw std::out_of_range("BIN+CUE 读取偏移超出镜像大小");
    }
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(image_size_ - offset, destination.size()));
    std::size_t copied = 0;
    while (copied < count) {
        const auto logical_offset = offset + copied;
        const auto track = std::upper_bound(
            tracks_.begin(), tracks_.end(), logical_offset,
            [](std::uint64_t value, const TrackExtent& candidate) {
                return value < candidate.logical_offset;
            });
        if (track == tracks_.begin()) {
            throw std::runtime_error("BIN+CUE 逻辑扇区未映射到任何轨道");
        }
        const auto& extent = *std::prev(track);
        const auto track_offset = logical_offset - extent.logical_offset;
        if (track_offset >= extent.logical_length) {
            throw std::runtime_error("BIN+CUE 逻辑读取超出轨道范围");
        }
        const auto sector = extent.file_sector_offset + track_offset / 2048;
        const auto in_sector = track_offset % 2048;
        const auto chunk = std::min<std::size_t>(
            std::min<std::size_t>(
                count - copied, static_cast<std::size_t>(2048 - in_sector)),
            static_cast<std::size_t>(extent.logical_length - track_offset));
        const auto physical_offset =
            sector * extent.stored_sector_size + extent.payload_offset +
            in_sector;
        if (physical_offset >
            static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            throw std::out_of_range("BIN+CUE 读取偏移超出当前平台文件流范围");
        }
        auto& input = inputs_[extent.file_index];
        input.clear();
        input.seekg(static_cast<std::streamoff>(physical_offset), std::ios::beg);
        if (!input) {
            throw std::runtime_error("定位 BIN+CUE 数据文件失败");
        }
        input.read(reinterpret_cast<char*>(destination.data() + copied),
                    static_cast<std::streamsize>(chunk));
        if (input.gcount() != static_cast<std::streamsize>(chunk)) {
            throw std::runtime_error("读取 BIN+CUE 数据时遇到意外 EOF");
        }
        copied += chunk;
    }
    return count;
}

std::uint64_t CueBinImageDriver::get_size() const noexcept {
    return image_size_;
}

std::uint32_t CueBinImageDriver::sector_size() const noexcept {
    return 2048;
}

std::filesystem::path CueBinImageDriver::source_path() const {
    return source_path_;
}

std::string_view CueBinImageDriver::format_name() const noexcept {
    return "BIN+CUE";
}

void CueBinImageDriver::close() noexcept {
    std::scoped_lock lock(mutex_);
    for (auto& input : inputs_) {
        if (input.is_open()) {
            input.close();
        }
    }
    inputs_.clear();
    source_path_.clear();
    image_size_ = 0;
    tracks_.clear();
}

} // namespace uife
