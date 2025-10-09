#pragma once

#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <mutex>
#include <chrono>

/**
 * @brief Lightweight function call counter for finding hot spots
 *
 * Usage:
 * void MyHotFunction() {
 *     FSTP_COUNT_CALL("MyHotFunction");
 *     // ... function code
 * }
 */
class FSTPCallCounter {
private:
    struct CallStats {
        size_t count = 0;
        std::chrono::steady_clock::time_point last_report;
    };

    static std::unordered_map<std::string, CallStats> counters_;
    static std::mutex mutex_;
    static std::chrono::steady_clock::time_point last_global_report_;

public:
    static void Count(const char* name) {
        std::lock_guard<std::mutex> lock(mutex_);
        counters_[name].count++;
    }

    static void Report() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();

        // Output only if a second has passed since last report
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_global_report_).count();
        if (elapsed_ms >= 1000) {
            double elapsed_sec = elapsed_ms / 1000.0;

            std::cout << "\n╔════════════════════════════════════════════════════════╗" << std::endl;
            std::cout << "║  🔥 Hot spots (calls/sec)                              ║" << std::endl;
            std::cout << "╠════════════════════════════════════════════════════════╣" << std::endl;

            // Sort by call count
            std::vector<std::pair<std::string, size_t>> sorted;
            for (const auto& [name, stats] : counters_) {
                // Normalize to calls/sec by dividing by real time
                size_t calls_per_sec = static_cast<size_t>(stats.count / elapsed_sec);
                sorted.push_back({name, calls_per_sec});
            }
            std::sort(sorted.begin(), sorted.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });

            // Output top-20
            int i = 0;
            for (const auto& [name, count] : sorted) {
                if (i++ >= 20) break;
                printf("║  %-45s %8zu  ║\n", name.c_str(), count);
            }

            std::cout << "╚════════════════════════════════════════════════════════╝" << std::endl;

            // Reset counters
            counters_.clear();
            last_global_report_ = now;
        }
    }

    static void ReportIfNeeded() {
        static auto last_check = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();

        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_check).count() >= 100) {
            Report();
            last_check = now;
        }
    }
};

// Static members
inline std::unordered_map<std::string, FSTPCallCounter::CallStats> FSTPCallCounter::counters_;
inline std::mutex FSTPCallCounter::mutex_;
inline std::chrono::steady_clock::time_point FSTPCallCounter::last_global_report_ = std::chrono::steady_clock::now();

// Convenience macros
#define FSTP_COUNT_CALL(name) FSTPCallCounter::Count(name)
#define FSTP_REPORT_HOTSPOTS() FSTPCallCounter::ReportIfNeeded()
