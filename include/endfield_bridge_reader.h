// Reference reader for include/endfield_bridge.h. Header-only, Windows, C++17.
// It applies every rule of the ABI: bounds-checked directory lookup, one
// seqlock per section tied to the header seq, size-prefix copies for
// forward compatibility, event ring overwrite detection and liveness.
#pragma once
#include <windows.h>
#include <cstring>
#include <vector>

#include "endfield_bridge.h"

namespace efb {

// A read-only view of the mapping (or of any buffer, for tests).
class Mapping {
public:
    Mapping() = default;
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    ~Mapping() { Close(); }
    bool Open(const wchar_t* name = EFB_MAPPING_NAME) {
        Close();
        handle_ = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        if (!handle_) return false;
        view_ = MapViewOfFile(handle_, FILE_MAP_READ, 0, 0, 0);
        MEMORY_BASIC_INFORMATION info{};
        if (!view_ || !VirtualQuery(view_, &info, sizeof(info))) { Close(); return false; }
        base_ = static_cast<const char*>(view_);
        size_ = info.RegionSize;
        return true;
    }
    void Attach(const void* base, size_t size) { Close(); base_ = static_cast<const char*>(base); size_ = size; }
    void Close() {
        if (view_) UnmapViewOfFile(view_);
        if (handle_) CloseHandle(handle_);
        view_ = handle_ = nullptr;
        base_ = nullptr;
        size_ = 0;
    }
    const char* base() const { return base_; }
    size_t size() const { return size_; }
private:
    void* view_ = nullptr;
    HANDLE handle_ = nullptr;
    const char* base_ = nullptr;
    size_t size_ = 0;
};

inline uint32_t LoadSeq(const char* p) { return *reinterpret_cast<const volatile uint32_t*>(p); }
// Copies the first min(sizeof(T), size) bytes of src into a zeroed T.
template<class T> void CopyPrefix(T& out, const void* src, size_t size) {
    memset(static_cast<void*>(&out), 0, sizeof(T));
    memcpy(static_cast<void*>(&out), src, size < sizeof(T) ? size : sizeof(T));
}

struct Header {
    uint32_t seq = 0;
    EFB_Header fields{};
    std::vector<EFB_Section> sections; // only those that fit inside total_size
    const EFB_Section* Find(uint32_t id) const {
        for (const auto& s : sections) if (s.id == id) return &s;
        return nullptr;
    }
};
enum class Result { Ok, Busy, Incompatible, Missing, Retry };

// Header and directory under the header seqlock.
inline Result ReadHeader(const Mapping& m, Header& out) {
    if (!m.base() || m.size() < sizeof(EFB_SectionHeader)) return Result::Incompatible;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const uint32_t s1 = LoadSeq(m.base());
        if (s1 & 1) { YieldProcessor(); continue; }
        MemoryBarrier();
        EFB_Header h;
        CopyPrefix(h, m.base(), m.size());
        const size_t headerSize = h.header_size < m.size() ? h.header_size : m.size();
        CopyPrefix(h, m.base(), headerSize);
        std::vector<EFB_Section> sections;
        const bool shapeOk = h.magic == EFB_MAGIC && h.abi_major == EFB_ABI_MAJOR &&
            h.total_size <= m.size() && h.section_entry_size >= sizeof(uint32_t) * 2 &&
            uint64_t(h.directory_offset) + uint64_t(h.section_count) * h.section_entry_size <= h.total_size;
        if (shapeOk) {
            for (uint32_t i = 0; i < h.section_count; ++i) {
                EFB_Section s;
                CopyPrefix(s, m.base() + h.directory_offset + size_t(i) * h.section_entry_size, h.section_entry_size);
                const uint64_t dataEnd = uint64_t(s.data_offset) + uint64_t(s.stride) * s.capacity;
                if (s.header_size >= sizeof(EFB_SectionHeader) && uint64_t(s.offset) + s.header_size <= s.data_offset &&
                    dataEnd <= h.total_size && (s.stride || !s.capacity))
                    sections.push_back(s);
            }
        }
        MemoryBarrier();
        if (LoadSeq(m.base()) != s1) continue;
        if (!shapeOk) return Result::Incompatible;
        out.seq = s1;
        out.fields = h;
        out.sections = std::move(sections);
        return Result::Ok;
    }
    return Result::Busy;
}

// A consistent copy of one section: its header plus all of its elements.
class Section {
public:
    const EFB_Section& info() const { return info_; }
    uint32_t capacity() const { return info_.capacity; }
    template<class T> T Head() const { T out; CopyPrefix(out, bytes_.data(), info_.header_size); return out; }
    template<class T> T Element(uint32_t i) const {
        T out;
        CopyPrefix(out, bytes_.data() + (info_.data_offset - info_.offset) + size_t(i) * info_.stride, info_.stride);
        return out;
    }
private:
    friend Result ReadSection(const Mapping&, const Header&, uint32_t, Section&);
    EFB_Section info_{};
    std::vector<char> bytes_;
};

// Retry: the header changed meanwhile; read the header again.
inline Result ReadSection(const Mapping& m, const Header& h, uint32_t id, Section& out) {
    const EFB_Section* s = h.Find(id);
    if (!s) return Result::Missing;
    const size_t span = size_t(s->data_offset - s->offset) + size_t(s->stride) * s->capacity;
    out.info_ = *s;
    out.bytes_.resize(span);
    const char* src = m.base() + s->offset;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const uint32_t s1 = LoadSeq(src);
        if (s1 & 1) { YieldProcessor(); continue; }
        MemoryBarrier();
        memcpy(out.bytes_.data(), src, span);
        MemoryBarrier();
        if (LoadSeq(src) != s1) continue;
        return LoadSeq(m.base()) == h.seq ? Result::Ok : Result::Retry;
    }
    return Result::Busy;
}

// Follows the event ring. Poll() returns events in order exactly once; when
// it sets `resync`, cached buff state is no longer trustworthy: re-read buffs.
struct EventBatch {
    bool resync = false;  // first poll, new session, or events were overwritten
    uint64_t lost = 0;    // events overwritten before they could be read
    std::vector<EFB_Event> events;
};
class EventCursor {
public:
    Result Poll(const Mapping& m, const Header& h, EventBatch& out) {
        out = EventBatch{};
        if (!started_ || h.fields.session != session_) {
            Section events;
            const Result r = ReadSection(m, h, EFB_SECTION_EVENTS, events);
            if (r != Result::Ok) return r;
            started_ = true;
            session_ = h.fields.session;
            last_ = events.Head<EFB_EventsHeader>().head;
            out.resync = true;
            return Result::Ok;
        }
        Section events;
        const Result r = ReadSection(m, h, EFB_SECTION_EVENTS, events);
        if (r != Result::Ok) return r;
        const uint64_t head = events.Head<EFB_EventsHeader>().head;
        const uint32_t capacity = events.capacity();
        uint64_t from = last_ + 1;
        if (capacity && head > last_ + capacity) {
            out.lost = head - last_ - capacity;
            out.resync = true;
            from = head - capacity + 1;
        }
        for (uint64_t n = from; capacity && n <= head; ++n) {
            const EFB_Event e = events.Element<EFB_Event>(uint32_t((n - 1) % capacity));
            if (e.serial != n) { ++out.lost; out.resync = true; continue; } // overwritten while we copied
            out.events.push_back(e);
        }
        last_ = head;
        return Result::Ok;
    }
private:
    bool started_ = false;
    uint32_t session_ = 0;
    uint64_t last_ = 0;
};
// The payload of an event, zero-filled past what the writer provided.
template<class T> T Payload(const EFB_Event& e) {
    T out;
    CopyPrefix(out, e.payload, e.payload_size < EFB_EVENT_PAYLOAD ? e.payload_size : EFB_EVENT_PAYLOAD);
    return out;
}

// The writer process is the one that created this state and has not exited.
inline bool WriterAlive(const EFB_Header& h) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, h.writer_pid);
    if (!process) return false;
    FILETIME created{}, exited{}, kernel{}, user{};
    DWORD code = 0;
    const bool ok = GetProcessTimes(process, &created, &exited, &kernel, &user) &&
        GetExitCodeProcess(process, &code) && code == STILL_ACTIVE &&
        ((uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime) == h.writer_start_time;
    CloseHandle(process);
    return ok;
}

inline int64_t Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
inline double SecondsSince(const EFB_Header& h, int64_t at, int64_t now) {
    return at && h.qpc_frequency ? double(now - at) / double(h.qpc_frequency) : 1e9;
}
constexpr double kStaleSeconds = 0.5;
inline bool Fresh(const EFB_Header& h, int64_t sampledAt, int64_t now) {
    return SecondsSince(h, sampledAt, now) < kStaleSeconds;
}
enum class Status { Online, Disabled, WriterGone, Stale };
// newestSample: the most recent sampled_at you read (team, operators or buffs).
inline Status Check(const EFB_Header& h, bool writerAlive, int64_t newestSample, int64_t now) {
    if (!writerAlive) return Status::WriterGone;
    if (!h.active) return Status::Disabled;
    return Fresh(h, newestSample, now) ? Status::Online : Status::Stale;
}
inline const char* StatusName(Status status) {
    switch (status) {
    case Status::Online: return "online";
    case Status::Disabled: return "disabled";
    case Status::WriterGone: return "writer gone";
    default: return "stale";
    }
}
// Remaining game seconds, extrapolated at most 0.25 s past the last sample.
inline double LifeLeft(const EFB_Header& h, const EFB_Buff& b, int64_t now, bool online) {
    if (b.life_type == EFB_LIFE_INFINITY) return -1;
    double elapsed = online ? SecondsSince(h, b.sampled_at, now) : 0;
    if (elapsed > 0.25) elapsed = 0.25;
    const double left = b.life_time - elapsed * b.time_dilation;
    return left > 0 ? left : 0;
}

} // namespace efb
