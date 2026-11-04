#pragma once

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace uife::iso9660 {

struct IsoExtent {
    std::uint32_t lba{};
    std::uint32_t size{};
};

struct IsoEntry {
    std::string name;
    std::uint32_t extent_lba{};
    std::uint64_t size{};
    bool is_directory{};
    std::vector<IsoExtent> extents;
    std::uint32_t mode{};
    std::uint32_t uid{};
    std::uint32_t gid{};
    std::uint64_t link_count{1};
    std::uint64_t hard_link_group{};
    bool has_posix_metadata{};
    bool is_symlink{};
    std::string symlink_target;
    std::chrono::system_clock::time_point modified{};
    bool has_modified_time{};
};

class Iso9660 {
public:
    using Reader = std::function<std::size_t(
        std::uint64_t, std::span<std::byte>)>;

    explicit Iso9660(const std::filesystem::path& image_path,
                     std::uint64_t byte_offset = 0,
                     std::uint64_t byte_length = 0);
    Iso9660(std::uint64_t source_size,
            Reader reader,
            std::uint64_t byte_offset = 0,
            std::uint64_t byte_length = 0);

    [[nodiscard]] std::vector<IsoEntry> list(const std::string& iso_path = "/") const;
    [[nodiscard]] IsoEntry stat(const std::string& iso_path) const;
    [[nodiscard]] std::size_t read_file(const std::string& iso_path,
                                        std::uint64_t offset,
                                        char* destination,
                                        std::size_t length) const;
    void extract_file(const std::string& iso_path,
                      const std::filesystem::path& destination) const;
    [[nodiscard]] std::uint64_t image_size() const noexcept;
    [[nodiscard]] std::uint32_t volume_sectors() const noexcept;

private:
    [[nodiscard]] std::vector<IsoEntry> read_directory(const IsoEntry& directory) const;
    [[nodiscard]] IsoEntry resolve(const std::string& iso_path) const;
    [[nodiscard]] std::size_t read_entry(const IsoEntry& entry,
                                         std::uint64_t offset,
                                         char* destination,
                                         std::size_t length) const;
    [[nodiscard]] std::uint64_t entry_offset(const IsoEntry& entry) const;
    void read_at(std::uint64_t offset, char* destination, std::size_t length) const;
    void initialize();

    std::filesystem::path image_path_;
    Reader reader_;
    std::uint64_t image_size_{};
    std::uint64_t byte_offset_{};
    std::uint32_t volume_sectors_{};
    bool joliet_{};
    bool rock_ridge_{};
    std::uint8_t rock_ridge_skip_{};
    IsoEntry root_;
};

} // namespace uife::iso9660
