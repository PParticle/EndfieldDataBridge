// EndfieldDataBridge shared memory (read-only for consumers), ABI 1.0.
// include/endfield_bridge_reader.h implements every rule below.
//
// Mapping: EFB_MAPPING_NAME, EFB_MAPPING_SIZE bytes reserved, total_size used.
//   HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, EFB_MAPPING_NAME);
//   const void* base = MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0); // whole mapping
//
// Layout: EFB_Header at offset 0; at directory_offset, section_count entries
// of section_entry_size bytes, each starting with EFB_Section.
// Each section is a section header (starting with EFB_SectionHeader) followed
// by `capacity` elements, element i at data_offset + i * stride. Never assume
// an offset, size, stride or capacity: take them from the header/directory.
//
// Consistency: the header (with the directory) and every section have their
// own seqlock `seq`, odd while the writer changes them. To read one:
//   do { s1 = seq; (retry while odd) copy it; s2 = seq; } while (s1 != s2);
// Sections belong to the header they were read under: read the header, then
// the sections you need, then check the header seq is unchanged; if not, a
// session restarted or the layout was rebuilt, so start over. An event that
// names a buff is published in the same write as the buff table change, with
// both sections busy until both are done: after reading events, a buffs read
// includes everything those events describe.
//
// Compatibility within one abi_major (only additive changes, abi_minor++):
//   - Header, section headers, elements and event payloads only grow at the
//     end; copy min(size you know, header_size / stride / payload_size) and
//     treat missing trailing bytes as zero. Fields added later say "since 1.x".
//   - New sections get new ids; skip ids you do not know. A missing section
//     means this writer does not provide that data.
//   - Skip event types you do not know. Ignore flag bits you do not know.
//   - Enum fields (faction, finish reason, life type) may carry new values.
//   - Capacities may change; always use the directory's.
// Anything else (removing, reordering, changing meaning) bumps abi_major and
// with it EFB_MAPPING_NAME, so old and new never share a mapping.
//
// Liveness: the mapping outlives a crashed game while any reader holds it, so
// `active` alone proves nothing. The writer is alive only if a process with
// writer_pid is running and its creation time (GetProcessTimes) equals
// writer_start_time. `active` may change at any time, outside the seqlock.
//
// Freshness: every sampled element has its own sampled_at (0 = not sampled
// yet in this session); treat it as stale when more than ~0.5 s old (battle
// HUD hidden, game paused). Timestamps are QueryPerformanceCounter values;
// seconds = (now - at) / qpc_frequency.
//
// Buff timers follow game time, which slows down or stops in cinematics:
//   elapsed   = min(seconds since sampled_at, 0.25)
//   life_left = max(life_time - elapsed * time_dilation, 0)
//
// Buffs: buffs reported by the squad portraits and enemy head bars. A buff
// keeps its `key` across refreshes within a session; EFB_BuffsHeader.flags
// says when the table may be incomplete.
//
// Events: a ring of EFB_Event; event n (serial, 1-based) sits in element
// (n - 1) % capacity. If that element's serial is not n, it was overwritten:
// re-read the buff table. Adds and finishes appear at most one frame after the
// game reported them (`frame`, `at` = when reported). In-place refreshes and
// stack changes are found when timers are re-read (every sample_interval_ms);
// several stack changes within one interval appear as one BUFF_ENHANCED. A
// buff added and finished within one frame gives BUFF_ADDED + BUFF_FINISHED
// with buff_slot == -1 and never appears in the buff table.
//
// Strings are UTF-8, NUL-terminated, truncated to the field size.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define EFB_MAPPING_NAME L"Local\\EndfieldDataBridge.1" /* the suffix is EFB_ABI_MAJOR */
#define EFB_MAPPING_SIZE (1u << 20)
#define EFB_MAGIC 0x44424645u /* "EFBD" */
#define EFB_ABI_MAJOR 1u
#define EFB_ABI_MINOR 0u
#define EFB_NAME_LEN 64
#define EFB_ID_LEN 96
#define EFB_EVENT_PAYLOAD 128

enum EFB_SectionId {
    EFB_SECTION_TEAM = 1,      // EFB_SectionHeader, 1 x EFB_Team
    EFB_SECTION_OPERATORS = 2, // EFB_SectionHeader, EFB_Operator per squad slot (index = char_index)
    EFB_SECTION_BUFFS = 3,     // EFB_BuffsHeader, EFB_Buff slots (skip key == 0)
    EFB_SECTION_EVENTS = 4,    // EFB_EventsHeader, EFB_Event ring
};
// Beyond.Gameplay.Core.FactionIndex
enum EFB_Faction { EFB_FACTION_INVALID = 0, EFB_FACTION_UNKNOWN = 1, EFB_FACTION_GOOD = 2,
    EFB_FACTION_BAD = 3, EFB_FACTION_NEUTRAL = 4 };
// Beyond.Gameplay.Core.Buff.LifeType
enum EFB_LifeType { EFB_LIFE_LIMITED = 0, EFB_LIFE_INFINITY = 1 };
// Beyond.Gameplay.Core.Buff.FinishReason, plus EFB_FINISH_LOST.
enum EFB_FinishReason { EFB_FINISH_LIFETIME = 0, EFB_FINISH_IGNITE = 1, EFB_FINISH_EARLY = 2,
    EFB_FINISH_DISPELLED = 3, EFB_FINISH_ABSORBED = 4, EFB_FINISH_OTHER = 100,
    EFB_FINISH_LOST = -1 /* vanished without a finish event (owner gone, object reused) */ };
// EFB_Event.type; all four carry EFB_BuffEventPayload.
enum EFB_EventType {
    EFB_EVENT_BUFF_ADDED = 1,     // new key in the buff table
    EFB_EVENT_BUFF_REFRESHED = 2, // timer restarted or duration changed; key unchanged
    EFB_EVENT_BUFF_ENHANCED = 3,  // enhance_cnt (stacks) changed
    EFB_EVENT_BUFF_FINISHED = 4,  // key left the table; finish_reason says why
};
// EFB_BuffsHeader.flags
#define EFB_BUFFS_PARTIAL 0x1u /* tracking began mid-game: buffs added before may be missing */
#define EFB_BUFFS_DROPPED 0x2u /* table was full: `dropped` buffs were never tracked this session */

#pragma pack(push, 8)
typedef struct EFB_Header {
    volatile uint32_t seq;
    uint32_t magic;            // EFB_MAGIC
    uint16_t abi_major;        // EFB_ABI_MAJOR
    uint16_t abi_minor;
    uint32_t header_size;      // bytes of this header as written
    uint32_t total_size;       // bytes in use from the mapping start
    uint32_t directory_offset;
    uint16_t section_count;
    uint16_t section_entry_size; // bytes per directory entry
    uint32_t session;          // changes whenever all sections restart
    uint32_t active;           // 0 while the plugin is disabled; stays 1 if the game crashes
    uint32_t writer_pid;
    uint32_t sample_interval_ms; // Manager setting
    uint32_t reserved;
    uint64_t writer_start_time; // writer process creation time (FILETIME as uint64)
    int64_t qpc_frequency;
    int64_t session_started_at;
} EFB_Header;

typedef struct EFB_Section {
    uint32_t id;               // EFB_SectionId
    uint32_t header_size;      // bytes of the section header
    uint32_t offset;           // section header, from the mapping start
    uint32_t data_offset;      // element 0, from the mapping start
    uint32_t stride;           // bytes per element
    uint32_t capacity;         // elements
} EFB_Section;

typedef struct EFB_SectionHeader {
    volatile uint32_t seq;
    uint32_t reserved;
    int64_t updated_at;        // last write
} EFB_SectionHeader;

typedef struct EFB_BuffsHeader {
    EFB_SectionHeader common;
    uint32_t count;            // live entries
    uint32_t flags;            // EFB_BUFFS_*
    uint32_t dropped;          // buffs never tracked this session
    uint32_t reserved;
} EFB_BuffsHeader;

typedef struct EFB_EventsHeader {
    EFB_SectionHeader common;
    uint64_t head;             // serial of the newest event, 0 = none; never decreases
} EFB_EventsHeader;

// An AbilitySystem as captured when the buff was added.
typedef struct EFB_Unit {
    uint32_t instance_uid;     // 0: unit was already gone
    int8_t char_index;         // squad slot 0-3, -1 = not in the squad (enemy, summon)
    int8_t faction;            // EFB_Faction, -1 = unknown
    uint16_t reserved;
    char entity_name[EFB_NAME_LEN]; // e.g. "chr_0007_ikut_4616825702491504642"
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
    uint64_t serial;           // 1-based
    uint32_t type;             // EFB_EventType
    uint32_t payload_size;     // bytes of payload written for this type
    int32_t frame;             // game frame (UnityEngine.Time.frameCount) the change was seen in
    uint32_t subject_uid;      // AbilitySystem it concerns (buff events: the owner)
    int64_t at;                // when the game reported it
    uint8_t payload[EFB_EVENT_PAYLOAD];
} EFB_Event;

typedef struct EFB_BuffEventPayload {
    uint32_t key;
    int32_t buff_slot;         // element in the buffs section (already cleared for BUFF_FINISHED), -1 = none
    int32_t finish_reason;     // BUFF_FINISHED only, else 0
    int32_t enhance_cnt;
    uint32_t instance_uid;
    uint32_t reserved;
    char buff_id[EFB_ID_LEN];
} EFB_BuffEventPayload;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(EFB_Header) == 72, "EFB_Header layout");
static_assert(sizeof(EFB_Section) == 24, "EFB_Section layout");
static_assert(sizeof(EFB_SectionHeader) == 16, "EFB_SectionHeader layout");
static_assert(sizeof(EFB_BuffsHeader) == 32, "EFB_BuffsHeader layout");
static_assert(sizeof(EFB_EventsHeader) == 24, "EFB_EventsHeader layout");
static_assert(sizeof(EFB_Unit) == 72, "EFB_Unit layout");
static_assert(sizeof(EFB_Team) == 24, "EFB_Team layout");
static_assert(sizeof(EFB_Operator) == 96, "EFB_Operator layout");
static_assert(sizeof(EFB_Buff) == 568, "EFB_Buff layout");
static_assert(offsetof(EFB_Event, payload) == 32 && sizeof(EFB_Event) == 32 + EFB_EVENT_PAYLOAD, "EFB_Event layout");
static_assert(sizeof(EFB_BuffEventPayload) <= EFB_EVENT_PAYLOAD, "EFB_BuffEventPayload fits");
#endif
