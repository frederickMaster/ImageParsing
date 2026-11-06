// 变异型 fuzz 驱动器：把语料喂给"驱动 + 解析器"这条只读路径，看它会不会崩。
//
// 为什么不是一个 LLVMFuzzerTestOneInput：
// 我们的镜像驱动都是按**路径**打开的（libewf/libtsm/libwim 都只接受文件名），
// libFuzzer 的覆盖率反馈再好，每次迭代也要落一次盘，绝大部分时间花在 I/O 上，
// 而且 Windows 上没有 clang 就完全跑不起来。所以这里做的是"确定性变异 + 自己
// 的看门狗"：同样的 seed 在任何平台上重复出同一批用例，能进每次 CI，也能在
// 夜间跑几个小时；崩溃/超时的输入留在磁盘上，可以直接 `--replay`。
//
// 覆盖范围：识别 → 打开 → 按扇区读 → 文件系统识别 → 挂载 → 列根目录 →
// 读第一个文件的前 64 KiB。导出（Extractor）不在里面：它会写盘，且在
// `uife_tests` 里已有独立的只读/深度/长路径用例。
//
// 用法：
//   uife_fuzz --seeds <目录> --work <目录> [--iterations N] [--seed N]
//             [--case-timeout-ms N] [--max-bytes N] [--seconds N] [--verbose]
//   uife_fuzz --replay <文件> [--case-timeout-ms N]
//
// 退出码：0 跑完没有发现崩溃/超时；2 用法错误；3 发现超时（输入已保存）。

#include "uife/DriverFactory.h"
#include "uife/FsParserFactory.h"
#include "uife/PartitionScanner.h"
#include "uife/drivers/fs/IFSParser.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define UIFE_EXIT_NOW(code) _exit(code)
#else
#include <unistd.h>
#define UIFE_EXIT_NOW(code) _exit(code)
#endif

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count());
}

// xorshift64*：跨平台结果一致（std::uniform_int_distribution 的实现细节
// 各标准库不同，用它会让"同样的 seed 复现"变成空话）。
class Rng {
public:
    explicit Rng(std::uint64_t seed)
        : state_(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}

    std::uint64_t next() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545F4914F6CDD1DULL;
    }

    std::size_t below(std::size_t bound) {
        return bound == 0 ? 0 : static_cast<std::size_t>(next() % bound);
    }

private:
    std::uint64_t state_;
};

struct Options {
    std::filesystem::path seeds;
    std::filesystem::path work;
    std::filesystem::path replay;
    std::size_t iterations{1000};
    std::uint64_t seed{1};
    long case_timeout_ms{5000};
    // 16 MiB：够装下整份 CI 语料（最大 6.9 MB），又不至于让真实镜像
    // 把内存吃满。0 表示不截断。
    std::size_t max_bytes{16U * 1024U * 1024U};
    long seconds{0};
    bool verbose{false};
};

// 看门狗看到的"当前用例"状态。崩溃时进程已经没了，所以输入与会话信息
// 必须在执行**之前**落到磁盘上。
struct CurrentCase {
    std::atomic<std::uint64_t> started_ns{0};
    std::atomic<long> timeout_ms{5000};
    std::atomic<bool> finished{true};
    // input 每个用例都要换（分卷样本要求同目录下还有 .E02…），看门狗线程会并发读，
    // 所以用一把锁而不是裸 path——竞态在这里不会崩，但会是数据竞争。
    std::mutex mutex;
    std::filesystem::path input;
    std::filesystem::path session;
    std::filesystem::path crash_directory;
};

CurrentCase g_current;

// 自检用：让每个用例故意睡一会儿，用来确认看门狗真的会把卡死的用例掐掉并把
// 输入留下来。看门狗是这套工具唯一"能发现死循环"的机制，它自己没被验证过
// 就等于没有——而真实死循环没法按需制造。
std::atomic<long> g_selftest_hang_ms{0};

[[noreturn]] void abort_case(const std::string& reason) {
    std::fprintf(stderr, "[fuzz] %s\n", reason.c_str());
    std::error_code error;
    std::filesystem::path input;
    {
        std::lock_guard<std::mutex> lock(g_current.mutex);
        input = g_current.input;
    }
    if (!input.empty() && std::filesystem::exists(input)) {
        std::filesystem::create_directories(g_current.crash_directory, error);
        const auto stamp = std::to_string(now_ns());
        const auto target = g_current.crash_directory / ("case-" + stamp + ".bin");
        std::filesystem::copy_file(input, target,
                                   std::filesystem::copy_options::overwrite_existing,
                                   error);
        std::fprintf(stderr, "[fuzz] 输入已保存: %s\n", target.string().c_str());
    }
    std::fflush(stderr);
    UIFE_EXIT_NOW(3);
}

void watchdog() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (g_current.finished.load(std::memory_order_relaxed)) {
            continue;
        }
        const auto started = g_current.started_ns.load(std::memory_order_relaxed);
        const auto limit = static_cast<std::uint64_t>(g_current.timeout_ms.load()) * 1000000ULL;
        if (started != 0 && now_ns() - started > limit) {
            abort_case("用例超过 " + std::to_string(g_current.timeout_ms.load()) +
                       " ms 没有返回（疑似死循环），已中止");
        }
    }
}

struct Stats {
    std::size_t cases{0};
    std::size_t rejected_at_identify{0};
    std::size_t opened{0};
    std::size_t mounted{0};
    std::size_t partitions{0};
    std::size_t files_read{0};
    std::uint64_t slowest_ns{0};
    std::map<std::string, std::size_t> exceptions;
    std::map<std::string, std::size_t> formats;
};

std::string describe(std::exception_ptr pointer) {
    try {
        std::rethrow_exception(pointer);
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
        return "未知异常";
    }
}

// 只读地"摸一遍"镜像。任何异常都是合法结果——畸形输入本来就该被拒绝；
// 我们只关心崩溃、越界（ASan 会报）和卡死。
void exercise(const std::filesystem::path& path, Stats& stats) {
    const auto info = uife::DriverFactory::instance().identify(path);
    if (!info.supported) {
        ++stats.rejected_at_identify;
        return;
    }
    ++stats.formats[info.format];

    // parse 的 mount 要 shared_ptr，而工厂返回 unique_ptr——这里转一次，
    // 后续读写都走同一个句柄，不会重复打开源镜像。
    std::shared_ptr<uife::IImageDriver> driver(
        uife::DriverFactory::instance().create(path));
    ++stats.opened;
    const auto size = driver->get_size();
    const auto sector = std::max<std::uint64_t>(driver->sector_size(), 512ULL);

    std::array<std::byte, 4096> buffer{};
    std::array<std::byte, 4096> sector_buffer{};
    // 头、中、尾三处：只读头会漏掉"表项指向镜像外"这类尾部错误。
    const std::array<std::uint64_t, 3> offsets = {
        0,
        size > sector * 2 ? ((size / 2) / sector) * sector : 0,
        size > sector ? ((size - sector) / sector) * sector : 0,
    };
    const auto sector_view = std::span<std::byte>(
        sector_buffer.data(),
        static_cast<std::size_t>(std::min<std::uint64_t>(sector, sector_buffer.size())));
    for (const auto offset : offsets) {
        (void)driver->read_sector(offset / sector, sector_view);
        (void)driver->read_at(offset, buffer);
    }
    (void)driver->integrity_note();

    // 和 CLI 一样：先试偏移 0，再按分区表逐项试。只试 0 会漏掉 MBR/GPT 镜像
    // ——它们的文件系统在 1 MiB 之后，而这类镜像正是现场取证最常见的形态。
    auto& factory = uife::FsParserFactory::instance();
    std::uint64_t offset = 0;
    std::uint64_t length = size;
    bool found = factory.supports(*driver, 0);
    if (!found) {
        for (const auto& partition : uife::scan_partitions(*driver)) {
            ++stats.partitions;
            if (factory.supports(*driver, partition.byte_offset)) {
                offset = partition.byte_offset;
                length = partition.byte_length;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        return;
    }
    auto parser = factory.create(*driver, offset);
    if (!parser) {
        return;
    }
    parser->mount(driver, offset, length);
    ++stats.mounted;
    (void)parser->filesystem_name();
    (void)parser->can_read_content();

    const auto root = parser->get_metadata("/");
    auto entries = parser->list_dir("/");
    // 深度 2 就够：再往下走，一次用例的耗时会被目录规模主导，
    // 变异带来的信号反而被摊薄。
    for (const auto& entry : entries) {
        if (entry.type != uife::NodeType::directory) {
            continue;
        }
        (void)parser->list_dir(entry.path);
        if (entries.size() > 64) {
            break;
        }
    }
    if (root.type == uife::NodeType::directory) {
        for (const auto& entry : entries) {
            if (entry.type != uife::NodeType::regular_file) {
                continue;
            }
            std::array<std::byte, 4096> content{};
            std::uint64_t offset = 0;
            while (offset < 64U * 1024U) {
                const auto read = parser->read_file(entry.path, offset, content);
                if (read == 0) {
                    break;
                }
                offset += read;
            }
            ++stats.files_read;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// 变异算子。每个都返回一句短描述，写进 current.txt，方便 --replay 时对上号。
// ---------------------------------------------------------------------------

std::string mutate_bit_flip(std::vector<std::byte>& data, Rng& rng) {
    if (data.empty()) {
        return {};
    }
    const auto flips = 1 + rng.below(16);
    for (std::size_t index = 0; index < flips; ++index) {
        const auto position = rng.below(data.size());
        const auto bit = static_cast<int>(rng.below(8));
        data[position] ^= static_cast<std::byte>(1U << bit);
    }
    return "bit-flip x" + std::to_string(flips);
}

std::string mutate_byte_smash(std::vector<std::byte>& data, Rng& rng) {
    if (data.empty()) {
        return {};
    }
    const auto count = std::min<std::size_t>(data.size(), 1 + rng.below(32));
    const auto start = rng.below(data.size() - count + 1);
    for (std::size_t index = 0; index < count; ++index) {
        data[start + index] = static_cast<std::byte>(rng.next() & 0xFF);
    }
    return "byte-smash " + std::to_string(count) + "@" + std::to_string(start);
}

// 边界值：0 与 -1 是解析器最容易漏掉的两种情况（"长度为 0 = 空"和
// "无符号回绕成 4 GB"），比随机字节更容易打中判断分支。
std::string mutate_boundary(std::vector<std::byte>& data, Rng& rng) {
    if (data.size() < 8) {
        return {};
    }
    const auto offset = rng.below(data.size() - 8);
    const bool all_ones = (rng.next() & 1) != 0;
    for (std::size_t index = 0; index < 8; ++index) {
        data[offset + index] = static_cast<std::byte>(all_ones ? 0xFF : 0x00);
    }
    return std::string(all_ones ? "boundary-ff@" : "boundary-00@") +
           std::to_string(offset);
}

std::string mutate_insert(std::vector<std::byte>& data, Rng& rng,
                          std::size_t max_bytes) {
    if (data.empty() || data.size() >= max_bytes) {
        return {};
    }
    const auto count = std::min<std::size_t>(max_bytes - data.size(),
                                            1 + rng.below(1024));
    const auto position = rng.below(data.size() + 1);
    std::vector<std::byte> block(count);
    for (auto& byte : block) {
        byte = static_cast<std::byte>(rng.next() & 0xFF);
    }
    data.insert(data.begin() + static_cast<std::ptrdiff_t>(position),
                block.begin(), block.end());
    return "insert " + std::to_string(count) + "@" + std::to_string(position);
}

std::string mutate_delete(std::vector<std::byte>& data, Rng& rng) {
    if (data.size() < 2) {
        return {};
    }
    const auto count = std::min<std::size_t>(data.size() - 1, 1 + rng.below(1024));
    const auto position = rng.below(data.size() - count + 1);
    data.erase(data.begin() + static_cast<std::ptrdiff_t>(position),
               data.begin() + static_cast<std::ptrdiff_t>(position + count));
    return "delete " + std::to_string(count) + "@" + std::to_string(position);
}

std::string mutate_duplicate(std::vector<std::byte>& data, Rng& rng,
                             std::size_t max_bytes) {
    if (data.size() < 16 || data.size() >= max_bytes) {
        return {};
    }
    const auto count = std::min<std::size_t>(
        {data.size() / 2, max_bytes - data.size(), 1 + rng.below(4096)});
    if (count == 0) {
        return {};
    }
    const auto from = rng.below(data.size() - count + 1);
    const auto to = rng.below(data.size() - count + 1);
    // 先拷出来再写回去：源区间与目标区间可能重叠，直接 copy_n 是未定义行为。
    const std::vector<std::byte> block(
        data.begin() + static_cast<std::ptrdiff_t>(from),
        data.begin() + static_cast<std::ptrdiff_t>(from + count));
    std::copy(block.begin(), block.end(),
              data.begin() + static_cast<std::ptrdiff_t>(to));
    return "duplicate " + std::to_string(count) + " " + std::to_string(from) +
           "->" + std::to_string(to);
}

std::string mutate_truncate(std::vector<std::byte>& data, Rng& rng) {
    if (data.size() < 32) {
        return {};
    }
    const auto keep = 1 + rng.below(data.size() - 1);
    data.resize(keep);
    return "truncate to " + std::to_string(keep);
}

std::string mutate(std::vector<std::byte>& data, Rng& rng, std::size_t max_bytes);

// 头部保护区变异：前 4 KiB（MBR/BPB/GPT 头/容器头）保持原样，只打后面的
// 元数据与数据区。盲变异有一大半用例死在"容量声明被改坏"这一步，根本走不到
// FAT 表、目录项、索引节点——而那里才是解析器最容易越界的地方。
std::string mutate_preserving_header(std::vector<std::byte>& data, Rng& rng) {
    constexpr std::size_t kGuard = 4096;
    if (data.size() <= kGuard + 512) {
        return mutate_bit_flip(data, rng);
    }
    std::vector<std::byte> region(data.begin() + static_cast<std::ptrdiff_t>(kGuard),
                                  data.end());
    const auto applied = mutate(region, rng, region.size());
    if (region.size() < 512) {
        return {};
    }
    data.resize(kGuard);
    data.insert(data.end(), region.begin(), region.end());
    return "preserve-header " + applied;
}

std::string mutate(std::vector<std::byte>& data, Rng& rng, std::size_t max_bytes) {
    switch (rng.below(7)) {
    case 0:
        return mutate_bit_flip(data, rng);
    case 1:
        return mutate_byte_smash(data, rng);
    case 2:
        return mutate_boundary(data, rng);
    case 3:
        return mutate_insert(data, rng, max_bytes);
    case 4:
        return mutate_delete(data, rng);
    case 5:
        return mutate_duplicate(data, rng, max_bytes);
    default:
        return mutate_truncate(data, rng);
    }
}

// ---------------------------------------------------------------------------
// 语料与磁盘交互
// ---------------------------------------------------------------------------

std::vector<std::filesystem::path> load_seed_paths(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> paths;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator iterator(root, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (!iterator->is_regular_file(error)) {
            continue;
        }
        paths.push_back(iterator->path());
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

std::vector<std::byte> read_head(const std::filesystem::path& path,
                                 std::size_t limit) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("无法读取语料: " + path.string());
    }
    if (limit == 0) {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        limit = error ? 0 : static_cast<std::size_t>(size);
    }
    std::vector<std::byte> buffer(limit);
    stream.read(reinterpret_cast<char*>(buffer.data()),
                static_cast<std::streamsize>(limit));
    buffer.resize(static_cast<std::size_t>(stream.gcount()));
    return buffer;
}

void write_case(const std::filesystem::path& target,
                const std::vector<std::byte>& data,
                const std::string& session) {
    {
        std::ofstream stream(target, std::ios::binary | std::ios::trunc);
        if (!stream) {
            throw std::runtime_error("无法写入用例文件: " + target.string());
        }
        stream.write(reinterpret_cast<const char*>(data.data()),
                     static_cast<std::streamsize>(data.size()));
    }
    std::ofstream session_stream(g_current.session,
                                 std::ios::binary | std::ios::trunc);
    session_stream << session << '\n';
    {
        std::lock_guard<std::mutex> lock(g_current.mutex);
        g_current.input = target;
    }
}

// 分卷样本（EWF 的 .E01…E06、BIN+CUE、分段 WIM）只有在同目录还能找到兄弟文件时
// 才打得开。把兄弟文件硬链接进用例目录，成本几乎为零，却能把一大半变异从
// "找不到分卷"变成真正走到解封装逻辑。硬链接失败（跨卷）时才退回拷贝，
// 且只拷不超过 max_bytes 的兄弟文件。
void prepare_case_directory(const std::filesystem::path& seed,
                            const std::filesystem::path& directory,
                            std::size_t max_bytes,
                            std::size_t& linked, std::size_t& copied) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const auto parent = seed.parent_path();
    if (parent.empty()) {
        return;
    }
    for (std::filesystem::directory_iterator iterator(parent, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (!iterator->is_regular_file(error)) {
            continue;
        }
        const auto sibling = iterator->path();
        if (sibling.filename() == seed.filename()) {
            continue;  // 这个位置留给变异后的输入
        }
        const auto target = directory / sibling.filename();
        std::filesystem::create_hard_link(sibling, target, error);
        if (!error) {
            ++linked;
            continue;
        }
        error.clear();
        const auto size = std::filesystem::file_size(sibling, error);
        if (error || (max_bytes != 0 && size > max_bytes)) {
            error.clear();
            continue;
        }
        std::filesystem::copy_file(sibling, target,
                                   std::filesystem::copy_options::overwrite_existing,
                                   error);
        if (!error) {
            ++copied;
        }
        error.clear();
    }
}

void run_case(const std::filesystem::path& path, Stats& stats) {
    const auto started = now_ns();
    g_current.started_ns.store(started, std::memory_order_relaxed);
    g_current.finished.store(false, std::memory_order_relaxed);
    if (g_selftest_hang_ms.load() > 0) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(g_selftest_hang_ms.load()));
    }
    try {
        exercise(path, stats);
    } catch (const std::exception&) {
        const auto message = describe(std::current_exception());
        // 只留前 60 个字符：同一类错误的消息体经常带着路径/偏移，聚合会碎成上千条。
        ++stats.exceptions[message.substr(0, 60)];
    }
    g_current.finished.store(true, std::memory_order_relaxed);
    const auto elapsed = now_ns() - started;
    stats.slowest_ns = std::max(stats.slowest_ns, elapsed);
    ++stats.cases;
}

void print_usage() {
    std::printf(
        "用法: uife_fuzz --seeds <目录> --work <目录> [选项]\n"
        "      uife_fuzz --replay <文件> [--case-timeout-ms <毫秒>]\n"
        "选项:\n"
        "  --iterations <N>        变异用例数（默认 1000）\n"
        "  --seed <N>              变异随机种子（默认 1；同种子同结果）\n"
        "  --case-timeout-ms <N>   单个用例的墙钟上限（默认 5000，超时即中止）\n"
        "  --max-bytes <N>         单个用例输入上限（默认 16 MiB；0 = 不截断）\n"
        "  --seconds <N>           整轮时间预算（默认 0 = 不限）\n"
        "  --verbose               逐个打印变异描述\n"
        "  --selftest-hang <毫秒>  自检：每个用例故意睡这么久，用于验证看门狗\n");
}

bool parse_size(const char* text, std::size_t& value) {
    char* end = nullptr;
    const auto parsed = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    value = static_cast<std::size_t>(parsed);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto next = [&]() -> const char* {
            if (index + 1 >= argc) {
                std::fprintf(stderr, "缺少 %s 的参数\n", argument.c_str());
                std::exit(2);
            }
            return argv[++index];
        };
        if (argument == "--seeds") {
            options.seeds = next();
        } else if (argument == "--work") {
            options.work = next();
        } else if (argument == "--replay") {
            options.replay = next();
        } else if (argument == "--iterations") {
            std::size_t value = 0;
            if (!parse_size(next(), value)) {
                std::fprintf(stderr, "--iterations 需要整数\n");
                return 2;
            }
            options.iterations = value;
        } else if (argument == "--seed") {
            std::size_t value = 0;
            if (!parse_size(next(), value)) {
                std::fprintf(stderr, "--seed 需要整数\n");
                return 2;
            }
            options.seed = value;
        } else if (argument == "--case-timeout-ms") {
            std::size_t value = 0;
            if (!parse_size(next(), value) || value == 0) {
                std::fprintf(stderr, "--case-timeout-ms 需要正整数\n");
                return 2;
            }
            options.case_timeout_ms = static_cast<long>(value);
        } else if (argument == "--max-bytes") {
            std::size_t value = 0;
            if (!parse_size(next(), value)) {
                std::fprintf(stderr, "--max-bytes 需要非负整数（0 = 不截断）\n");
                return 2;
            }
            options.max_bytes = value;
        } else if (argument == "--seconds") {
            std::size_t value = 0;
            if (!parse_size(next(), value)) {
                std::fprintf(stderr, "--seconds 需要整数\n");
                return 2;
            }
            options.seconds = static_cast<long>(value);
        } else if (argument == "--selftest-hang") {
            std::size_t value = 0;
            if (!parse_size(next(), value) || value == 0) {
                std::fprintf(stderr, "--selftest-hang 需要正整数毫秒\n");
                return 2;
            }
            g_selftest_hang_ms.store(static_cast<long>(value));
        } else if (argument == "--verbose") {
            options.verbose = true;
        } else if (argument == "--help" || argument == "-h") {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "未知参数: %s\n", argument.c_str());
            print_usage();
            return 2;
        }
    }

    g_current.timeout_ms.store(options.case_timeout_ms);
    std::thread(watchdog).detach();

    Stats stats;

    if (!options.replay.empty()) {
        // 回放：用户手里有一个可疑文件，想确认它是否还能让工具崩。
        run_case(options.replay, stats);
        std::printf("回放完成: %s\n", options.replay.string().c_str());
        return 0;
    }

    if (options.seeds.empty() || options.work.empty()) {
        print_usage();
        return 2;
    }

    const auto seeds = load_seed_paths(options.seeds);
    if (seeds.empty()) {
        std::fprintf(stderr, "语料目录里没有文件: %s\n",
                     options.seeds.string().c_str());
        return 2;
    }
    std::error_code error;
    std::filesystem::create_directories(options.work, error);
    g_current.session = options.work / "current.txt";
    g_current.crash_directory = options.work / "crash";

    std::vector<std::vector<std::byte>> corpus;
    corpus.reserve(seeds.size());
    for (const auto& path : seeds) {
        corpus.push_back(read_head(path, options.max_bytes));
    }

    std::size_t linked = 0;
    std::size_t copied = 0;
    std::vector<std::filesystem::path> case_directories;
    case_directories.reserve(seeds.size());
    for (std::size_t index = 0; index < seeds.size(); ++index) {
        const auto directory = options.work / "cases" / std::to_string(index);
        prepare_case_directory(seeds[index], directory, options.max_bytes, linked,
                               copied);
        case_directories.push_back(directory);
    }

    std::printf("[fuzz] 语料 %zu 个文件，变异 %zu 例，种子 %llu\n", corpus.size(),
                options.iterations,
                static_cast<unsigned long long>(options.seed));
    std::printf("[fuzz] 分卷兄弟文件：硬链接 %zu 个，拷贝 %zu 个\n", linked, copied);

    Rng rng(options.seed);
    const auto started = Clock::now();
    std::size_t completed = 0;
    for (std::size_t iteration = 0; iteration < options.iterations; ++iteration) {
        if (options.seconds > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                Clock::now() - started).count();
            if (elapsed >= options.seconds) {
                std::printf("[fuzz] 达到时间预算 %ld s，提前结束\n", options.seconds);
                break;
            }
        }
        const auto seed_index = rng.below(corpus.size());
        auto data = corpus[seed_index];
        std::string description = seeds[seed_index].filename().string();
        const auto operators = 1 + rng.below(3);
        // 一半用例保留头部：不然绝大多数变异只会得到"容量声明无效"，
        // 挂载与读文件这两段代码一年也跑不到。
        const bool preserve_header = rng.below(2) == 0;
        for (std::size_t stack = 0; stack < operators; ++stack) {
            const auto applied = preserve_header
                ? mutate_preserving_header(data, rng)
                : mutate(data, rng, options.max_bytes);
            if (!applied.empty()) {
                description += " | " + applied;
            }
        }
        const auto target = case_directories[seed_index] / seeds[seed_index].filename();
        write_case(target, data,
                   "seed=" + std::to_string(options.seed) +
                       " iteration=" + std::to_string(iteration) +
                       " source=" + description);
        if (options.verbose) {
            std::printf("[fuzz] %zu: %s\n", iteration, description.c_str());
        }
        run_case(target, stats);
        ++completed;
    }

    const auto elapsed = std::chrono::duration<double>(Clock::now() - started).count();
    std::printf("[fuzz] 完成 %zu 例，用时 %.1f s（%.0f 例/秒）\n", completed, elapsed,
                elapsed > 0 ? static_cast<double>(completed) / elapsed : 0.0);
    std::printf("[fuzz] 识别通过 %zu，打开成功 %zu，扫到分区 %zu，挂载成功 %zu，"
                "读到文件 %zu\n",
                stats.cases - stats.rejected_at_identify, stats.opened,
                stats.partitions, stats.mounted, stats.files_read);
    std::printf("[fuzz] 最慢用例 %.1f ms\n",
                static_cast<double>(stats.slowest_ns) / 1e6);
    if (!stats.formats.empty()) {
        std::printf("[fuzz] 识别出的容器格式:");
        for (const auto& [format, count] : stats.formats) {
            std::printf(" %s×%zu", format.c_str(), count);
        }
        std::printf("\n");
    }
    if (!stats.exceptions.empty()) {
        std::printf("[fuzz] 被拒绝的输入（%zu 类，属正常结果）:\n",
                    stats.exceptions.size());
        std::size_t shown = 0;
        for (const auto& [message, count] : stats.exceptions) {
            std::printf("        %5zu × %s\n", count, message.c_str());
            if (++shown == 8) {
                break;
            }
        }
    }
    return 0;
}
