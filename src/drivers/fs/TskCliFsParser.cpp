#include "uife/drivers/fs/TskCliFsParser.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace uife {
namespace {

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~UniqueHandle() {
        reset();
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    void reset(HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_{};
};

std::wstring quote_argument(const std::wstring& argument) {
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(character);
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(character);
        }
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::string wide_to_utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        throw std::runtime_error("无法将 Sleuth Kit 输出转换为 UTF-8");
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.data(),
            static_cast<int>(value.size()), result.data(), count, nullptr, nullptr) != count) {
        throw std::runtime_error("无法将 Sleuth Kit 输出转换为 UTF-8");
    }
    return result;
}

std::string decode_output(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int wide_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (wide_count > 0) {
        std::wstring wide(static_cast<std::size_t>(wide_count), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), wide.data(), wide_count);
        return wide_to_utf8(wide);
    }
    const int fallback_count = MultiByteToWideChar(CP_ACP, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (fallback_count <= 0) {
        return value;
    }
    std::wstring wide(static_cast<std::size_t>(fallback_count), L'\0');
    MultiByteToWideChar(CP_ACP, 0, value.data(), static_cast<int>(value.size()),
        wide.data(), fallback_count);
    return wide_to_utf8(wide);
}

std::wstring find_tool(const wchar_t* filename) {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = SearchPathW(nullptr, filename, nullptr,
        static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (length > 0 && length < buffer.size()) {
        return std::wstring(buffer.data(), length);
    }

    if (const wchar_t* configured = _wgetenv(L"UIFE_TSK_BIN")) {
        const auto path = std::filesystem::path(configured) / filename;
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error)) {
            return path.wstring();
        }
    }

    constexpr wchar_t default_bin[] =
        L"C:\\ProgramData\\chocolatey\\lib\\sleuthkit\\tools\\"
        L"sleuthkit-4.4.0-win32\\bin";
    const auto path = std::filesystem::path(default_bin) / filename;
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error)) {
        return path.wstring();
    }
    return {};
}

std::wstring fls_path() {
    return find_tool(L"fls.exe");
}

std::wstring icat_path() {
    return find_tool(L"icat.exe");
}

const wchar_t* filesystem_type_argument(const std::string& name) {
    if (name == "NTFS") {
        return L"ntfs";
    }
    if (name == "FAT") {
        return L"fat";
    }
    if (name == "exFAT") {
        return L"exfat";
    }
    if (name == "HFS+") {
        return L"hfs";
    }
    return L"ext4";
}

std::wstring make_command_line(const std::wstring& executable,
                               const std::vector<std::wstring>& arguments) {
    std::wstring command = quote_argument(executable);
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command += quote_argument(argument);
    }
    return command;
}

std::string run_listing(const std::wstring& executable,
                        const std::vector<std::wstring>& arguments) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE raw_read_pipe = nullptr;
    HANDLE raw_write_pipe = nullptr;
    if (!CreatePipe(&raw_read_pipe, &raw_write_pipe, &security, 0)) {
        throw std::runtime_error("无法创建 Sleuth Kit 输出管道");
    }
    UniqueHandle read_pipe(raw_read_pipe);
    UniqueHandle write_pipe(raw_write_pipe);
    if (!SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0)) {
        throw std::runtime_error("无法配置 Sleuth Kit 输出管道");
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe.get();
    startup.hStdError = write_pipe.get();
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    auto command = make_command_line(executable, arguments);
    const BOOL created = CreateProcessW(executable.c_str(), command.data(),
        nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    write_pipe.reset();
    if (!created) {
        throw std::runtime_error("无法启动 Sleuth Kit fls.exe");
    }
    UniqueHandle process_handle(process.hProcess);
    UniqueHandle thread_handle(process.hThread);

    std::string output;
    std::array<char, 16384> buffer{};
    DWORD bytes_read = 0;
    DWORD read_error = ERROR_SUCCESS;
    while (true) {
        if (!ReadFile(read_pipe.get(), buffer.data(),
                      static_cast<DWORD>(buffer.size()), &bytes_read, nullptr)) {
            read_error = GetLastError();
            break;
        }
        if (bytes_read == 0) {
            break;
        }
        output.append(buffer.data(), bytes_read);
    }
    if (read_error != ERROR_SUCCESS && read_error != ERROR_BROKEN_PIPE) {
        throw std::runtime_error("读取 Sleuth Kit fls 输出失败");
    }
    if (WaitForSingleObject(process_handle.get(), INFINITE) != WAIT_OBJECT_0) {
        throw std::runtime_error("等待 Sleuth Kit fls 进程结束失败");
    }
    DWORD exit_code = 1;
    if (!GetExitCodeProcess(process_handle.get(), &exit_code)) {
        throw std::runtime_error("无法读取 Sleuth Kit fls 进程退出码");
    }
    if (exit_code != 0) {
        throw std::runtime_error("Sleuth Kit fls 解析目录失败: " + decode_output(output));
    }
    return decode_output(output);
}

void run_icat_to_file(const std::wstring& executable,
                      const std::vector<std::wstring>& arguments,
                      const std::filesystem::path& destination) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    // readonly-audit: destination 是 icat 的输出落点（临时文件或导出目标）
    UniqueHandle output_file(CreateFileW(destination.c_str(), GENERIC_WRITE, 0,
        &security, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr));
    if (output_file.get() == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("无法创建临时文件以读取镜像文件内容");
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output_file.get();
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    auto command = make_command_line(executable, arguments);
    const BOOL created = CreateProcessW(executable.c_str(), command.data(),
        nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    output_file.reset();
    if (!created) {
        throw std::runtime_error("无法启动 Sleuth Kit icat.exe");
    }
    UniqueHandle process_handle(process.hProcess);
    UniqueHandle thread_handle(process.hThread);
    if (WaitForSingleObject(process_handle.get(), INFINITE) != WAIT_OBJECT_0) {
        throw std::runtime_error("等待 Sleuth Kit icat 进程结束失败");
    }
    DWORD exit_code = 1;
    if (!GetExitCodeProcess(process_handle.get(), &exit_code)) {
        throw std::runtime_error("无法读取 Sleuth Kit icat 进程退出码");
    }
    if (exit_code != 0) {
        std::error_code ignored;
        std::filesystem::remove(destination, ignored);
        throw std::runtime_error("Sleuth Kit icat 无法读取 inode 内容");
    }
}

std::string normalize_path(const std::string& path) {
    if (path.empty()) {
        return "/";
    }
    if (path.front() != '/' || path.find('\\') != std::string::npos ||
        path.find('\0') != std::string::npos ||
        path.find("/../") != std::string::npos || path.ends_with("/..") ||
        path.find("/./") != std::string::npos || path.ends_with("/.")) {
        throw std::invalid_argument("VFS 路径必须是规范的 POSIX 风格路径");
    }
    return path;
}

bool parse_u64(std::string_view value, std::uint64_t& result) {
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

std::vector<std::string_view> split_tabs(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start <= line.size()) {
        const auto end = line.find('\t', start);
        fields.push_back(line.substr(start, end == std::string_view::npos
            ? line.size() - start : end - start));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return fields;
}

VFSNode parse_entry(std::string_view line, const std::string& parent_path) {
    const auto colon = line.find(':');
    if (colon == std::string_view::npos || colon < 3) {
        throw std::runtime_error("无法解析 Sleuth Kit fls 目录项");
    }
    const auto prefix = line.substr(0, colon);
    const auto separator = prefix.find_last_of(' ');
    if (separator == std::string_view::npos || separator + 1 >= prefix.size()) {
        throw std::runtime_error("fls 目录项缺少 inode 编号");
    }
    VFSNode node;
    if (!parse_u64(prefix.substr(separator + 1), node.metadata.inode)) {
        throw std::runtime_error("fls 返回了无效 inode 编号");
    }
    switch (prefix.front()) {
    case 'd':
        node.type = NodeType::directory;
        node.metadata.mode = 0755;
        break;
    case 'r':
        node.type = NodeType::regular_file;
        node.metadata.mode = 0644;
        break;
    case 'l':
        node.type = NodeType::symbolic_link;
        node.metadata.mode = 0777;
        break;
    default:
        node.type = NodeType::other;
        break;
    }
    const auto fields = split_tabs(line);
    if (fields.size() >= 4) {
        std::uint64_t gid = 0;
        std::uint64_t uid = 0;
        std::uint64_t size = 0;
        if (parse_u64(fields[fields.size() - 3], size) &&
            parse_u64(fields[fields.size() - 2], uid) &&
            parse_u64(fields[fields.size() - 1], gid)) {
            node.metadata.size = size;
            if (uid <= std::numeric_limits<std::uint32_t>::max()) {
                node.metadata.uid = static_cast<std::uint32_t>(uid);
            }
            if (gid <= std::numeric_limits<std::uint32_t>::max()) {
                node.metadata.gid = static_cast<std::uint32_t>(gid);
            }
            node.metadata.has_owner =
                uid <= std::numeric_limits<std::uint32_t>::max() &&
                gid <= std::numeric_limits<std::uint32_t>::max();
        }
    }
    const auto name_start = line.find('\t', colon);
    if (name_start == std::string_view::npos || name_start + 1 >= line.size()) {
        throw std::runtime_error("fls 目录项缺少名称");
    }
    auto name_end = line.find('\t', name_start + 1);
    if (name_end == std::string_view::npos) {
        name_end = line.size();
    }
    auto name = line.substr(name_start + 1, name_end - name_start - 1);
    if (!name.empty() && name.back() == '\r') {
        name.remove_suffix(1);
    }
    node.name.assign(name);
    if (node.name.empty() || node.name == "." || node.name == "..") {
        throw std::runtime_error("fls 返回了无效目录项名称");
    }
    node.path = parent_path == "/" ? "/" + node.name : parent_path + "/" + node.name;
    return node;
}

} // namespace

struct TskCliFsParser::Impl {
    std::shared_ptr<IImageDriver> image;
    std::filesystem::path spool_path;
    std::uint64_t offset{};
    std::uint64_t length{};
    std::uint64_t sector_size{};
    mutable std::mutex mutex;
    mutable std::vector<VFSNode> cached_entries;
    mutable std::string cached_directory;
    mutable std::map<std::string, VFSNode> known_nodes;
    mutable std::string cached_file;
    mutable std::uint64_t cached_inode{};
};

TskCliFsParser::TskCliFsParser(std::string filesystem_name)
    : filesystem_name_(std::move(filesystem_name)) {}

TskCliFsParser::~TskCliFsParser() {
    if (impl_ && !impl_->spool_path.empty()) {
        std::error_code ignored;
        std::filesystem::remove(impl_->spool_path, ignored);
    }
}

bool TskCliFsParser::available() noexcept {
    return !fls_path().empty() && !icat_path().empty();
}

void TskCliFsParser::mount(std::shared_ptr<IImageDriver> image,
                           std::uint64_t byte_offset,
                           std::uint64_t byte_length) {
    if (!image || byte_offset > image->get_size() ||
        byte_length > image->get_size() - byte_offset ||
        byte_offset % image->sector_size() != 0) {
        throw std::invalid_argument("Sleuth Kit 挂载范围超出镜像边界或未按扇区对齐");
    }
    if (!available()) {
        throw std::runtime_error(
            "找不到 Sleuth Kit fls.exe/icat.exe；请将其加入 PATH 或设置 UIFE_TSK_BIN");
    }
    auto state = std::make_unique<Impl>();
    state->image = std::move(image);
    state->offset = byte_offset;
    state->length = byte_length;
    state->sector_size = state->image->sector_size();
    impl_ = std::move(state);
}

std::vector<VFSNode> TskCliFsParser::list_dir(const std::string& input_path) const {
    if (!impl_) {
        throw std::runtime_error(filesystem_name_ + " 尚未挂载");
    }
    const auto path = normalize_path(input_path);
    if (path != "/") {
        (void)list_dir("/");
    }
    std::size_t component_end = path.find('/', 1);
    while (component_end != std::string::npos) {
        (void)list_dir(path.substr(0, component_end));
        component_end = path.find('/', component_end + 1);
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->cached_directory == path) {
        return impl_->cached_entries;
    }

    const auto executable = fls_path();
    std::vector<std::wstring> arguments{L"-u", L"-l", L"-f",
        filesystem_type_argument(filesystem_name_)};
    if (impl_->offset != 0) {
        arguments.emplace_back(L"-o");
        arguments.push_back(std::to_wstring(impl_->offset / impl_->sector_size));
    }
    arguments.push_back(impl_->image->source_path().wstring());
    if (path != "/") {
        const auto directory = impl_->known_nodes.find(path);
        if (directory == impl_->known_nodes.end() ||
            directory->second.type != NodeType::directory) {
            throw std::runtime_error("无法解析 VFS 目录（请先列出其父目录）: " + path);
        }
        arguments.push_back(std::to_wstring(directory->second.metadata.inode));
    }

    const auto output = run_listing(executable, arguments);
    std::vector<VFSNode> entries;
    std::size_t start = 0;
    while (start < output.size()) {
        const auto end = output.find('\n', start);
        auto line = std::string_view(output).substr(start,
            end == std::string::npos ? output.size() - start : end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (!line.empty()) {
            auto node = parse_entry(line, path);
            if (line.size() >= 3 && line[2] != '-') {
                impl_->known_nodes[node.path] = node;
                entries.push_back(std::move(node));
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    impl_->cached_directory = path;
    impl_->cached_entries = entries;
    return entries;
}

VFSNode TskCliFsParser::get_metadata(const std::string& input_path) const {
    if (!impl_) {
        throw std::runtime_error(filesystem_name_ + " 尚未挂载");
    }
    const auto path = normalize_path(input_path);
    if (path == "/") {
        VFSNode root;
        root.name = "/";
        root.path = "/";
        root.type = NodeType::directory;
        root.metadata.inode = filesystem_name_ == "NTFS" ? 5 : 2;
        root.metadata.mode = 0755;
        return root;
    }
    auto parent_end = path.find_last_of('/');
    const auto parent = parent_end == 0 ? "/" : path.substr(0, parent_end);
    const auto name = path.substr(parent_end + 1);
    const auto children = list_dir(parent);
    const auto found = std::find_if(children.begin(), children.end(),
        [&name](const VFSNode& node) { return node.name == name; });
    if (found == children.end()) {
        throw std::runtime_error("镜像中不存在该路径: " + path);
    }
    return *found;
}

std::size_t TskCliFsParser::read_file(const std::string& input_path,
                                      std::uint64_t offset,
                                      std::span<std::byte> destination) const {
    if (!impl_) {
        throw std::runtime_error(filesystem_name_ + " 尚未挂载");
    }
    const auto path = normalize_path(input_path);
    const auto node = get_metadata(path);
    if (node.type != NodeType::regular_file) {
        throw std::invalid_argument("指定路径不是普通文件: " + path);
    }
    if (offset >= node.metadata.size || destination.empty()) {
        return 0;
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->cached_file != path || impl_->cached_inode != node.metadata.inode) {
        if (!impl_->spool_path.empty()) {
            std::error_code ignored;
            std::filesystem::remove(impl_->spool_path, ignored);
        }
        std::array<wchar_t, MAX_PATH> temp_directory{};
        std::array<wchar_t, MAX_PATH> temp_name{};
        if (GetTempPathW(static_cast<DWORD>(temp_directory.size()),
                         temp_directory.data()) == 0 ||
            GetTempFileNameW(temp_directory.data(), L"uif", 0, temp_name.data()) == 0) {
            throw std::runtime_error("无法创建 Sleuth Kit 临时提取文件");
        }
        impl_->spool_path = temp_name.data();
        std::vector<std::wstring> arguments{L"-f",
            filesystem_type_argument(filesystem_name_)};
        if (impl_->offset != 0) {
            arguments.emplace_back(L"-o");
            arguments.push_back(std::to_wstring(impl_->offset / impl_->sector_size));
        }
        arguments.push_back(impl_->image->source_path().wstring());
        arguments.push_back(std::to_wstring(node.metadata.inode));
        run_icat_to_file(icat_path(), arguments, impl_->spool_path);
        std::error_code size_error;
        const auto extracted_size = std::filesystem::file_size(
            impl_->spool_path, size_error);
        if (size_error || extracted_size != node.metadata.size) {
            std::error_code ignored;
            std::filesystem::remove(impl_->spool_path, ignored);
            impl_->spool_path.clear();
            throw std::runtime_error(
                "icat 输出大小与文件系统元数据不匹配，拒绝返回不完整文件");
        }
        impl_->cached_file = path;
        impl_->cached_inode = node.metadata.inode;
    }

    std::ifstream input(impl_->spool_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("无法打开 Sleuth Kit 临时读取文件");
    }
    input.seekg(static_cast<std::streamoff>(offset));
    const auto request = std::min<std::uint64_t>(destination.size(), node.metadata.size - offset);
    input.read(reinterpret_cast<char*>(destination.data()),
               static_cast<std::streamsize>(request));
    return static_cast<std::size_t>(input.gcount());
}

std::string_view TskCliFsParser::filesystem_name() const noexcept {
    return filesystem_name_;
}

} // namespace uife
#else
namespace uife {
struct TskCliFsParser::Impl {};

TskCliFsParser::TskCliFsParser(std::string filesystem_name)
    : filesystem_name_(std::move(filesystem_name)) {}
TskCliFsParser::~TskCliFsParser() = default;
bool TskCliFsParser::available() noexcept { return false; }
void TskCliFsParser::mount(std::shared_ptr<IImageDriver>, std::uint64_t, std::uint64_t) {
    throw std::runtime_error("Sleuth Kit 命令行后端仅支持 Windows");
}
std::vector<VFSNode> TskCliFsParser::list_dir(const std::string&) const {
    throw std::runtime_error("Sleuth Kit 命令行后端仅支持 Windows");
}
VFSNode TskCliFsParser::get_metadata(const std::string&) const {
    throw std::runtime_error("Sleuth Kit 命令行后端仅支持 Windows");
}
std::size_t TskCliFsParser::read_file(const std::string&, std::uint64_t,
                                      std::span<std::byte>) const {
    throw std::runtime_error("Sleuth Kit 命令行后端仅支持 Windows");
}
std::string_view TskCliFsParser::filesystem_name() const noexcept {
    return filesystem_name_;
}
} // namespace uife
#endif
