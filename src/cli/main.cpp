#include "uife/DriverFactory.h"
#include "uife/Extractor.h"
#include "uife/FsParserFactory.h"
#include "uife/PartitionScanner.h"
#include "uife/vfs/VFS.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

// 由 CMake 注入；单独编译 main.cpp（比如手工 g++ 试一下）时留可见的占位，
// 而不是让 `capabilities` 因为找不到宏而编译失败。
#ifndef UIFE_VERSION
#define UIFE_VERSION "unknown"
#endif
#ifndef UIFE_BUILD_TYPE
#define UIFE_BUILD_TYPE "unknown"
#endif
#ifndef UIFE_COMPILER
#define UIFE_COMPILER "unknown"
#endif

namespace {

// ---------------------------------------------------------------------------
// 退出码约定（见 README「CLI 退出码」一节）:
//   0  成功
//   1  运行时错误（I/O、解析失败、无法打开清单等）
//   2  用法错误（未知命令/选项、非法或缺失参数、分区序号越界等）
//   3  镜像格式已识别但未接入读取驱动
//   4  文件系统已识别但当前构建没有可用解析器
// ---------------------------------------------------------------------------
constexpr int kExitSuccess = 0;
constexpr int kExitRuntimeError = 1;
constexpr int kExitUsageError = 2;
constexpr int kExitDriverMissing = 3;
constexpr int kExitFsParserMissing = 4;
// 资源预算用尽：和"镜像有问题"是两件事，批处理脚本据此决定是调大预算还是换镜像。
constexpr int kExitResourceLimit = 5;

// 携带预期退出码的错误；由 CLI 各决策点抛出，顶层按 exit_code 返回。
struct CliError : std::runtime_error {
    int exit_code;
    CliError(int code, const std::string& message)
        : std::runtime_error(message), exit_code(code) {}
};

struct MountedFilesystem {
    std::unique_ptr<uife::IFSParser> parser;
    std::uint64_t offset{};
    std::uint64_t length{};
};

void print_names(std::ostream& out, const std::vector<std::string>& names) {
    if (names.empty()) {
        out << "无";
        return;
    }
    for (std::size_t index = 0; index < names.size(); ++index) {
        out << (index == 0 ? "" : "、") << names[index];
    }
}

void print_usage(std::ostream& out) {
    out <<
        "UIFE - Universal Image Forensics Engine (只读预览版)\n"
        "用法:\n"
        "  uife info <镜像>\n"
        "  uife inspect <镜像>  (info 的别名)\n"
        "  uife probe <镜像>    逐位置报告文件系统识别与可用后端\n"
        "  uife list <镜像> [VFS路径]\n"
        "  uife extract <镜像> <VFS路径> -o <目录> [--manifest <文件>]\n"
        "  uife restore <镜像> -o <目录> [--manifest <文件>]\n"
        "  uife bench <镜像> [--size <字节>]  顺序读取吞吐基准\n"
        "  uife batch <清单文件>\n"
        "  uife capabilities    报告本构建支持的格式、后端与能力（配合 --json 供脚本采集）\n"
        "选项:\n"
        "  --partition <序号> 选择 info 显示的分区（也可用 -p）\n"
        "  --password-stdin   从标准输入读取 APFS 加密卷密码（仅 list/extract/restore）\n"
        "  --manifest <文件>  导出后写入 sha256sum 兼容清单（仅 extract/restore）\n"
        "  --metadata-report <文件>  写出后备元数据报告 JSON（仅 extract/restore）；\n"
        "                     默认只在有属性未能还原时写到 <导出目录>/uife-metadata.json\n"
        "  --restore-acl      把源对象的 ACL 写回导出文件（仅 Windows 的 extract/restore；\n"
        "                     还原后的 DACL 可能让当前账户读不了导出文件）\n"
        "  --max-entries <N>  list/extract/restore 的节点数上限，超出即失败（退出码 5）\n"
        "  --time-limit <秒>  list/extract/restore 的墙钟预算，超时即失败（退出码 5）\n"
        "  --size <字节>      bench 读取上限（默认整个镜像）\n"
        "  --json             以 JSON 输出结果（全局选项，支持全部命令；batch 为逐行 JSON）\n"
        "  --log-format <格式> 打开操作日志：text（人类可读）或 json（每行一个对象）；\n"
        "                     时间戳固定 UTC 带毫秒，默认关闭\n"
        "  --log-file <文件>  把操作日志写到文件而不是 stderr（自动建父目录）\n"
        "  -h, --help         显示此帮助\n\n"
        "退出码:\n"
        "  0 成功  1 运行时错误  2 用法错误  3 镜像驱动未接入  4 文件系统解析器缺失\n"
        "  5 资源上限（--max-entries / --time-limit）用尽，导出已中止\n\n"
        "清单文件格式 (batch): 每行一条完整命令（省略开头的 uife），\n"
        "空行与以 # 开头的行被忽略。\n\n"
        "命名数据流导出约定: NTFS 备用数据流导出为同级侧车文件\n"
        "  <文件名>__ads__<编码后的流名>；流名仅保留 [A-Za-z0-9._-]，其余字节按 %XX 转义。\n\n"
        "当前可读取的镜像格式:\n  ";
    print_names(out, uife::DriverFactory::instance().supported_formats());
    out << "\n当前已注册的文件系统解析器:\n  ";
    print_names(out, uife::FsParserFactory::instance().supported_names());

    const auto recognized_only =
        uife::DriverFactory::instance().recognized_only_formats();
    if (!recognized_only.empty()) {
        out << "\n\n仅能识别、尚不能读取的镜像格式:\n  ";
        print_names(out, recognized_only);
    }
    out << "\n\n说明: 文件系统解析器列表取决于当前构建和可用依赖；"
           "镜像容器可读取不代表其内部文件系统一定受支持。\n";
}

std::filesystem::path path_from_utf8(std::string_view value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const unsigned char character : value) {
        encoded.push_back(static_cast<char8_t>(character));
    }
    return std::filesystem::path(encoded);
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(encoded.data()),
                       encoded.size());
}

// ---------------------------------------------------------------------------
// JSON 输出辅助
// ---------------------------------------------------------------------------
std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2);
    char buffer[7];
    for (const char character : value) {
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                    static_cast<unsigned char>(character));
                result += buffer;
            } else {
                result += character;
            }
        }
    }
    return result;
}

std::string json_string(std::string_view value) {
    return "\"" + json_escape(value) + "\"";
}

std::string json_bool(bool value) {
    return value ? "true" : "false";
}

std::string node_type_label(uife::NodeType type) {
    switch (type) {
    case uife::NodeType::directory:
        return "directory";
    case uife::NodeType::regular_file:
        return "file";
    case uife::NodeType::symbolic_link:
        return "symlink";
    default:
        return "other";
    }
}

std::string json_number(double value) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(3);
    out << value;
    return out.str();
}

std::string json_time(bool present,
                      const std::chrono::system_clock::time_point& value) {
    if (!present) {
        return "null";
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        value.time_since_epoch());
    return std::to_string(seconds.count());
}

// ACL 数组：掩码以十六进制字符串输出（与 Windows 工具的显示习惯一致），
// SID 保持 SDDL 原文，不做账户名解析。
std::string json_ace_list(const std::vector<uife::VFSAce>& aces) {
    std::string json = "[";
    for (std::size_t index = 0; index < aces.size(); ++index) {
        if (index != 0) {
            json += ",";
        }
        const auto& ace = aces[index];
        json += "{\"type\":" + json_string(ace.type);
        json += ",\"inherited\":" + json_bool(ace.inherited);
        json += ",\"mask\":\"" + [&ace] {
            char buffer[11]{};
            std::snprintf(buffer, sizeof(buffer), "0x%08x", ace.mask);
            return std::string(buffer);
        }() + "\"";
        json += ",\"sid\":" + json_string(ace.sid) + "}";
    }
    json += "]";
    return json;
}

std::string json_security(const uife::VFSSecurityDescriptor& security) {
    std::string json = "{";
    json += "\"owner\":" + json_string(security.owner_sid);
    json += ",\"group\":" + json_string(security.group_sid);
    json += ",\"dacl_present\":" + json_bool(security.dacl_present);
    json += ",\"dacl_protected\":" + json_bool(security.dacl_protected);
    json += ",\"dacl\":" + json_ace_list(security.dacl);
    json += ",\"sacl_present\":" + json_bool(security.sacl_present);
    json += ",\"sacl_protected\":" + json_bool(security.sacl_protected);
    json += ",\"sacl\":" + json_ace_list(security.sacl);
    json += "}";
    return json;
}

// list --json 的元数据对象：把解析器可提供的取证元数据全部输出，
// 使下游无需再实现一遍解析逻辑（不可用字段以 null / false 表示）。
std::string json_metadata(const uife::VFSMetadata& metadata) {
    std::string json = "{";
    json += "\"size\":" + std::to_string(metadata.size);
    json += ",\"size_valid\":" + json_bool(metadata.size_valid);
    json += ",\"inode\":" + std::to_string(metadata.inode);
    json += ",\"link_count\":" + std::to_string(metadata.link_count);
    json += ",\"mode\":" + std::to_string(metadata.mode);
    json += ",\"uid\":" + std::to_string(metadata.uid);
    json += ",\"gid\":" + std::to_string(metadata.gid);
    json += ",\"has_owner\":" + json_bool(metadata.has_owner);
    json += ",\"created\":" + json_time(metadata.has_created_time, metadata.created);
    json += ",\"accessed\":" + json_time(metadata.has_accessed_time, metadata.accessed);
    json += ",\"modified\":" + json_time(metadata.has_modified_time, metadata.modified);
    json += ",\"sparse\":" + json_bool(metadata.sparse);
    json += ",\"compressed\":" + json_bool(metadata.compressed);
    json += ",\"encrypted\":" + json_bool(metadata.encrypted);
    json += ",\"reparse_point\":" + json_bool(metadata.reparse_point);
    if (metadata.has_file_attributes) {
        json += ",\"file_attributes\":\"" + [&metadata] {
            char buffer[11]{};
            std::snprintf(buffer, sizeof(buffer), "0x%08x", metadata.file_attributes);
            return std::string(buffer);
        }() + "\"";
    } else {
        json += ",\"file_attributes\":null";
    }
    if (metadata.has_security_id) {
        json += ",\"security_id\":" + std::to_string(metadata.security_id);
    } else {
        json += ",\"security_id\":null";
    }
    if (metadata.has_security && metadata.security.valid) {
        json += ",\"security\":" + json_security(metadata.security);
    } else {
        json += ",\"security\":null";
    }
    if (metadata.has_reparse_tag) {
        json += ",\"reparse_tag\":\"" + [&metadata] {
            char buffer[11]{};
            std::snprintf(buffer, sizeof(buffer), "0x%08x", metadata.reparse_tag);
            return std::string(buffer);
        }() + "\"";
    } else {
        json += ",\"reparse_tag\":null";
    }
    json += ",\"symlink_target\":";
    json += metadata.symlink_target.empty()
        ? "null" : json_string(metadata.symlink_target);
    json += ",\"extents\":[";
    for (std::size_t index = 0; index < metadata.extents.size(); ++index) {
        if (index != 0) {
            json += ",";
        }
        const auto& extent = metadata.extents[index];
        json += "{\"offset\":" + std::to_string(extent.offset);
        json += ",\"length\":" + std::to_string(extent.length);
        json += ",\"allocated\":" + json_bool(extent.allocated) + "}";
    }
    json += "],\"extents_truncated\":";
    json += json_bool(metadata.extents_truncated);
    json += ",\"ads\":[";
    for (std::size_t index = 0; index < metadata.alternate_streams.size(); ++index) {
        if (index != 0) {
            json += ",";
        }
        const auto& stream = metadata.alternate_streams[index];
        json += "{\"name\":" + json_string(stream.name);
        json += ",\"size\":" + std::to_string(stream.size);
        json += ",\"size_valid\":" + json_bool(stream.size_valid);
        json += ",\"readable\":" + json_bool(stream.readable) + "}";
    }
    json += "]}";
    return json;
}

// 提取所有命令共享的全局 JSON 开关，并将其从参数中移除。
// 带毫秒的 UTC 时间戳。操作日志要给"谁在什么时候对哪个镜像做了什么"留痕，
// 秒级精度在多条命令拼一条时间线时不够用（batch 一行一条）。
std::string log_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds);
    const auto epoch = std::chrono::system_clock::to_time_t(seconds);
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &epoch);
#else
    gmtime_r(&epoch, &parts);
#endif
    std::array<char, 32> buffer{};
    std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%S", &parts);
    char result[48]{};
    std::snprintf(result, sizeof(result), "%s.%03dZ", buffer.data(),
                  static_cast<int>(millis.count()));
    return result;
}

enum class LogFormat { text, json };

// 操作日志：默认关闭（不改变现有行为）；`--log-format text|json` 打开，
// `--log-file` 重定向到文件。取证场景里这份日志本身就是一条证据链记录，
// 所以时间戳一律 UTC、带毫秒，事件名保持稳定以便脚本消费。
class OperationLog {
public:
    void configure(LogFormat format, const std::optional<std::filesystem::path>& path) {
        format_ = format;
        if (path) {
            std::error_code directory_error;
            if (path->has_parent_path()) {
                std::filesystem::create_directories(path->parent_path(), directory_error);
            }
            file_.open(*path, std::ios::binary | std::ios::trunc);
            if (!file_) {
                throw std::runtime_error("无法写入操作日志: " + path->string());
            }
            stream_ = &file_;
        } else {
            stream_ = &std::cerr;
        }
    }

    [[nodiscard]] bool enabled() const noexcept { return stream_ != nullptr; }
    [[nodiscard]] bool json() const noexcept { return format_ == LogFormat::json; }
    // 日志写到 stderr 时不能同时画进度条：进度用回车覆盖当前行，会把日志行擦掉。
    [[nodiscard]] bool uses_stderr() const noexcept {
        return stream_ != nullptr && stream_ == &std::cerr;
    }

    void event(std::string_view level, std::string_view name,
               std::string_view detail = {}) {
        if (!enabled()) {
            return;
        }
        if (format_ == LogFormat::json) {
            *stream_ << "{\"ts\":" << json_string(log_timestamp())
                     << ",\"level\":" << json_string(level)
                     << ",\"event\":" << json_string(name);
            if (!detail.empty()) {
                *stream_ << ",\"detail\":" << json_string(detail);
            }
            *stream_ << "}\n";
        } else {
            *stream_ << log_timestamp() << " [" << level << "] " << name;
            if (!detail.empty()) {
                *stream_ << ": " << detail;
            }
            *stream_ << '\n';
        }
        stream_->flush();
    }

private:
    LogFormat format_{LogFormat::text};
    // readonly-audit: 操作日志文件（--log-file），与源镜像无关
    std::ofstream file_;
    std::ostream* stream_{nullptr};
};

OperationLog g_operation_log;

// 解析全局选项 --log-format / --log-file，并从参数表里摘掉（和 --json 一样
// 可以出现在任意位置）。返回值表示日志是否已启用。
bool configure_operation_log(std::vector<std::string>& args) {
    bool enabled = false;
    LogFormat format = LogFormat::text;
    std::optional<std::filesystem::path> path;
    std::vector<std::string> kept;
    kept.reserve(args.size());
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (args[index] == "--log-format") {
            if (index + 1 >= args.size()) {
                throw std::invalid_argument("--log-format 需要 text 或 json");
            }
            const auto& value = args[++index];
            if (value == "json") {
                format = LogFormat::json;
            } else if (value == "text") {
                format = LogFormat::text;
            } else {
                throw std::invalid_argument("--log-format 只能是 text 或 json");
            }
            enabled = true;
            continue;
        }
        if (args[index] == "--log-file") {
            if (index + 1 >= args.size() || args[index + 1].empty()) {
                throw std::invalid_argument("--log-file 需要非空路径");
            }
            path = path_from_utf8(args[++index]);
            enabled = true;
            continue;
        }
        kept.push_back(std::move(args[index]));
    }
    args = std::move(kept);
    if (enabled) {
        g_operation_log.configure(format, path);
    }
    return enabled;
}

bool extract_json_flag(std::vector<std::string>& args) {
    bool json_mode = false;
    std::vector<std::string> kept;
    kept.reserve(args.size());
    for (auto& argument : args) {
        if (argument == "--json") {
            json_mode = true;
        } else {
            kept.push_back(std::move(argument));
        }
    }
    args = std::move(kept);
    return json_mode;
}

// ---------------------------------------------------------------------------
// 命令行解析
// ---------------------------------------------------------------------------
struct CommandOptions {
    std::optional<std::size_t> partition;
    std::optional<std::string> path;
    std::optional<std::filesystem::path> output;
    std::optional<std::filesystem::path> manifest;
    std::optional<std::uint64_t> limit;
    std::optional<std::filesystem::path> metadata_report;
    std::optional<std::size_t> max_entries;
    std::optional<std::uint64_t> time_limit;
    bool password_stdin{};
    bool restore_acl{};
};

CommandOptions parse_command_options(const std::vector<std::string>& args,
                                    std::size_t begin,
                                    bool allow_path,
                                    bool require_path,
                                    bool allow_output,
                                    bool require_output,
                                    bool allow_size = false) {
    CommandOptions options;
    for (std::size_t index = begin; index < args.size(); ++index) {
        const auto& argument = args[index];
        if (argument == "--partition" || argument == "-p") {
            if (options.partition || index + 1 >= args.size()) {
                throw std::invalid_argument(
                    "--partition 只能指定一次，且必须提供从 1 开始的分区序号");
            }
            std::size_t partition_number = 0;
            const auto& value = args[++index];
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), partition_number);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != value.data() + value.size() ||
                partition_number == 0) {
                throw std::invalid_argument("分区序号必须是大于 0 的整数");
            }
            options.partition = partition_number;
            continue;
        }
        if (argument == "-o" || argument == "--output") {
            if (!allow_output) {
                throw std::invalid_argument("此命令不接受输出目录选项");
            }
            if (options.output || index + 1 >= args.size() ||
                args[index + 1].empty()) {
                throw std::invalid_argument(
                    "输出目录选项只能指定一次，且必须提供非空路径");
            }
            options.output = path_from_utf8(args[++index]);
            continue;
        }
        if (argument == "--manifest") {
            if (!allow_output) {
                throw std::invalid_argument("此命令不接受清单选项");
            }
            if (options.manifest || index + 1 >= args.size() ||
                args[index + 1].empty()) {
                throw std::invalid_argument(
                    "清单选项只能指定一次，且必须提供非空路径");
            }
            options.manifest = path_from_utf8(args[++index]);
            continue;
        }
        if (argument == "--size") {
            if (!allow_size) {
                throw std::invalid_argument("此命令不接受 --size 选项");
            }
            if (options.limit || index + 1 >= args.size()) {
                throw std::invalid_argument(
                    "--size 只能指定一次，且必须提供正整数字节数");
            }
            std::uint64_t limit = 0;
            const auto& value = args[++index];
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), limit);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != value.data() + value.size() || limit == 0) {
                throw std::invalid_argument("--size 必须是大于 0 的整数字节数");
            }
            options.limit = limit;
            continue;
        }
        if (argument == "--password-stdin") {
            if (options.password_stdin) {
                throw std::invalid_argument("--password-stdin 只能指定一次");
            }
            options.password_stdin = true;
            continue;
        }
        if (argument == "--metadata-report") {
            if (!allow_output) {
                throw std::invalid_argument("--metadata-report 只适用于 extract/restore");
            }
            if (options.metadata_report || index + 1 >= args.size() ||
                args[index + 1].empty()) {
                throw std::invalid_argument(
                    "--metadata-report 只能指定一次，且必须提供非空路径");
            }
            options.metadata_report = path_from_utf8(args[++index]);
            continue;
        }
        if (argument == "--max-entries") {
            if (!allow_output && !allow_path) {
                throw std::invalid_argument("--max-entries 只适用于 list/extract/restore");
            }
            if (options.max_entries || index + 1 >= args.size()) {
                throw std::invalid_argument(
                    "--max-entries 只能指定一次，且必须提供大于 0 的整数");
            }
            std::size_t value = 0;
            const auto& text = args[++index];
            const auto parsed = std::from_chars(
                text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                value == 0) {
                throw std::invalid_argument("--max-entries 必须是大于 0 的整数");
            }
            options.max_entries = value;
            continue;
        }
        if (argument == "--time-limit") {
            if (!allow_output && !allow_path) {
                throw std::invalid_argument("--time-limit 只适用于 list/extract/restore");
            }
            if (options.time_limit || index + 1 >= args.size()) {
                throw std::invalid_argument(
                    "--time-limit 只能指定一次，且必须提供大于 0 的秒数");
            }
            std::uint64_t value = 0;
            const auto& text = args[++index];
            const auto parsed = std::from_chars(
                text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                value == 0) {
                throw std::invalid_argument("--time-limit 必须是大于 0 的整数秒");
            }
            options.time_limit = value;
            continue;
        }
        if (argument == "--restore-acl") {
            if (!allow_output) {
                throw std::invalid_argument("--restore-acl 只适用于 extract/restore");
            }
            options.restore_acl = true;
            continue;
        }
        if (!argument.empty() && argument.front() == '-') {
            throw std::invalid_argument("未知选项: " + argument);
        }
        if (!allow_path || options.path) {
            throw std::invalid_argument("命令包含多余的位置参数: " + argument);
        }
        options.path = argument;
    }

    if (require_path && !options.path) {
        throw std::invalid_argument("extract 命令需要指定要导出的 VFS 路径");
    }
    if (require_output && !options.output) {
        throw std::invalid_argument("必须使用 -o <目录> 指定导出目录");
    }
    return options;
}

MountedFilesystem mount_filesystem(
    const std::shared_ptr<uife::IImageDriver>& image,
    std::optional<std::size_t> selected_partition,
    std::string_view password) {
    auto& factory = uife::FsParserFactory::instance();
    const auto parser_names = factory.supported_names();
    std::string parser_summary;
    if (parser_names.empty()) {
        parser_summary = "无（需安装 libtsk 开发库，或在 Windows 上安装 "
                         "Sleuth Kit 命令行工具）";
    } else {
        for (std::size_t index = 0; index < parser_names.size(); ++index) {
            parser_summary += (index == 0 ? "" : ", ") + parser_names[index];
        }
    }
    const auto parser_error_suffix =
        "（当前已启用解析器: " + parser_summary + "）";
    if (selected_partition) {
        const auto partitions = uife::scan_partitions(*image);
        if (*selected_partition > partitions.size()) {
            throw std::out_of_range(
                "分区序号超出范围；请先运行 info 查看可用分区");
        }
        const auto& partition = partitions[*selected_partition - 1];
        const auto filesystem_name = factory.identify(*image, partition.byte_offset);
        if (!password.empty() && filesystem_name != "APFS") {
            throw std::invalid_argument(
                "--password-stdin 只适用于 APFS 加密卷");
        }
        if (!factory.supports(*image, partition.byte_offset)) {
            throw CliError(kExitFsParserMissing,
                "分区 " + std::to_string(*selected_partition) +
                " 的文件系统为 " + filesystem_name +
                "，当前构建没有可用解析器" + parser_error_suffix);
        }
        auto parser = factory.create(*image, partition.byte_offset, password);
        parser->mount(image, partition.byte_offset, partition.byte_length);
        return {std::move(parser), partition.byte_offset, partition.byte_length};
    }

    const auto direct_name = factory.identify(*image, 0);
    if (factory.supports(*image, 0)) {
        if (!password.empty() && direct_name != "APFS") {
            throw std::invalid_argument(
                "--password-stdin 只适用于 APFS 加密卷");
        }
        auto parser = factory.create(*image, 0, password);
        parser->mount(image, 0, image->get_size());
        return {std::move(parser), 0, image->get_size()};
    }
    if (direct_name.starts_with("ext") || direct_name.starts_with("NTFS")) {
        throw CliError(kExitFsParserMissing, "检测到" + direct_name +
            "，但尚未接入可用的文件系统后端；请安装 libtsk 开发库，"
            "或在 Windows 上安装 Sleuth Kit 命令行工具");
    }
    if (direct_name.starts_with("ReFS")) {
        throw CliError(kExitFsParserMissing,
            "检测到 ReFS，但当前构建未接入可用的 ReFS 解析后端");
    }

    const auto partitions = uife::scan_partitions(*image);
    for (const auto& partition : partitions) {
        const auto filesystem_name = factory.identify(*image, partition.byte_offset);
        if (factory.supports(*image, partition.byte_offset)) {
            if (!password.empty() && filesystem_name != "APFS") {
                throw std::invalid_argument(
                    "--password-stdin 只适用于 APFS 加密卷");
            }
            auto parser = factory.create(*image, partition.byte_offset, password);
            parser->mount(image, partition.byte_offset, partition.byte_length);
            return {std::move(parser), partition.byte_offset, partition.byte_length};
        }
        if (filesystem_name.starts_with("ReFS")) {
            throw CliError(kExitFsParserMissing,
                "分区中检测到 ReFS，但当前构建未接入可用的 ReFS 解析后端");
        }
        if (filesystem_name.starts_with("ext") ||
            filesystem_name.starts_with("NTFS")) {
            throw CliError(kExitFsParserMissing, "分区中检测到" + filesystem_name +
                "，但尚未接入可用的文件系统后端；请安装 libtsk 开发库，"
                "或在 Windows 上安装 Sleuth Kit 命令行工具");
        }
    }
    throw CliError(kExitFsParserMissing,
        "未发现当前构建支持的文件系统（当前已启用解析器: " +
        parser_summary + "）");
}

void wipe_password(std::string& password) noexcept {
    volatile char* value = password.data();
    for (std::size_t index = 0; index < password.size(); ++index) {
        value[index] = '\0';
    }
    password.clear();
}

class SensitivePassword {
public:
    ~SensitivePassword() {
        wipe_password(value_);
    }

    [[nodiscard]] std::string& value() noexcept {
        return value_;
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return value_;
    }

private:
    std::string value_;
};

void print_progress(std::uint64_t completed, std::uint64_t total) {
    const auto percent = total == 0 ? 100U : static_cast<unsigned int>(
        std::min<long double>(100.0L,
            static_cast<long double>(completed) * 100.0L / static_cast<long double>(total)));
    std::cerr << "\r导出进度: " << percent << "% (" << completed << "/" << total
              << " bytes)" << std::flush;
    if (completed >= total) {
        std::cerr << '\n';
    }
}

void write_manifest(const std::filesystem::path& manifest_path,
                    const std::vector<uife::ExtractedFile>& files) {
    // readonly-audit: 导出清单是本次运行产生的输出文件，与源镜像无关
    std::ofstream output(manifest_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error(
            "无法写入清单文件: " + path_to_utf8(manifest_path) +
            "（请检查目录是否存在且有写权限）");
    }
    for (const auto& file : files) {
        output << file.sha256 << "  " << file.output << '\n';
    }
    output.flush();
    if (!output) {
        throw std::runtime_error(
            "写入清单文件失败: " + path_to_utf8(manifest_path));
    }
}


// 后备元数据报告：元数据无法忠实还原时（平台不支持、权限不足、语义无法表达），
// 把 VFS 路径、字段与原因写成 JSON 放在导出目录里。内容已经导出并校验过哈希，
// 这份报告回答的是"哪些属性没能带回来"，而不是让整次导出失败。
// 条目里 opt_in=true 的是"用户没开可选还原开关"（--restore-acl）：NTFS 镜像里
// 几乎每个对象都带安全描述符，这类条目单独计数，不参与"是否要写报告"的判断。
std::string utc_timestamp() {
    const auto now = std::time(nullptr);
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &now);
#else
    gmtime_r(&now, &parts);
#endif
    std::array<char, 32> buffer{};
    std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &parts);
    return buffer.data();
}

std::size_t count_metadata_failures(const std::vector<uife::MetadataNote>& notes) {
    std::size_t failures = 0;
    for (const auto& note : notes) {
        if (!note.opt_in) {
            ++failures;
        }
    }
    return failures;
}

void write_metadata_report(const std::filesystem::path& path,
                           const std::string& command,
                           const std::filesystem::path& source,
                           const std::string& filesystem_name,
                           const std::vector<uife::ExtractedFile>& extracted,
                           const std::vector<uife::MetadataNote>& issues) {
    std::map<std::string, std::string> output_by_path;
    for (const auto& file : extracted) {
        output_by_path.emplace(file.path, file.output);
    }
    const auto failures = count_metadata_failures(issues);
    std::string json = "{";
    json += "\"tool\":\"uife\"";
    json += ",\"command\":" + json_string(command);
    json += ",\"source\":" + json_string(path_to_utf8(source));
    json += ",\"filesystem\":" + json_string(filesystem_name);
    json += ",\"generated\":" + json_string(utc_timestamp());
    json += ",\"extracted_files\":" + std::to_string(extracted.size());
    json += ",\"issue_count\":" + std::to_string(issues.size());
    json += ",\"failure_count\":" + std::to_string(failures);
    json += ",\"issues\":[";
    for (std::size_t index = 0; index < issues.size(); ++index) {
        if (index != 0) {
            json += ",";
        }
        const auto& issue = issues[index];
        json += "{\"path\":" + json_string(issue.path);
        const auto output = output_by_path.find(issue.path);
        json += ",\"output\":";
        json += output == output_by_path.end()
            ? "null" : json_string(output->second);
        json += ",\"field\":" + json_string(issue.field);
        json += ",\"reason\":" + json_string(issue.reason);
        json += ",\"opt_in\":";
        json += issue.opt_in ? "true" : "false";
        json += "}";
    }
    json += "]}";
    // --metadata-report 允许指向导出目录之外的位置，父目录不存在时补建，
    // 免得报告写不进去把一次已经成功的导出整体判成失败。
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    // readonly-audit: 后备元数据报告写到导出目录，不写源镜像
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("无法写入后备元数据报告: " + path_to_utf8(path));
    }
    stream << json << '\n';
    stream.flush();
    if (!stream) {
        throw std::runtime_error("写入后备元数据报告失败: " + path_to_utf8(path));
    }
}

// ---------------------------------------------------------------------------
// capabilities：让二进制自己申报"我能做什么"。
//
// 版本兼容表如果靠手抄，迟早会出现"文档写支持 ReFS，可发行版里没有"这种事。
// 这里把格式、后端、能力都从注册表与编译宏里取，脚本按版本收一次就是一列
// （scripts/capability-matrix.py）。申报必须诚实：没打通的能力写 false 并
// 写明原因，不写"计划支持"。
// ---------------------------------------------------------------------------

struct CapabilityFeature {
    std::string_view key;
    bool supported;
    std::string_view note;
};

std::vector<CapabilityFeature> capability_features() {
    std::vector<CapabilityFeature> features;
    features.push_back({"alternate_streams", true,
        "NTFS 备用数据流导出为 <文件>__ads__<转义流名> 侧车文件"});
    features.push_back({"sparse_files", true,
        "导出时保留稀疏区，不把空洞写成实数据"});
#ifdef _WIN32
    features.push_back({"acl_restore", true, "extract/restore 支持 --restore-acl"});
    features.push_back({"reparse_points", true, "目录联接与符号链接按目标重建"});
    features.push_back({"long_paths", true,
        "导出路径超过 MAX_PATH 仍可写入（导出根走扩展长度路径）"});
#else
    features.push_back({"acl_restore", false, "ACL 还原只在 Windows 构建提供"});
    features.push_back({"reparse_points", false, "重解析点还原只在 Windows 构建提供"});
    features.push_back({"long_paths", true, "POSIX 路径本身没有 260 字符上限"});
#endif
    features.push_back({"resource_limits", true,
        "--max-entries / --time-limit，超限以退出码 5 中止"});
    features.push_back({"operation_log", true, "--log-format text|json 与 --log-file"});
#ifdef UIFE_WITH_LIBFSAPFS
    features.push_back({"encrypted_apfs", true, "--password-stdin 解锁加密 APFS 卷"});
#else
    features.push_back({"encrypted_apfs", false, "本构建未包含 libfsapfs"});
#endif
    // 已知缺口也写在这里：用户能直接看出哪条路径还没打通，而不是等到导出时才发现。
    features.push_back({"wim_named_streams", false,
        "wimlib 没有单流导出 API，WIM 内的命名流暂不可导出"});
    return features;
}

struct CapabilityBackend {
    std::string_view name;
    bool enabled;
};

std::vector<CapabilityBackend> capability_backends() {
    std::vector<CapabilityBackend> backends;
    backends.push_back({"raw", true});   // 内置：裸镜像与分区容器
    backends.push_back({"zlib", true});  // 核心依赖：DMG/GHO 压缩块
#ifdef UIFE_WITH_LIBEWF
    backends.push_back({"ewf", true});
#else
    backends.push_back({"ewf", false});
#endif
#ifdef UIFE_WITH_LIBWIM
    backends.push_back({"wim", true});
#else
    backends.push_back({"wim", false});
#endif
#ifdef UIFE_WITH_LIBFSAPFS
    backends.push_back({"apfs", true});
#else
    backends.push_back({"apfs", false});
#endif
#ifdef UIFE_WITH_LIBFSREFS
    backends.push_back({"refs", true});
#else
    backends.push_back({"refs", false});
#endif
#ifdef UIFE_WITH_LIBTSK
    backends.push_back({"tsk", true});
#else
    backends.push_back({"tsk", false});
#endif
    return backends;
}

void write_json_names(std::ostream& out, const std::vector<std::string>& names) {
    out << '[';
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (index != 0) {
            out << ',';
        }
        out << json_string(names[index]);
    }
    out << ']';
}

void print_capabilities(std::ostream& out, bool json_mode) {
    const auto formats = uife::DriverFactory::instance().supported_formats();
    const auto recognized_only =
        uife::DriverFactory::instance().recognized_only_formats();
    const auto filesystems = uife::FsParserFactory::instance().supported_names();
    const auto features = capability_features();
    const auto backends = capability_backends();

    if (json_mode) {
        out << "{\"tool\":\"uife\",\"version\":" << json_string(UIFE_VERSION)
            << ",\"build\":{\"compiler\":" << json_string(UIFE_COMPILER)
            << ",\"type\":" << json_string(UIFE_BUILD_TYPE) << "}"
            << ",\"image_formats\":{\"supported\":";
        write_json_names(out, formats);
        out << ",\"recognized_only\":";
        write_json_names(out, recognized_only);
        out << "},\"filesystems\":";
        write_json_names(out, filesystems);
        out << ",\"backends\":{";
        for (std::size_t index = 0; index < backends.size(); ++index) {
            if (index != 0) {
                out << ',';
            }
            out << json_string(std::string(backends[index].name)) << ':'
                << (backends[index].enabled ? "true" : "false");
        }
        out << "},\"features\":{";
        for (std::size_t index = 0; index < features.size(); ++index) {
            if (index != 0) {
                out << ',';
            }
            out << json_string(std::string(features[index].key)) << ':'
                << (features[index].supported ? "true" : "false");
        }
        out << "},\"feature_notes\":{";
        for (std::size_t index = 0; index < features.size(); ++index) {
            if (index != 0) {
                out << ',';
            }
            out << json_string(std::string(features[index].key)) << ':'
                << json_string(std::string(features[index].note));
        }
        out << "},\"exit_codes\":{\"success\":0,\"runtime_error\":1,"
               "\"usage_error\":2,\"driver_missing\":3,"
               "\"fs_parser_missing\":4,\"resource_limit\":5}}\n";
        return;
    }

    out << "uife " << UIFE_VERSION << " (" << UIFE_COMPILER << ", "
        << UIFE_BUILD_TYPE << ")\n";
    out << "可读镜像格式: ";
    print_names(out, formats);
    out << "\n仅识别（无读取驱动）: ";
    print_names(out, recognized_only);
    out << "\n文件系统解析器: ";
    print_names(out, filesystems);
    out << "\n后端: ";
    for (std::size_t index = 0; index < backends.size(); ++index) {
        out << (index == 0 ? "" : "、") << backends[index].name
            << (backends[index].enabled ? " 已启用" : " 未启用");
    }
    out << "\n能力:\n";
    std::size_t width = 0;
    for (const auto& feature : features) {
        width = std::max(width, feature.key.size());
    }
    for (const auto& feature : features) {
        out << (feature.supported ? "  [+] " : "  [-] ") << feature.key
            << std::string(width - feature.key.size() + 2, ' ') << feature.note
            << '\n';
    }
}

// ---------------------------------------------------------------------------
// 单条命令执行（info/list/extract/restore），输出写入 out。
// JSON 模式下输出单个完整 JSON 对象（不含换行），供 batch 逐行包装复用。
// ---------------------------------------------------------------------------
void run_single_command(const std::vector<std::string>& args,
                        std::ostream& out,
                        bool json_mode) {
    if (args.size() < 3) {
        throw CliError(kExitUsageError, "缺少镜像路径参数");
    }
    const auto& command = args[1];
    CommandOptions command_options;
    if (command == "info" || command == "inspect") {
        command_options = parse_command_options(args, 3, false, false, false, false);
    } else if (command == "probe") {
        command_options = parse_command_options(args, 3, false, false, false, false);
    } else if (command == "bench") {
        command_options = parse_command_options(args, 3, false, false, false, false, true);
    } else if (command == "list") {
        command_options = parse_command_options(args, 3, true, false, false, false);
    } else if (command == "extract") {
        command_options = parse_command_options(args, 3, true, true, true, true);
    } else if (command == "restore") {
        command_options = parse_command_options(args, 3, false, false, true, true);
    } else {
        throw CliError(kExitUsageError, "未知命令: " + command);
    }
    if (command_options.password_stdin &&
        command != "list" && command != "extract" && command != "restore") {
        throw std::invalid_argument(
            "--password-stdin 仅适用于 list、extract 和 restore");
    }
    SensitivePassword password;
    if (command_options.password_stdin) {
        if (!std::getline(std::cin, password.value())) {
            throw std::runtime_error("无法从标准输入读取 APFS 密码");
        }
        if (password.value().empty() ||
            password.value().find('\0') != std::string::npos) {
            throw std::invalid_argument("APFS 密码不能为空且不能包含 NUL 字符");
        }
    }
    const std::filesystem::path image_path = path_from_utf8(args[2]);
    const auto image_info = uife::DriverFactory::instance().identify(image_path);
    if (!image_info.supported) {
        throw CliError(kExitDriverMissing,
            "镜像格式 " + image_info.format +
            " 仅能识别，当前尚未接入该格式的解封装驱动");
    }
    auto driver_unique = uife::DriverFactory::instance().create(image_path);
    std::shared_ptr<uife::IImageDriver> driver(std::move(driver_unique));
    g_operation_log.event("info", "open",
        image_info.format + " " + path_to_utf8(image_path));

    // 镜像不完整（例如 EWF 只拿到首个分卷）时先提示，别让用户在浏览或导出
    // 走到一半才碰到读取失败。info/probe 会把它写进正式报告，其余命令走 stderr。
    const auto integrity_note = driver->integrity_note();
    if (!integrity_note.empty()) {
        g_operation_log.event("warn", "integrity", integrity_note);
    }
    if (!integrity_note.empty() && !json_mode && command != "probe" &&
        command != "info" && command != "inspect") {
        std::cerr << "警告: " << integrity_note << '\n';
    }

    if (command == "info" || command == "inspect") {
        const auto partitions = uife::scan_partitions(*driver);
        if (json_mode) {
            std::string json = "{";
            json += "\"format\":" + json_string(image_info.format);
            json += ",\"supported\":true";
            json += ",\"size\":" + std::to_string(driver->get_size());
            json += ",\"sector_size\":" + std::to_string(driver->sector_size());
            if (!integrity_note.empty()) {
                json += ",\"integrity_note\":" + json_string(integrity_note);
            }
            const auto direct_fs =
                uife::FsParserFactory::instance().identify(*driver, 0);
            if (direct_fs != "未知") {
                json += ",\"direct_filesystem\":" + json_string(direct_fs);
            }
            json += ",\"partitions\":[";
            for (std::size_t index = 0; index < partitions.size(); ++index) {
                const auto& partition = partitions[index];
                if (index != 0) {
                    json += ",";
                }
                json += "{\"number\":" + std::to_string(index + 1);
                json += ",\"scheme\":" + json_string(partition.scheme);
                json += ",\"offset\":" + std::to_string(partition.byte_offset);
                json += ",\"length\":" + std::to_string(partition.byte_length);
                json += ",\"type\":" + json_string(partition.type);
                json += ",\"filesystem\":" + json_string(
                    uife::FsParserFactory::instance().identify(
                        *driver, partition.byte_offset));
                json += "}";
            }
            json += "],\"filesystem_parsers\":[";
            const auto names =
                uife::FsParserFactory::instance().supported_names();
            for (std::size_t index = 0; index < names.size(); ++index) {
                if (index != 0) {
                    json += ",";
                }
                json += json_string(names[index]);
            }
            json += "]}";
            out << json;
            return;
        }
        out << "镜像格式: " << image_info.format << " (可读取)\n";
        out << "镜像大小: " << driver->get_size() << " bytes\n"
            << "扇区大小: " << driver->sector_size() << " bytes\n";
        if (!integrity_note.empty()) {
            out << "镜像完整性: " << integrity_note << '\n';
        }
        const auto direct_fs = uife::FsParserFactory::instance().identify(*driver, 0);
        if (direct_fs != "未知") {
            out << "直接文件系统: " << direct_fs << '\n';
        }
        if (partitions.empty()) {
            out << "分区表: 未发现有效的 MBR/GPT（可能是分区/文件系统原始镜像）\n";
        } else {
            out << "分区数: " << partitions.size() << '\n';
            for (std::size_t index = 0; index < partitions.size(); ++index) {
                const auto& partition = partitions[index];
                out << "  " << (index + 1) << ": " << partition.scheme
                    << " offset=" << partition.byte_offset
                    << " length=" << partition.byte_length
                    << " type=" << partition.type
                    << " fs=" << uife::FsParserFactory::instance().identify(
                           *driver, partition.byte_offset) << '\n';
            }
        }
        const auto names = uife::FsParserFactory::instance().supported_names();
        out << "已启用文件系统解析器: ";
        if (names.empty()) {
            out << "无（需安装 libtsk 开发库，或在 Windows 上安装 Sleuth Kit 命令行工具）";
        } else {
            for (std::size_t index = 0; index < names.size(); ++index) {
                out << (index == 0 ? "" : ", ") << names[index];
            }
        }
        out << '\n';
        return;
    }

    if (command == "probe") {
        // 逐位置报告“识别到什么文件系统 / 当前构建能否浏览”，
        // 供兼容性矩阵和现场取证时快速判断能力边界。
        struct Location {
            std::string label;
            std::uint64_t offset{};
            std::uint64_t length{};
            std::string scheme;
            std::string type;
        };
        std::vector<Location> locations;
        locations.push_back({"direct", 0, driver->get_size(), {}, {}});
        const auto partitions = uife::scan_partitions(*driver);
        for (std::size_t index = 0; index < partitions.size(); ++index) {
            const auto& partition = partitions[index];
            locations.push_back({"partition " + std::to_string(index + 1),
                                 partition.byte_offset, partition.byte_length,
                                 partition.scheme, partition.type});
        }
        auto& factory = uife::FsParserFactory::instance();
        if (json_mode) {
            std::string json = "{";
            json += "\"format\":" + json_string(image_info.format);
            json += ",\"size\":" + std::to_string(driver->get_size());
            json += ",\"sector_size\":" + std::to_string(driver->sector_size());
            if (!integrity_note.empty()) {
                json += ",\"integrity_note\":" + json_string(integrity_note);
            }
            json += ",\"locations\":[";
            for (std::size_t index = 0; index < locations.size(); ++index) {
                if (index != 0) {
                    json += ",";
                }
                const auto& location = locations[index];
                const auto filesystem = factory.identify(*driver, location.offset);
                const auto readable = factory.supports(*driver, location.offset);
                json += "{\"label\":" + json_string(location.label);
                json += ",\"offset\":" + std::to_string(location.offset);
                json += ",\"length\":" + std::to_string(location.length);
                json += ",\"scheme\":";
                json += location.scheme.empty() ? "null" : json_string(location.scheme);
                json += ",\"partition_type\":";
                json += location.type.empty() ? "null" : json_string(location.type);
                json += ",\"filesystem\":" + json_string(filesystem);
                json += ",\"readable\":" + json_bool(readable);
                json += "}";
            }
            json += "]}";
            out << json;
            return;
        }
        out << "镜像格式: " << image_info.format << " (可读取)\n";
        out << "镜像大小: " << driver->get_size() << " bytes\n";
        if (!integrity_note.empty()) {
            out << "镜像完整性: " << integrity_note << '\n';
        }
        for (const auto& location : locations) {
            const auto filesystem = factory.identify(*driver, location.offset);
            const auto readable = factory.supports(*driver, location.offset);
            out << location.label << ": offset=" << location.offset
                << " length=" << location.length;
            if (!location.scheme.empty()) {
                out << " scheme=" << location.scheme
                    << " type=" << location.type;
            }
            out << " fs=" << filesystem
                << " [" << (readable ? "可浏览" : "不可浏览") << "]\n";
        }
        return;
    }

    if (command == "bench") {
        // 只读顺序读取基准：衡量镜像驱动（含解封装/解压缩）的吞吐上限，
        // 不挂载文件系统，因此结果与文件系统解析性能无关。
        const auto image_size = driver->get_size();
        const auto limit = std::min<std::uint64_t>(
            image_size, command_options.limit.value_or(image_size));
        std::vector<std::byte> buffer(1024U * 1024U);
        const auto started = std::chrono::steady_clock::now();
        std::uint64_t offset = 0;
        while (offset < limit) {
            const auto requested = static_cast<std::size_t>(
                std::min<std::uint64_t>(limit - offset, buffer.size()));
            const auto count = driver->read_at(
                offset, std::span<std::byte>(buffer).first(requested));
            if (count == 0 || count > requested) {
                throw std::runtime_error("镜像驱动返回了无效的读取长度");
            }
            offset += count;
        }
        const auto seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        const auto mebibytes = static_cast<double>(offset) / (1024.0 * 1024.0);
        const auto throughput = seconds > 0.0 ? mebibytes / seconds : 0.0;
        if (json_mode) {
            std::string json = "{";
            json += "\"format\":" + json_string(image_info.format);
            json += ",\"bytes\":" + std::to_string(offset);
            json += ",\"seconds\":" + json_number(seconds);
            json += ",\"mib_per_second\":" + json_number(throughput);
            json += ",\"sector_size\":" + std::to_string(driver->sector_size());
            json += "}";
            out << json;
            return;
        }
        out << "镜像格式: " << image_info.format << '\n';
        out << "读取字节数: " << offset << '\n';
        out << "耗时: " << json_number(seconds) << " s\n";
        out << "吞吐: " << json_number(throughput) << " MiB/s\n";
        return;
    }

    if (command == "list") {
        const auto path = command_options.path.value_or("/");
        auto mounted = mount_filesystem(
            driver, command_options.partition, password.view());
        const uife::VFS filesystem(*mounted.parser);
        // 先把结果落到局部变量：这样单目录条目数上限可以在渲染之前判定，
        // 不会出现"输出了 200 万行才发现超限"。解析器为单个目录分配的
        // 内存仍由后端决定，这里的上限管的是 CLI 累积与输出的规模。
        const auto entries = filesystem.list_dir(path);
        if (command_options.max_entries && entries.size() > *command_options.max_entries) {
            throw uife::ResourceLimitError(
                "目录条目数 " + std::to_string(entries.size()) + " 超过 --max-entries 上限 " +
                std::to_string(*command_options.max_entries) + "，已停止输出");
        }
        g_operation_log.event("info", "list",
            filesystem.filesystem_name().empty()
                ? path + " (" + std::to_string(entries.size()) + " 项)"
                : std::string(filesystem.filesystem_name()) + " " + path + " (" +
                      std::to_string(entries.size()) + " 项)");
        if (json_mode) {
            std::string json = "{";
            json += "\"filesystem\":" + json_string(filesystem.filesystem_name());
            json += ",\"offset\":" + std::to_string(mounted.offset);
            json += ",\"path\":" + json_string(path);
            json += ",\"entries\":[";
            bool first = true;
            for (const auto& node : entries) {
                if (!first) {
                    json += ",";
                }
                first = false;
                json += "{\"name\":" + json_string(node.name);
                json += ",\"type\":" + json_string(node_type_label(node.type));
                json += ",\"size\":" + std::to_string(node.metadata.size);
                json += ",\"size_valid\":" + json_bool(node.metadata.size_valid);
                json += ",\"metadata\":" + json_metadata(node.metadata);
                json += "}";
            }
            json += "]}";
            out << json;
            return;
        }
        out << "文件系统: " << filesystem.filesystem_name()
            << " offset=" << mounted.offset << '\n';
        for (const auto& node : entries) {
            const char* type_label = "[?] ";
            if (node.type == uife::NodeType::directory) {
                type_label = "[D] ";
            } else if (node.type == uife::NodeType::regular_file) {
                type_label = node.metadata.size_valid ? "[F] " : "[!] ";
            } else if (node.type == uife::NodeType::symbolic_link) {
                type_label = "[L] ";
            }
            out << type_label << node.name;
            if (node.type == uife::NodeType::regular_file &&
                !node.metadata.size_valid) {
                out << " (文件大小元数据异常，禁止提取)";
            } else if (node.type == uife::NodeType::regular_file) {
                out << " (" << node.metadata.size << " bytes)";
            }
            if (node.type == uife::NodeType::symbolic_link &&
                !node.metadata.symlink_target.empty()) {
                out << " -> " << node.metadata.symlink_target;
            }
            if (node.metadata.sparse) {
                out << " [sparse]";
            }
            if (node.metadata.compressed) {
                out << " [compressed]";
            }
            if (node.metadata.encrypted) {
                out << " [encrypted]";
            }
            if (!node.metadata.alternate_streams.empty()) {
                out << " [ads:" << node.metadata.alternate_streams.size() << "]";
            }
            if (node.metadata.has_security && node.metadata.security.valid &&
                !node.metadata.security.owner_sid.empty()) {
                out << " [owner:" << node.metadata.security.owner_sid
                    << " dacl:"
                    << (node.metadata.security.dacl_present
                            ? std::to_string(node.metadata.security.dacl.size())
                            : std::string("-"))
                    << "]";
            }
            out << '\n';
        }
        return;
    }

    if (command == "extract" || command == "restore") {
        auto mounted = mount_filesystem(
            driver, command_options.partition, password.view());
        const uife::VFS filesystem(*mounted.parser);
        uife::ExtractOptions extract_options;
        extract_options.output_directory = command_options.output.value();
        extract_options.restore_acl = command_options.restore_acl;
        if (command_options.max_entries) {
            extract_options.limits.max_entries = *command_options.max_entries;
        }
        if (command_options.time_limit) {
            extract_options.limits.time_limit =
                std::chrono::seconds(*command_options.time_limit);
        }
        // JSON 输出必须是纯 JSON，进度信息（stderr）在 JSON 模式下关闭；
        // JSON 操作日志同理——进度用的回车会破坏一行一条的结构。
        if (!json_mode && !g_operation_log.json() && !g_operation_log.uses_stderr()) {
            extract_options.progress = print_progress;
        }
        std::vector<uife::MetadataNote> metadata_issues;
        extract_options.metadata_note =
            [&metadata_issues](const uife::MetadataNote& note) {
                metadata_issues.push_back(note);
            };
        const auto started = std::chrono::steady_clock::now();
        g_operation_log.event("info", command + "_start",
            command == "restore" ? path_to_utf8(command_options.output.value())
                                 : command_options.path.value() + " -> " +
                                       path_to_utf8(command_options.output.value()));
        const auto extracted = command == "restore"
            ? uife::Extractor::restore(filesystem, extract_options)
            : uife::Extractor::extract(
                filesystem, command_options.path.value(), extract_options);
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        std::uint64_t exported_bytes = 0;
        for (const auto& file : extracted) {
            exported_bytes += file.size;
        }
        char summary[160]{};
        std::snprintf(summary, sizeof(summary),
                      "%zu 个文件, %llu 字节, %.3f s", extracted.size(),
                      static_cast<unsigned long long>(exported_bytes), elapsed);
        g_operation_log.event("info", command + "_done", summary);
        if (command_options.manifest) {
            write_manifest(command_options.manifest.value(), extracted);
        }
        // 默认只在确实有属性没还原时才落一份报告，干净的导出不多写文件；
        // 显式指定 --metadata-report 时即使没有问题也写，便于批量流程固定路径。
        // "没开关可选还原"（opt_in）不算失败：NTFS 镜像里几乎每个对象都带安全
        // 描述符，否则任何一次 NTFS 导出都会顺手写出上万条记录。
        const auto metadata_failures = count_metadata_failures(metadata_issues);
        std::optional<std::filesystem::path> metadata_report;
        if (metadata_failures != 0 || command_options.metadata_report) {
            metadata_report = command_options.metadata_report.value_or(
                command_options.output.value() / "uife-metadata.json");
            write_metadata_report(*metadata_report, command, image_path,
                std::string(mounted.parser->filesystem_name()), extracted,
                metadata_issues);
        }
        if (json_mode) {
            std::string json = "{";
            json += "\"command\":" + json_string(command);
            json += ",\"count\":" + std::to_string(extracted.size());
            if (command_options.manifest) {
                json += ",\"manifest\":" + json_string(
                    path_to_utf8(command_options.manifest.value()));
            }
            if (metadata_report) {
                json += ",\"metadata_report\":" + json_string(
                    path_to_utf8(*metadata_report));
                json += ",\"metadata_issues\":" +
                    std::to_string(metadata_failures);
            }
            json += ",\"files\":[";
            for (std::size_t index = 0; index < extracted.size(); ++index) {
                if (index != 0) {
                    json += ",";
                }
                json += "{\"path\":" + json_string(extracted[index].path);
                json += ",\"stream\":";
                json += extracted[index].stream.empty()
                    ? "null" : json_string(extracted[index].stream);
                json += ",\"output\":" + json_string(extracted[index].output);
                json += ",\"sha256\":" + json_string(extracted[index].sha256);
                json += ",\"size\":" + std::to_string(extracted[index].size);
                json += "}";
            }
            json += "]}";
            out << json;
            return;
        }
        out << "已导出文件数: " << extracted.size() << '\n';
        if (command_options.manifest) {
            out << "清单文件: "
                << path_to_utf8(command_options.manifest.value()) << '\n';
        }
        if (metadata_report) {
            if (metadata_failures != 0) {
                out << "元数据: " << metadata_failures
                    << " 项未能还原（文件内容已导出并通过校验），详见 "
                    << path_to_utf8(*metadata_report) << '\n';
            }
            const auto skipped = metadata_issues.size() - metadata_failures;
            if (skipped != 0) {
                out << "元数据: " << skipped
                    << " 项按默认策略跳过（未启用可选还原开关），详见 "
                    << path_to_utf8(*metadata_report) << '\n';
            }
        }
        for (const auto& file : extracted) {
            out << file.sha256 << "  " << file.output << '\n';
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// batch：逐行执行清单中的命令
// ---------------------------------------------------------------------------
std::vector<std::string> tokenize_manifest_line(const std::string& line) {
    std::vector<std::string> tokens;
    std::string current;
    bool in_quotes = false;
    bool has_content = false;
    for (const char character : line) {
        if (in_quotes) {
            if (character == '"') {
                in_quotes = false;
            } else {
                current += character;
            }
            continue;
        }
        if (character == '"') {
            in_quotes = true;
            has_content = true;
            continue;
        }
        if (character == ' ' || character == '\t') {
            if (has_content || !current.empty()) {
                tokens.push_back(std::move(current));
                current.clear();
                has_content = false;
            }
            continue;
        }
        current += character;
    }
    if (has_content || !current.empty()) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

int exit_code_for(const std::exception& error) noexcept {
    if (dynamic_cast<const CliError*>(&error) != nullptr) {
        return static_cast<const CliError&>(error).exit_code;
    }
    if (dynamic_cast<const uife::ResourceLimitError*>(&error) != nullptr) {
        return kExitResourceLimit;
    }
    if (dynamic_cast<const std::invalid_argument*>(&error) != nullptr ||
        dynamic_cast<const std::out_of_range*>(&error) != nullptr) {
        return kExitUsageError;
    }
    return kExitRuntimeError;
}

int run_batch(const std::vector<std::string>& args,
              std::ostream& out,
              bool json_mode) {
    if (args.size() < 3 || args[2].empty()) {
        throw CliError(kExitUsageError, "batch 命令需要提供清单文件路径");
    }
    const auto manifest_path = path_from_utf8(args[2]);
    std::ifstream manifest(manifest_path, std::ios::binary);
    if (!manifest) {
        throw CliError(kExitRuntimeError,
            "无法打开清单文件: " + path_to_utf8(manifest_path));
    }
    std::string contents((std::istreambuf_iterator<char>(manifest)),
                         std::istreambuf_iterator<char>());
    if (contents.size() >= 3 && static_cast<unsigned char>(contents[0]) == 0xEF &&
        static_cast<unsigned char>(contents[1]) == 0xBB &&
        static_cast<unsigned char>(contents[2]) == 0xBF) {
        contents.erase(0, 3);
    }

    std::size_t line_number = 0;
    std::size_t succeeded = 0;
    std::size_t failed = 0;
    std::istringstream lines(contents);
    std::string line;
    while (std::getline(lines, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto first_non_space = line.find_first_not_of(" \t");
        if (first_non_space == std::string::npos ||
            line[first_non_space] == '#') {
            continue;
        }
        const auto tokens = tokenize_manifest_line(line);
        if (tokens.empty()) {
            continue;
        }
        // 清单行内可直接写 --help / -h / help 查看用法
        if (tokens.size() == 1 &&
            (tokens[0] == "--help" || tokens[0] == "-h" ||
             tokens[0] == "help")) {
            ++succeeded;
            if (json_mode) {
                out << "{\"line\":" << line_number
                    << ",\"command\":" << json_string(line)
                    << ",\"status\":\"ok\",\"result\":{\"help\":true}}\n";
            } else {
                print_usage(out);
            }
            continue;
        }
        // 清单行省略了开头的程序名，补一个占位 token 以对齐 run_single_command 的参数布局
        std::vector<std::string> command_args;
        command_args.reserve(tokens.size() + 1);
        command_args.emplace_back("uife");
        command_args.insert(command_args.end(), tokens.begin(), tokens.end());
        if (json_mode) {
            std::ostringstream result;
            try {
                run_single_command(command_args, result, json_mode);
                ++succeeded;
                out << "{\"line\":" << line_number
                    << ",\"command\":" << json_string(line)
                    << ",\"status\":\"ok\",\"result\":" << result.str()
                    << "}\n";
            } catch (const std::exception& error) {
                ++failed;
                out << "{\"line\":" << line_number
                    << ",\"command\":" << json_string(line)
                    << ",\"status\":\"error\",\"code\":"
                    << exit_code_for(error)
                    << ",\"message\":" << json_string(error.what())
                    << "}\n";
            }
        } else {
            std::cerr << "--- 批处理 第 " << line_number << " 行: "
                      << line << " ---\n";
            try {
                run_single_command(command_args, out, json_mode);
                ++succeeded;
            } catch (const std::exception& error) {
                ++failed;
                std::cerr << "批处理第 " << line_number
                          << " 行失败(退出码 " << exit_code_for(error)
                          << "): " << error.what() << '\n';
            }
        }
    }
    if (!json_mode) {
        out << "批处理完成: 成功 " << succeeded << " 行, 失败 " << failed
            << " 行\n";
    }
    return failed == 0 ? kExitSuccess : kExitRuntimeError;
}

// ---------------------------------------------------------------------------
// 顶层分发与错误报告
// ---------------------------------------------------------------------------
void report_error(int exit_code,
                  std::string_view message,
                  bool json_mode) {
    g_operation_log.event("error", "command_failed",
        std::string(message) + "（退出码 " + std::to_string(exit_code) + "）");
    if (json_mode) {
        std::cout << "{\"error\":{\"code\":" << exit_code
                  << ",\"message\":" << json_string(message) << "}}\n";
        return;
    }
    std::cerr << "错误: " << message << '\n';
}

int run_cli(const std::vector<std::string>& raw_args) {
    std::vector<std::string> args = raw_args;
    const bool json_mode = extract_json_flag(args);
    try {
        configure_operation_log(args);
        if (args.size() == 1 ||
            (args.size() >= 2 &&
             (args[1] == "--help" || args[1] == "-h" || args[1] == "help"))) {
            print_usage(std::cout);
            return kExitSuccess;
        }
        if (args.size() < 2) {
            print_usage(std::cout);
            return kExitUsageError;
        }
        const auto& command = args[1];
        if (command == "batch") {
            return run_batch(args, std::cout, json_mode);
        }
        if (command == "capabilities") {
            if (args.size() > 2) {
                throw CliError(kExitUsageError,
                               "capabilities 不接受除 --json 之外的参数");
            }
            print_capabilities(std::cout, json_mode);
            return kExitSuccess;
        }
        if (args.size() >= 2 && command != "info" && command != "inspect" &&
            command != "probe" && command != "bench" && command != "list" &&
            command != "extract" && command != "restore") {
            print_usage(std::cerr);
            throw CliError(kExitUsageError, "未知命令: " + command);
        }
        run_single_command(args, std::cout, json_mode);
        return kExitSuccess;
    } catch (const CliError& error) {
        report_error(error.exit_code, error.what(), json_mode);
        return error.exit_code;
    } catch (const std::invalid_argument& error) {
        report_error(kExitUsageError, error.what(), json_mode);
        return kExitUsageError;
    } catch (const std::out_of_range& error) {
        report_error(kExitUsageError, error.what(), json_mode);
        return kExitUsageError;
    } catch (const uife::ResourceLimitError& error) {
        // 必须排在 std::exception 之前：它是 runtime_error 的子类，否则会被
        // 归进"运行时错误"，批处理脚本就分不清"镜像坏了"和"预算小了"。
        report_error(kExitResourceLimit, error.what(), json_mode);
        return kExitResourceLimit;
    } catch (const std::exception& error) {
        report_error(kExitRuntimeError, error.what(), json_mode);
        return kExitRuntimeError;
    }
}

#ifdef _WIN32
std::string to_utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        throw std::runtime_error("Windows 命令行参数无法转换为 UTF-8");
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) != size) {
        throw std::runtime_error("Windows 命令行参数 UTF-8 转换失败");
    }
    return result;
}
#endif

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    try {
        std::vector<std::string> args;
        args.reserve(static_cast<std::size_t>(argc));
        for (int index = 0; index < argc; ++index) {
            args.push_back(to_utf8(argv[index]));
        }
        return run_cli(args);
    } catch (const std::exception& error) {
        // to_utf8 失败等无法归类的场景：无 JSON 上下文，按人类可读输出
        std::cerr << "错误: " << error.what() << '\n';
        return kExitRuntimeError;
    }
}
#else
int main(int argc, char** argv) {
    try {
        std::vector<std::string> args;
        args.reserve(static_cast<std::size_t>(argc));
        for (int index = 0; index < argc; ++index) {
            args.emplace_back(argv[index]);
        }
        return run_cli(args);
    } catch (const std::exception& error) {
        std::cerr << "错误: " << error.what() << '\n';
        return kExitRuntimeError;
    }
}
#endif
