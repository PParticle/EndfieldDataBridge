// Read-only observer for SP, squad ultimate energy / combo CD and buffs.
// UI hooks sample on the game thread and publish into shared memory
// (include/endfield_bridge.h). The Manager "debug" setting enables
// plugin\EndfieldDataBridge_log.txt and, at enable, the metadata dump;
// with it off the plugin writes no files.
//
// This file: debug log, IL2CPP exports and stop protection, metadata lookups,
// hook installation, Manager settings, plugin entry points.
#include "bridge.h"

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace bridge {
// ---- Debug log ----
SRWLOCK g_logLock = SRWLOCK_INIT;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
void LogF(const char* fmt, ...) noexcept {
    char line[1024]{};
    int n = std::snprintf(line, sizeof(line), "[EndfieldDataBridge] ");
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line + n, sizeof(line) - n - 2, fmt, args);
    va_end(args);
    strcat_s(line, "\n");
    OutputDebugStringA(line);
    AcquireSRWLockExclusive(&g_logLock);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_logFile, line, DWORD(strlen(line)), &written, nullptr);
    }
    ReleaseSRWLockExclusive(&g_logLock);
}
void Log(const char* event, uint64_t epoch, APH_Result result) noexcept {
    LogF("%s owner=%llu epoch=%llu result=%ld", event,
        static_cast<unsigned long long>(g_client.owner()),
        static_cast<unsigned long long>(epoch), static_cast<long>(result));
}
std::atomic<long> g_probeLines{0}, g_probeFaults{0};
constexpr long PROBE_LINE_BUDGET = 60000;
std::atomic<bool> g_debug{false};
bool ProbeLogAllowed() { return g_debug.load() && g_probeLines.fetch_add(1) < PROBE_LINE_BUDGET; }
bool g_logOpenedOnce = false;
void SetDebug(bool on) {
    AcquireSRWLockExclusive(&g_logLock);
    if (on && g_logFile == INVALID_HANDLE_VALUE && g_pluginDir[0]) {
        wchar_t path[MAX_PATH]{};
        swprintf_s(path, L"%ls\\EndfieldDataBridge_log.txt", g_pluginDir);
        // A new process starts a fresh log; toggling within a process appends.
        g_logFile = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
            g_logOpenedOnce ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        g_logOpenedOnce = g_logOpenedOnce || g_logFile != INVALID_HANDLE_VALUE;
    } else if (!on && g_logFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
    g_debug = on && g_logFile != INVALID_HANDLE_VALUE;
    ReleaseSRWLockExclusive(&g_logLock);
}

// ---- IL2CPP exports and runtime stop protection ----
std::atomic<bool> g_stopping{false};
std::atomic<long> g_inMetadata{0};
ULONGLONG g_deadline = 0;
Il2Cpp il;
HMODULE g_gameAssembly = nullptr;
bool ResolveIl2Cpp(HMODULE module) {
    bool ok = true;
#define X(ret, name, ...) \
    il.name = reinterpret_cast<decltype(il.name)>(GetProcAddress(module, #name)); \
    if (!il.name) { LogF("missing IL2CPP export %s", #name); ok = false; }
    IL2CPP_EXPORTS(X)
#undef X
    return ok;
}
bool RuntimeOpen() {
    if (g_stopping.load()) return false;
    if (g_runtime.probe_open) return g_runtime.probe_open(g_client.owner()) == APH_OK;
    APH_RuntimeSnapshot snapshot{};
    return g_runtime.snapshot && g_runtime.snapshot(sizeof(snapshot), &snapshot) == APH_OK &&
        snapshot.state < APH_RUNTIME_STOPPING;
}
bool WorkOpen() {
    return RuntimeOpen() && WaitForSingleObject(g_cancel, 0) == WAIT_TIMEOUT &&
        GetTickCount64() < g_deadline;
}
void* FindClassAny(void** assemblies, size_t count, const char* ns, const char* name) {
    for (size_t i = 0; i < count; ++i)
        if (void* image = il.il2cpp_assembly_get_image(assemblies[i]))
            if (void* k = il.il2cpp_class_from_name(image, ns, name)) return k;
    return nullptr;
}
bool WorkerAttachReady() {
    // Non-allocating probes; corlib is published after GC/thread list init.
    if (!il.il2cpp_get_corlib()) return false;
    size_t count = 0;
    return il.il2cpp_thread_get_all_attached_threads(&count) && count != 0;
}
bool WaitUntil(bool (*ready)()) {
    while (WorkOpen()) {
        if (ready()) return true;
        WaitForSingleObject(g_cancel, 50);
    }
    return false;
}
bool GameAssemblyLoaded() {
    g_gameAssembly = GetModuleHandleW(L"GameAssembly.dll");
    return g_gameAssembly != nullptr;
}

// ---- Metadata lookups by name ----
std::string TypeName(void* type) {
    if (!type) return "?";
    char* name = il.il2cpp_type_get_name(type);
    if (!name) return "?";
    std::string result(name);
    il.il2cpp_free(name);
    return result;
}
std::string FullName(void* klass) {
    if (!klass) return "?";
    const char* name = il.il2cpp_class_get_name(klass);
    void* outer = il.il2cpp_class_get_declaring_type(klass);
    if (outer) return FullName(outer) + "/" + (name ? name : "?");
    const char* ns = il.il2cpp_class_get_namespace(klass);
    std::string result = ns && *ns ? std::string(ns) + "." : std::string();
    return result + (name ? name : "?");
}
const char* OuterNamespace(void* klass) {
    for (void* outer; (outer = il.il2cpp_class_get_declaring_type(klass));) klass = outer;
    const char* ns = il.il2cpp_class_get_namespace(klass);
    return ns ? ns : "";
}
bool IsBeyond(void* klass) { return strncmp(OuterNamespace(klass), "Beyond", 6) == 0; }
// Class of an instance field's declared type, nullptr if missing.
void* FieldClass(void* klass, const char* name) {
    for (void* k = klass; k; k = il.il2cpp_class_get_parent(k)) {
        void* it = nullptr;
        while (void* f = il.il2cpp_class_get_fields(k, &it)) {
            const char* n = il.il2cpp_field_get_name(f);
            if (n && !strcmp(n, name) && !(il.il2cpp_field_get_flags(f) & FIELD_STATIC))
                return il.il2cpp_class_from_type(il.il2cpp_field_get_type(f));
        }
    }
    return nullptr;
}
int FieldOffset(void* klass, const char* name) {
    for (void* k = klass; k; k = il.il2cpp_class_get_parent(k)) {
        void* it = nullptr;
        while (void* f = il.il2cpp_class_get_fields(k, &it)) {
            const char* n = il.il2cpp_field_get_name(f);
            if (n && !strcmp(n, name) && !(il.il2cpp_field_get_flags(f) & FIELD_STATIC))
                return int(il.il2cpp_field_get_offset(f));
        }
    }
    LogF("[ABI] field %s.%s not found", klass ? il.il2cpp_class_get_name(klass) : "?", name);
    return -1;
}
void* FindMethodChecked(void* klass, const char* name, const char* ret,
    std::initializer_list<const char*> params) {
    if (!klass) return nullptr;
    void* it = nullptr;
    while (void* m = il.il2cpp_class_get_methods(klass, &it)) {
        const char* n = il.il2cpp_method_get_name(m);
        if (!n || strcmp(n, name) || il.il2cpp_method_get_param_count(m) != params.size()) continue;
        uint32_t impl = 0;
        if (il.il2cpp_method_is_generic(m) || (il.il2cpp_method_get_flags(m, &impl) & METHOD_STATIC) ||
            !*reinterpret_cast<void**>(m) || TypeName(il.il2cpp_method_get_return_type(m)) != ret) continue;
        uint32_t i = 0;
        bool ok = true;
        for (const char* p : params) {
            void* type = il.il2cpp_method_get_param(m, i++);
            ok = ok && TypeName(type) == p;
        }
        if (ok) return m;
    }
    LogF("[ABI] method %s.%s not found or signature changed", il.il2cpp_class_get_name(klass), name);
    return nullptr;
}
void LogEnum(void* klass) {
    if (!klass || !il.il2cpp_class_is_enum(klass)) return;
    std::string line = FullName(klass) + ":";
    void* it = nullptr;
    while (void* f = il.il2cpp_class_get_fields(klass, &it)) {
        if (!(il.il2cpp_field_get_flags(f) & FIELD_LITERAL)) continue;
        int64_t value = 0;
        il.il2cpp_field_static_get_value(f, &value);
        char item[96];
        std::snprintf(item, sizeof(item), " %s=%lld", il.il2cpp_field_get_name(f), (long long)value);
        line += item;
    }
    LogF("%s", line.c_str());
}
bool OneSlotNonFloat(void* type) {
    if (!type) return false;
    const int t = il.il2cpp_type_get_type(type);
    return t != 0x0C && t != 0x0D; // R4 / R8
}
void* FindMethodArity(void* klass, const char* name, uint32_t params) {
    void* it = nullptr;
    while (void* m = klass ? il.il2cpp_class_get_methods(klass, &it) : nullptr) {
        const char* n = il.il2cpp_method_get_name(m);
        uint32_t impl = 0;
        if (n && !strcmp(n, name) && il.il2cpp_method_get_param_count(m) == params && !il.il2cpp_method_is_generic(m) &&
            !(il.il2cpp_method_get_flags(m, &impl) & METHOD_STATIC) && *reinterpret_cast<void**>(m))
            return m;
    }
    LogF("[ABI] method %s.%s/%u not found", klass ? il.il2cpp_class_get_name(klass) : "?", name, params);
    return nullptr;
}
bool EnumIsI32(void* type) {
    void* klass = type ? il.il2cpp_class_from_type(type) : nullptr;
    if (!klass || !il.il2cpp_class_is_enum(klass)) return false;
    void* base = il.il2cpp_class_enum_basetype(klass);
    return base && il.il2cpp_type_get_type(base) == 8; // IL2CPP_TYPE_I4
}
bool BuffRefIs16(void* method) {
    void* type = il.il2cpp_method_get_param(method, 0);
    void* klass = type ? il.il2cpp_class_from_type(type) : nullptr;
    uint32_t align = 0;
    return klass && il.il2cpp_class_is_valuetype(klass) && il.il2cpp_class_value_size(klass, &align) == 16;
}

// ---- Hooks ----
std::atomic<bool> g_active{false};
std::atomic<bool>& ActiveFlag() { return g_active; }
bool g_probesOk = false;
bool BeginProbe(APH_NativeBinding*, APH_NativeAdmission*) { return g_active.load() && RuntimeOpen(); }
void EndProbe(APH_NativeAdmission*) {}
applepie::NativeOptions ProbeOptions() {
    applepie::NativeOptions o{};
    o.begin = BeginProbe;
    o.end = EndProbe;
    return o;
}
// Every x64 argument occupies one 8-byte slot: a value of 1/2/4/8 bytes or a
// pointer to a caller copy. Only float position matters (XMM registers).
using TickFn = void(*)(void*, float, void*);
using UiBuffIconFn = void(*)(void*, ObjPtr, bool, int32_t, void*); // ObjectPtr<Buff> by hidden pointer
using AtbTickHook = applepie::NativeHook<1, TickFn>;
using SquadTickHook = applepie::NativeHook<2, TickFn>;
using SquadBuffHook = applepie::NativeHook<3, UiBuffIconFn>;
using HeadBarBuffHook = applepie::NativeHook<4, UiBuffIconFn>;
using HeadBarTickHook = applepie::NativeHook<5, TickFn>;
// AbilitySystem.BeforeCastStart(Skill, TargetHandleView) and
// Skill.CastEnd(FinishType, InterruptReason, Nullable<InterruptContext>):
// TargetHandleView and the Nullable are one non-float slot each.
using CastStartFn = void(*)(void*, void*, uint64_t, void*);
using CastEndFn = void(*)(void*, int32_t, int32_t, uint64_t, void*);
using CastStartHook = applepie::NativeHook<6, CastStartFn>;
using CastEndHook = applepie::NativeHook<7, CastEndFn>;
void OnAtbTick(void* self, float dt, void* method) {
    AtbTickHook::Original(self, dt, method);
    Guarded([&] { ProbeAtb(self); });
    Guarded([] { Tick(); });
}
void OnSquadTick(void* self, float dt, void* method) {
    SquadTickHook::Original(self, dt, method);
    Guarded([&] { ProbeSquad(self); });
    Guarded([] { Tick(); });
}
void OnSquadBuff(void* self, ObjPtr buff, bool applied, int32_t reason, void* method) {
    // Read before original: the handler may finish/recycle the buff on removal.
    Guarded([&] { ProbeBuff("squad", buff, applied, reason); });
    SquadBuffHook::Original(self, buff, applied, reason, method);
}
void OnHeadBarBuff(void* self, ObjPtr buff, bool applied, int32_t reason, void* method) {
    Guarded([&] { ProbeBuff("headbar", buff, applied, reason); });
    HeadBarBuffHook::Original(self, buff, applied, reason, method);
}
void OnHeadBarTick(void* self, float dt, void* method) {
    HeadBarTickHook::Original(self, dt, method);
    Guarded([&] { ProbeHeadBar(self); });
}
void OnCastStart(void* self, void* skill, uint64_t target, void* method) {
    CastStartHook::Original(self, skill, target, method);
    Guarded([&] { ProbeCastStart(skill); });
}
void OnCastEnd(void* self, int32_t finishType, int32_t interruptReason, uint64_t context, void* method) {
    Guarded([&] { ProbeCastEnd(self, finishType, interruptReason); });
    CastEndHook::Original(self, finishType, interruptReason, context, method);
}
APH_Result InstallProbes(void** asms, size_t count) {
    if (!ResolveProbeOffsets(asms, count)) return APH_E_SIGNATURE;
    void* node = FindClassAny(asms, count, "Beyond.UI", "AtbNode");
    void* squad = FindClassAny(asms, count, "Beyond.UI", "SquadIcon");
    void* headBar = FindClassAny(asms, count, "Beyond.UI", "UIHeadBar");
    const char* buffParams[] = {"Beyond.ObjectPtr<Beyond.Gameplay.Core.Buff>", "System.Boolean",
        "Beyond.Gameplay.Core.Buff.FinishReason"};
    APH_Result r = InstallProbe<AtbTickHook>(
        FindMethodChecked(node, "LateTick", "System.Void", {"System.Single"}), 1, OnAtbTick,
        "void(self,f32,MethodInfo*)", "AtbNode.LateTick");
    if (r != APH_OK) return r;
    r = InstallProbe<SquadTickHook>(
        FindMethodChecked(squad, "LateTick", "System.Void", {"System.Single"}), 2, OnSquadTick,
        "void(self,f32,MethodInfo*)", "SquadIcon.LateTick");
    if (r != APH_OK) return r;
    void* m = FindMethodChecked(squad, "_OnBuffIconChange", "System.Void",
        {buffParams[0], buffParams[1], buffParams[2]});
    if (!m || !BuffRefIs16(m) || !EnumIsI32(il.il2cpp_method_get_param(m, 2))) return APH_E_SIGNATURE;
    r = InstallProbe<SquadBuffHook>(m, 3, OnSquadBuff, "void(self,ObjectPtr<Buff>16*,bool,i32,MethodInfo*)",
        "SquadIcon._OnBuffIconChange");
    if (r != APH_OK) return r;
    m = FindMethodChecked(headBar, "_OnBuffIconChange", "System.Void",
        {buffParams[0], buffParams[1], buffParams[2]});
    if (!m || !BuffRefIs16(m) || !EnumIsI32(il.il2cpp_method_get_param(m, 2))) return APH_E_SIGNATURE;
    r = InstallProbe<HeadBarBuffHook>(m, 4, OnHeadBarBuff, "void(self,ObjectPtr<Buff>16*,bool,i32,MethodInfo*)",
        "UIHeadBar._OnBuffIconChange");
    if (r != APH_OK) return r;
    // Optional: without them the units section holds the squad only and no cast events appear.
    g_provides = 0;
    if (off.hbTarget >= 0 && InstallProbe<HeadBarTickHook>(
            FindMethodChecked(headBar, "UpdateData", "System.Void", {"System.Single"}), 5, OnHeadBarTick,
            "void(self,f32,MethodInfo*)", "UIHeadBar.UpdateData") == APH_OK)
        g_provides |= EFB_PROVIDES_HEAD_BAR_UNITS;
    void* skill = FindClassAny(asms, count, "Beyond.Gameplay.Core", "Skill");
    void* as = FindClassAny(asms, count, "Beyond.Gameplay.Core", "AbilitySystem");
    void* start = FindMethodArity(as, "BeforeCastStart", 2);
    void* end = FindMethodArity(skill, "CastEnd", 3);
    const bool castAbi = start && end && g_mIsCasting && off.skillType >= 0 && off.skillOwner >= 0 &&
        TypeName(il.il2cpp_method_get_param(start, 0)) == "Beyond.Gameplay.Core.Skill" &&
        OneSlotNonFloat(il.il2cpp_method_get_param(start, 1)) &&
        EnumIsI32(il.il2cpp_method_get_param(end, 0)) && EnumIsI32(il.il2cpp_method_get_param(end, 1)) &&
        OneSlotNonFloat(il.il2cpp_method_get_param(end, 2)) &&
        TypeName(il.il2cpp_method_get_return_type(start)) == "System.Void" &&
        TypeName(il.il2cpp_method_get_return_type(end)) == "System.Void";
    if (!castAbi) LogF("[ABI] AbilitySystem.BeforeCastStart/Skill.CastEnd signature changed: no cast events");
    else if (InstallProbe<CastStartHook>(start, 6, OnCastStart, "void(self,Skill*,TargetHandleView slot,MethodInfo*)",
                 "AbilitySystem.BeforeCastStart") == APH_OK &&
             InstallProbe<CastEndHook>(end, 7, OnCastEnd, "void(self,i32,i32,Nullable slot,MethodInfo*)",
                 "Skill.CastEnd") == APH_OK)
        g_provides |= EFB_PROVIDES_SKILL_CASTS;
    LogF("optional probes: provides=0x%x", g_provides);
    return APH_OK;
}

// ---- Manager settings ----
// Basic settings (Manager: plugin\config\applepie.endfield.data_bridge.ini)
APH_ConfigApi g_configApi{};
const APH_ConfigOption kIntervals[]{{"50ms"}, {"100ms"}, {"150ms"}, {"200ms"}};
constexpr uint32_t CONFIG_FIELDS = 2; // debug, sample_interval
APH_Result SyncConfig() {
    APH_ConfigSnapshot state{};
    APH_ConfigValue values[CONFIG_FIELDS]{};
    const APH_Result r = g_configApi.snapshot(g_client.owner(), sizeof(state), &state, CONFIG_FIELDS, values);
    if (r != APH_OK) return r;
    const bool on = values[0].integer != 0;
    if (on != g_debug.load()) {
        if (!on) LogF("debug log off");
        SetDebug(on);
        if (on) LogF("debug log on");
    }
    // "50ms" .. "200ms"; the host only stores listed options.
    const int interval = atoi(values[1].text);
    if (interval >= 50 && interval <= 200 && interval != g_sampleIntervalMs.exchange(interval))
        LogF("sample interval %d ms", interval);
    return APH_OK;
}
void APH_CALL ConfigChanged(APH_Owner, void*) { SyncConfig(); }
APH_Result RegisterConfig() {
    APH_Result r = g_client.Service(g_configApi);
    if (r != APH_OK || !g_configApi.register_schema || !g_configApi.snapshot) return APH_E_UNAVAILABLE;
    APH_ConfigField fields[CONFIG_FIELDS]{};
    for (auto& f : fields) f.size = sizeof(f);
    fields[0].type = fields[0].initial.type = APH_CONFIG_BOOL;
    strcpy_s(fields[0].key, "debug");
    strcpy_s(fields[0].label, "Debug log / 调试日志");
    strcpy_s(fields[0].description,
        "Write plugin\\EndfieldDataBridge_log.txt / 输出 plugin\\EndfieldDataBridge_log.txt");
    fields[1].type = fields[1].initial.type = APH_CONFIG_ENUM;
    strcpy_s(fields[1].key, "sample_interval");
    strcpy_s(fields[1].label, "Sample interval / 采样频率");
    strcpy_s(fields[1].description,
        "How often SP, operators and buff timers are read / 技力、干员和 Buff 计时的读取间隔");
    fields[1].options = kIntervals;
    fields[1].option_count = 4;
    strcpy_s(fields[1].initial.text, "50ms");
    const APH_ConfigSchema schema{sizeof(schema), CONFIG_FIELDS, fields};
    r = g_configApi.register_schema(g_client.owner(), &schema);
    if (r != APH_OK) return r;
    if (g_configApi.set_notifier) {
        r = g_configApi.set_notifier(g_client.owner(), ConfigChanged, nullptr);
        if (r != APH_OK) return r;
    }
    return SyncConfig();
}

// ---- Plugin entry ----
applepie::Client g_client;
APH_RuntimeApi g_runtime{};
HANDLE g_cancel = nullptr;
HANDLE g_worker = nullptr;
SRWLOCK g_workerLock = SRWLOCK_INIT;
wchar_t g_pluginDir[MAX_PATH]{};
APH_Result Setup() {
    if (!WaitUntil(GameAssemblyLoaded)) return APH_E_UNAVAILABLE;
    // A loaded PE's exports do not appear later; fail closed if one is missing.
    if (!ResolveIl2Cpp(g_gameAssembly)) return APH_E_UNAVAILABLE;
    void* shutdown = GetProcAddress(g_gameAssembly, "il2cpp_shutdown");
    if (!shutdown) return APH_E_UNAVAILABLE;
    APH_Result r = ShutdownHook::Install((APH_NativeProc)shutdown, "void() C-export",
        "il2cpp.runtime.shutdown");
    LogF("shutdown stop hook: %ld", static_cast<long>(r));
    if (r != APH_OK) return r;
    if (!WaitUntil(WorkerAttachReady)) return APH_E_UNAVAILABLE;
    void* domain = nullptr;
    while (WorkOpen() && !(domain = il.il2cpp_domain_get())) WaitForSingleObject(g_cancel, 50);
    if (!domain) return APH_E_UNAVAILABLE;
    void* thread = il.il2cpp_thread_attach(domain);
    if (!thread) return APH_E_UNAVAILABLE;
    r = APH_E_UNAVAILABLE;
    __try {
        size_t count = 0;
        void** assemblies = nullptr;
        while (WorkOpen()) {
            assemblies = il.il2cpp_domain_get_assemblies(domain, &count);
            if (assemblies && count && FindClassAny(assemblies, count, "Beyond.UI", "MainCharHpBar")) break;
            assemblies = nullptr;
            WaitForSingleObject(g_cancel, 50);
        }
        if (!assemblies) __leave;
        void* app = FindClassAny(assemblies, count, "UnityEngine", "Application");
        void* quit = app ? il.il2cpp_class_get_method_from_name(app, "Internal_ApplicationQuit", 0) : nullptr;
        if (quit && *reinterpret_cast<void**>(quit)) {
            r = QuitHook::Install(*reinterpret_cast<APH_NativeProc*>(quit),
                "void(MethodInfo*) static", "unity.Application.Internal_ApplicationQuit");
            LogF("quit stop hook: %ld", static_cast<long>(r));
            if (r != APH_OK) __leave;
        }
        wchar_t path[MAX_PATH]{};
        swprintf_s(path, L"%ls\\EndfieldDataBridge_dump.txt", g_pluginDir);
        if (g_debug && _wfopen_s(&g_out, path, L"wb") == 0 && g_out) {
            const ULONGLONG start = GetTickCount64();
            const bool ok = DumpAllGuarded(assemblies, count);
            std::fclose(g_out);
            g_out = nullptr;
            LogF("dump %s in %llu ms -> EndfieldDataBridge_dump.txt", ok ? "complete" : "aborted",
                static_cast<unsigned long long>(GetTickCount64() - start));
            if (!ok) { r = APH_E_REVOKED; __leave; }
        }
        g_probesOk = OpenShared() && InstallProbes(assemblies, count) == APH_OK;
        r = APH_OK;
    } __finally {
        // Once shutdown has begun the runtime owns its threads.
        if (RuntimeOpen()) il.il2cpp_thread_detach(thread);
    }
    return r;
}
DWORD WINAPI Worker(void* parameter) {
    const uint64_t epoch = (uint64_t)(uintptr_t)parameter;
    APH_Result r = APH_E_FAULT;
    try { r = Setup(); } catch (...) { r = APH_E_FAULT; }
    ClearDump();
    Log("setup finished", epoch, r);
    // A failed setup still enables the plugin so the log explains the
    // failure; probe bodies stay closed unless everything installed.
    APH_OwnerStatus status{};
    AcquireSRWLockExclusive(&g_workerLock);
    if (g_client.Status(status) == APH_OK && status.state == APH_OWNER_ENABLING &&
        status.activation_epoch == epoch && g_client.CompleteEnable(epoch, APH_OK) == APH_OK &&
        g_probesOk && WaitForSingleObject(g_cancel, 0) == WAIT_TIMEOUT) {
        StartSession();
        g_active = true;
    }
    ReleaseSRWLockExclusive(&g_workerLock);
    LogF("probes %s", g_active.load() ? "active" : "inactive");
    return 0;
}
void APH_CALL Disabling(APH_Owner owner, uint64_t epoch, void*) {
    if (owner != g_client.owner()) return;
    g_active = false;
    if (g_cancel) SetEvent(g_cancel);
    Log("disabling", epoch, APH_OK);
}
APH_Result APH_CALL Restore(APH_Owner owner, const APH_Cleanup* cleanup, void*) {
    if (owner != g_client.owner() || !cleanup || cleanup->size < sizeof(*cleanup))
        return APH_E_ARGUMENT;
    AcquireSRWLockExclusive(&g_workerLock);
    APH_Result r = APH_OK;
    if (g_worker) {
        const DWORD wait = WaitForSingleObject(g_worker, 0);
        if (wait == WAIT_TIMEOUT) r = APH_PENDING;
        else if (wait != WAIT_OBJECT_0) r = APH_E_NEEDS_RESTART;
        else { CloseHandle(g_worker); g_worker = nullptr; }
    }
    if (r == APH_OK) {
        // Game-object snapshots never survive a disable; StartSession clears them.
        g_active = false;
        EndSession();
    }
    ReleaseSRWLockExclusive(&g_workerLock);
    if (r == APH_OK) Log("restored", 0, APH_OK);
    return r;
}
} // namespace bridge

using namespace bridge;

extern "C" __declspec(dllexport) const APH_Manifest APH_PluginManifest = {
    sizeof(APH_Manifest), APH_ABI_MAJOR, APH_ABI_MINOR, APH_ARCH_X64,
    APH_PLUGIN_LIFECYCLE | APH_PLUGIN_LATE_LOAD | APH_PLUGIN_MODULE_OBSERVER,
    APH_CAP_CORE | APH_CAP_HOOKS, 0,
    "endfield.data_bridge", "Endfield Data Bridge"
};

extern "C" __declspec(dllexport) APH_Result APH_CALL APH_Attach(
    const APH_AttachArgs* args) {
    const APH_Result attached = g_client.Attach(args);
    if (attached != APH_OK) return attached;
    if (g_client.lifecycle()) return APH_OK;

    // Optional: absent outside the game (offline verification host).
    g_client.Service(g_runtime);
    g_cancel = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (!g_cancel) return APH_E_UNAVAILABLE;

    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&APH_PluginManifest), &self);
    const DWORD length = GetModuleFileNameW(self, g_pluginDir, MAX_PATH);
    if (wchar_t* slash = length && length < MAX_PATH ? wcsrchr(g_pluginDir, L'\\') : nullptr)
        *slash = 0;
    else g_pluginDir[0] = 0;

    const APH_LifecycleDesc lifecycle{
        sizeof(APH_LifecycleDesc), 10000, Disabling, Restore, nullptr
    };
    const APH_Result result = g_client.RegisterLifecycle(lifecycle);
    if (result != APH_OK) return result;
    // Settings are optional: without them (older Manager, unreadable file) debug stays off.
    const APH_Result config = RegisterConfig();
    LogF("attached owner=%llu config=%ld debug=%d", static_cast<unsigned long long>(g_client.owner()),
        static_cast<long>(config), int(g_debug.load()));
    return APH_OK;
}

extern "C" __declspec(dllexport) APH_Result APH_CALL APH_Enable(
    APH_Owner owner, uint64_t epoch) {
    if (!owner || owner != g_client.owner() || !epoch || !g_client.lifecycle())
        return APH_E_ARGUMENT;

    APH_OwnerStatus status{};
    const APH_Result result = g_client.Status(status);
    if (result != APH_OK) return result;
    if (status.state != APH_OWNER_ENABLING || status.activation_epoch != epoch)
        return APH_E_STALE;

    // Outside the game (offline host) there is nothing to dump. In the game,
    // UnityPlayer is an import of the EXE, while GameAssembly loads later.
    if (!GetModuleHandleW(L"UnityPlayer.dll")) {
        Log("enabled (not a Unity process, probes skipped)", epoch, APH_OK);
        return APH_OK;
    }
    if (!g_runtime.snapshot) return APH_E_UNAVAILABLE;

    AcquireSRWLockExclusive(&g_workerLock);
    APH_Result r = APH_PENDING;
    if (g_worker || g_stopping.load()) r = APH_E_UNAVAILABLE;
    else {
        g_deadline = GetTickCount64() + 120000;
        ResetEvent(g_cancel);
        g_worker = CreateThread(nullptr, 0, Worker, (void*)(uintptr_t)epoch, 0, nullptr);
        if (!g_worker) { SetEvent(g_cancel); r = APH_E_UNAVAILABLE; }
    }
    ReleaseSRWLockExclusive(&g_workerLock);
    Log("enable", epoch, r);
    return r;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
