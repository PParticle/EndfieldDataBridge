// EndfieldDataBridge shared-memory layout (read-only for consumers).
//
// The plugin creates a named file mapping and the game thread writes the
// latest state into it; no file or socket I/O happens on the game thread.
//
//   HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, EFB_MAPPING_NAME);
//   const EFB_Shared* s = (const EFB_Shared*)MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(EFB_Shared));
//
// Consistency (seqlock): `seq` is odd while the game thread writes.
//   do { s1 = seq; (retry while odd) copy what you need; s2 = seq; } while (s1 != s2);
// Check magic, layout_version and layout_size before trusting anything else.
//
// Time: every *_at field is a QueryPerformanceCounter value of this machine;
// seconds = (now_qpc - x_at) / qpc_frequency.
//
// Buff timers follow game time, which slows down or stops in cinematics. To
// show a smooth countdown between samples:
//   elapsed   = min(seconds since sampled_at, 0.25)
//   life_left = max(life_time - elapsed * time_dilation, 0)
// Stop extrapolating when published_at goes stale (game paused, HUD hidden).
// Treat the data as offline when active == 0 or writer_pid is not running.
//
// Change tracking: `session` changes whenever the table restarts (plugin
// enabled again, game restarted); drop everything cached and re-read. Within a
// session, a buff keeps its `key` across refreshes. Events give the order of
// discrete changes; if event_head advanced by more than EFB_MAX_EVENTS since
// your last read, events were lost: re-read the buff table instead.
#pragma once
#include <stdint.h>

#define EFB_MAPPING_NAME L"Local\\EndfieldDataBridge.v1"
#define EFB_MAGIC 0x31424645u /* "EFB1" */
#define EFB_LAYOUT_VERSION 1u
#define EFB_MAX_OPERATORS 4
#define EFB_MAX_BUFFS 256
#define EFB_MAX_EVENTS 512
#define EFB_NAME_LEN 64
#define EFB_ID_LEN 96

// Beyond.Gameplay.Core.FactionIndex
enum EFB_Faction { EFB_FACTION_INVALID = 0, EFB_FACTION_UNKNOWN = 1, EFB_FACTION_GOOD = 2,
    EFB_FACTION_BAD = 3, EFB_FACTION_NEUTRAL = 4 };
// Beyond.Gameplay.Core.Buff.LifeType
enum EFB_LifeType { EFB_LIFE_LIMITED = 0, EFB_LIFE_INFINITY = 1 };
// Beyond.Gameplay.Core.Buff.FinishReason, plus EFB_FINISH_LOST.
enum EFB_FinishReason { EFB_FINISH_LIFETIME = 0, EFB_FINISH_IGNITE = 1, EFB_FINISH_EARLY = 2,
    EFB_FINISH_DISPELLED = 3, EFB_FINISH_ABSORBED = 4, EFB_FINISH_OTHER = 100,
    EFB_FINISH_LOST = -1 /* vanished without a finish event (owner gone, object reused) */ };
enum EFB_EventType {
    EFB_EVENT_BUFF_ADDED = 1,     // new key in the buff table
    EFB_EVENT_BUFF_REFRESHED = 2, // timer restarted or duration changed; key unchanged
    EFB_EVENT_BUFF_ENHANCED = 3,  // enhance_cnt (stacks) changed
    EFB_EVENT_BUFF_FINISHED = 4,  // key left the table; finish_reason says why
};

#pragma pack(push, 8)
// An AbilitySystem as captured when the buff was added.
typedef struct EFB_Unit {
    uint32_t instance_uid;     // 0: unit was already gone
    int8_t char_index;         // squad slot 0-3, -1 = not in the squad (enemy, summon)
    int8_t faction;            // EFB_Faction, -1 = unknown
    uint16_t reserved;
    char entity_name[EFB_NAME_LEN]; // UTF-8, e.g. "chr_0007_ikut_4616825702491504642"
} EFB_Unit;

typedef struct EFB_Team {
    float atb;                 // current SP
    float atb_max;             // SP cap of the current mode (300, 700, 1000, ...)
    int32_t atb_per_cell;      // SP per skill cell, -1 = unknown
    uint32_t reserved;
    int64_t sampled_at;
} EFB_Team;

typedef struct EFB_Operator {
    uint8_t present;           // slot holds a live character
    uint8_t is_main_character; // currently controlled
    uint16_t reserved0;
    uint32_t instance_uid;
    char entity_name[EFB_NAME_LEN];
    float ultimate_sp;
    float max_ultimate_sp;
    float combo_cd_remaining;  // -1 = no combo skill
    float combo_cooldown;
    int64_t sampled_at;
} EFB_Operator;

typedef struct EFB_Buff {
    uint32_t key;              // 0 = empty slot; stable across refreshes within a session
    uint32_t instance_uid;     // game uid; changes on refresh
    char buff_id[EFB_ID_LEN];  // e.g. "buff_wpn_sword_0012_atk_up"
    char icon_path[EFB_ID_LEN];
    char parent_buff_id[EFB_ID_LEN]; // set for child buffs, "" otherwise
    EFB_Unit owner;            // who carries it
    EFB_Unit source;           // who applied it (child buffs: usually the owner itself)
    EFB_Unit root_source;      // source of the parent buff, else same as source
    int8_t is_debuff;          // 1: root_source faction differs from owner, 0: same, -1: unknown
    uint8_t life_type;         // EFB_LifeType
    uint8_t show_in_squad_icon;
    uint8_t show_in_head_bar;
    uint8_t only_show_for_main_character;
    uint8_t reserved0[3];
    int32_t enhance_cnt;       // stacks
    float duration;            // seconds; meaningless for EFB_LIFE_INFINITY
    float life_time;           // remaining game seconds at sampled_at
    float passed_time;         // elapsed game seconds at sampled_at
    float time_dilation;       // game seconds per real second at sampled_at (0 = frozen)
    uint32_t reserved1;
    int64_t added_at;          // first time this key appeared
    int64_t refreshed_at;      // last BUFF_REFRESHED, else added_at
    int64_t sampled_at;        // when life_time/passed_time were read
} EFB_Buff;

typedef struct EFB_Event {
    uint64_t serial;           // 1-based; ring slot = (serial - 1) % EFB_MAX_EVENTS
    int32_t type;              // EFB_EventType
    int32_t finish_reason;     // BUFF_FINISHED only, else 0
    uint32_t key;
    int32_t buff_slot;         // index into buffs[] (already cleared for BUFF_FINISHED)
    uint32_t owner_uid;
    int32_t enhance_cnt;
    int64_t at;
    char buff_id[EFB_ID_LEN];
} EFB_Event;

typedef struct EFB_Shared {
    volatile uint32_t seq;     // seqlock, odd while writing
    uint32_t magic;            // EFB_MAGIC
    uint32_t layout_version;   // EFB_LAYOUT_VERSION
    uint32_t layout_size;      // sizeof(EFB_Shared)
    uint32_t session;
    uint32_t active;           // 0 while the plugin is disabled or the game is closing
    uint32_t writer_pid;
    uint32_t buff_count;       // live entries in buffs[]
    int64_t qpc_frequency;
    int64_t published_at;      // last write by the game thread
    uint64_t event_head;       // serial of the newest event, 0 = none
    EFB_Team team;
    EFB_Operator operators[EFB_MAX_OPERATORS]; // index = char_index
    EFB_Buff buffs[EFB_MAX_BUFFS];             // unordered; skip key == 0
    EFB_Event events[EFB_MAX_EVENTS];
} EFB_Shared;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(EFB_Unit) == 72, "EFB_Unit layout");
static_assert(sizeof(EFB_Team) == 24, "EFB_Team layout");
static_assert(sizeof(EFB_Operator) == 96, "EFB_Operator layout");
static_assert(sizeof(EFB_Buff) == 568, "EFB_Buff layout");
static_assert(sizeof(EFB_Event) == 136, "EFB_Event layout");
static_assert(sizeof(EFB_Shared) == 215504, "EFB_Shared layout");
#endif
