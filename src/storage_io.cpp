// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage_io.hpp"

#include <algorithm>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if CLOUDPLAY_PS5
extern "C" {
int sceKernelOpen(const char *path, int flags, uint16_t mode);
int sceKernelClose(int descriptor);
int64_t sceKernelRead(int descriptor, void *buffer, size_t length);
int64_t sceKernelWrite(int descriptor, const void *buffer, size_t length);
int sceKernelRename(const char *from, const char *to);
int sceKernelUnlink(const char *path);
}
#endif

namespace cloudplay {
namespace {

int open_read(const char *path) {
#if CLOUDPLAY_PS5
    return sceKernelOpen(path, 0x0000, 0);
#else
    return open(path, O_RDONLY);
#endif
}

int open_write(const char *path) {
#if CLOUDPLAY_PS5
    return sceKernelOpen(path, 0x0601, 0666);
#else
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
#endif
}

int64_t read_bytes(int fd, void *data, size_t size) {
#if CLOUDPLAY_PS5
    return sceKernelRead(fd, data, size);
#else
    return read(fd, data, size);
#endif
}

int64_t write_bytes(int fd, const void *data, size_t size) {
#if CLOUDPLAY_PS5
    return sceKernelWrite(fd, data, size);
#else
    return write(fd, data, size);
#endif
}

bool close_file(int fd) {
#if CLOUDPLAY_PS5
    return sceKernelClose(fd) == 0;
#else
    return close(fd) == 0;
#endif
}

bool rename_file(const char *from, const char *to) {
#if CLOUDPLAY_PS5
    if (sceKernelRename(from, to) >= 0) return true;
    // ProsperoLight retries after removing the old destination on PS5.
    (void)sceKernelUnlink(to);
    return sceKernelRename(from, to) >= 0;
#else
    return rename(from, to) == 0;
#endif
}

} // namespace

bool storage_read_file(const std::string &path, size_t maximum, std::string &data) {
    data.clear();
    const int fd = open_read(path.c_str());
    if (fd < 0) return false;
    char buffer[4096];
    bool good = true;
    for (;;) {
        const size_t remaining = maximum - data.size();
        const size_t requested = std::min(sizeof(buffer), remaining + 1);
        const int64_t count = read_bytes(fd, buffer, requested);
#if !CLOUDPLAY_PS5
        if (count < 0 && errno == EINTR) continue;
#endif
        if (count < 0 || static_cast<size_t>(count) > remaining) {
            good = false;
            break;
        }
        if (count == 0) break;
        data.append(buffer, static_cast<size_t>(count));
    }
    if (!close_file(fd)) good = false;
    if (!good) data.clear();
    return good;
}

bool storage_write_atomic(const std::string &path, std::string_view data) {
    const std::string temporary = path + ".tmp";
    const int fd = open_write(temporary.c_str());
    if (fd < 0) return false;
    bool good = true;
#if !CLOUDPLAY_PS5
    good = fchmod(fd, 0600) == 0;
#endif
    size_t at = 0;
    while (good && at < data.size()) {
        const int64_t count = write_bytes(fd, data.data() + at, data.size() - at);
#if !CLOUDPLAY_PS5
        if (count < 0 && errno == EINTR) continue;
#endif
        if (count <= 0) good = false;
        else at += static_cast<size_t>(count);
    }
#if !CLOUDPLAY_PS5
    if (good) good = fsync(fd) == 0;
#endif
    if (!close_file(fd)) good = false;
    if (good) good = rename_file(temporary.c_str(), path.c_str());
    if (!good) storage_remove(temporary);
    return good;
}

bool storage_remove(const std::string &path) {
#if CLOUDPLAY_PS5
    return sceKernelUnlink(path.c_str()) >= 0;
#else
    return unlink(path.c_str()) == 0;
#endif
}

bool storage_exists(const std::string &path) {
    const int fd = open_read(path.c_str());
    if (fd < 0) return false;
    (void)close_file(fd);
    return true;
}

} // namespace cloudplay
