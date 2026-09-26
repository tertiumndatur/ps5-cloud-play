// SPDX-License-Identifier: GPL-3.0-or-later
#include "cloud_cover_downloader.hpp"
#include "app_log.hpp"
#include "psn_resolve.h"
#include "ca_bundle.h"
#include "ps5_pngdec.hpp"

#if CLOUDPLAY_PS5
#include <curl/curl.h>

#include <algorithm>
#include <csetjmp>
#include <climits>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

#include <jpeglib.h>

namespace cloudplay {
namespace {
constexpr size_t max_image_bytes = 8 * 1024 * 1024;
constexpr size_t max_decoded_bytes = 64 * 1024 * 1024;
constexpr size_t max_decoder_work_bytes = 32 * 1024 * 1024;
// Match the card's artwork viewport. Covers are centre-cropped to this aspect
// ratio so the preview fills its frame without stretching or letterboxing.
constexpr int cover_width = 244;
constexpr int cover_height = 226;
constexpr size_t catalog_columns = 6;
constexpr size_t catalog_slots = 18;
constexpr uint16_t png_decoder_module = 0x008c;

extern "C" int sceSysmoduleLoadModule(uint16_t module_id);

static_assert(sizeof(ScePngDecCreateParam) == 12);
static_assert(sizeof(ScePngDecDecodeParam) == 32);
static_assert(sizeof(ScePngDecParseParam) == 16);
static_assert(sizeof(ScePngDecImageInfo) == 16);

size_t collect(char *ptr, size_t width, size_t count, void *user) {
    auto *bytes = static_cast<std::vector<unsigned char> *>(user);
    if (width && count > SIZE_MAX / width) return 0;
    const size_t size = width * count;
    if (size > max_image_bytes - bytes->size()) return 0;
    bytes->insert(bytes->end(), ptr, ptr + size);
    return size;
}

int progress(void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<std::atomic<bool> *>(user)->load(std::memory_order_relaxed) ? 1 : 0;
}

struct JpegErrorManager {
    jpeg_error_mgr base;
    std::jmp_buf jump;
};

void jpeg_error_exit(j_common_ptr context) {
    auto *error = reinterpret_cast<JpegErrorManager *>(context->err);
    std::longjmp(error->jump, 1);
}

bool decode_jpeg(const std::vector<unsigned char> &source,
                 std::vector<unsigned char> &decoded,
                 uint32_t &width, uint32_t &height) {
    if (source.size() < 3 || source[0] != 0xff || source[1] != 0xd8 ||
        source[2] != 0xff)
        return false;

    jpeg_decompress_struct context{};
    JpegErrorManager error{};
    std::vector<unsigned char> pixels;
    context.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_error_exit;
    if (setjmp(error.jump)) {
        jpeg_destroy_decompress(&context);
        return false;
    }

    jpeg_create_decompress(&context);
    jpeg_mem_src(&context, source.data(), static_cast<unsigned long>(source.size()));
    if (jpeg_read_header(&context, TRUE) != JPEG_HEADER_OK || context.image_width == 0 ||
        context.image_height == 0 || context.image_width > 4096 ||
        context.image_height > 4096) {
        jpeg_destroy_decompress(&context);
        return false;
    }
    context.out_color_space = JCS_RGB;
    if (!jpeg_start_decompress(&context) || context.output_components != 3) {
        jpeg_destroy_decompress(&context);
        return false;
    }

    const uint64_t image_bytes = static_cast<uint64_t>(context.output_width) *
        context.output_height * context.output_components;
    if (image_bytes == 0 || image_bytes > max_decoded_bytes) {
        jpeg_destroy_decompress(&context);
        return false;
    }
    pixels.resize(static_cast<size_t>(image_bytes));
    const size_t pitch = static_cast<size_t>(context.output_width) * 3;
    while (context.output_scanline < context.output_height) {
        JSAMPROW row = pixels.data() + static_cast<size_t>(context.output_scanline) * pitch;
        if (jpeg_read_scanlines(&context, &row, 1) != 1) {
            jpeg_destroy_decompress(&context);
            return false;
        }
    }
    width = context.output_width;
    height = context.output_height;
    if (!jpeg_finish_decompress(&context)) {
        jpeg_destroy_decompress(&context);
        return false;
    }
    jpeg_destroy_decompress(&context);
    decoded.swap(pixels);
    return true;
}

bool decode_cover(const std::vector<unsigned char> &source,
                  CloudCoverImage &output, bool decoder_available) {
    std::vector<unsigned char> decoded;
    uint32_t source_width = 0;
    uint32_t source_height = 0;
    bool source_is_bgra = false;
    if (source.size() >= 8 && source.size() <= UINT32_MAX && source[0] == 0x89 &&
        source[1] == 'P' && source[2] == 'N' && source[3] == 'G') {
        if (!decoder_available) return false;
        ScePngDecParseParam parse{source.data(), static_cast<uint32_t>(source.size()), 0};
        ScePngDecImageInfo info{};
        if (scePngDecParseHeader(&parse, &info) < 0 || info.image_width == 0 ||
            info.image_height == 0 || info.image_width > 4096 || info.image_height > 4096)
            return false;

        const uint64_t image_bytes = static_cast<uint64_t>(info.image_width) *
            info.image_height * 4;
        if (image_bytes > max_decoded_bytes || image_bytes > UINT32_MAX)
            return false;
        ScePngDecCreateParam create{
            sizeof(ScePngDecCreateParam), info.bit_depth > 8 ? 1u : 0u,
            info.image_width};
        const int work_size = scePngDecQueryMemorySize(&create);
        if (work_size <= 0 || static_cast<size_t>(work_size) > max_decoder_work_bytes)
            return false;
        std::vector<unsigned char> work(static_cast<size_t>(work_size));
        decoded.resize(static_cast<size_t>(image_bytes));
        void *handle = nullptr;
        if (scePngDecCreate(&create, work.data(), static_cast<uint32_t>(work.size()),
                            &handle) < 0 || !handle)
            return false;
        ScePngDecDecodeParam decode{
            source.data(), decoded.data(), static_cast<uint32_t>(source.size()),
            static_cast<uint32_t>(decoded.size()), 1, 255, info.image_width * 4};
        ScePngDecImageInfo output_info{};
        const int result = scePngDecDecode(handle, &decode, &output_info);
        (void)scePngDecDelete(handle);
        if (result < 0) return false;
        source_width = info.image_width;
        source_height = info.image_height;
        source_is_bgra = true;
    } else if (!decode_jpeg(source, decoded, source_width, source_height)) {
        return false;
    }

    output.width = cover_width;
    output.height = cover_height;
    output.rgba.resize(static_cast<size_t>(cover_width) * cover_height * 4);
    constexpr unsigned char background[] = {0x24, 0x29, 0x36, 0xff};
    for (size_t pixel = 0; pixel < output.rgba.size(); pixel += 4) {
        output.rgba[pixel + 0] = background[0];
        output.rgba[pixel + 1] = background[1];
        output.rgba[pixel + 2] = background[2];
        output.rgba[pixel + 3] = background[3];
    }

    uint32_t crop_x = 0;
    uint32_t crop_y = 0;
    uint32_t crop_width = source_width;
    uint32_t crop_height = source_height;
    if (static_cast<uint64_t>(source_width) * cover_height >
        static_cast<uint64_t>(source_height) * cover_width) {
        crop_width = std::max<uint32_t>(1, static_cast<uint32_t>(
            static_cast<uint64_t>(source_height) * cover_width / cover_height));
        crop_x = (source_width - crop_width) / 2;
    } else {
        crop_height = std::max<uint32_t>(1, static_cast<uint32_t>(
            static_cast<uint64_t>(source_width) * cover_height / cover_width));
        crop_y = (source_height - crop_height) / 2;
    }
    const size_t components = source_is_bgra ? 4U : 3U;

    // Bilinear sampling is not an antialiasing filter when a 1024px source is
    // reduced to the card size. Average every source pixel covered by each destination
    // pixel instead. The integer regions partition the source, so the work is
    // linear in the source image size and fine detail cannot fold into jaggies.
    for (int y = 0; y < cover_height; ++y) {
        const uint32_t source_y0 = crop_y + static_cast<uint32_t>(
            static_cast<uint64_t>(y) * crop_height / cover_height);
        const uint32_t source_y1 = crop_y + std::max<uint32_t>(
            static_cast<uint32_t>(static_cast<uint64_t>(y + 1) * crop_height / cover_height),
            static_cast<uint32_t>(static_cast<uint64_t>(y) * crop_height / cover_height) + 1);
        for (int x = 0; x < cover_width; ++x) {
            const uint32_t source_x0 = crop_x + static_cast<uint32_t>(
                static_cast<uint64_t>(x) * crop_width / cover_width);
            const uint32_t source_x1 = crop_x + std::max<uint32_t>(
                static_cast<uint32_t>(static_cast<uint64_t>(x + 1) * crop_width / cover_width),
                static_cast<uint32_t>(static_cast<uint64_t>(x) * crop_width / cover_width) + 1);
            uint64_t sums[3]{};
            uint64_t count = 0;
            for (uint32_t source_y = source_y0;
                 source_y < std::min(source_y1, source_height); ++source_y) {
                const unsigned char *pixel = decoded.data() +
                    (static_cast<size_t>(source_y) * source_width + source_x0) * components;
                for (uint32_t source_x = source_x0;
                     source_x < std::min(source_x1, source_width); ++source_x) {
                    if (source_is_bgra) {
                        const uint32_t alpha = pixel[3];
                        sums[0] += (static_cast<uint32_t>(pixel[2]) * alpha +
                                    background[0] * (255 - alpha) + 127) / 255;
                        sums[1] += (static_cast<uint32_t>(pixel[1]) * alpha +
                                    background[1] * (255 - alpha) + 127) / 255;
                        sums[2] += (static_cast<uint32_t>(pixel[0]) * alpha +
                                    background[2] * (255 - alpha) + 127) / 255;
                    } else {
                        sums[0] += pixel[0];
                        sums[1] += pixel[1];
                        sums[2] += pixel[2];
                    }
                    pixel += components;
                    ++count;
                }
            }
            if (count == 0) continue;
            unsigned char *target = output.rgba.data() +
                (static_cast<size_t>(y) * cover_width + x) * 4;
            target[0] = static_cast<unsigned char>((sums[0] + count / 2) / count);
            target[1] = static_cast<unsigned char>((sums[1] + count / 2) / count);
            target[2] = static_cast<unsigned char>((sums[2] + count / 2) / count);
            target[3] = 0xff;
        }
    }
    return true;
}
} // namespace

CloudCoverDownloader::CloudCoverDownloader()
    : pending_mutex_ready_(chiaki_mutex_init(&pending_mutex_, false) == CHIAKI_ERR_SUCCESS),
      png_decoder_available_(sceSysmoduleLoadModule(png_decoder_module) >= 0) {
    app_log("covers", "pending_mutex_ready", pending_mutex_ready_ ? 1 : 0);
    app_log("covers", "png_decoder_ready", png_decoder_available_ ? 1 : 0);
}

CloudCoverDownloader::~CloudCoverDownloader() {
    cancelled_.store(true);
    if (busy_) chiaki_thread_join(&worker_, nullptr);
    if (pending_mutex_ready_) chiaki_mutex_fini(&pending_mutex_);
}

void CloudCoverDownloader::reset() {
    if (busy_) { reset_pending_ = true; cancelled_.store(true); return; }
    attempted_.clear();
    images_.clear();
    clear_pending();
    ++generation_;
}

std::shared_ptr<const CloudCoverImage> CloudCoverDownloader::image(
    const std::string &key) const {
    const auto found = images_.find(key);
    return found == images_.end() ? nullptr : found->second;
}

void CloudCoverDownloader::schedule(const std::vector<CloudGame> &games,
                                     const std::vector<size_t> &visible,
                                     size_t selected) {
    if (busy_ || !pending_mutex_ready_) return;
    const size_t first = selected / catalog_columns > 1 ?
        (selected / catalog_columns - 1) * catalog_columns : 0;
    jobs_.clear();
    for (size_t i = first; i < visible.size() && i < first + catalog_slots; ++i) {
        const CloudGame &game = games[visible[i]];
        const std::string id = game.product_id.empty() ? game.identifier : game.product_id;
        if (id.empty() || game.image_url.rfind("https://", 0) != 0 ||
            game.image_url.size() > 2048) continue;
        const std::string key = cloud_cover_key(id);
        if (attempted_.count(key) || images_.count(key)) continue;
        attempted_.insert(key);
        jobs_.push_back({key, game.image_url});
    }
    if (jobs_.empty()) return;
    app_log("covers", "batch_scheduled", static_cast<int>(jobs_.size()));
    completed_ = 0;
    clear_pending();
    failure_details_ = 0;
    cancelled_.store(false);
    finished_.store(false);
    if (chiaki_thread_create(&worker_, run, this) == CHIAKI_ERR_SUCCESS) busy_ = true;
    else app_log("covers", "worker_start_failed");
}

void CloudCoverDownloader::poll() {
    if (!busy_) return;
    if (!finished_.load(std::memory_order_acquire)) {
        if (!reset_pending_) publish_pending();
        return;
    }
    chiaki_thread_join(&worker_, nullptr);
    busy_ = false;
    app_log("covers", "batch_finished", static_cast<int>(completed_));
    if (!cancelled_.load()) app_log("covers", "batch_failed",
        static_cast<int>(jobs_.size() - completed_));
    jobs_.clear();
    if (reset_pending_) {
        clear_pending();
        attempted_.clear();
        images_.clear();
        reset_pending_ = false;
        ++generation_;
        return;
    }
    publish_pending();
}

void CloudCoverDownloader::publish_pending() {
    if (!pending_mutex_ready_) return;
    std::vector<std::pair<std::string, std::shared_ptr<CloudCoverImage>>> completed;
    {
        ChiakiMutexLock lock(&pending_mutex_);
        completed.swap(pending_images_);
    }
    for (auto &entry : completed)
        images_[entry.first] = std::move(entry.second);
    generation_ += static_cast<unsigned>(completed.size());
}

void CloudCoverDownloader::clear_pending() {
    if (!pending_mutex_ready_) {
        pending_images_.clear();
        return;
    }
    ChiakiMutexLock lock(&pending_mutex_);
    pending_images_.clear();
}

void *CloudCoverDownloader::run(void *user) {
    auto *self = static_cast<CloudCoverDownloader *>(user);
    CURL *curl = curl_easy_init();
    if (!curl) {
        app_log("covers", "curl_init_failed");
        self->finished_.store(true, std::memory_order_release);
        return nullptr;
    }
    for (const Job &job : self->jobs_) {
        if (self->cancelled_.load(std::memory_order_relaxed)) break;
        std::shared_ptr<CloudCoverImage> image;
        if (self->fetch(job, curl, image)) {
            {
                ChiakiMutexLock lock(&self->pending_mutex_);
                self->pending_images_.emplace_back(job.key, std::move(image));
            }
            ++self->completed_;
        }
    }
    curl_easy_cleanup(curl);
    self->finished_.store(true, std::memory_order_release);
    return nullptr;
}

bool CloudCoverDownloader::fetch(const Job &job, void *curl_handle,
                                  std::shared_ptr<CloudCoverImage> &image) {
    CURL *curl = static_cast<CURL *>(curl_handle);
    curl_easy_reset(curl);
    std::vector<unsigned char> source;
    curl_slist *resolves = cloudplay_psn_resolves(job.url.c_str());
    if (!resolves || curl_easy_setopt(curl, CURLOPT_RESOLVE, resolves) != CURLE_OK) {
        if (failure_details_++ < 3) app_log("covers", "ip_map_failed");
        curl_slist_free_all(resolves);
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, job.url.c_str());
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    size_t ca_size = 0;
    const unsigned char *ca_data = cloudplay_ca_bundle(&ca_size);
    curl_blob ca_blob{const_cast<unsigned char *>(ca_data), ca_size, CURL_BLOB_NOCOPY};
    if (!ca_data || curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca_blob) != CURLE_OK) {
        if (failure_details_++ < 3) app_log("covers", "ca_bundle_unavailable");
        curl_slist_free_all(resolves);
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                     static_cast<curl_off_t>(max_image_bytes));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &source);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancelled_);
    app_log("covers", "transfer_started");
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(resolves);
    app_log("covers", "transfer_finished", static_cast<int>(result));
    app_log("covers", "response_status", static_cast<int>(status));
    app_log("covers", "response_bytes", static_cast<int>(source.size()));
    if (result != CURLE_OK || status != 200 || source.empty()) {
        if (failure_details_++ < 3) {
            app_log("covers", "curl_result", static_cast<int>(result));
            app_log("covers", "http_status", static_cast<int>(status));
            app_log_allocator("covers", "allocator_after_failure");
        }
        return false;
    }
    auto output = std::make_shared<CloudCoverImage>();
    app_log("covers", "decode_started");
    if (!decode_cover(source, *output, png_decoder_available_)) {
        if (failure_details_++ < 3) app_log("covers", "decode_failed");
        return false;
    }
    app_log("covers", "decode_finished", static_cast<int>(output->rgba.size()));
    image = std::move(output);
    return true;
}

} // namespace cloudplay
#endif
