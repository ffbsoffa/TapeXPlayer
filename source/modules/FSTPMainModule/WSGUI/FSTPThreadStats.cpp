#include "FSTPThreadStats.h"

#if defined(__APPLE__) || defined(__linux__)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <sys/resource.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <pthread.h>
#else
#include <dirent.h>
#include <fstream>
#include <unistd.h>
#endif

namespace {

#if defined(__APPLE__)
// Static initialisation runs on the main thread, before main(): remember its id.
const uint64_t g_main_tid = [] { uint64_t tid = 0; pthread_threadid_np(nullptr, &tid); return tid; }();
#endif

struct ThreadSample {
    std::string name;
    uint64_t cpu_ns = 0;
};

// CPU time of every live thread of this process, keyed by OS thread id.
std::map<uint64_t, ThreadSample> SnapshotThreads() {
    std::map<uint64_t, ThreadSample> out;
#if defined(__APPLE__)
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) return out;
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        thread_extended_info_data_t ext;
        mach_msg_type_number_t ext_n = THREAD_EXTENDED_INFO_COUNT;
        thread_identifier_info_data_t ident;
        mach_msg_type_number_t ident_n = THREAD_IDENTIFIER_INFO_COUNT;
        if (thread_info(threads[i], THREAD_EXTENDED_INFO, (thread_info_t)&ext, &ext_n) == KERN_SUCCESS &&
            thread_info(threads[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&ident, &ident_n) == KERN_SUCCESS) {
            ThreadSample s;
            s.name = (ident.thread_id == g_main_tid) ? "main" : (ext.pth_name[0] ? ext.pth_name : "unnamed");
            s.cpu_ns = ext.pth_user_time + ext.pth_system_time;
            out[ident.thread_id] = std::move(s);
        }
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_act_t));
#else
    const std::string pid = std::to_string(getpid());
    DIR* dir = opendir("/proc/self/task");
    if (!dir) return out;
    while (dirent* e = readdir(dir)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        const std::string base = std::string("/proc/self/task/") + e->d_name;
        std::ifstream comm(base + "/comm"), sched(base + "/schedstat");
        ThreadSample s;
        std::getline(comm, s.name);
        if (!(sched >> s.cpu_ns)) continue;   // schedstat: ns on CPU, then wait ns, then slices
        if (pid == e->d_name) s.name = "main";
        out[std::stoull(e->d_name)] = std::move(s);
    }
    closedir(dir);
#endif
    return out;
}

// Whole-process CPU, including threads that have already exited.
uint64_t ProcessCpuNs() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    auto ns = [](const timeval& tv) { return uint64_t(tv.tv_sec) * 1000000000ull + uint64_t(tv.tv_usec) * 1000ull; };
    return ns(ru.ru_utime) + ns(ru.ru_stime);
}

}  // namespace

void FSTPPrintThreadCPU() {
    static bool primed = false;
    static std::map<uint64_t, uint64_t> last_thread_ns;
    static uint64_t last_process_ns = 0;
    static auto last_time = std::chrono::steady_clock::now();

    const auto now_time = std::chrono::steady_clock::now();
    const auto threads = SnapshotThreads();
    const uint64_t process_ns = ProcessCpuNs();
    const double window_ns = std::chrono::duration<double, std::nano>(now_time - last_time).count();

    if (primed && window_ns > 0.0) {
        std::map<std::string, std::pair<double, int>> by_name;   // name -> (ns, thread count)
        double live_ns = 0.0;
        for (const auto& [tid, s] : threads) {
            auto it = last_thread_ns.find(tid);
            const uint64_t prev = (it != last_thread_ns.end()) ? it->second : 0;   // new: count from its start
            const double d = (s.cpu_ns > prev) ? double(s.cpu_ns - prev) : 0.0;
            auto& slot = by_name[s.name];
            slot.first += d;
            slot.second += 1;
            live_ns += d;
        }
        const double total_ns = (process_ns > last_process_ns) ? double(process_ns - last_process_ns) : 0.0;
        // rusage ticks coarser than schedstat on some kernels: never report a negative remainder.
        const double exited_ns = std::max(0.0, total_ns - live_ns);

        std::vector<std::pair<std::string, double>> rows;
        for (const auto& [name, v] : by_name) {
            std::string label = name;
            if (v.second > 1) label += "(x" + std::to_string(v.second) + ")";
            rows.emplace_back(label, v.first);
        }
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

        auto pct = [window_ns](double ns) { return 100.0 * ns / window_ns; };
        std::string line;
        char buf[160];
        for (const auto& [label, ns] : rows) {
            if (pct(ns) < 0.5) continue;
            std::snprintf(buf, sizeof(buf), " %s=%.1f%%", label.c_str(), pct(ns));
            line += buf;
        }
        if (pct(exited_ns) >= 0.5) {
            std::snprintf(buf, sizeof(buf), " exited=%.1f%%", pct(exited_ns));
            line += buf;
        }
        std::printf("🧵 [THREAD CPU] total=%.1f%% |%s\n", pct(total_ns), line.c_str());
        std::fflush(stdout);
    }

    last_thread_ns.clear();
    for (const auto& [tid, s] : threads) last_thread_ns[tid] = s.cpu_ns;
    last_process_ns = process_ns;
    last_time = now_time;
    primed = true;
}

#else

void FSTPPrintThreadCPU() {}

#endif
