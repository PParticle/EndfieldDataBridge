#include <windows.h>
#include <atomic>
#include <cctype>
#include <initializer_list>
#include <string>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "applepie_client.h"
#include "../include/endfield_bridge.h"

#if !defined(_M_X64)
#error EndfieldDataBridge requires an x64 MSVC build.
#endif

// Read-only observer for SP, squad ultimate energy / combo CD and buffs.
// UI hooks sample on the game thread and publish into shared memory
// (include/endfield_bridge.h). The Manager "debug" setting enables
// plugin\EndfieldDataBridge_log.txt and, at enable, the metadata dump;
// with it off the plugin writes no files.

namespace {
applepie::Client g_client;
APH_RuntimeApi g_runtime{};
HANDLE g_cancel = nullptr;
HANDLE g_worker = nullptr;
SRWLOCK g_workerLock = SRWLOCK_INIT;
SRWLOCK g_logLock = SRWLOCK_INIT;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
std::atomic<bool> g_stopping{false};
std::atomic<long> g_inMetadata{0};
ULONGLONG g_deadline = 0;
wchar_t g_pluginDir[MAX_PATH]{};

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

// ============================================================================
// IL2CPP exports (resolved from GameAssembly.dll, metadata use only)
// ============================================================================
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
    X(void, il2cpp_free, void*)

struct Il2Cpp {
#define X(ret, name, ...) ret (*name)(__VA_ARGS__) = nullptr;
    IL2CPP_EXPORTS(X)
#undef X
} il;
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

constexpr int FIELD_STATIC = 0x10, FIELD_LITERAL = 0x40;
constexpr uint32_t METHOD_STATIC = 0x10;
// ============================================================================
// Runtime stop protection (same shared contracts as Manager / CombatHUD)
// ============================================================================
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
std::atomic<bool>& ActiveFlag();
void MarkWriterStopped();
void BeginRuntimeStop() {
    g_stopping.store(true);
    ActiveFlag() = false;
    MarkWriterStopped();
    if (g_cancel) SetEvent(g_cancel);
    // Give an in-flight metadata query a bounded chance to leave IL2CPP.
    for (int i = 0; i < 500 && g_inMetadata.load(); ++i) Sleep(1);
}
APH_Result APH_CALL ObserveStop(APH_Owner, const APH_SharedEvent* e, void*) {
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
// ============================================================================
// Metadata dump helpers
// ============================================================================
FILE* g_out = nullptr;
void Out(const char* fmt, ...) {
    if (!g_out) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_out, fmt, args);
    va_end(args);
}

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
// Nested types report an empty namespace; use the outermost declaring type.
const char* OuterNamespace(void* klass) {
    for (void* outer; (outer = il.il2cpp_class_get_declaring_type(klass));) klass = outer;
    const char* ns = il.il2cpp_class_get_namespace(klass);
    return ns ? ns : "";
}
bool IsBeyond(void* klass) { return strncmp(OuterNamespace(klass), "Beyond", 6) == 0; }

const char* StrIStr(const char* s, const char* pat) {
    const size_t n = strlen(pat);
    for (; *s; ++s) if (_strnicmp(s, pat, n) == 0) return s;
    return nullptr;
}
// "Atb"/"ATB"/"m_atb"/"atbCost" but not "combatBuff".
bool HasAtbToken(const char* s) {
    for (const char* p = s; (p = StrIStr(p, "atb")); ++p)
        if (p[0] == 'A' || p == s || !isalpha((unsigned char)p[-1])) return true;
    return false;
}
bool HasAny(const char* s, std::initializer_list<const char*> pats) {
    for (const char* p : pats) if (strstr(s, p)) return true;
    return false;
}
bool IsSpRelated(const char* s) {
    return HasAtbToken(s) || HasAny(s, {"SkillPoint", "skillPoint", "SkillEnergy", "skillEnergy",
        "SpBar", "SPBar", "SpNode", "SpCost", "spCost", "TeamSp", "teamSp"});
}

std::vector<void*> g_detail;
std::unordered_set<void*> g_detailSet;
constexpr size_t MAX_DETAIL = 600;
void QueueDetail(void* klass) {
    if (!klass || g_detailSet.count(klass)) return;
    if (g_detail.size() >= MAX_DETAIL) {
        Out("!! detail cap reached, skipped %s\n", FullName(klass).c_str());
        return;
    }
    g_detailSet.insert(klass);
    g_detail.push_back(klass);
}

void DumpFields(void* klass, bool followEnums) {
    const bool isEnum = il.il2cpp_class_is_enum(klass);
    void* it = nullptr;
    while (void* f = il.il2cpp_class_get_fields(klass, &it)) {
        const char* name = il.il2cpp_field_get_name(f);
        const int flags = il.il2cpp_field_get_flags(f);
        void* type = il.il2cpp_field_get_type(f);
        // Read literal values only for enum members (integers, no allocation);
        // string constants elsewhere would create managed strings.
        if ((flags & FIELD_LITERAL) && !isEnum) {
            Out("    const %s %s\n", TypeName(type).c_str(), name ? name : "?");
            continue;
        }
        if (flags & FIELD_LITERAL) {
            int64_t value = 0;
            il.il2cpp_field_static_get_value(f, &value);
            Out("    const %s %s = %lld\n", TypeName(type).c_str(), name ? name : "?",
                static_cast<long long>(value));
            continue;
        }
        Out("    [0x%03zX]%s %s %s\n", il.il2cpp_field_get_offset(f),
            (flags & FIELD_STATIC) ? " static" : "", TypeName(type).c_str(), name ? name : "?");
        if (!followEnums || !type) continue;
        void* fieldClass = il.il2cpp_class_from_type(type);
        if (fieldClass && il.il2cpp_class_is_enum(fieldClass)) QueueDetail(fieldClass);
    }
}
void DumpMethods(void* klass) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_gameAssembly);
    void* it = nullptr;
    while (void* m = il.il2cpp_class_get_methods(klass, &it)) {
        uint32_t impl = 0;
        const uint32_t flags = il.il2cpp_method_get_flags(m, &impl);
        const uintptr_t ptr = *reinterpret_cast<uintptr_t*>(m); // MethodInfo::methodPointer
        std::string line = TypeName(il.il2cpp_method_get_return_type(m)) + " " +
            (il.il2cpp_method_get_name(m) ? il.il2cpp_method_get_name(m) : "?") + "(";
        const uint32_t count = il.il2cpp_method_get_param_count(m);
        for (uint32_t i = 0; i < count; ++i) {
            const char* pname = il.il2cpp_method_get_param_name(m, i);
            line += (i ? ", " : "") + TypeName(il.il2cpp_method_get_param(m, i)) + " " +
                (pname ? pname : "?");
        }
        if (ptr) Out("    RVA 0x%08llX%s %s)\n", static_cast<unsigned long long>(ptr - base),
            (flags & METHOD_STATIC) ? " static" : "", line.c_str());
        else Out("    RVA -%s %s)\n", (flags & METHOD_STATIC) ? " static" : "", line.c_str());
    }
}
void DumpClass(void* klass) {
    const bool isEnum = il.il2cpp_class_is_enum(klass);
    void* parent = il.il2cpp_class_get_parent(klass);
    Out("\n== %s : %s  size=0x%X%s\n", FullName(klass).c_str(),
        parent ? FullName(parent).c_str() : "-", il.il2cpp_class_instance_size(klass),
        isEnum ? " enum" : il.il2cpp_class_is_valuetype(klass) ? " struct" : "");
    Out("  fields:\n");
    DumpFields(klass, !isEnum);
    if (isEnum) return;
    // Inherited instance fields, up to the first engine/system base type.
    for (void* p = parent; p && IsBeyond(p); p = il.il2cpp_class_get_parent(p)) {
        Out("  inherited from %s:\n", FullName(p).c_str());
        DumpFields(p, true);
    }
    Out("  methods:\n");
    DumpMethods(klass);
}
// Explicit targets: the classes the old CombatHUD depends on plus guesses.
const char* const kTargets[][2] = {
    {"Beyond.Gameplay.Core", "Buff"}, {"Beyond.Gameplay.Core", "BuffData"},
    {"Beyond.Gameplay.Core", "BuffIconConfig"}, {"Beyond.Gameplay.Core", "BuffContainer"},
    {"Beyond.Gameplay.Core", "AbilitySystem"}, {"Beyond.Gameplay.Core", "Skill"},
    {"Beyond.UI", "MainCharHpBar"}, {"Beyond.UI", "GPUIBuffNode"}, {"Beyond.UI", "UIBuffNode"},
    {"Beyond.UI", "UIBuffCell"}, {"Beyond.UI", "GPUIBuffCell"}, {"Beyond.UI", "SkillButton"},
    {"Beyond.UI", "ComboSkillPanel"}, {"Beyond.UI", "AtbNode"}, {"Beyond.UI", "SquadIcon"},
    {"Beyond.Gameplay.Core", "BattleManager"}, {"Beyond.Gameplay.Core", "Entity"},
    {"Beyond.Gameplay.Core", "FactionIndex"}, {"Beyond.Gameplay", "SkillSetting"},
};
// Class-name keywords (Beyond.* only) for the compact index.
const char* const kIndexWords[] = {
    "Buff", "HpBar", "HPBar", "HeadBar", "HeadInfo", "Enemy", "Boss", "Monster",
    "Team", "Squad", "Battle", "SkillPanel", "SkillButton", "UltimateSp", "Energy",
};
// Class-name keywords (Beyond.UI) that get a full dump.
const char* const kUiDetailWords[] = {"HpBar", "HPBar", "HeadBar", "Boss", "BuffNode", "BuffCell"};

struct Hit { void* klass; std::string what; };

void ScanClass(void* klass, std::vector<Hit>& hits, std::vector<void*>& index) {
    const char* name = il.il2cpp_class_get_name(klass);
    if (!name || !IsBeyond(klass)) return;
    const std::string full = FullName(klass);
    const bool ui = strncmp(OuterNamespace(klass), "Beyond.UI", 9) == 0;

    if (IsSpRelated(name)) { hits.push_back({klass, "class name"}); QueueDetail(klass); }
    for (const char* w : kIndexWords) if (strstr(name, w)) { index.push_back(klass); break; }
    if (ui) for (const char* w : kUiDetailWords) if (strstr(name, w)) { QueueDetail(klass); break; }
    // Nested types (enums, controllers, contexts) of the classes this plugin reads.
    if (void* outer = il.il2cpp_class_get_declaring_type(klass)) {
        const char* o = il.il2cpp_class_get_name(outer);
        const char* oNs = il.il2cpp_class_get_namespace(outer);
        static const char* const kOuter[] = {"Skill", "AbilitySystem", "Buff", "UIHeadBar", "HeadBarCtrl"};
        if (name[0] != '<' && o && oNs && (!strcmp(oNs, "Beyond.Gameplay.Core") || !strcmp(oNs, "Beyond.UI")))
            for (const char* k : kOuter) if (!strcmp(o, k)) { QueueDetail(klass); break; }
    }

    void* it = nullptr;
    while (void* f = il.il2cpp_class_get_fields(klass, &it)) {
        const char* fname = il.il2cpp_field_get_name(f);
        if (!fname) continue;
        const std::string type = TypeName(il.il2cpp_field_get_type(f));
        if (IsSpRelated(fname) || IsSpRelated(type.c_str()))
            hits.push_back({klass, "field " + type + " " + fname});
        // Any UI class owning a buff node/cell is a candidate unit frame.
        if (ui && HasAny(type.c_str(), {"BuffNode", "BuffCell"})) QueueDetail(klass);
    }
    it = nullptr;
    while (void* m = il.il2cpp_class_get_methods(klass, &it)) {
        const char* mname = il.il2cpp_method_get_name(m);
        if (!mname) continue;
        bool buffParam = false;
        const uint32_t count = il.il2cpp_method_get_param_count(m);
        for (uint32_t i = 0; i < count && !buffParam; ++i)
            buffParam = TypeName(il.il2cpp_method_get_param(m, i)).find(
                "ObjectPtr<Beyond.Gameplay.Core.Buff>") != std::string::npos;
        if (IsSpRelated(mname)) hits.push_back({klass, std::string("method ") + mname});
        // UI classes that react to buff add/remove events.
        if (ui && buffParam) { hits.push_back({klass, std::string("buff-event ") + mname}); QueueDetail(klass); }
    }
}

bool DumpAll(void** assemblies, size_t count) {
    std::vector<Hit> hits;
    std::vector<void*> index;
    size_t classes = 0;
    Out("# EndfieldDataBridge metadata dump\n# GameAssembly base=%p\n", g_gameAssembly);
    for (size_t a = 0; a < count; ++a) {
        void* image = il.il2cpp_assembly_get_image(assemblies[a]);
        if (!image) continue;
        const char* imageName = il.il2cpp_image_get_name(image);
        for (const auto& t : kTargets)
            if (void* k = il.il2cpp_class_from_name(image, t[0], t[1])) QueueDetail(k);
        // Nested Buff.BuffContainer: reach it through the field that holds it.
        if (void* as = il.il2cpp_class_from_name(image, "Beyond.Gameplay.Core", "AbilitySystem")) {
            void* it = nullptr;
            while (void* f = il.il2cpp_class_get_fields(as, &it)) {
                const char* n = il.il2cpp_field_get_name(f);
                if (n && !strcmp(n, "m_buffContainer"))
                    if (void* k = il.il2cpp_class_from_type(il.il2cpp_field_get_type(f))) QueueDetail(k);
            }
        }
        const size_t n = il.il2cpp_image_get_class_count(image);
        Out("# image %s classes=%zu\n", imageName ? imageName : "?", n);
        for (size_t c = 0; c < n; ++c) {
            if ((c & 255) == 0 && !WorkOpen()) return false;
            if (void* k = il.il2cpp_image_get_class(image, c)) { ScanClass(k, hits, index); ++classes; }
        }
    }
    LogF("scanned %zu classes, %zu hits, %zu indexed, %zu detailed", classes,
        hits.size(), index.size(), g_detail.size());

    Out("\n######## SP/ATB hits (%zu)\n", hits.size());
    for (const auto& h : hits) Out("%s :: %s\n", FullName(h.klass).c_str(), h.what.c_str());
    Out("\n######## Class index (%zu)\n", index.size());
    for (void* k : index) {
        void* parent = il.il2cpp_class_get_parent(k);
        Out("%s : %s\n", FullName(k).c_str(), parent ? FullName(parent).c_str() : "-");
    }
    Out("\n######## Details\n");
    // Detail dumps may queue enum types of their fields; iterate by index.
    for (size_t i = 0; i < g_detail.size(); ++i) {
        if (!WorkOpen()) return false;
        DumpClass(g_detail[i]);
    }
    Out("\n# END\n");
    return true;
}
bool DumpAllGuarded(void** assemblies, size_t count) {
    g_inMetadata.fetch_add(1);
    bool ok = false;
    __try { ok = DumpAll(assemblies, count); }
    __except (EXCEPTION_EXECUTE_HANDLER) { LogF("dump faulted: 0x%08lX", GetExceptionCode()); }
    g_inMetadata.fetch_sub(1);
    return ok;
}
// ============================================================================
// Worker: wait for IL2CPP, install stop hooks, dump once, complete Enable
// ============================================================================
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

// ============================================================================
// Runtime probes (read-only observers; each calls the original exactly once)
// ============================================================================
std::atomic<bool> g_active{false};
std::atomic<bool>& ActiveFlag() { return g_active; }
bool g_probesOk = false;
std::atomic<DWORD> g_uiThread{0};
bool BeginProbe(APH_NativeBinding*, APH_NativeAdmission*) { return g_active.load() && RuntimeOpen(); }
void EndProbe(APH_NativeAdmission*) {}
applepie::NativeOptions ProbeOptions() {
    applepie::NativeOptions o{};
    o.begin = BeginProbe;
    o.end = EndProbe;
    return o;
}

struct ObjPtr { void* object; uint32_t uid; uint32_t padding; };
static_assert(sizeof(ObjPtr) == 16, "ObjectPtr<T> ABI");

// Every x64 argument occupies one 8-byte slot: a value of 1/2/4/8 bytes or a
// pointer to a caller copy. Only float position matters (XMM registers).
using TickFn = void(*)(void*, float, void*);
using UiBuffIconFn = void(*)(void*, ObjPtr, bool, int32_t, void*); // ObjectPtr<Buff> by hidden pointer
using AtbTickHook = applepie::NativeHook<1, TickFn>;
using SquadTickHook = applepie::NativeHook<2, TickFn>;
using SquadBuffHook = applepie::NativeHook<3, UiBuffIconFn>;
using HeadBarBuffHook = applepie::NativeHook<4, UiBuffIconFn>;
using HeadBarTickHook = applepie::NativeHook<5, TickFn>;
// Skill.DoCast(Action, Action, Nullable<CastSkillOptions>, bool) and
// Skill.CastEnd(FinishType, InterruptReason, Nullable<InterruptContext>):
// each Nullable is one non-float slot (value or pointer to a caller copy).
using CastStartFn = void(*)(void*, void*, void*, uint64_t, bool, void*);
using CastEndFn = void(*)(void*, int32_t, int32_t, uint64_t, void*);
using CastStartHook = applepie::NativeHook<6, CastStartFn>;
using CastEndHook = applepie::NativeHook<7, CastEndFn>;

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
} off;
void* g_mGetBattleManager = nullptr;
void* g_mGetSkillSetting = nullptr;
void* g_mMaxUltSp = nullptr;
void* g_mCdRemaining = nullptr;
void* g_mCooldown = nullptr;
void* g_mFaction = nullptr;
void* g_mSkillCastId = nullptr;
void* g_mSkillInstId = nullptr;
void* g_mIsCasting = nullptr;

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
// Probe bodies use only plain C data so the SEH wrappers need no unwinding.
std::atomic<long> g_probeLines{0}, g_probeFaults{0};
constexpr long PROBE_LINE_BUDGET = 60000;
std::atomic<bool> g_debug{false};
bool ProbeLogAllowed() { return g_debug.load() && g_probeLines.fetch_add(1) < PROBE_LINE_BUDGET; }

template<class T> T At(void* obj, int offset) { return *reinterpret_cast<T*>(static_cast<char*>(obj) + offset); }
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
// ObjectPtr<AbilitySystem> is valid only while the target still carries its UID.
bool AbilityPtrLive(const ObjPtr& p) {
    return p.object && off.asUid >= 0 && At<uint32_t>(p.object, off.asUid) == p.uid;
}
void NoteThread(const char* who) {
    const DWORD t = GetCurrentThreadId();
    DWORD expected = 0;
    if (!g_uiThread.compare_exchange_strong(expected, t) && expected != t && ProbeLogAllowed())
        LogF("[THREAD] %s on thread %lu, first probe thread %lu", who, t, expected);
}

// Call a validated 0-arg instance getter through its native pointer.
template<class R> R CallGetter(void* method, void* self) {
    using Fn = R (*)(void*, void*);
    return reinterpret_cast<Fn>(*reinterpret_cast<void**>(method))(self, method);
}
void EntityName(void* ability, char* out, int size) {
    out[0] = 0;
    void* entity = ability && off.asEntity >= 0 ? At<void*>(ability, off.asEntity) : nullptr;
    if (entity && off.entName >= 0) ReadManagedString(At<void*>(entity, off.entName), out, size);
}
int FactionOf(void* ability) {
    return ability && g_mFaction ? CallGetter<int32_t>(g_mFaction, ability) : -1;
}

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
HANDLE g_mapping = nullptr;
Layout* g_layout = nullptr;
const wchar_t* g_mappingName = EFB_MAPPING_NAME; // tests use a private name
SRWLOCK g_dataLock = SRWLOCK_INIT;
int64_t g_qpcFreq = 1;
int64_t RealQpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
int64_t (*g_clock)() = RealQpc;
int64_t QpcNow() { return g_clock(); }
float SecondsBetween(int64_t from, int64_t to) { return float(double(to - from) / double(g_qpcFreq)); }
uint64_t FileTimeValue(const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; }
uint64_t ProcessStartTime() {
    FILETIME created{}, exited{}, kernel{}, user{};
    return GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) ? FileTimeValue(created) : 0;
}

enum : unsigned { W_HEADER = 1, W_TEAM = 2, W_OPS = 4, W_BUFFS = 8, W_EVENTS = 16, W_UNITS = 32 };
void OpenSeq(volatile uint32_t& seq) { seq |= 1; }
void CloseSeq(volatile uint32_t& seq) { seq += 1; }
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
// Stop path: may run on any thread while the game thread is inside Write().
void MarkWriterStopped() {
    if (g_layout) InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_layout->header.active), 0);
}
// Manager setting "sample_interval"; 0 samples every frame (tests only).
std::atomic<int> g_sampleIntervalMs{50};
bool SampleDue(int64_t& last, int64_t now) {
    if (last && double(now - last) * 1000 < double(g_sampleIntervalMs.load()) * double(g_qpcFreq)) return false;
    last = now;
    return true;
}

// ---- SP via AtbNode.LateTick -> BattleManager ----
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

// ---- Squad slots via SquadIcon.LateTick: ultimate energy + combo CD ----
struct SquadSlot { void* ability; uint32_t uid; };
SquadSlot g_squad[OPERATOR_SLOTS]{};
int64_t g_opSampledAt[OPERATOR_SLOTS]{};
struct SlotLog { ULONGLONG at; float ult, cd; bool main; };
SlotLog g_slotLog[OPERATOR_SLOTS]{};
int SquadIndexOf(void* ability) {
    for (int i = 0; ability && i < OPERATOR_SLOTS; ++i)
        if (g_squad[i].ability == ability) return i;
    return -1;
}
// AbilitySystem.m_skills is List<Skill>: items array at +0x10, size at +0x18.
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
void SampleUnit(const ObjPtr& ability, int charIndex, bool onHeadBar);
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
TrackedBuff g_tracked[BUFF_SLOTS]{};
uint32_t g_nextKey = 1;
bool g_buffsDirty = false;     // a NEW or FINISHED entry waits for settlement
int32_t g_settledFrame = INT32_MIN, g_tickedFrame = INT32_MIN;
void* g_buffClass = nullptr;
void* g_mFrameCount = nullptr;

int32_t UnityFrame() {
    using Fn = int32_t (*)(void*);
    if (g_mFrameCount) return reinterpret_cast<Fn>(*reinterpret_cast<void**>(g_mFrameCount))(g_mFrameCount);
    return int32_t(GetTickCount64() / 16); // fallback: ~one frame at 60 fps
}
int32_t (*g_frameClock)() = UnityFrame;
int32_t FrameNow() { return g_frameClock(); }
bool IsBuff(void* obj) {
    for (void* k = obj ? il.il2cpp_object_get_class(obj) : nullptr; k; k = il.il2cpp_class_get_parent(k))
        if (k == g_buffClass) return true;
    return false;
}
// Recently finished or dropped uids: a finish for one of these is a duplicate
// from another panel, not evidence of a missed add.
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
// Mirrors the private table into the buffs section. Inside Write(W_BUFFS).
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
// Settles every add/finish reported before `frame`. Inside Write(W_BUFFS | W_EVENTS).
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
// Re-read of every live buff, once per sample interval. Inside Write(W_BUFFS | W_EVENTS).
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
UnitTrack g_units[UNIT_SLOTS]{};
uint32_t g_hudFrames = 0;       // Tick() calls: frames in which the battle HUD ran
uint32_t g_provides = 0;        // EFB_PROVIDES_*, set when optional probes install
// Unit getters on AbilitySystem; any may be missing, then the field stays 0.
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
template<class R> R GetOr(void* method, void* self, R fallback) { return method ? CallGetter<R>(method, self) : fallback; }
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
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Game thread. charIndex -1 = not a squad slot.
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
                "progress=%.3f recTime=%.3f sumRecTime=%.3f", tick, s.instance_uid, s.char_index, s.on_head_bar,
                s.entity_name, s.hp, s.max_hp, s.poise, s.max_poise, s.has_poise, s.is_zero_poise,
                s.in_poise_recovery, s.poise_recovery_progress, s.poise_rec_time, s.sum_poise_rec_time);
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
// Inside Write(W_UNITS).
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

// ---- Skill casts: Skill.DoCast / Skill.CastEnd ----
int32_t SkillTypeOf(void* skill) { return At<int32_t>(skill, off.skillType); }
// Normal attacks and dodges are not reported (EFB_SkillType).
bool ReportedSkill(int32_t type) {
    return type != EFB_SKILL_ATTACK && type != EFB_SKILL_BREAKING_ATTACK && type != EFB_SKILL_DODGE &&
        type != EFB_SKILL_PASSIVE;
}
uint32_t g_castsSeen[10]{};     // debug: casts per SkillType + 1
void PushSkillEvent(uint32_t type, void* skill, int32_t finishType, int32_t interruptReason) {
    const ObjPtr owner = At<ObjPtr>(skill, off.skillOwner);
    EFB_SkillEventPayload p{};
    const bool live = AbilityPtrLive(owner);
    p.caster_uid = live ? owner.uid : 0;
    p.char_index = int8_t(live ? SquadIndexOf(owner.object) : -1);
    p.faction = int8_t(live ? FactionOf(owner.object) : -1);
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
// After the original: DoCast may decline to start.
void ProbeCastStart(void* skill) {
    if (!(g_provides & EFB_PROVIDES_SKILL_CASTS)) return;
    NoteThread("Skill.DoCast");
    const int32_t type = SkillTypeOf(skill);
    if (type >= -1 && type < 9) ++g_castsSeen[type + 1];
    if (!ReportedSkill(type) || !CallGetter<bool>(g_mIsCasting, skill)) return;
    PushSkillEvent(EFB_EVENT_SKILL_CAST_STARTED, skill, 0, 0);
}
// Before the original, while the skill still reports itself casting.
void ProbeCastEnd(void* skill, int32_t finishType, int32_t interruptReason) {
    if (!(g_provides & EFB_PROVIDES_SKILL_CASTS)) return;
    if (!ReportedSkill(SkillTypeOf(skill)) || !CallGetter<bool>(g_mIsCasting, skill)) return;
    PushSkillEvent(EFB_EVENT_SKILL_CAST_ENDED, skill, finishType, interruptReason);
}
// UI buff callbacks (game thread): record the change at the moment the game
// reports it; settlement happens on the next frame.
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
// HUD LateTick hooks (game thread): once per frame.
uint32_t g_sessionsStarted = 0;
// Adds and finishes settle every frame; timers and stacks follow the interval.
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
// Worker thread, before probes open: a fresh session with empty tables.
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
// Restore: probes are drained, so no game-thread write is in flight.
void EndSession() {
    if (g_layout) Write(W_HEADER, [] { g_layout->header.active = 0; });
}
template<class Fn> void Guarded(Fn fn) {
    __try { fn(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_probeFaults.fetch_add(1) < 20) LogF("[FAULT] probe exception 0x%08lX", GetExceptionCode());
    }
}
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
void OnCastStart(void* self, void* onEnd, void* onExclusive, uint64_t options, bool skipCost, void* method) {
    CastStartHook::Original(self, onEnd, onExclusive, options, skipCost, method);
    Guarded([&] { ProbeCastStart(self); });
}
void OnCastEnd(void* self, int32_t finishType, int32_t interruptReason, uint64_t context, void* method) {
    Guarded([&] { ProbeCastEnd(self, finishType, interruptReason); });
    CastEndHook::Original(self, finishType, interruptReason, context, method);
}

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

// A value type of 1/2/4/8 bytes travels in one register, a bigger one as a
// pointer to a caller copy: either way one non-float slot.
bool OneSlotNonFloat(void* type) {
    if (!type) return false;
    const int t = il.il2cpp_type_get_type(type);
    return t != 0x0C && t != 0x0D; // R4 / R8
}
// An instance method by name and parameter count (types checked by the caller).
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
// ObjectPtr<Buff> must still be the 16-byte (pointer, uid) struct.
bool BuffRefIs16(void* method) {
    void* type = il.il2cpp_method_get_param(method, 0);
    void* klass = type ? il.il2cpp_class_from_type(type) : nullptr;
    uint32_t align = 0;
    return klass && il.il2cpp_class_is_valuetype(klass) && il.il2cpp_class_value_size(klass, &align) == 16;
}
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
    void* start = FindMethodArity(skill, "DoCast", 4);
    void* end = FindMethodArity(skill, "CastEnd", 3);
    const bool castAbi = start && end && g_mIsCasting && off.skillType >= 0 && off.skillOwner >= 0 &&
        OneSlotNonFloat(il.il2cpp_method_get_param(start, 0)) && OneSlotNonFloat(il.il2cpp_method_get_param(start, 1)) &&
        OneSlotNonFloat(il.il2cpp_method_get_param(start, 2)) &&
        TypeName(il.il2cpp_method_get_param(start, 3)) == "System.Boolean" &&
        EnumIsI32(il.il2cpp_method_get_param(end, 0)) && EnumIsI32(il.il2cpp_method_get_param(end, 1)) &&
        OneSlotNonFloat(il.il2cpp_method_get_param(end, 2)) &&
        TypeName(il.il2cpp_method_get_return_type(start)) == "System.Void" &&
        TypeName(il.il2cpp_method_get_return_type(end)) == "System.Void";
    if (!castAbi) LogF("[ABI] Skill.DoCast/CastEnd signature changed: no cast events");
    else if (InstallProbe<CastStartHook>(start, 6, OnCastStart, "void(self,Action*,Action*,Nullable slot,bool,MethodInfo*)",
                 "Skill.DoCast") == APH_OK &&
             InstallProbe<CastEndHook>(end, 7, OnCastEnd, "void(self,i32,i32,Nullable slot,MethodInfo*)",
                 "Skill.CastEnd") == APH_OK)
        g_provides |= EFB_PROVIDES_SKILL_CASTS;
    LogF("optional probes: provides=0x%x", g_provides);
    return APH_OK;
}

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
    g_detail.clear();
    g_detailSet.clear();
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
// ============================================================================
// Basic settings (Manager: plugin\config\applepie.endfield.data_bridge.ini)
// ============================================================================
APH_ConfigApi g_configApi{};
bool g_logOpenedOnce = false;
// Opens or closes plugin\EndfieldDataBridge_log.txt. Notifier or Attach thread.
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
// Optional: an older Manager without basic settings simply leaves debug off.
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
} // namespace

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
