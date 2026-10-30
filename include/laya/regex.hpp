/**
 * @file laya_regex.hpp
 * @brief High-performance Google RE2 regular expression engine with intelligent LRU cache.
 *
 * Provides linear-time DLP pattern evaluation immune to ReDoS catastrophic backtracking.
 * Implements bounded capacity, idle TTL expiration, policy epoch delta invalidation,
 * and SIGHUP cache flushing.
 */

#pragma once

#include <string>
#include <vector>
#include <list>
#include <unordered_map>
#include <memory>
#include <chrono>
#include <shared_mutex>
#include <mutex>
#include <iostream>
#include <re2/re2.h>

namespace laya {

/**
 * @struct DlpPattern
 * @brief Dynamic DLP pattern specification supplied dynamically via IPC.
 */
struct DlpPattern {
    std::string name;
    std::string regex_str;
    std::string verdict = "SECRET_LEAK";
    int score = 90;
};

/**
 * @struct DlpScanResult
 * @brief Outcome of DLP regex evaluation over extracted document text.
 */
struct DlpScanResult {
    bool matched = false;
    std::string primary_pattern_name;
    std::string primary_verdict;
    int max_score = 0;
    std::vector<std::string> matched_pattern_names;
};

/**
 * @class RegexCache
 * @brief Thread-safe LRU cache of compiled RE2 regular expression instances.
 */
class RegexCache {
public:
    RegexCache(size_t max_entries = 512, int idle_ttl_seconds = 900)
        : max_entries_(max_entries), idle_ttl_seconds_(idle_ttl_seconds), current_epoch_(0) {}

    /**
     * @brief Update cache limits from configuration.
     */
    void configure(size_t max_entries, int idle_ttl_seconds) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        max_entries_ = max_entries;
        idle_ttl_seconds_ = idle_ttl_seconds;
        prune_overflow();
    }

    /**
     * @brief Instantly invalidate all cached patterns (e.g. on SIGHUP).
     */
    void clear() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        cache_map_.clear();
        lru_list_.clear();
    }

    /**
     * @brief Invalidate cache if client policy epoch counter incremented.
     * @param epoch Client policy version from IPC header.
     */
    void check_epoch(uint64_t epoch) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (epoch != current_epoch_) {
            cache_map_.clear();
            lru_list_.clear();
            current_epoch_ = epoch;
        }
    }

    /**
     * @brief Obtain or compile an RE2 instance for the given pattern string.
     * @param pattern Regular expression string.
     * @return Shared pointer to compiled RE2, or nullptr if syntax error.
     */
    std::shared_ptr<re2::RE2> get_or_compile(const std::string& pattern) {
        auto now = std::chrono::steady_clock::now();

        // 1. Read lock check
        {
            std::shared_lock<std::shared_mutex> read_lock(mutex_);
            auto it = cache_map_.find(pattern);
            if (it != cache_map_.end()) {
                // Check idle TTL
                auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second->last_accessed).count();
                if (age <= idle_ttl_seconds_) {
                    it->second->last_accessed = now;
                    return it->second->re;
                }
            }
        }

        // 2. Write lock compile and insert
        std::unique_lock<std::shared_mutex> write_lock(mutex_);
        auto it = cache_map_.find(pattern);
        if (it != cache_map_.end()) {
            it->second->last_accessed = now;
            return it->second->re;
        }

        // Compile new RE2 pattern
        re2::RE2::Options opts;
        opts.set_log_errors(false);
        opts.set_longest_match(false);
        auto re = std::make_shared<re2::RE2>(pattern, opts);

        if (!re->ok()) {
            std::cerr << "[laya_regex] Compilation failed for pattern '" << pattern 
                      << "': " << re->error() << "\n";
            return nullptr;
        }

        // Evict LRU if capacity reached
        if (lru_list_.size() >= max_entries_) {
            auto oldest = lru_list_.back();
            cache_map_.erase(oldest.pattern_str);
            lru_list_.pop_back();
        }

        // Insert at front of LRU
        lru_list_.push_front({pattern, re, now});
        cache_map_[pattern] = lru_list_.begin();

        return re;
    }

    /**
     * @brief Scan text against a list of dynamic patterns.
     * @param text Target text string.
     * @param patterns Vector of DLP patterns.
     * @param epoch Policy epoch passed by client.
     * @return DlpScanResult with matches and highest score.
     */
    DlpScanResult scan(const std::string& text, const std::vector<DlpPattern>& patterns, uint64_t epoch) {
        check_epoch(epoch);

        DlpScanResult result;
        if (text.empty() || patterns.empty()) {
            return result;
        }

        for (const auto& pat : patterns) {
            auto re = get_or_compile(pat.regex_str);
            if (!re) continue;

            re2::StringPiece sp(text);
            if (re2::RE2::PartialMatch(sp, *re)) {
                result.matched = true;
                result.matched_pattern_names.push_back(pat.name);

                if (pat.score > result.max_score) {
                    result.max_score = pat.score;
                    result.primary_pattern_name = pat.name;
                    result.primary_verdict = pat.verdict;
                }
            }
        }

        return result;
    }

private:
    struct CacheItem {
        std::string pattern_str;
        std::shared_ptr<re2::RE2> re;
        std::chrono::steady_clock::time_point last_accessed;
    };

    void prune_overflow() {
        while (lru_list_.size() > max_entries_) {
            auto oldest = lru_list_.back();
            cache_map_.erase(oldest.pattern_str);
            lru_list_.pop_back();
        }
    }

    size_t max_entries_;
    int idle_ttl_seconds_;
    uint64_t current_epoch_;
    mutable std::shared_mutex mutex_;
    std::list<CacheItem> lru_list_;
    std::unordered_map<std::string, std::list<CacheItem>::iterator> cache_map_;
};

} // namespace laya
