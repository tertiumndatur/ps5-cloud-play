// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cloud_catalog.hpp"
#include "cloud_cover_image.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <chiaki/thread.h>

namespace cloudplay {

class CloudCoverDownloader {
public:
    CloudCoverDownloader();
    ~CloudCoverDownloader();
    void schedule(const std::vector<CloudGame> &games,
                  const std::vector<size_t> &visible, size_t selected);
    void poll();
    void reset();
    unsigned generation() const { return generation_; }
    std::shared_ptr<const CloudCoverImage> image(const std::string &key) const;

private:
    struct Job { std::string key, url; };
    static void *run(void *user);
    bool fetch(const Job &job, void *curl_handle,
               std::shared_ptr<CloudCoverImage> &image);
    void publish_pending();
    void clear_pending();
    std::vector<Job> jobs_;
    std::vector<std::pair<std::string, std::shared_ptr<CloudCoverImage>>> pending_images_;
    std::unordered_map<std::string, std::shared_ptr<const CloudCoverImage>> images_;
    std::unordered_set<std::string> attempted_;
    ChiakiMutex pending_mutex_{};
    ChiakiThread worker_{};
    std::atomic<bool> finished_{false};
    std::atomic<bool> cancelled_{false};
    bool busy_ = false;
    bool reset_pending_ = false;
    bool pending_mutex_ready_ = false;
    unsigned generation_ = 0;
    unsigned completed_ = 0;
    unsigned failure_details_ = 0;
    bool png_decoder_available_ = false;
};

} // namespace cloudplay
