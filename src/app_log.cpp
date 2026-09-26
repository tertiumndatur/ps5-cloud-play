// SPDX-License-Identifier: GPL-3.0-or-later
#include "app_log.hpp"
#include "storage_io.hpp"

#include <chrono>
#include <mutex>
#include <string>

namespace cloudplay {

#if CLOUDPLAY_PS5
extern "C" size_t cloudplay_allocator_metric(int metric);
#endif

namespace {

void append_log(const char *component, const char *event, std::string_view detail) {
#if CLOUDPLAY_PS5
    static std::mutex mutex;
    static std::string contents;
    static unsigned sequence = 0;
    static const auto started = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> guard(mutex);
    if (sequence == 0) {
        if (!storage_read_file("/download0/debug.log", 131072, contents))
            contents = "Cloud Play log\n";
        if (contents.size() > 65536) contents.erase(0, contents.size() - 65536);
        // Keep the growing diagnostic buffer out of the small libc heap.  The
        // PS5 C++ allocation bridge maps allocations of this size directly.
        contents.reserve(131072);
        contents += "\n--- launch ---\n";
    }
    if (contents.size() > 120000) {
        contents.erase(0, contents.size() - 65536);
        contents.insert(0, "... previous events trimmed ...\n");
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    contents += std::to_string(++sequence) + " +" + std::to_string(elapsed) +
        "ms " + component + " " + event;
    const size_t limit = detail.size() < 512 ? detail.size() : 512;
    for (size_t i = 0; i < limit; ++i) {
        const unsigned char ch = static_cast<unsigned char>(detail[i]);
        contents += ch == '\r' || ch == '\n' || ch < 0x20 ? ' ' : static_cast<char>(ch);
    }
    if (detail.size() > limit) contents += " [truncated]";
    contents += '\n';
    (void)storage_write_atomic("/download0/debug.log", contents);
#else
    (void)component;
    (void)event;
    (void)detail;
#endif
}

} // namespace

void app_log(const char *component, const char *event, int result) {
    append_log(component, event, " result=" + std::to_string(result));
}

void app_log_text(const char *component, const char *event, std::string_view detail) {
    append_log(component, event, detail);
}

void app_log_allocator(const char *component, const char *event) {
#if CLOUDPLAY_PS5
    append_log(component, event,
        " pools=" + std::to_string(cloudplay_allocator_metric(0)) +
        " mapped=" + std::to_string(cloudplay_allocator_metric(1)) +
        " live=" + std::to_string(cloudplay_allocator_metric(2)) +
        " peak=" + std::to_string(cloudplay_allocator_metric(3)) +
        " failures=" + std::to_string(cloudplay_allocator_metric(4)));
#else
    (void)component;
    (void)event;
#endif
}

} // namespace cloudplay

extern "C" void cloudplay_runtime_fault(const char *message) {
    cloudplay::app_log_text("runtime", "fault", message ? message : "unknown");
}
