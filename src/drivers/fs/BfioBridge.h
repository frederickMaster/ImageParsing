#pragma once

// libyal 系列库把 libbfio、libcerror 等内部库静态打包进各自的 .a，但 `make install`
// 不导出它们的头文件。这里只声明用到的两个 libbfio 入口，并提供一个建立在
// uife::IImageDriver 之上的随机读写适配层，供 libfsapfs / libfsrefs 共用。
//
// 声明必须放在全局作用域：extern "C" 若写在匿名命名空间内会变成内部链接，
// 调用时报 "used but never defined"。

#include "uife/drivers/image/IImageDriver.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <span>

extern "C" {
struct uife_libbfio_handle;
struct uife_libcerror_error;

using uife_bfio_handle_t = uife_libbfio_handle;
using uife_bfio_error_t = uife_libcerror_error;

// libyal 的 size64_t 就是 64 位无符号整数。这里不引用它的头文件（本桥接层刻意
// 与具体 libyal 库解耦），用 uint64_t 保持同 ABI。
using uife_bfio_size64_t = std::uint64_t;

int libbfio_handle_initialize(
    uife_bfio_handle_t** handle,
    intptr_t* io_handle,
    int (*free_io_handle)(intptr_t** io_handle, uife_bfio_error_t** error),
    int (*clone_io_handle)(intptr_t** destination, intptr_t* source,
                           uife_bfio_error_t** error),
    int (*open)(intptr_t* io_handle, int access_flags, uife_bfio_error_t** error),
    int (*close)(intptr_t* io_handle, uife_bfio_error_t** error),
    ssize_t (*read)(intptr_t* io_handle, uint8_t* buffer, size_t size,
                    uife_bfio_error_t** error),
    ssize_t (*write)(intptr_t* io_handle, const uint8_t* buffer, size_t size,
                     uife_bfio_error_t** error),
    off64_t (*seek_offset)(intptr_t* io_handle, off64_t offset, int whence,
                           uife_bfio_error_t** error),
    int (*exists)(intptr_t* io_handle, uife_bfio_error_t** error),
    int (*is_open)(intptr_t* io_handle, uife_bfio_error_t** error),
    int (*get_size)(intptr_t* io_handle, uife_bfio_size64_t* size, uife_bfio_error_t** error),
    uint8_t flags,
    uife_bfio_error_t** error);

int libbfio_handle_free(uife_bfio_handle_t** handle, uife_bfio_error_t** error);
}

namespace uife {
namespace bfio {

// 一个只读的游标：把 libbfio 的 read/seek 语义映射到 IImageDriver::read_at。
struct ImageContext {
    const IImageDriver* image{};
    std::uint64_t base{};
    std::uint64_t length{};
    std::uint64_t position{};
    bool open{};
};

inline int free_context(intptr_t** io_handle, uife_bfio_error_t** error) {
    (void)error;
    if (io_handle == nullptr || *io_handle == nullptr) {
        return -1;
    }
    delete reinterpret_cast<ImageContext*>(*io_handle);
    *io_handle = nullptr;
    return 1;
}

inline int clone_context(intptr_t** destination, intptr_t* source,
                         uife_bfio_error_t** error) {
    (void)destination;
    (void)source;
    (void)error;
    // 解析过程不克隆句柄；显式拒绝比返回半初始化的副本安全。
    return -1;
}

inline int open_context(intptr_t* io_handle, int access_flags,
                        uife_bfio_error_t** error) {
    (void)access_flags;
    (void)error;
    auto* context = reinterpret_cast<ImageContext*>(io_handle);
    if (context == nullptr) {
        return -1;
    }
    context->open = true;
    context->position = 0;
    return 1;
}

inline int close_context(intptr_t* io_handle, uife_bfio_error_t** error) {
    (void)error;
    auto* context = reinterpret_cast<ImageContext*>(io_handle);
    if (context == nullptr) {
        return -1;
    }
    context->open = false;
    return 0;
}

inline ssize_t read_context(intptr_t* io_handle, uint8_t* buffer, size_t size,
                            uife_bfio_error_t** error) {
    (void)error;
    auto* context = reinterpret_cast<ImageContext*>(io_handle);
    if (context == nullptr || !context->open || buffer == nullptr) {
        return -1;
    }
    if (context->position >= context->length) {
        return 0;
    }
    const auto available = context->length - context->position;
    const auto wanted = std::min<std::uint64_t>(size, available);
    const std::span<std::byte> destination(reinterpret_cast<std::byte*>(buffer),
                                           static_cast<std::size_t>(wanted));
    const auto got = context->image->read_at(context->base + context->position,
                                             destination);
    context->position += got;
    return static_cast<ssize_t>(got);
}

inline ssize_t write_context(intptr_t* io_handle, const uint8_t* buffer, size_t size,
                             uife_bfio_error_t** error) {
    (void)io_handle;
    (void)buffer;
    (void)size;
    (void)error;
    return -1;
}

inline off64_t seek_context(intptr_t* io_handle, off64_t offset, int whence,
                            uife_bfio_error_t** error) {
    (void)error;
    auto* context = reinterpret_cast<ImageContext*>(io_handle);
    if (context == nullptr) {
        return -1;
    }
    std::int64_t origin = 0;
    switch (whence) {
    case SEEK_SET: origin = 0; break;
    case SEEK_CUR: origin = static_cast<std::int64_t>(context->position); break;
    case SEEK_END: origin = static_cast<std::int64_t>(context->length); break;
    default: return -1;
    }
    const auto target = origin + static_cast<std::int64_t>(offset);
    if (target < 0 || static_cast<std::uint64_t>(target) > context->length) {
        return -1;
    }
    context->position = static_cast<std::uint64_t>(target);
    return static_cast<off64_t>(context->position);
}

inline int exists_context(intptr_t* io_handle, uife_bfio_error_t** error) {
    (void)error;
    return io_handle != nullptr ? 1 : 0;
}

inline int is_open_context(intptr_t* io_handle, uife_bfio_error_t** error) {
    (void)error;
    auto* context = reinterpret_cast<ImageContext*>(io_handle);
    return context != nullptr && context->open ? 1 : 0;
}

inline int size_context(intptr_t* io_handle, uife_bfio_size64_t* size, uife_bfio_error_t** error) {
    (void)error;
    auto* context = reinterpret_cast<ImageContext*>(io_handle);
    if (context == nullptr || size == nullptr) {
        return -1;
    }
    *size = context->length;
    return 1;
}

// 为 [base, base+length) 区间创建一个 libbfio 句柄；失败返回 nullptr。
inline uife_bfio_handle_t* create(const IImageDriver& image, std::uint64_t base,
                                 std::uint64_t length) {
    auto* context = new ImageContext{&image, base, length, 0, false};
    uife_bfio_handle_t* handle = nullptr;
    uife_bfio_error_t* error = nullptr;
    if (libbfio_handle_initialize(&handle, reinterpret_cast<intptr_t*>(context),
                                  free_context, clone_context, open_context,
                                  close_context, read_context, write_context,
                                  seek_context, exists_context, is_open_context,
                                  size_context, 0, &error) != 1) {
        delete context;
        return nullptr;
    }
    return handle;
}

inline void destroy(uife_bfio_handle_t* handle) noexcept {
    if (handle != nullptr) {
        libbfio_handle_free(&handle, nullptr);
    }
}

} // namespace bfio
} // namespace uife
