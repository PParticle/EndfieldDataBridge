#pragma once
// Internal declarations shared by plugin.cpp (Applepie plumbing, IL2CPP,
// hooks, settings), data.cpp (game-thread sampling, buff tracking, shared
// memory) and dump.cpp (debug metadata dump).
#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>

#include "applepie_client.h"
#include "../include/endfield_bridge.h"

#if !defined(_M_X64)
#error EndfieldDataBridge requires an x64 MSVC build.
#endif

namespace bridge {
// ---- Plugin entry ----
extern applepie::Client g_client;
extern APH_RuntimeApi g_runtime;
extern HANDLE g_cancel;
extern wchar_t g_pluginDir[MAX_PATH];

// ---- Debug log ----
void LogF(const char* fmt, ...) noexcept;
void Log(const char* event, uint64_t epoch, APH_Result result) noexcept;
// Probe bodies use only plain C data so the SEH wrappers need no unwinding.
extern std::atomic<long> g_probeLines, g_probeFaults;
extern std::atomic<bool> g_debug;
bool ProbeLogAllowed();
// Opens or closes plugin\EndfieldDataBridge_log.txt. Notifier or Attach thread.
void SetDebug(bool on);

// ---- IL2CPP exports and runtime stop protection ----
extern std::atomic<bool> g_stopping;
extern std::atomic<long> g_inMetadata;
extern ULONGLONG g_deadline;
// IL2CPP exports (resolved from GameAssembly.dll, metadata use only)
#define IL2CPP_EXPORTS(X) \
    X(void*, il2cpp_get_corlib, void) \
    X(void**, il2cpp_thread_get_all_attached_threads, size_t*) \
    X(void*, il2cpp_domain_get, void) \
    X(void*, il2cpp_thread_attach, void*) \
    X(void, il2cpp_thread_detach, void*) \
    X(void**, il2cpp_domain_get_assemblies, void*, size_t*) \
    X(void*, il2cpp_assembly_get_image, void*) \
    X(const char*, il2cpp_image_get_name, void*) \
    X(size_t, il2cpp_image_get_class_count, void*) \
    X(void*, il2cpp_image_get_class, void*, size_t) \
    X(void*, il2cpp_class_from_name, void*, const char*, const char*) \
    X(const char*, il2cpp_class_get_name, void*) \
    X(const char*, il2cpp_class_get_namespace, void*) \
    X(void*, il2cpp_class_get_parent, void*) \
    X(void*, il2cpp_class_get_declaring_type, void*) \
    X(void*, il2cpp_class_get_fields, void*, void**) \
    X(void*, il2cpp_class_get_methods, void*, void**) \
    X(void*, il2cpp_class_get_method_from_name, void*, const char*, int) \
    X(bool, il2cpp_class_is_enum, void*) \
    X(bool, il2cpp_class_is_valuetype, void*) \
    X(int32_t, il2cpp_class_instance_size, void*) \
    X(void*, il2cpp_class_from_type, void*) \
    X(const char*, il2cpp_field_get_name, void*) \
    X(size_t, il2cpp_field_get_offset, void*) \
    X(void*, il2cpp_field_get_type, void*) \
    X(int, il2cpp_field_get_flags, void*) \
    X(void, il2cpp_field_static_get_value, void*, void*) \
    X(const char*, il2cpp_method_get_name, void*) \
    X(uint32_t, il2cpp_method_get_param_count, void*) \
    X(void*, il2cpp_method_get_param, void*, uint32_t) \
    X(const char*, il2cpp_method_get_param_name, void*, uint32_t) \
    X(void*, il2cpp_method_get_return_type, void*) \
    X(uint32_t, il2cpp_method_get_flags, void*, uint32_t*) \
    X(char*, il2cpp_type_get_name, void*) \
    X(int, il2cpp_type_get_type, void*) \
    X(void*, il2cpp_object_get_class, void*) \
    X(int32_t, il2cpp_class_value_size, void*, uint32_t*) \
    X(void*, il2cpp_class_enum_basetype, void*) \
    X(bool, il2cpp_method_is_generic, void*) \
    X(void*, il2cpp_class_get_element_class, void*) \
    X(void, il2cpp_free, void*)

struct Il2Cpp {
#define X(ret, name, ...) ret (*name)(__VA_ARGS__) = nullptr;
    IL2CPP_EXPORTS(X)
#undef X
};
extern Il2Cpp il;
extern HMODULE g_gameAssembly;
bool ResolveIl2Cpp(HMODULE module);
constexpr int FIELD_STATIC = 0x10, FIELD_LITERAL = 0x40;
constexpr uint32_t METHOD_STATIC = 0x10;
// Runtime stop protection (same shared contracts as Manager / CombatHUD)
bool RuntimeOpen();
bool WorkOpen();
std::atomic<bool>& ActiveFlag();
void MarkWriterStopped();
inline void BeginRuntimeStop() {
    g_stopping.store(true);
    ActiveFlag() = false;
    MarkWriterStopped();
    if (g_cancel) SetEvent(g_cancel);
    // Give an in-flight metadata query a bounded chance to leave IL2CPP.
    for (int i = 0; i < 500 && g_inMetadata.load(); ++i) Sleep(1);
}
inline APH_Result APH_CALL ObserveStop(APH_Owner, const APH_SharedEvent* e, void*) {
    if (e->phase == APH_BEFORE) BeginRuntimeStop();
    return APH_OK;
}
template<unsigned Tag, class... Args> struct StopHook {
    inline static applepie::SharedHook hook{};
    inline static applepie::Subscription observer{};
    inline static APH_NativeProc original = nullptr;
    static LONG Fault(EXCEPTION_POINTERS* e, unsigned phase) {
        hook.api()->native_fault(hook.owner(), hook.handle(), e->ExceptionRecord->ExceptionCode,
            (uint64_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress, phase);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    static void Publish(const APH_SharedCall* call, unsigned phase) {
        if (hook.api()->publish(call, phase, nullptr, 0) < 0)
            RaiseException(0xe0411001u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    }
    static void Entry(Args... args) {
        APH_SharedCall call{};
        unsigned phase = APH_ORIGINAL_NOT_ENTERED;
        const APH_Result entered = hook.api()->enter(hook.owner(), hook.handle(), &call);
        __try {
            __try {
                if (entered == APH_OK) Publish(&call, APH_BEFORE);
                BeginRuntimeStop();
                phase = APH_ORIGINAL_RUNNING;
                ((void(*)(Args...))original)(args...);
                phase = APH_ORIGINAL_RETURNED;
                if (entered == APH_OK) Publish(&call, APH_AFTER);
            } __except(Fault(GetExceptionInformation(), phase)) {}
        } __finally { if (entered == APH_OK) hook.api()->leave(&call); }
    }
    static APH_Result Install(APH_NativeProc target, const char* signature, const char* contract) {
        APH_SharedRequest request{};
        APH_Result r = applepie::MakeSharedRequest(request, Tag, target, (APH_NativeProc)Entry,
            signature, contract, 1, 0);
        if (r != APH_OK) return r;
        r = hook.Request(g_client, request);
        if (r < 0) return r;
        if (!observer.handle()) {
            const APH_SharedListen desc{sizeof(desc), APH_BEFORE, APH_MODULE_OBSERVER,
                APH_RECOVERY_NONE, ObserveStop, nullptr};
            r = observer.Listen(hook, desc);
            if (r != APH_OK) return r;
        }
        for (;;) {
            if (!WorkOpen()) return APH_E_REVOKED;
            r = hook.Advance(&original);
            if (r != APH_PENDING && r != APH_E_BUSY) return r;
            WaitForSingleObject(g_cancel, 5);
        }
    }
};
using ShutdownHook = StopHook<100>;
using QuitHook = StopHook<101, void*>;
// Worker thread: waits for IL2CPP to come up.
void* FindClassAny(void** assemblies, size_t count, const char* ns, const char* name);
bool WorkerAttachReady();
bool WaitUntil(bool (*ready)());
bool GameAssemblyLoaded();

// ---- Metadata lookups by name ----
std::string TypeName(void* type);
std::string FullName(void* klass);
// Nested types report an empty namespace; use the outermost declaring type.
const char* OuterNamespace(void* klass);
bool IsBeyond(void* klass);
void* FieldClass(void* klass, const char* name);
int FieldOffset(void* klass, const char* name);
void* FindMethodChecked(void* klass, const char* name, const char* ret,
    std::initializer_list<const char*> params);
// A value type of 1/2/4/8 bytes travels in one register, a bigger one as a
// pointer to a caller copy: either way one non-float slot.
bool OneSlotNonFloat(void* type);
// An instance method by name and parameter count (types checked by the caller).
void* FindMethodArity(void* klass, const char* name, uint32_t params);
bool EnumIsI32(void* type);
// ObjectPtr<Buff> must still be the 16-byte (pointer, uid) struct.
bool BuffRefIs16(void* method);

// ---- Debug metadata dump ----
extern FILE* g_out;
// Frees the detail queue after a dump (worker thread).
void ClearDump();
bool DumpAllGuarded(void** assemblies, size_t count);

// ---- Clocks ----
extern int64_t g_qpcFreq;
int64_t RealQpc();
extern int64_t (*g_clock)();
int64_t QpcNow();
float SecondsBetween(int64_t from, int64_t to);
// Manager setting "sample_interval"; 0 samples every frame (tests only).
extern std::atomic<int> g_sampleIntervalMs;
bool SampleDue(int64_t& last, int64_t now);
extern void* g_mFrameCount;
extern int32_t (*g_frameClock)();
int32_t FrameNow();

// ---- Game object access ----
extern std::atomic<DWORD> g_uiThread;
struct ObjPtr { void* object; uint32_t uid; uint32_t padding; };
static_assert(sizeof(ObjPtr) == 16, "ObjectPtr<T> ABI");
// Offsets resolved by name at install time; -1 = unavailable.
struct Offsets {
    int buffId = -1, buffDuration = -1, buffLifeTime = -1, buffPassed = -1, buffEnhance = -1,
        buffOwner = -1, buffSource = -1, buffUid = -1, buffLifeType = -1, buffData = -1,
        buffFinished = -1, buffParent = -1;
    int dataIcon = -1, iconPath = -1, iconSquad = -1, iconHead = -1, iconMainOnly = -1;
    int asEntity = -1, asUid = -1, asUltSp = -1, asDanger = -1, asSkills = -1;
    int sqIndex = -1, sqMain = -1, sqAbility = -1, sqEmpty = -1;
    int bmAtb = -1, bmAtbMax = -1, setPerCell = -1;
    int skillType = -1, skillOwner = -1, skillId = -1, entName = -1, hbTarget = -1;
    // AbilitySystem.m_poiseController -> PoiseController.m_poiseRecoverTimer -> PeriodicTimer
    int asPoiseCtl = -1, pcRecoverTimer = -1, ptRemaining = -1;
};
extern Offsets off;
extern void* g_mGetBattleManager;
extern void* g_mGetSkillSetting;
extern void* g_mMaxUltSp;
extern void* g_mCdRemaining;
extern void* g_mCooldown;
extern void* g_mFaction;
extern void* g_mSkillCastId;
extern void* g_mSkillInstId;
extern void* g_mIsCasting;
template<class T> T At(void* obj, int offset) { return *reinterpret_cast<T*>(static_cast<char*>(obj) + offset); }
void ReadManagedString(void* str, char* out, int size);
const char* ClassNameOf(void* obj);
// ObjectPtr<AbilitySystem> is valid only while the target still carries its UID.
bool AbilityPtrLive(const ObjPtr& p);
void NoteThread(const char* who);
// Call a validated 0-arg instance getter through its native pointer.
template<class R> R CallGetter(void* method, void* self) {
    using Fn = R (*)(void*, void*);
    return reinterpret_cast<Fn>(*reinterpret_cast<void**>(method))(self, method);
}
void EntityName(void* ability, char* out, int size);
int FactionOf(void* ability);
// Unit getters on AbilitySystem; any may be missing, then the field stays 0.
extern void* g_mHp;
extern void* g_mMaxHp;
extern void* g_mPoise;
extern void* g_mMaxPoise;
extern void* g_mHasPoise;
extern void* g_mZeroPoise;
extern void* g_mInPoiseRecovery;
extern void* g_mPoiseProgress;
extern void* g_mPoiseRecTime;
extern void* g_mSumPoiseRecTime;
template<class R> R GetOr(void* method, void* self, R fallback) { return method ? CallGetter<R>(method, self) : fallback; }
bool ResolveProbeOffsets(void** asms, size_t count);
// ---- Shared memory (include/endfield_bridge.h) ----
// This writer's layout of ABI 1.0. Readers find everything through the
// directory, so capacities and the order of sections may change in a minor
// version. Each section has its own seqlock; Write() opens only the ones a
// change touches. Writers: the game thread while probes are active; the
// worker only between sessions.
constexpr uint32_t OPERATOR_SLOTS = 4, BUFF_SLOTS = 256, EVENT_SLOTS = 512, UNIT_SLOTS = 64;
struct Layout {
    EFB_Header header;
    EFB_Section directory[5];
    alignas(8) EFB_SectionHeader teamHead;
    EFB_Team team;
    alignas(8) EFB_SectionHeader opsHead;
    EFB_Operator ops[OPERATOR_SLOTS];
    alignas(8) EFB_BuffsHeader buffsHead;
    EFB_Buff buffs[BUFF_SLOTS];
    alignas(8) EFB_EventsHeader eventsHead;
    EFB_Event events[EVENT_SLOTS];
    alignas(8) EFB_UnitsHeader unitsHead;
    EFB_UnitState units[UNIT_SLOTS];
};
static_assert(sizeof(Layout) <= EFB_MAPPING_SIZE, "layout fits the reserved mapping");
extern Layout* g_layout;
extern const wchar_t* g_mappingName;
extern SRWLOCK g_dataLock;
inline void OpenSeq(volatile uint32_t& seq) { seq |= 1; }
inline void CloseSeq(volatile uint32_t& seq) { seq += 1; }
enum : unsigned { W_HEADER = 1, W_TEAM = 2, W_OPS = 4, W_BUFFS = 8, W_EVENTS = 16, W_UNITS = 32 };
// Runs fn under the data lock with the selected sections marked busy; with
// none selected it only serializes private state. All selected sections are
// busy before fn changes any of them, so a change spanning buffs and events
// is seen by readers as a whole. The header closes last.
template<class Fn> void Write(unsigned sections, Fn fn) {
    Layout& l = *g_layout;
    AcquireSRWLockExclusive(&g_dataLock);
    if (sections & W_HEADER) OpenSeq(l.header.seq);
    if (sections & W_TEAM) OpenSeq(l.teamHead.seq);
    if (sections & W_OPS) OpenSeq(l.opsHead.seq);
    if (sections & W_BUFFS) OpenSeq(l.buffsHead.common.seq);
    if (sections & W_EVENTS) OpenSeq(l.eventsHead.common.seq);
    if (sections & W_UNITS) OpenSeq(l.unitsHead.common.seq);
    MemoryBarrier();
    __try { fn(); }
    __finally {
        const int64_t now = QpcNow();
        if (sections & W_TEAM) l.teamHead.updated_at = now;
        if (sections & W_OPS) l.opsHead.updated_at = now;
        if (sections & W_BUFFS) l.buffsHead.common.updated_at = now;
        if (sections & W_EVENTS) l.eventsHead.common.updated_at = now;
        if (sections & W_UNITS) l.unitsHead.common.updated_at = now;
        MemoryBarrier();
        if (sections & W_TEAM) CloseSeq(l.teamHead.seq);
        if (sections & W_OPS) CloseSeq(l.opsHead.seq);
        if (sections & W_BUFFS) CloseSeq(l.buffsHead.common.seq);
        if (sections & W_EVENTS) CloseSeq(l.eventsHead.common.seq);
        if (sections & W_UNITS) CloseSeq(l.unitsHead.common.seq);
        MemoryBarrier();
        if (sections & W_HEADER) CloseSeq(l.header.seq);
        ReleaseSRWLockExclusive(&g_dataLock);
    }
}
bool OpenShared();
// Stop path: may run on any thread while the game thread is inside Write().
void MarkWriterStopped();
// ---- Units: the squad and every unit with an enemy head bar ----
// Sampled from SquadIcon.LateTick and UIHeadBar.UpdateData at the sample
// interval. A unit leaves when its object goes away, or when nothing has
// shown it for 60 HUD frames and 1 s (the head bar was removed).
struct UnitTrack {
    void* ability;              // valid only while its instance uid == state.instance_uid
    uint32_t lastSeenFrame;     // g_hudFrames when a probe last saw it
    int64_t lastSeenAt;
    int64_t sampledAt;          // sample interval throttle
    ULONGLONG loggedAt;
    EFB_UnitState state;
};
extern UnitTrack g_units[UNIT_SLOTS];
extern uint32_t g_hudFrames;
extern uint32_t g_provides;
// Game thread. charIndex -1 = not a squad slot.
void SampleUnit(const ObjPtr& ability, int charIndex, bool onHeadBar);
// Inside Write(W_UNITS).
void MirrorUnits(int64_t now);
void ProbeHeadBar(void* bar);
// ---- SP via AtbNode.LateTick -> BattleManager ----
extern int64_t g_teamSampledAt;
void ProbeAtb(void* node);
// ---- Squad slots via SquadIcon.LateTick: ultimate energy + combo CD ----
struct SquadSlot { void* ability; uint32_t uid; };
extern SquadSlot g_squad[OPERATOR_SLOTS];
extern int64_t g_opSampledAt[OPERATOR_SLOTS];
struct SlotLog { ULONGLONG at; float ult, cd; bool main; };
extern SlotLog g_slotLog[OPERATOR_SLOTS];
int SquadIndexOf(void* ability);
// AbilitySystem.m_skills is List<Skill>: items array at +0x10, size at +0x18.
void* FindSkill(void* ability, int type);
void ProbeSquad(void* icon);

// ---- Buff table ----
enum TrackState : uint8_t { TRACK_FREE, TRACK_NEW, TRACK_LIVE, TRACK_FINISHED };
struct TrackedBuff {
    TrackState state;
    int32_t addFrame, finishFrame;
    int64_t addAt, finishAt;   // when the UI reported it
    int32_t reason;            // FinishReason, EFB_FINISH_LOST when none came
    void* buff;                // valid only while buff->instanceUid == info.instance_uid
    void* ownerAbility;
    int64_t finishedSince;     // first frame m_isFinished was seen without a finish report
    int64_t rateAt;            // time_dilation baseline
    float ratePassed;
    char parentClass[48];      // debug: non-buff parent root
    EFB_Buff info;             // what gets published
};
extern TrackedBuff g_tracked[BUFF_SLOTS];
extern bool g_buffsDirty;
extern int32_t g_settledFrame, g_tickedFrame;
extern void* g_buffClass;
// Recently finished or dropped uids: a finish for one of these is a duplicate
// from another panel, not evidence of a missed add.
extern uint32_t g_recentUids[128];
// Mirrors the private table into the buffs section. Inside Write(W_BUFFS).
void MirrorBuffs();
// Settles every add/finish reported before `frame`. Inside Write(W_BUFFS | W_EVENTS).
void SettleBuffs(int32_t frame);
// Re-read of every live buff, once per sample interval. Inside Write(W_BUFFS | W_EVENTS).
void SampleBuffs(int32_t frame, int64_t now);
// UI buff callbacks (game thread): record the change at the moment the game
// reports it; settlement happens on the next frame.
void ProbeBuff(const char* via, ObjPtr ref, bool applied, int32_t reason);

// ---- Skill casts ----
extern uint32_t g_castsSeen[10];
// After AbilitySystem.BeforeCastStart(skill, target): the skill is starting.
void ProbeCastStart(void* skill);
// Skill.CastEnd, before the original, while the skill still reports itself casting.
void ProbeCastEnd(void* skill, int32_t finishType, int32_t interruptReason);

// ---- Per-frame tick and sessions ----
// HUD LateTick hooks (game thread): once per frame.
extern uint32_t g_sessionsStarted;
// Adds and finishes settle every frame; timers and stacks follow the interval.
extern int64_t g_buffsSampledAt;
void Tick();
// Worker thread, before probes open: a fresh session with empty tables.
void StartSession();
// Restore: probes are drained, so no game-thread write is in flight.
void EndSession();

// ---- Hooks ----
// Runtime probes (read-only observers; each calls the original exactly once)
extern std::atomic<bool> g_active;
std::atomic<bool>& ActiveFlag();
extern bool g_probesOk;
template<class Fn> void Guarded(Fn fn) {
    __try { fn(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_probeFaults.fetch_add(1) < 20) LogF("[FAULT] probe exception 0x%08lX", GetExceptionCode());
    }
}
applepie::NativeOptions ProbeOptions();
template<class H, class Fn> APH_Result InstallProbe(void* method, uint64_t key, Fn body,
    const char* signature, const char* label) {
    if (!method) return APH_E_SIGNATURE;
    APH_Result r = H::Request(g_client, key, *reinterpret_cast<Fn*>(method), body, signature, label,
        ProbeOptions());
    while (r == APH_PENDING || r == APH_E_BUSY) {
        if (!WorkOpen()) return APH_E_REVOKED;
        r = H::Advance();
        if (r == APH_PENDING || r == APH_E_BUSY) WaitForSingleObject(g_cancel, 5);
    }
    LogF("probe %s: %ld", label, static_cast<long>(r));
    return r;
}
APH_Result InstallProbes(void** asms, size_t count);

// ---- Manager settings ----
// Optional: an older Manager without basic settings simply leaves debug off.
APH_Result RegisterConfig();
} // namespace bridge
