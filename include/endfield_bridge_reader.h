// Reference reader for include/endfield_bridge.h: seqlock snapshot plus the
// liveness / freshness rules. Header-only, Windows, C++17.
#pragma once
#include <windows.h>
#include <cstring>

#include "endfield_bridge.h"

namespace efb {

// Copies the whole state; false if the writer kept it busy for ~1000 tries.
inline bool Snapshot(const EFB_Shared* shared, EFB_Shared& out) {
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const uint32_t before = shared->seq;
        if (before & 1) { YieldProcessor(); continue; }
        MemoryBarrier();
        memcpy(&out, const_cast<const EFB_Shared*>(shared), sizeof(out));
        MemoryBarrier();
        if (shared->seq == before) return true;
    }
    return false;
}

inline bool LayoutMatches(const EFB_Shared& s) {
    return s.magic == EFB_MAGIC && s.layout_version == EFB_LAYOUT_VERSION && s.layout_size == sizeof(EFB_Shared);
}

// The writer process is the one that created this state and has not exited.
inline bool WriterAlive(const EFB_Shared& s) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, s.writer_pid);
    if (!process) return false;
    FILETIME created{}, exited{}, kernel{}, user{};
    DWORD code = 0;
    const bool ok = GetProcessTimes(process, &created, &exited, &kernel, &user) &&
        GetExitCodeProcess(process, &code) && code == STILL_ACTIVE &&
        ((uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime) == s.writer_start_time;
    CloseHandle(process);
    return ok;
}

inline int64_t Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
inline double SecondsSince(const EFB_Shared& s, int64_t at, int64_t now) {
    return at && s.qpc_frequency ? double(now - at) / double(s.qpc_frequency) : 1e9;
}

constexpr double kStaleSeconds = 0.5;
enum class Status { Online, Disabled, WriterGone, Stale };
// Online: writer alive, plugin active, HUD sampling (any part fresh).
inline Status Check(const EFB_Shared& s, int64_t now, bool writerAlive) {
    if (!writerAlive) return Status::WriterGone;
    if (!s.active) return Status::Disabled;
    bool fresh = SecondsSince(s, s.team.sampled_at, now) < kStaleSeconds;
    for (const auto& o : s.operators) fresh = fresh || SecondsSince(s, o.sampled_at, now) < kStaleSeconds;
    return fresh ? Status::Online : Status::Stale;
}
inline bool TeamFresh(const EFB_Shared& s, int64_t now) {
    return (s.valid_mask & EFB_VALID_TEAM) && SecondsSince(s, s.team.sampled_at, now) < kStaleSeconds;
}
inline bool OperatorFresh(const EFB_Shared& s, int i, int64_t now) {
    return (s.valid_mask & EFB_VALID_OPERATOR(i)) && SecondsSince(s, s.operators[i].sampled_at, now) < kStaleSeconds;
}
// Remaining game seconds, extrapolated at most 0.25 s past the last sample.
inline double LifeLeft(const EFB_Shared& s, const EFB_Buff& b, int64_t now, bool fresh) {
    if (b.life_type == EFB_LIFE_INFINITY) return -1;
    double elapsed = fresh ? SecondsSince(s, b.sampled_at, now) : 0;
    if (elapsed > 0.25) elapsed = 0.25;
    const double left = b.life_time - elapsed * b.time_dilation;
    return left > 0 ? left : 0;
}
inline const char* StatusName(Status status) {
    switch (status) {
    case Status::Online: return "online";
    case Status::Disabled: return "disabled";
    case Status::WriterGone: return "writer gone";
    default: return "stale";
    }
}

} // namespace efb
