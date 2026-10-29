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

#include "../include/endfield_bridge_reader.h"

namespace {
const EFB_Shared* g_shared = nullptr;
EFB_Shared g_state;

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
std::string Flags(const EFB_Shared& s) {
    std::string f;
    if (s.flags & EFB_FLAG_BUFFS_PARTIAL) f += " PARTIAL(buffs from before tracking may be missing)";
    if (s.flags & EFB_FLAG_BUFFS_DROPPED) f += " DROPPED(" + std::to_string(s.buffs_dropped) + ")";
    return f.empty() ? " -" : f;
}

void PrintState(const EFB_Shared& s) {
    const int64_t now = efb::Now();
    const efb::Status status = efb::Check(s, now, efb::WriterAlive(s));
    const bool online = status == efb::Status::Online;
    std::printf("[%s] session=%u pid=%u published %.2fs ago  buffs=%u events=%llu  flags:%s\n\n",
        efb::StatusName(status), s.session, s.writer_pid, efb::SecondsSince(s, s.published_at, now), s.buff_count,
        (unsigned long long)s.event_head, Flags(s).c_str());
    if (status == efb::Status::WriterGone || status == efb::Status::Disabled) return;
    if (s.valid_mask & EFB_VALID_TEAM)
        std::printf("SP %.1f / %.1f  (per cell %d)%s\n\n", s.team.atb, s.team.atb_max, s.team.atb_per_cell,
            efb::TeamFresh(s, now) ? "" : "  [stale]");
    else
        std::printf("SP -\n\n");
    for (int i = 0; i < EFB_MAX_OPERATORS; ++i) {
        const EFB_Operator& o = s.operators[i];
        if (!(s.valid_mask & EFB_VALID_OPERATOR(i))) { std::printf("[%d] (not sampled)\n", i); continue; }
        if (!o.present) { std::printf("[%d] -\n", i); continue; }
        std::printf("[%d]%s %-20s ult %6.1f/%-6.1f combo %5.1f/%-5.1f uid=%u%s\n", i,
            o.is_main_character ? "*" : " ", Short(o.entity_name).c_str(), o.ultimate_sp, o.max_ultimate_sp,
            o.combo_cd_remaining, o.combo_cooldown, o.instance_uid, efb::OperatorFresh(s, i, now) ? "" : "  [stale]");
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
        char life[32];
        const double left = efb::LifeLeft(s, *b, now, online);
        if (left < 0) std::snprintf(life, sizeof(life), "inf");
        else std::snprintf(life, sizeof(life), "%.1f/%.1f", left, b->duration);
        std::printf("%-5u %-20.20s %-44.44s %-14s %4d %5.2f %3d %s\n", b->key, owner.c_str(), b->buff_id, life,
            b->enhance_cnt, b->time_dilation, b->is_debuff, from.c_str());
    }
}

int Events() {
    uint64_t last = 0;
    uint32_t session = 0;
    bool wasAlive = true;
    for (;;) {
        Sleep(50);
        if (!efb::Snapshot(g_shared, g_state)) continue;
        const bool alive = efb::WriterAlive(g_state);
        if (alive != wasAlive) std::printf("--- writer %s ---\n", alive ? "back" : "gone");
        wasAlive = alive;
        if (g_state.session != session) {
            std::printf("--- session %u (re-read the buff table) ---\n", g_state.session);
            session = g_state.session;
            last = g_state.event_head;
        }
        if (g_state.event_head - last > EFB_MAX_EVENTS) {
            std::printf("--- lost %llu events ---\n", (unsigned long long)(g_state.event_head - last - EFB_MAX_EVENTS));
            last = g_state.event_head - EFB_MAX_EVENTS;
        }
        for (uint64_t n = last + 1; n <= g_state.event_head; ++n) {
            const EFB_Event& e = g_state.events[(n - 1) % EFB_MAX_EVENTS];
            std::printf("#%-6llu f=%-7d %-9s key=%-5u %-44s stacks=%d", (unsigned long long)e.serial, e.frame,
                EventName(e.type), e.key, e.buff_id, e.enhance_cnt);
            if (e.type == EFB_EVENT_BUFF_FINISHED) std::printf(" reason=%d", e.finish_reason);
            if (e.buff_slot < 0) std::printf(" (same-frame)");
            std::printf("\n");
        }
        last = g_state.event_head;
    }
}
} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, EFB_MAPPING_NAME);
    if (!h) { std::printf("%ls not found (game not running or plugin inactive)\n", EFB_MAPPING_NAME); return 1; }
    g_shared = static_cast<const EFB_Shared*>(MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(EFB_Shared)));
    if (!g_shared) { std::printf("MapViewOfFile failed: %lu\n", GetLastError()); return 1; }
    if (!efb::Snapshot(g_shared, g_state) || !efb::LayoutMatches(g_state)) {
        std::printf("layout mismatch (version %u, size %u)\n", g_state.layout_version, g_state.layout_size);
        return 1;
    }
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "--events") return Events();
    if (mode == "--once") {
        PrintState(g_state);
        return 0;
    }
    DWORD console = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleMode(out, &console)) SetConsoleMode(out, console | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    for (;;) {
        if (efb::Snapshot(g_shared, g_state)) {
            std::printf("\x1b[H\x1b[2J");
            PrintState(g_state);
        }
        Sleep(250);
    }
}
