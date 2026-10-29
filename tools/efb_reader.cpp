// Test consumer for the EndfieldDataBridge shared memory.
//   efb_reader          live view of SP, operators and buffs
//   efb_reader --once   print one snapshot and exit
//   efb_reader --events print buff events as they happen
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "../include/endfield_bridge_reader.h"

namespace {
efb::Mapping g_map;

// "chr_0007_ikut_4616825702491504642" -> "chr_0007_ikut"
std::string Short(const char* name) {
    std::string n = name[0] ? name : "-";
    const size_t cut = n.find_last_of('_');
    if (cut != std::string::npos && n.size() - cut > 8 &&
        n.find_first_not_of("0123456789", cut + 1) == std::string::npos)
        n.resize(cut);
    return n;
}
const char* EventName(uint32_t type) {
    switch (type) {
    case EFB_EVENT_BUFF_ADDED: return "ADDED";
    case EFB_EVENT_BUFF_REFRESHED: return "REFRESHED";
    case EFB_EVENT_BUFF_ENHANCED: return "ENHANCED";
    case EFB_EVENT_BUFF_FINISHED: return "FINISHED";
    default: return nullptr;
    }
}

// Everything the live view shows, read under one header seq.
struct State {
    efb::Header header;
    EFB_Team team{};
    std::vector<EFB_Operator> ops;
    EFB_BuffsHeader buffsHead{};
    std::vector<EFB_Buff> buffs;
};
bool Read(State& st) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (efb::ReadHeader(g_map, st.header) != efb::Result::Ok) { Sleep(1); continue; }
        efb::Section team, ops, buffs;
        if (efb::ReadSection(g_map, st.header, EFB_SECTION_TEAM, team) != efb::Result::Ok ||
            efb::ReadSection(g_map, st.header, EFB_SECTION_OPERATORS, ops) != efb::Result::Ok ||
            efb::ReadSection(g_map, st.header, EFB_SECTION_BUFFS, buffs) != efb::Result::Ok)
            continue;
        st.team = team.capacity() ? team.Element<EFB_Team>(0) : EFB_Team{};
        st.ops.clear();
        for (uint32_t i = 0; i < ops.capacity(); ++i) st.ops.push_back(ops.Element<EFB_Operator>(i));
        st.buffsHead = buffs.Head<EFB_BuffsHeader>();
        st.buffs.clear();
        for (uint32_t i = 0; i < buffs.capacity(); ++i) {
            const EFB_Buff b = buffs.Element<EFB_Buff>(i);
            if (b.key) st.buffs.push_back(b);
        }
        return true;
    }
    return false;
}

void Print(const State& st) {
    const EFB_Header& h = st.header.fields;
    const int64_t now = efb::Now();
    int64_t newest = st.team.sampled_at;
    for (const auto& o : st.ops) newest = std::max(newest, o.sampled_at);
    const efb::Status status = efb::Check(h, efb::WriterAlive(h), newest, now);
    std::string flags;
    if (st.buffsHead.flags & EFB_BUFFS_PARTIAL) flags += " PARTIAL";
    if (st.buffsHead.flags & EFB_BUFFS_DROPPED) flags += " DROPPED(" + std::to_string(st.buffsHead.dropped) + ")";
    std::printf("[%s] ABI %u.%u session=%u pid=%u interval=%ums buffs=%u flags:%s\n\n", efb::StatusName(status),
        h.abi_major, h.abi_minor, h.session, h.writer_pid, h.sample_interval_ms, st.buffsHead.count,
        flags.empty() ? " -" : flags.c_str());
    if (status == efb::Status::WriterGone || status == efb::Status::Disabled) return;
    if (st.team.sampled_at)
        std::printf("SP %.1f / %.1f  (per cell %d)%s\n\n", st.team.atb, st.team.atb_max, st.team.atb_per_cell,
            efb::Fresh(h, st.team.sampled_at, now) ? "" : "  [stale]");
    else
        std::printf("SP -\n\n");
    for (size_t i = 0; i < st.ops.size(); ++i) {
        const EFB_Operator& o = st.ops[i];
        if (!o.sampled_at) { std::printf("[%zu] (not sampled)\n", i); continue; }
        if (!o.present) { std::printf("[%zu] -\n", i); continue; }
        std::printf("[%zu]%s %-20s ult %6.1f/%-6.1f combo %5.1f/%-5.1f uid=%u%s\n", i,
            o.is_main_character ? "*" : " ", Short(o.entity_name).c_str(), o.ultimate_sp, o.max_ultimate_sp,
            o.combo_cd_remaining, o.combo_cooldown, o.instance_uid, efb::Fresh(h, o.sampled_at, now) ? "" : "  [stale]");
    }
    std::vector<const EFB_Buff*> buffs;
    for (const auto& b : st.buffs) buffs.push_back(&b);
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
        const double left = efb::LifeLeft(h, *b, now, status == efb::Status::Online);
        if (left < 0) std::snprintf(life, sizeof(life), "inf");
        else std::snprintf(life, sizeof(life), "%.1f/%.1f", left, b->duration);
        std::printf("%-5u %-20.20s %-44.44s %-14s %4d %5.2f %3d %s\n", b->key, owner.c_str(), b->buff_id, life,
            b->enhance_cnt, b->time_dilation, b->is_debuff, from.c_str());
    }
}

int Events() {
    efb::EventCursor cursor;
    efb::Header header;
    bool wasAlive = true;
    uint32_t session = 0;
    for (;;) {
        Sleep(50);
        if (efb::ReadHeader(g_map, header) != efb::Result::Ok) continue;
        const bool alive = efb::WriterAlive(header.fields);
        if (alive != wasAlive) std::printf("--- writer %s ---\n", alive ? "back" : "gone");
        wasAlive = alive;
        efb::EventBatch batch;
        if (cursor.Poll(g_map, header, batch) != efb::Result::Ok) continue;
        if (batch.resync) {
            if (header.fields.session != session) std::printf("--- session %u ---\n", header.fields.session);
            if (batch.lost) std::printf("--- lost %llu events ---\n", (unsigned long long)batch.lost);
            std::printf("--- re-read the buff table ---\n");
            session = header.fields.session;
        }
        for (const EFB_Event& e : batch.events) {
            const char* name = EventName(e.type);
            if (!name) continue; // a type from a newer writer
            const auto p = efb::Payload<EFB_BuffEventPayload>(e);
            std::printf("#%-6llu f=%-7d %-9s key=%-5u %-44s stacks=%d", (unsigned long long)e.serial, e.frame, name,
                p.key, p.buff_id, p.enhance_cnt);
            if (e.type == EFB_EVENT_BUFF_FINISHED) std::printf(" reason=%d", p.finish_reason);
            if (p.buff_slot < 0) std::printf(" (same-frame)");
            std::printf("\n");
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (!g_map.Open()) { std::printf("%ls not found (game not running or plugin inactive)\n", EFB_MAPPING_NAME); return 1; }
    efb::Header header;
    const efb::Result r = efb::ReadHeader(g_map, header);
    if (r == efb::Result::Incompatible) { std::printf("not an EndfieldDataBridge ABI %u mapping\n", EFB_ABI_MAJOR); return 1; }
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "--events") return Events();
    State st;
    if (mode == "--once") {
        if (!Read(st)) return 1;
        Print(st);
        return 0;
    }
    DWORD console = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleMode(out, &console)) SetConsoleMode(out, console | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    for (;;) {
        if (Read(st)) {
            std::printf("\x1b[H\x1b[2J");
            Print(st);
        }
        Sleep(250);
    }
}
