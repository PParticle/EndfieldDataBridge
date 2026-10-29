// Test consumer for the EndfieldDataBridge shared memory.
//   efb_reader          live view of SP, operators and buffs
//   efb_reader --once   print one snapshot and exit
//   efb_reader --events print buff events as they happen
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../include/endfield_bridge.h"

namespace {
const EFB_Shared* g_shared = nullptr;

bool Snapshot(EFB_Shared& out) {
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const uint32_t before = g_shared->seq;
        if (before & 1) { YieldProcessor(); continue; }
        MemoryBarrier();
        memcpy(&out, const_cast<const EFB_Shared*>(g_shared), sizeof(out));
        MemoryBarrier();
        if (g_shared->seq == before) return true;
    }
    return false;
}
int64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
double Ago(const EFB_Shared& s, int64_t at) { return at ? double(Qpc() - at) / double(s.qpc_frequency) : -1; }
// "chr_0007_ikut_4616825702491504642" -> "chr_0007_ikut"
std::string Short(const char* name) {
    std::string n = name[0] ? name : "-";
    const size_t cut = n.find_last_of('_');
    if (cut != std::string::npos && n.size() - cut > 8 &&
        n.find_first_not_of("0123456789", cut + 1) == std::string::npos)
        n.resize(cut);
    return n;
}
const char* EventName(int type) {
    switch (type) {
    case EFB_EVENT_BUFF_ADDED: return "ADDED";
    case EFB_EVENT_BUFF_REFRESHED: return "REFRESHED";
    case EFB_EVENT_BUFF_ENHANCED: return "ENHANCED";
    case EFB_EVENT_BUFF_FINISHED: return "FINISHED";
    default: return "?";
    }
}
std::string LifeLeft(const EFB_Shared& s, const EFB_Buff& b) {
    if (b.life_type == EFB_LIFE_INFINITY) return "inf";
    const double elapsed = std::min(Ago(s, b.sampled_at), 0.25);
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f/%.1f", std::max(b.life_time - elapsed * b.time_dilation, 0.0),
        b.duration);
    return text;
}

void PrintState(const EFB_Shared& s) {
    std::printf("session=%u active=%u pid=%u published %.2fs ago  buffs=%u events=%llu\n\n", s.session,
        s.active, s.writer_pid, Ago(s, s.published_at), s.buff_count, (unsigned long long)s.event_head);
    std::printf("SP %.1f / %.1f  (per cell %d, %.2fs ago)\n\n", s.team.atb, s.team.atb_max,
        s.team.atb_per_cell, Ago(s, s.team.sampled_at));
    for (int i = 0; i < EFB_MAX_OPERATORS; ++i) {
        const EFB_Operator& o = s.operators[i];
        if (!o.present) { std::printf("[%d] -\n", i); continue; }
        std::printf("[%d]%s %-20s ult %6.1f/%-6.1f combo %5.1f/%-5.1f uid=%u\n", i, o.is_main_character ? "*" : " ",
            Short(o.entity_name).c_str(), o.ultimate_sp, o.max_ultimate_sp, o.combo_cd_remaining, o.combo_cooldown,
            o.instance_uid);
    }
    std::vector<const EFB_Buff*> buffs;
    for (const auto& b : s.buffs) if (b.key) buffs.push_back(&b);
    std::sort(buffs.begin(), buffs.end(), [](const EFB_Buff* a, const EFB_Buff* b) {
        return a->owner.instance_uid != b->owner.instance_uid ? a->owner.instance_uid < b->owner.instance_uid
                                                              : a->key < b->key;
    });
    std::printf("\n%-5s %-20s %-44s %-14s %4s %5s %-3s %s\n", "key", "owner", "buff_id", "life", "stk", "dil",
        "dbf", "source / root_source");
    for (const EFB_Buff* b : buffs) {
        std::string owner = Short(b->owner.entity_name);
        if (b->owner.char_index >= 0) owner = "[" + std::to_string(b->owner.char_index) + "]" + owner;
        std::string from = Short(b->source.entity_name);
        if (b->root_source.instance_uid != b->source.instance_uid) from += " < " + Short(b->root_source.entity_name);
        std::printf("%-5u %-20.20s %-44.44s %-14s %4d %5.2f %3d %s\n", b->key, owner.c_str(), b->buff_id,
            LifeLeft(s, *b).c_str(), b->enhance_cnt, b->time_dilation, b->is_debuff, from.c_str());
    }
}

int Events() {
    static EFB_Shared s;
    uint64_t last = 0;
    uint32_t session = 0;
    for (;;) {
        if (!Snapshot(s)) { Sleep(10); continue; }
        if (s.session != session) {
            std::printf("--- session %u (re-read the buff table) ---\n", s.session);
            session = s.session;
            last = s.event_head;
        }
        if (s.event_head - last > EFB_MAX_EVENTS) {
            std::printf("--- lost %llu events ---\n", (unsigned long long)(s.event_head - last - EFB_MAX_EVENTS));
            last = s.event_head - EFB_MAX_EVENTS;
        }
        for (uint64_t n = last + 1; n <= s.event_head; ++n) {
            const EFB_Event& e = s.events[(n - 1) % EFB_MAX_EVENTS];
            std::printf("#%-6llu %-9s key=%-5u %-44s stacks=%d", (unsigned long long)e.serial, EventName(e.type),
                e.key, e.buff_id, e.enhance_cnt);
            if (e.type == EFB_EVENT_BUFF_FINISHED) std::printf(" reason=%d", e.finish_reason);
            std::printf("\n");
        }
        last = s.event_head;
        Sleep(50);
    }
}
} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, EFB_MAPPING_NAME);
    if (!h) { std::printf("%ls not found (game not running or plugin inactive)\n", EFB_MAPPING_NAME); return 1; }
    g_shared = static_cast<const EFB_Shared*>(MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(EFB_Shared)));
    if (!g_shared) { std::printf("MapViewOfFile failed: %lu\n", GetLastError()); return 1; }
    if (g_shared->magic != EFB_MAGIC || g_shared->layout_version != EFB_LAYOUT_VERSION ||
        g_shared->layout_size != sizeof(EFB_Shared)) {
        std::printf("layout mismatch (version %u, size %u)\n", g_shared->layout_version, g_shared->layout_size);
        return 1;
    }
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "--events") return Events();
    static EFB_Shared s;
    if (mode == "--once") {
        if (!Snapshot(s)) return 1;
        PrintState(s);
        return 0;
    }
    DWORD console = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleMode(out, &console)) SetConsoleMode(out, console | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    for (;;) {
        if (Snapshot(s)) {
            std::printf("\x1b[H\x1b[2J");
            PrintState(s);
        }
        Sleep(250);
    }
}
