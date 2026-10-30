// Game-thread side: clocks, game object reads, the shared memory layout, and
// the SP / squad / buff / unit / skill cast trackers that fill it.
#include "bridge.h"

#include <cstring>

namespace bridge {
// ---- Clocks ----
int64_t g_qpcFreq = 1;
int64_t RealQpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
int64_t (*g_clock)() = RealQpc;
int64_t QpcNow() { return g_clock(); }
float SecondsBetween(int64_t from, int64_t to) { return float(double(to - from) / double(g_qpcFreq)); }
std::atomic<int> g_sampleIntervalMs{50};
bool SampleDue(int64_t& last, int64_t now) {
    if (last && double(now - last) * 1000 < double(g_sampleIntervalMs.load()) * double(g_qpcFreq)) return false;
    last = now;
    return true;
}
void* g_mFrameCount = nullptr;
int32_t UnityFrame() {
    using Fn = int32_t (*)(void*);
    if (g_mFrameCount) return reinterpret_cast<Fn>(*reinterpret_cast<void**>(g_mFrameCount))(g_mFrameCount);
    return int32_t(GetTickCount64() / 16); // fallback: ~one frame at 60 fps
}
int32_t (*g_frameClock)() = UnityFrame;
int32_t FrameNow() { return g_frameClock(); }

// ---- Game object access ----
std::atomic<DWORD> g_uiThread{0};
Offsets off;
void* g_mGetBattleManager = nullptr;
void* g_mGetSkillSetting = nullptr;
void* g_mMaxUltSp = nullptr;
void* g_mCdRemaining = nullptr;
void* g_mCooldown = nullptr;
void* g_mFaction = nullptr;
void* g_mSkillCastId = nullptr;
void* g_mSkillInstId = nullptr;
void* g_mIsCasting = nullptr;
void ReadManagedString(void* str, char* out, int size) {
    out[0] = 0;
    if (!str) return;
    const int32_t len = At<int32_t>(str, 0x10);
    if (len <= 0 || len > 512) return;
    const int n = WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<wchar_t*>(static_cast<char*>(str) + 0x14),
        len, out, size - 1, nullptr, nullptr);
    out[n > 0 ? n : 0] = 0;
}
const char* ClassNameOf(void* obj) {
    void* k = obj ? il.il2cpp_object_get_class(obj) : nullptr;
    const char* n = k ? il.il2cpp_class_get_name(k) : nullptr;
    return n ? n : "-";
}
bool AbilityPtrLive(const ObjPtr& p) {
    return p.object && off.asUid >= 0 && At<uint32_t>(p.object, off.asUid) == p.uid;
}
void NoteThread(const char* who) {
    const DWORD t = GetCurrentThreadId();
    DWORD expected = 0;
    if (!g_uiThread.compare_exchange_strong(expected, t) && expected != t && ProbeLogAllowed())
        LogF("[THREAD] %s on thread %lu, first probe thread %lu", who, t, expected);
}
void EntityName(void* ability, char* out, int size) {
    out[0] = 0;
    void* entity = ability && off.asEntity >= 0 ? At<void*>(ability, off.asEntity) : nullptr;
    if (entity && off.entName >= 0) ReadManagedString(At<void*>(entity, off.entName), out, size);
}
int FactionOf(void* ability) {
    return ability && g_mFaction ? CallGetter<int32_t>(g_mFaction, ability) : -1;
}
void* g_mHp = nullptr;
void* g_mMaxHp = nullptr;
void* g_mPoise = nullptr;
void* g_mMaxPoise = nullptr;
void* g_mHasPoise = nullptr;
void* g_mZeroPoise = nullptr;
void* g_mInPoiseRecovery = nullptr;
void* g_mPoiseProgress = nullptr;
void* g_mPoiseRecTime = nullptr;
void* g_mSumPoiseRecTime = nullptr;
bool ResolveProbeOffsets(void** asms, size_t count) {
    void* buff = FindClassAny(asms, count, "Beyond.Gameplay.Core", "Buff");
    void* buffData = FindClassAny(asms, count, "Beyond.Gameplay.Core", "BuffData");
    void* iconCfg = FindClassAny(asms, count, "Beyond.Gameplay.Core", "BuffIconConfig");
    void* as = FindClassAny(asms, count, "Beyond.Gameplay.Core", "AbilitySystem");
    void* bm = FindClassAny(asms, count, "Beyond.Gameplay.Core", "BattleManager");
    void* skill = FindClassAny(asms, count, "Beyond.Gameplay.Core", "Skill");
    void* entity = FindClassAny(asms, count, "Beyond.Gameplay.Core", "Entity");
    void* setting = FindClassAny(asms, count, "Beyond.Gameplay", "SkillSetting");
    void* squad = FindClassAny(asms, count, "Beyond.UI", "SquadIcon");
    void* node = FindClassAny(asms, count, "Beyond.UI", "AtbNode");
    void* headBar = FindClassAny(asms, count, "Beyond.UI", "UIHeadBar");
    if (!buff || !buffData || !iconCfg || !as || !bm || !skill || !entity || !setting || !squad ||
        !node || !headBar) {
        LogF("[ABI] probe class missing");
        return false;
    }
    off.buffId = FieldOffset(buff, "<id>k__BackingField");
    off.buffDuration = FieldOffset(buff, "m_duration");
    off.buffLifeTime = FieldOffset(buff, "m_lifeTime");
    off.buffPassed = FieldOffset(buff, "m_passedTime");
    off.buffEnhance = FieldOffset(buff, "m_enhanceCnt");
    off.buffOwner = FieldOffset(buff, "<owner>k__BackingField");
    off.buffSource = FieldOffset(buff, "<source>k__BackingField");
    off.buffUid = FieldOffset(buff, "<instanceUid>k__BackingField");
    off.buffLifeType = FieldOffset(buff, "m_lifeType");
    off.buffData = FieldOffset(buff, "m_data");
    off.buffFinished = FieldOffset(buff, "m_isFinished");
    off.buffParent = FieldOffset(buff, "m_parentBuffRoot");
    g_buffClass = buff;
    off.dataIcon = FieldOffset(buffData, "iconConfig");
    off.iconPath = FieldOffset(iconCfg, "_spritePath");
    off.iconSquad = FieldOffset(iconCfg, "showInSquadIcon");
    off.iconHead = FieldOffset(iconCfg, "showInHeadBarCommon");
    off.iconMainOnly = FieldOffset(iconCfg, "onlyShowForMainCharacter");
    off.asEntity = FieldOffset(as, "<entity>k__BackingField");
    off.asUid = FieldOffset(as, "<instanceUid>k__BackingField");
    off.asUltSp = FieldOffset(as, "m_ultimateSp");
    off.asDanger = FieldOffset(as, "<isDangerousEnemy>k__BackingField");
    off.asSkills = FieldOffset(as, "m_skills");
    off.skillType = FieldOffset(skill, "<skillType>k__BackingField");
    off.skillOwner = FieldOffset(skill, "m_owner");
    off.skillId = FieldOffset(skill, "m_skillId");
    off.entName = FieldOffset(entity, "<name>k__BackingField");
    off.sqIndex = FieldOffset(squad, "m_charIndex");
    off.sqMain = FieldOffset(squad, "m_isMainCharacter");
    off.sqAbility = FieldOffset(squad, "m_charAbilityPtr");
    off.sqEmpty = FieldOffset(squad, "<isEmpty>k__BackingField");
    off.hbTarget = FieldOffset(headBar, "m_target");
    // Poise recovery countdown (optional): PoiseController.m_poiseRecoverTimer.m_remainingTime.
    void* poiseCtl = FieldClass(as, "m_poiseController");
    void* recoverTimer = poiseCtl ? FieldClass(poiseCtl, "m_poiseRecoverTimer") : nullptr;
    off.asPoiseCtl = FieldOffset(as, "m_poiseController");
    off.pcRecoverTimer = poiseCtl ? FieldOffset(poiseCtl, "m_poiseRecoverTimer") : -1;
    off.ptRemaining = recoverTimer ? FieldOffset(recoverTimer, "m_remainingTime") : -1;
    off.bmAtb = FieldOffset(bm, "m_atb");
    off.bmAtbMax = FieldOffset(bm, "<atbMax>k__BackingField");
    off.setPerCell = FieldOffset(setting, "atbPerCell");
    g_mGetBattleManager = FindMethodChecked(node, "get_battleManager", "Beyond.Gameplay.Core.BattleManager", {});
    g_mGetSkillSetting = FindMethodChecked(node, "get_skillSetting", "Beyond.Gameplay.SkillSetting", {});
    g_mMaxUltSp = FindMethodChecked(as, "get_maxUltimateSp", "System.Single", {});
    g_mCdRemaining = FindMethodChecked(skill, "get_cdRemainingTime", "System.Single", {});
    g_mCooldown = FindMethodChecked(skill, "get_cooldown", "System.Single", {});
    g_mFaction = FindMethodChecked(as, "get_factionIndex", "Beyond.Gameplay.Core.FactionIndex", {});
    if (g_mFaction && !EnumIsI32(il.il2cpp_method_get_return_type(g_mFaction))) g_mFaction = nullptr;
    // Units (all optional: a missing getter leaves its field 0).
    g_mHp = FindMethodChecked(as, "get_hp", "System.Double", {});
    g_mMaxHp = FindMethodChecked(as, "get_maxHp", "System.Double", {});
    g_mPoise = FindMethodChecked(as, "get_poise", "System.Single", {});
    g_mMaxPoise = FindMethodChecked(as, "get_maxPoise", "System.Single", {});
    g_mHasPoise = FindMethodChecked(as, "get_hasPoise", "System.Boolean", {});
    g_mZeroPoise = FindMethodChecked(as, "get_isZeroPoise", "System.Boolean", {});
    g_mInPoiseRecovery = FindMethodChecked(as, "get_inPoiseRecovery", "System.Boolean", {});
    g_mPoiseProgress = FindMethodChecked(as, "get_poiseRecoveryProgress", "System.Single", {});
    g_mPoiseRecTime = FindMethodChecked(as, "get_poiseRecTime", "System.Single", {});
    g_mSumPoiseRecTime = FindMethodChecked(as, "get_sumPoiseRecTime", "System.Single", {});
    // Skill casts (optional together with their hooks).
    g_mSkillCastId = FindMethodChecked(skill, "get_skillCastId", "System.UInt32", {});
    g_mSkillInstId = FindMethodChecked(skill, "get_instId", "System.UInt64", {});
    g_mIsCasting = FindMethodChecked(skill, "get_isCasting", "System.Boolean", {});
    // Static icall: int32_t(MethodInfo*). Pairs a refresh's DEL and ADD by frame.
    void* time = FindClassAny(asms, count, "UnityEngine", "Time");
    void* fc = time ? il.il2cpp_class_get_method_from_name(time, "get_frameCount", 0) : nullptr;
    uint32_t impl = 0;
    if (fc && *reinterpret_cast<void**>(fc) && (il.il2cpp_method_get_flags(fc, &impl) & METHOD_STATIC) &&
        TypeName(il.il2cpp_method_get_return_type(fc)) == "System.Int32")
        g_mFrameCount = fc;
    else
        LogF("[ABI] UnityEngine.Time.get_frameCount unavailable, pairing refreshes by 16 ms ticks");
    // Required: everything the hot paths dereference without a check.
    const int required[] = {off.buffId, off.buffDuration, off.buffLifeTime, off.buffPassed, off.buffEnhance,
        off.buffOwner, off.buffSource, off.buffUid, off.buffLifeType, off.buffData, off.iconPath,
        off.iconSquad, off.iconHead, off.iconMainOnly, off.asUid, off.asUltSp, off.sqIndex, off.sqAbility,
        off.bmAtb, off.bmAtbMax};
    for (int v : required) if (v < 0) return false;
    return g_mGetBattleManager != nullptr;
}

// ---- Shared memory ----
HANDLE g_mapping = nullptr;
Layout* g_layout = nullptr;
const wchar_t* g_mappingName = EFB_MAPPING_NAME; // tests use a private name
SRWLOCK g_dataLock = SRWLOCK_INIT;
uint64_t FileTimeValue(const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; }
uint64_t ProcessStartTime() {
    FILETIME created{}, exited{}, kernel{}, user{};
    return GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) ? FileTimeValue(created) : 0;
}
template<class T> EFB_Section Describe(uint32_t id, const void* head, uint32_t headSize, const T* data,
    uint32_t capacity) {
    const char* base = reinterpret_cast<const char*>(g_layout);
    return EFB_Section{id, headSize, uint32_t(static_cast<const char*>(head) - base),
        uint32_t(reinterpret_cast<const char*>(data) - base), uint32_t(sizeof(T)), capacity};
}
// Builds the layout. The header seq is odd throughout, so no reader can
// complete a read of the old content or of the half-built new one.
void InitLayout(uint32_t session) {
    Layout& l = *g_layout;
    const uint32_t seq = l.header.seq | 1;
    l.header.seq = seq;
    MemoryBarrier();
    memset(reinterpret_cast<char*>(&l) + sizeof(uint32_t), 0, EFB_MAPPING_SIZE - sizeof(uint32_t));
    EFB_Header& h = l.header;
    h.magic = EFB_MAGIC;
    h.abi_major = EFB_ABI_MAJOR;
    h.abi_minor = EFB_ABI_MINOR;
    h.header_size = sizeof(EFB_Header);
    h.total_size = sizeof(Layout);
    h.directory_offset = uint32_t(offsetof(Layout, directory));
    h.section_count = 5;
    h.section_entry_size = sizeof(EFB_Section);
    h.session = session;
    h.writer_pid = GetCurrentProcessId();
    h.writer_start_time = ProcessStartTime();
    h.qpc_frequency = g_qpcFreq;
    h.sample_interval_ms = 50;
    l.directory[0] = Describe(EFB_SECTION_TEAM, &l.teamHead, sizeof(l.teamHead), &l.team, 1);
    l.directory[1] = Describe(EFB_SECTION_OPERATORS, &l.opsHead, sizeof(l.opsHead), l.ops, OPERATOR_SLOTS);
    l.directory[2] = Describe(EFB_SECTION_BUFFS, &l.buffsHead, sizeof(l.buffsHead), l.buffs, BUFF_SLOTS);
    l.directory[3] = Describe(EFB_SECTION_EVENTS, &l.eventsHead, sizeof(l.eventsHead), l.events, EVENT_SLOTS);
    l.directory[4] = Describe(EFB_SECTION_UNITS, &l.unitsHead, sizeof(l.unitsHead), l.units, UNIT_SLOTS);
    MemoryBarrier();
    l.header.seq = seq + 1;
}
bool OpenShared() {
    if (g_layout) return true;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcFreq = f.QuadPart;
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, EFB_MAPPING_SIZE, g_mappingName);
    if (!g_mapping) { LogF("[SHM] CreateFileMapping failed: %lu", GetLastError()); return false; }
    // Exists when a reader still holds the mapping of a game that has exited.
    const bool reused = GetLastError() == ERROR_ALREADY_EXISTS;
    g_layout = static_cast<Layout*>(MapViewOfFile(g_mapping, FILE_MAP_WRITE, 0, 0, EFB_MAPPING_SIZE));
    if (!g_layout) {
        LogF("[SHM] MapViewOfFile failed: %lu", GetLastError());
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return false;
    }
    // New content is a new session, also for a reader that outlived the old writer.
    const EFB_Header& old = g_layout->header;
    const bool ours = reused && old.magic == EFB_MAGIC && old.abi_major == EFB_ABI_MAJOR;
    InitLayout(ours ? old.session + 1 : 0);
    LogF("[SHM] %ls ready (%u bytes%s)", g_mappingName, unsigned(sizeof(Layout)), reused ? ", reused" : "");
    return true;
}
void MarkWriterStopped() {
    if (g_layout) InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_layout->header.active), 0);
}

// ---- SP and squad slots ----
int64_t g_teamSampledAt = 0;
ULONGLONG g_lastAtbLog = 0;
float g_lastAtb = -1, g_lastAtbMax = -1;
void ProbeAtb(void* node) {
    NoteThread("AtbNode.LateTick");
    const int64_t now = QpcNow();
    if (!SampleDue(g_teamSampledAt, now)) return;
    void* bm = CallGetter<void*>(g_mGetBattleManager, node);
    if (!bm) return;
    void* setting = g_mGetSkillSetting ? CallGetter<void*>(g_mGetSkillSetting, node) : nullptr;
    EFB_Team team{};
    team.atb = At<float>(bm, off.bmAtb);
    team.atb_max = At<float>(bm, off.bmAtbMax);
    team.atb_per_cell = setting && off.setPerCell >= 0 ? At<int32_t>(setting, off.setPerCell) : -1;
    team.sampled_at = now;
    Write(W_TEAM, [&] { g_layout->team = team; });
    const ULONGLONG tick = GetTickCount64();
    const bool changed = team.atb - g_lastAtb > 0.5f || g_lastAtb - team.atb > 0.5f || team.atb_max != g_lastAtbMax;
    if (!(changed && tick - g_lastAtbLog >= 200) && tick - g_lastAtbLog < 5000) return;
    g_lastAtb = team.atb; g_lastAtbMax = team.atb_max; g_lastAtbLog = tick;
    if (ProbeLogAllowed())
        LogF("[ATB] t=%llu atb=%.2f max=%.2f perCell=%d", tick, team.atb, team.atb_max, team.atb_per_cell);
}
SquadSlot g_squad[OPERATOR_SLOTS]{};
int64_t g_opSampledAt[OPERATOR_SLOTS]{};
SlotLog g_slotLog[OPERATOR_SLOTS]{};
int SquadIndexOf(void* ability) {
    for (int i = 0; ability && i < OPERATOR_SLOTS; ++i)
        if (g_squad[i].ability == ability) return i;
    return -1;
}
void* FindSkill(void* ability, int type) {
    if (off.asSkills < 0 || off.skillType < 0) return nullptr;
    void* list = At<void*>(ability, off.asSkills);
    void* items = list ? At<void*>(list, 0x10) : nullptr;
    const int32_t size = list ? At<int32_t>(list, 0x18) : 0;
    if (!items || size <= 0 || size > 64) return nullptr;
    for (int i = 0; i < size; ++i) {
        void* skill = At<void*>(items, 0x20 + i * 8);
        if (skill && At<int32_t>(skill, off.skillType) == type) return skill;
    }
    return nullptr;
}
void ProbeSquad(void* icon) {
    NoteThread("SquadIcon.LateTick");
    const int index = At<int32_t>(icon, off.sqIndex);
    if (index < 0 || index >= OPERATOR_SLOTS) return;
    const int64_t now = QpcNow();
    if (!SampleDue(g_opSampledAt[index], now)) return;
    const ObjPtr ability = At<ObjPtr>(icon, off.sqAbility);
    const bool empty = off.sqEmpty >= 0 && At<bool>(icon, off.sqEmpty);
    const bool main = off.sqMain >= 0 && At<bool>(icon, off.sqMain);
    const bool live = !empty && AbilityPtrLive(ability);
    g_squad[index] = {live ? ability.object : nullptr, live ? ability.uid : 0};
    EFB_Operator op{};
    op.present = live;
    op.is_main_character = main;
    op.instance_uid = g_squad[index].uid;
    op.combo_cd_remaining = op.combo_cooldown = -1;
    op.sampled_at = now;
    if (live) {
        EntityName(ability.object, op.entity_name, sizeof(op.entity_name));
        op.ultimate_sp = At<float>(ability.object, off.asUltSp);
        op.max_ultimate_sp = g_mMaxUltSp ? CallGetter<float>(g_mMaxUltSp, ability.object) : -1;
        void* combo = FindSkill(ability.object, 6); // SkillType.ComboSkill
        if (combo && g_mCdRemaining) op.combo_cd_remaining = CallGetter<float>(g_mCdRemaining, combo);
        if (combo && g_mCooldown) op.combo_cooldown = CallGetter<float>(g_mCooldown, combo);
    }
    Write(W_OPS, [&] { g_layout->ops[index] = op; });
    if (live) SampleUnit(ability, index, false);
    SlotLog& last = g_slotLog[index];
    const ULONGLONG tick = GetTickCount64();
    const float ult = op.ultimate_sp, cd = op.combo_cd_remaining;
    const bool changed = main != last.main || ult - last.ult > 1 || last.ult - ult > 1 ||
        (cd <= 0) != (last.cd <= 0) || cd - last.cd > 1 || last.cd - cd > 1;
    if (!(changed && tick - last.at >= 250) && tick - last.at < 5000) return;
    last = {tick, ult, cd, main};
    if (ProbeLogAllowed())
        LogF("[SQUAD] t=%llu slot=%d main=%d name=%s ult=%.2f/%.2f cd=%.2f/%.2f", tick, index, int(main),
            op.entity_name, ult, op.max_ultimate_sp, cd, op.combo_cooldown);
}
// ---- Buff table: buffs seen through squad portraits and enemy head bars ----
// Identity, owner and source are captured once on add; timers and stacks are
// re-read from the live Buff object every sample interval, so consumers never
// run their own clocks. An add or finish reported by the UI is settled at the
// first hook call of the next frame, because the game refreshes some buffs
// as finish(old uid) + add(new uid) in one frame, in either order; that pair
// becomes one BUFF_REFRESHED and keeps the key. Other buffs restart in place
// (passed_time drops), which the per-frame read reports the same way.
TrackedBuff g_tracked[BUFF_SLOTS]{};
uint32_t g_nextKey = 1;
bool g_buffsDirty = false;     // a NEW or FINISHED entry waits for settlement
int32_t g_settledFrame = INT32_MIN, g_tickedFrame = INT32_MIN;
void* g_buffClass = nullptr;
bool IsBuff(void* obj) {
    for (void* k = obj ? il.il2cpp_object_get_class(obj) : nullptr; k; k = il.il2cpp_class_get_parent(k))
        if (k == g_buffClass) return true;
    return false;
}
uint32_t g_recentUids[128]{};
unsigned g_recentNext = 0;
void RememberUid(uint32_t uid) { g_recentUids[g_recentNext++ % 128] = uid; }
bool IsRecentUid(uint32_t uid) {
    for (uint32_t u : g_recentUids) if (u == uid) return true;
    return false;
}
void CaptureUnit(EFB_Unit& u, const ObjPtr& p) {
    u = EFB_Unit{};
    u.char_index = -1;
    u.faction = -1;
    if (!AbilityPtrLive(p)) return;
    u.instance_uid = p.uid;
    u.char_index = int8_t(SquadIndexOf(p.object));
    u.faction = int8_t(FactionOf(p.object));
    EntityName(p.object, u.entity_name, sizeof(u.entity_name));
}
// False once the Buff object no longer carries this uid.
bool ReadTimers(TrackedBuff& t) {
    if (At<uint32_t>(t.buff, off.buffUid) != t.info.instance_uid) return false;
    t.info.duration = At<float>(t.buff, off.buffDuration);
    t.info.life_time = At<float>(t.buff, off.buffLifeTime); // FLT_MAX for LifeType.Infinity
    t.info.passed_time = At<float>(t.buff, off.buffPassed);
    t.info.enhance_cnt = At<int32_t>(t.buff, off.buffEnhance);
    t.info.life_type = At<uint8_t>(t.buff, off.buffLifeType);
    return true;
}
void CaptureBuff(TrackedBuff& t, void* buff, uint32_t uid, int64_t now) {
    EFB_Buff& b = t.info;
    t.buff = buff;
    b.instance_uid = uid;
    ReadManagedString(At<void*>(buff, off.buffId), b.buff_id, sizeof(b.buff_id));
    void* data = At<void*>(buff, off.buffData);
    void* cfg = data && off.dataIcon >= 0 ? At<void*>(data, off.dataIcon) : nullptr;
    if (cfg) {
        ReadManagedString(At<void*>(cfg, off.iconPath), b.icon_path, sizeof(b.icon_path));
        b.show_in_squad_icon = At<bool>(cfg, off.iconSquad);
        b.show_in_head_bar = At<bool>(cfg, off.iconHead);
        b.only_show_for_main_character = At<bool>(cfg, off.iconMainOnly);
    }
    const ObjPtr owner = At<ObjPtr>(buff, off.buffOwner);
    CaptureUnit(b.owner, owner);
    t.ownerAbility = b.owner.instance_uid ? owner.object : nullptr;
    CaptureUnit(b.source, At<ObjPtr>(buff, off.buffSource));
    b.root_source = b.source;
    // Child buffs name the owner as source; the parent root tells who started it.
    void* parent = off.buffParent >= 0 ? At<void*>(buff, off.buffParent) : nullptr;
    if (parent && IsBuff(parent)) {
        ReadManagedString(At<void*>(parent, off.buffId), b.parent_buff_id, sizeof(b.parent_buff_id));
        CaptureUnit(b.root_source, At<ObjPtr>(parent, off.buffSource));
    } else if (parent) {
        std::snprintf(t.parentClass, sizeof(t.parentClass), "%s", ClassNameOf(parent));
    }
    const int8_t of = b.owner.faction, rf = b.root_source.faction;
    b.is_debuff = of < 0 || rf < 0 ? -1 : int8_t(of != rf);
    ReadTimers(t);
    b.time_dilation = 1;
    b.sampled_at = now;
}
void LogBuff(const char* tag, const TrackedBuff& t, int32_t frame) {
    if (!ProbeLogAllowed()) return;
    const EFB_Buff& b = t.info;
    LogF("[BUFF%s] t=%llu frame=%d key=%u uid=%u id=%s owner=%s(%d f=%d) source=%s(%d f=%d) root=%s(%d f=%d) "
        "parent=%s%s debuff=%d life=%.2f/%.2f passed=%.2f dil=%.2f stacks=%d lifeType=%u reason=%d icon=%s",
        tag, GetTickCount64(), frame, b.key, b.instance_uid, b.buff_id,
        b.owner.entity_name, b.owner.char_index, b.owner.faction,
        b.source.entity_name, b.source.char_index, b.source.faction,
        b.root_source.entity_name, b.root_source.char_index, b.root_source.faction,
        b.parent_buff_id[0] ? b.parent_buff_id : "-", t.parentClass, b.is_debuff,
        b.life_type == EFB_LIFE_INFINITY ? -1.0f : b.life_time, b.duration, b.passed_time, b.time_dilation,
        b.enhance_cnt, unsigned(b.life_type), t.reason, b.icon_path);
}
TrackedBuff* FindTracked(void* buff, uint32_t uid) {
    for (auto& t : g_tracked)
        if (t.state != TRACK_FREE && t.buff == buff && t.info.instance_uid == uid) return &t;
    return nullptr;
}
int SlotOf(const TrackedBuff& t) { return int(&t - g_tracked); }
uint32_t NextKey() {
    if (!g_nextKey) g_nextKey = 1; // 0 marks an empty slot
    return g_nextKey++;
}
bool SameBuff(const EFB_Buff& a, const EFB_Buff& b) {
    return a.owner.instance_uid == b.owner.instance_uid && a.source.instance_uid == b.source.instance_uid &&
        !strcmp(a.buff_id, b.buff_id);
}
const char* EventTag(int type) {
    switch (type) {
    case EFB_EVENT_BUFF_ADDED: return "+";
    case EFB_EVENT_BUFF_REFRESHED: return "~";
    case EFB_EVENT_BUFF_ENHANCED: return "^";
    default: return "-";
    }
}
// Inside Write(W_EVENTS).
void PushEvent(int type, const TrackedBuff& t, int64_t at, int32_t frame, int slot) {
    const uint64_t serial = ++g_layout->eventsHead.head;
    EFB_Event& e = g_layout->events[(serial - 1) % EVENT_SLOTS];
    e = EFB_Event{};
    e.serial = serial;
    e.type = uint32_t(type);
    e.payload_size = sizeof(EFB_BuffEventPayload);
    e.frame = frame;
    e.subject_uid = t.info.owner.instance_uid;
    e.at = at;
    EFB_BuffEventPayload p{};
    p.key = t.info.key;
    p.buff_slot = slot;
    p.finish_reason = type == EFB_EVENT_BUFF_FINISHED ? t.reason : 0;
    p.enhance_cnt = t.info.enhance_cnt;
    p.instance_uid = t.info.instance_uid;
    memcpy(p.buff_id, t.info.buff_id, sizeof(p.buff_id));
    memcpy(e.payload, &p, sizeof(p));
    LogBuff(EventTag(type), t, frame);
}
void MirrorBuffs() {
    uint32_t count = 0;
    for (int i = 0; i < BUFF_SLOTS; ++i) {
        const TrackedBuff& t = g_tracked[i];
        // A pending finish stays visible until its frame settles, so a
        // finish+add refresh never blinks out of the table.
        if (t.state == TRACK_LIVE || (t.state == TRACK_FINISHED && t.info.key)) {
            g_layout->buffs[i] = t.info;
            ++count;
        } else if (g_layout->buffs[i].key) {
            g_layout->buffs[i] = EFB_Buff{};
        }
    }
    g_layout->buffsHead.count = count;
}
void SettleBuffs(int32_t frame) {
    bool pending = false;
    for (auto& n : g_tracked) {
        if (n.state != TRACK_NEW) continue;
        if (n.addFrame >= frame) { pending = true; continue; }
        TrackedBuff* old = nullptr;
        for (auto& e : g_tracked)
            if (e.state == TRACK_FINISHED && e.info.key && e.finishFrame == n.addFrame && SameBuff(e.info, n.info)) {
                old = &e;
                break;
            }
        n.state = TRACK_LIVE;
        if (old) {
            n.info.key = old->info.key;
            n.info.added_at = old->info.added_at;
            n.info.refreshed_at = n.addAt;
            RememberUid(old->info.instance_uid);
            *old = TrackedBuff{};
            PushEvent(EFB_EVENT_BUFF_REFRESHED, n, n.addAt, n.addFrame, SlotOf(n));
        } else {
            n.info.key = NextKey();
            n.info.added_at = n.info.refreshed_at = n.addAt;
            PushEvent(EFB_EVENT_BUFF_ADDED, n, n.addAt, n.addFrame, SlotOf(n));
        }
    }
    for (auto& e : g_tracked) {
        if (e.state != TRACK_FINISHED) continue;
        if (e.finishFrame >= frame) { pending = true; continue; }
        if (!e.info.key) {
            // Added and finished within one frame: never entered buffs[].
            e.info.key = NextKey();
            e.info.added_at = e.info.refreshed_at = e.addAt;
            PushEvent(EFB_EVENT_BUFF_ADDED, e, e.addAt, e.addFrame, -1);
            PushEvent(EFB_EVENT_BUFF_FINISHED, e, e.finishAt, e.finishFrame, -1);
        } else {
            PushEvent(EFB_EVENT_BUFF_FINISHED, e, e.finishAt, e.finishFrame, SlotOf(e));
        }
        RememberUid(e.info.instance_uid);
        e = TrackedBuff{};
    }
    g_buffsDirty = pending;
}
// False once the buff is gone without a finish report.
bool SampleTracked(TrackedBuff& t, int64_t now) {
    __try {
        if (!ReadTimers(t)) return false; // object reused
        if (t.ownerAbility && At<uint32_t>(t.ownerAbility, off.asUid) != t.info.owner.instance_uid) return false;
        if (off.buffFinished < 0 || !At<bool>(t.buff, off.buffFinished)) { t.finishedSince = 0; return true; }
        // A finished buff normally reports its finish within the frame; give it time.
        if (!t.finishedSince) t.finishedSince = now;
        return SecondsBetween(t.finishedSince, now) < 0.5f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Game seconds per real second, measured over at least 0.1 s.
void UpdateDilation(TrackedBuff& t, int64_t now) {
    if (!t.rateAt) { t.rateAt = now; t.ratePassed = t.info.passed_time; return; }
    const float wall = SecondsBetween(t.rateAt, now);
    if (wall < 0.1f) return;
    float d = (t.info.passed_time - t.ratePassed) / wall;
    if (d < 0.05f) d = 0;
    else if (d > 0.95f && d < 1.05f) d = 1;
    else if (d > 1.5f) d = 1; // hitch: keep the nominal rate
    t.info.time_dilation = d;
    t.rateAt = now;
    t.ratePassed = t.info.passed_time;
}
void SampleBuffs(int32_t frame, int64_t now) {
    for (auto& t : g_tracked) {
        if (t.state != TRACK_LIVE) continue;
        const float passed = t.info.passed_time, duration = t.info.duration;
        const int32_t stacks = t.info.enhance_cnt;
        if (!SampleTracked(t, now)) {
            t.state = TRACK_FINISHED;
            t.finishFrame = frame;
            t.finishAt = now;
            t.reason = EFB_FINISH_LOST;
            g_buffsDirty = true;
            continue;
        }
        t.info.sampled_at = now;
        if (t.info.passed_time < passed - 0.05f || t.info.duration != duration) {
            t.info.refreshed_at = now;
            t.rateAt = now;
            t.ratePassed = t.info.passed_time;
            PushEvent(EFB_EVENT_BUFF_REFRESHED, t, now, frame, SlotOf(t));
        } else {
            UpdateDilation(t, now);
        }
        if (t.info.enhance_cnt != stacks) PushEvent(EFB_EVENT_BUFF_ENHANCED, t, now, frame, SlotOf(t));
    }
}
void ProbeBuff(const char* via, ObjPtr ref, bool applied, int32_t reason) {
    NoteThread(via);
    void* buff = ref.object;
    if (!buff || At<uint32_t>(buff, off.buffUid) != ref.uid) return; // stale head-bar replay
    const int32_t frame = FrameNow();
    const int64_t now = QpcNow();
    // Recording a change is private; sections open only when this call publishes.
    const bool settle = g_buffsDirty && frame > g_settledFrame;
    uint32_t flags = 0;
    Write(settle ? W_BUFFS | W_EVENTS : 0u, [&] {
        // Events may name buff slots, so the table follows every settlement.
        if (settle) {
            SettleBuffs(frame);
            g_settledFrame = frame;
            MirrorBuffs();
        }
        TrackedBuff* t = FindTracked(buff, ref.uid);
        if (!applied) {
            if (!t) {
                // A finish for a buff never seen added: tracking began after it.
                if (!IsRecentUid(ref.uid)) flags |= EFB_BUFFS_PARTIAL;
                return;
            }
            if (t->state == TRACK_FINISHED) return; // another panel already reported it
            ReadTimers(*t);
            t->state = TRACK_FINISHED;
            t->finishFrame = frame;
            t->finishAt = now;
            t->reason = reason;
            g_buffsDirty = true;
            return;
        }
        if (t) return; // the same buff reported by another panel
        for (auto& slot : g_tracked) if (slot.state == TRACK_FREE) { t = &slot; break; }
        if (!t) {
            flags |= EFB_BUFFS_DROPPED;
            RememberUid(ref.uid);
            if (ProbeLogAllowed()) LogF("[BUFF] table full, dropped uid=%u", ref.uid);
            return;
        }
        *t = TrackedBuff{};
        CaptureBuff(*t, buff, ref.uid, now);
        t->state = TRACK_NEW;
        t->addFrame = frame;
        t->addAt = now;
        t->reason = EFB_FINISH_LOST;
        g_buffsDirty = true;
    });
    if (flags) Write(W_BUFFS, [&] {
        g_layout->buffsHead.flags |= flags;
        if (flags & EFB_BUFFS_DROPPED) ++g_layout->buffsHead.dropped;
    });
}

// ---- Units ----
UnitTrack g_units[UNIT_SLOTS]{};
uint32_t g_hudFrames = 0;       // Tick() calls: frames in which the battle HUD ran
uint32_t g_provides = 0;        // EFB_PROVIDES_*, set when optional probes install
// False when the object faulted while being read.
bool ReadUnitStats(void* ability, EFB_UnitState& u) {
    __try {
        u.hp = GetOr<double>(g_mHp, ability, 0);
        u.max_hp = GetOr<double>(g_mMaxHp, ability, 0);
        u.has_poise = GetOr<bool>(g_mHasPoise, ability, false);
        u.poise = GetOr<float>(g_mPoise, ability, 0);
        u.max_poise = GetOr<float>(g_mMaxPoise, ability, 0);
        u.is_zero_poise = GetOr<bool>(g_mZeroPoise, ability, false);
        u.in_poise_recovery = GetOr<bool>(g_mInPoiseRecovery, ability, false);
        u.poise_recovery_progress = GetOr<float>(g_mPoiseProgress, ability, 0);
        u.poise_rec_time = GetOr<float>(g_mPoiseRecTime, ability, 0);
        u.sum_poise_rec_time = GetOr<float>(g_mSumPoiseRecTime, ability, 0);
        u.poise_recovery_left = -1;
        if (u.in_poise_recovery && off.asPoiseCtl >= 0 && off.pcRecoverTimer >= 0 && off.ptRemaining >= 0) {
            void* ctl = At<void*>(ability, off.asPoiseCtl);
            void* timer = ctl ? At<void*>(ctl, off.pcRecoverTimer) : nullptr;
            if (timer) u.poise_recovery_left = At<float>(timer, off.ptRemaining);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void SampleUnit(const ObjPtr& ability, int charIndex, bool onHeadBar) {
    if (!AbilityPtrLive(ability)) return;
    const int64_t now = QpcNow();
    Write(0, [&] {
        UnitTrack* t = nullptr;
        UnitTrack* free = nullptr;
        for (auto& u : g_units) {
            if (u.ability == ability.object && u.state.instance_uid == ability.uid) { t = &u; break; }
            if (!free && !u.state.instance_uid) free = &u;
        }
        if (!t) {
            if (!free) return; // more units than slots: the rest wait for a free one
            t = free;
            *t = UnitTrack{};
            t->ability = ability.object;
            t->state.instance_uid = ability.uid;
            t->state.char_index = -1;
            t->state.faction = int8_t(FactionOf(ability.object));
            EntityName(ability.object, t->state.entity_name, sizeof(t->state.entity_name));
        }
        t->lastSeenFrame = g_hudFrames;
        t->lastSeenAt = now;
        t->state.char_index = int8_t(charIndex);
        if (onHeadBar) t->state.on_head_bar = 1;
        if (!SampleDue(t->sampledAt, now)) return;
        EFB_UnitState s = t->state;
        if (!ReadUnitStats(ability.object, s)) return;
        s.sampled_at = now;
        const bool poiseChanged = s.is_zero_poise != t->state.is_zero_poise ||
            s.in_poise_recovery != t->state.in_poise_recovery;
        t->state = s;
        const ULONGLONG tick = GetTickCount64();
        if ((poiseChanged || tick - t->loggedAt >= 2000) && ProbeLogAllowed()) {
            t->loggedAt = tick;
            LogF("[UNIT] t=%llu uid=%u slot=%d head=%d name=%s hp=%.1f/%.1f poise=%.2f/%.2f has=%d zero=%d rec=%d "
                "progress=%.3f recTime=%.3f sumRecTime=%.3f left=%.2f", tick, s.instance_uid, s.char_index,
                s.on_head_bar, s.entity_name, s.hp, s.max_hp, s.poise, s.max_poise, s.has_poise, s.is_zero_poise,
                s.in_poise_recovery, s.poise_recovery_progress, s.poise_rec_time, s.sum_poise_rec_time,
                s.poise_recovery_left);
        }
    });
}
bool UnitGone(const UnitTrack& u, int64_t now) {
    __try {
        if (At<uint32_t>(u.ability, off.asUid) != u.state.instance_uid) return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
    return g_hudFrames - u.lastSeenFrame > 60 && SecondsBetween(u.lastSeenAt, now) > 1.0f;
}
void MirrorUnits(int64_t now) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < UNIT_SLOTS; ++i) {
        UnitTrack& u = g_units[i];
        if (u.state.instance_uid && UnitGone(u, now)) u = UnitTrack{};
        if (u.state.instance_uid && u.state.sampled_at) {
            g_layout->units[i] = u.state;
            ++count;
        } else if (g_layout->units[i].instance_uid) {
            g_layout->units[i] = EFB_UnitState{};
        }
    }
    g_layout->unitsHead.count = count;
}
void ProbeHeadBar(void* bar) {
    NoteThread("UIHeadBar.UpdateData");
    const ObjPtr target = At<ObjPtr>(bar, off.hbTarget);
    SampleUnit(target, SquadIndexOf(target.object), true);
}
// ---- Skill casts: AbilitySystem.BeforeCastStart / Skill.CastEnd ----
int32_t SkillTypeOf(void* skill) { return At<int32_t>(skill, off.skillType); }
// Normal attacks and dodges are not reported (EFB_SkillType).
bool ReportedSkill(int32_t type) {
    return type != EFB_SKILL_ATTACK && type != EFB_SKILL_BREAKING_ATTACK && type != EFB_SKILL_DODGE &&
        type != EFB_SKILL_PASSIVE;
}
uint32_t g_castsSeen[10]{};     // debug: casts per SkillType + 1
// Squad members and units with an enemy head bar; projectiles, summons and
// other helper units cast their own skills, which are not reported.
bool OnHeadBar(const ObjPtr& p) {
    for (const auto& u : g_units)
        if (u.ability == p.object && u.state.instance_uid == p.uid) return u.state.on_head_bar != 0;
    return false;
}
void PushSkillEvent(uint32_t type, void* skill, int32_t finishType, int32_t interruptReason) {
    const ObjPtr owner = At<ObjPtr>(skill, off.skillOwner);
    const bool live = AbilityPtrLive(owner);
    if (!live || (SquadIndexOf(owner.object) < 0 && !OnHeadBar(owner))) return;
    EFB_SkillEventPayload p{};
    p.caster_uid = owner.uid;
    p.char_index = int8_t(SquadIndexOf(owner.object));
    p.faction = int8_t(FactionOf(owner.object));
    p.skill_type = SkillTypeOf(skill);
    p.skill_cast_id = GetOr<uint32_t>(g_mSkillCastId, skill, 0);
    p.skill_inst_id = GetOr<uint64_t>(g_mSkillInstId, skill, 0);
    p.finish_type = finishType;
    p.interrupt_reason = interruptReason;
    if (off.skillId >= 0) ReadManagedString(At<void*>(skill, off.skillId), p.skill_id, sizeof(p.skill_id));
    const int32_t frame = FrameNow();
    const int64_t now = QpcNow();
    Write(W_EVENTS, [&] {
        const uint64_t serial = ++g_layout->eventsHead.head;
        EFB_Event& e = g_layout->events[(serial - 1) % EVENT_SLOTS];
        e = EFB_Event{};
        e.serial = serial;
        e.type = type;
        e.payload_size = sizeof(EFB_SkillEventPayload);
        e.frame = frame;
        e.subject_uid = p.caster_uid;
        e.at = now;
        memcpy(e.payload, &p, sizeof(p));
    });
    if (ProbeLogAllowed())
        LogF("[CAST%s] t=%llu frame=%d caster=%u slot=%d f=%d type=%d id=%s castId=%u inst=%llu finish=%d interrupt=%d",
            type == EFB_EVENT_SKILL_CAST_STARTED ? "+" : "-", GetTickCount64(), frame, p.caster_uid, p.char_index,
            p.faction, p.skill_type, p.skill_id, p.skill_cast_id, (unsigned long long)p.skill_inst_id, finishType,
            interruptReason);
}
void ProbeCastStart(void* skill) {
    if (!(g_provides & EFB_PROVIDES_SKILL_CASTS)) return;
    NoteThread("AbilitySystem.BeforeCastStart");
    if (!skill) return;
    const int32_t type = SkillTypeOf(skill);
    if (type >= -1 && type < 9) ++g_castsSeen[type + 1];
    if (!ReportedSkill(type)) return;
    PushSkillEvent(EFB_EVENT_SKILL_CAST_STARTED, skill, 0, 0);
}
void ProbeCastEnd(void* skill, int32_t finishType, int32_t interruptReason) {
    if (!(g_provides & EFB_PROVIDES_SKILL_CASTS)) return;
    if (!ReportedSkill(SkillTypeOf(skill)) || !CallGetter<bool>(g_mIsCasting, skill)) return;
    PushSkillEvent(EFB_EVENT_SKILL_CAST_ENDED, skill, finishType, interruptReason);
}

// ---- Per-frame tick and sessions ----
uint32_t g_sessionsStarted = 0;
int64_t g_buffsSampledAt = 0;
void Tick() {
    const int32_t frame = FrameNow();
    if (frame == g_tickedFrame) return;
    g_tickedFrame = frame;
    ++g_hudFrames;
    const int64_t now = QpcNow();
    const bool settle = g_buffsDirty && frame > g_settledFrame;
    const bool sample = SampleDue(g_buffsSampledAt, now);
    if (!settle && !sample) return;
    const uint32_t interval = uint32_t(g_sampleIntervalMs.load());
    const bool header = g_layout->header.sample_interval_ms != interval;
    Write(W_BUFFS | W_EVENTS | (header ? W_HEADER : 0u) | (sample ? W_UNITS : 0u), [&] {
        if (header) g_layout->header.sample_interval_ms = interval;
        if (settle) {
            SettleBuffs(frame);
            g_settledFrame = frame;
        }
        if (sample) {
            SampleBuffs(frame, now);
            MirrorUnits(now);
        }
        MirrorBuffs();
    });
}
void StartSession() {
    for (auto& t : g_tracked) t = TrackedBuff{};
    for (auto& s : g_squad) s = SquadSlot{};
    for (auto& l : g_slotLog) l = SlotLog{};
    memset(g_opSampledAt, 0, sizeof(g_opSampledAt));
    memset(g_recentUids, 0, sizeof(g_recentUids));
    g_teamSampledAt = g_buffsSampledAt = 0;
    g_buffsDirty = false;
    g_settledFrame = g_tickedFrame = INT32_MIN;
    for (auto& u : g_units) u = UnitTrack{};
    memset(g_castsSeen, 0, sizeof(g_castsSeen));
    g_hudFrames = 0;
    g_uiThread = 0;
    Write(W_HEADER | W_TEAM | W_OPS | W_BUFFS | W_EVENTS | W_UNITS, [&] {
        Layout& l = *g_layout;
        ++l.header.session;
        l.header.session_started_at = QpcNow();
        l.header.sample_interval_ms = uint32_t(g_sampleIntervalMs.load());
        l.team = EFB_Team{};
        memset(l.ops, 0, sizeof(l.ops));
        memset(l.buffs, 0, sizeof(l.buffs));
        l.buffsHead.count = l.buffsHead.dropped = 0;
        memset(l.units, 0, sizeof(l.units));
        l.unitsHead.count = 0;
        l.header.provides = g_provides;
        // A re-enable starts in the middle of the game.
        l.buffsHead.flags = g_sessionsStarted++ ? EFB_BUFFS_PARTIAL : 0;
        l.header.active = 1;
    });
    LogF("[SHM] session %u started", g_layout->header.session);
}
void EndSession() {
    if (g_layout) Write(W_HEADER, [] { g_layout->header.active = 0; });
}
} // namespace bridge
