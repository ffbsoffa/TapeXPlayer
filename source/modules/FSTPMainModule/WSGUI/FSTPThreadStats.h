#pragma once

// FSTP_RENDER_STATS=1 diagnostics: where the process CPU goes, thread by thread.
//
// Called once a second from the UI loop's stats block, right after the RENDER STATS line. Prints
//
//   🧵 [THREAD CPU] total=54.1% | fstp-render=21.3% fstp-fullres=12.0% main=6.2% ... exited=3.1%
//
// in percent of one core over the last window (100% = one core busy, like top). Threads are
// grouped by name (FSTPSetThreadName); "exited" is CPU spent by threads that ended within the
// window (the proxy's per-segment workers live for a fraction of a second).
// Linux: /proc/self/task/<tid>/{comm,schedstat}. macOS: task_threads + THREAD_EXTENDED_INFO.
// Windows: not implemented (no-op).

void FSTPPrintThreadCPU();
