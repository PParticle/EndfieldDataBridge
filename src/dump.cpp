// Debug metadata dump (EndfieldDataBridge_dump.txt), written once at enable
// when the "debug" setting is on.
#include "bridge.h"

#include <cctype>
#include <cstdarg>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace bridge {
FILE* g_out = nullptr;
void Out(const char* fmt, ...) {
    if (!g_out) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_out, fmt, args);
    va_end(args);
}
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
// Detail queue. `follow` is how many more levels of field types to expand
// from this class; containers (System.Collections.Generic, ObjectPtr)
// and arrays pass it through unchanged so List<Buff> reaches Buff.
struct Detail { void* klass; int follow; };
std::vector<Detail> g_detail;
std::unordered_map<void*, size_t> g_detailIndex;
constexpr size_t MAX_DETAIL = 4000;
size_t g_detailSkipped = 0;
void QueueDetail(void* klass, int follow = 0) {
    if (!klass) return;
    if (auto it = g_detailIndex.find(klass); it != g_detailIndex.end()) {
        // Not yet dumped classes may still gain depth; dumped ones keep theirs.
        int& f = g_detail[it->second].follow;
        if (follow > f) f = follow;
        return;
    }
    if (g_detail.size() >= MAX_DETAIL) { ++g_detailSkipped; return; }
    g_detailIndex.emplace(klass, g_detail.size());
    g_detail.push_back({klass, follow});
}
constexpr int IL2CPP_TYPE_CLASS = 0x12, IL2CPP_TYPE_VALUETYPE = 0x11, IL2CPP_TYPE_SZARRAY = 0x1d,
    IL2CPP_TYPE_ARRAY = 0x14, IL2CPP_TYPE_GENERICINST = 0x15;
bool IsTransparent(void* klass) {
    const char* ns = OuterNamespace(klass);
    const char* name = il.il2cpp_class_get_name(klass);
    return !strcmp(ns, "System.Collections.Generic") ||
        (name && !strncmp(name, "ObjectPtr`", 10) && !strcmp(ns, "Beyond"));
}
// Field types worth expanding: gameplay types and the containers holding
// them. UI widgets, Unity and other System types stay leaves.
void FollowFieldType(void* type, int follow) {
    if (!type || follow <= 0) return;
    const int t = il.il2cpp_type_get_type(type);
    if (t != IL2CPP_TYPE_CLASS && t != IL2CPP_TYPE_VALUETYPE && t != IL2CPP_TYPE_GENERICINST &&
        t != IL2CPP_TYPE_SZARRAY && t != IL2CPP_TYPE_ARRAY) return;
    void* klass = il.il2cpp_class_from_type(type);
    if (klass && (t == IL2CPP_TYPE_SZARRAY || t == IL2CPP_TYPE_ARRAY))
        klass = il.il2cpp_class_get_element_class(klass);
    if (!klass || il.il2cpp_class_is_enum(klass)) return;
    if (IsTransparent(klass)) { QueueDetail(klass, follow); return; }
    if (!IsBeyond(klass) || !strncmp(OuterNamespace(klass), "Beyond.UI", 9)) return;
    QueueDetail(klass, follow - 1);
}
// First bytes of a getter: trivial ones are a field load + ret, which the
// design work uses to tell backing-field getters from computed ones.
bool CopyCode(const void* src, uint8_t* dst, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void DumpFields(void* klass, bool followEnums, int follow) {
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
        if (!(flags & FIELD_STATIC)) FollowFieldType(type, follow);
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
        // Instance getters: append the first code bytes ("| 48 8B 81 ...").
        std::string code;
        uint8_t bytes[24];
        if (ptr && count == 0 && !(flags & METHOD_STATIC) && !il.il2cpp_method_is_generic(m) &&
            CopyCode(reinterpret_cast<const void*>(ptr), bytes, sizeof(bytes))) {
            code = " |";
            char hex[4];
            for (uint8_t b : bytes) { snprintf(hex, sizeof(hex), " %02X", b); code += hex; }
        }
        if (ptr) Out("    RVA 0x%08llX%s %s)%s\n", static_cast<unsigned long long>(ptr - base),
            (flags & METHOD_STATIC) ? " static" : "", line.c_str(), code.c_str());
        else Out("    RVA -%s %s)\n", (flags & METHOD_STATIC) ? " static" : "", line.c_str());
    }
}
void DumpClass(void* klass, int follow) {
    const bool isEnum = il.il2cpp_class_is_enum(klass);
    void* parent = il.il2cpp_class_get_parent(klass);
    Out("\n== %s : %s  size=0x%X%s\n", FullName(klass).c_str(),
        parent ? FullName(parent).c_str() : "-", il.il2cpp_class_instance_size(klass),
        isEnum ? " enum" : il.il2cpp_class_is_valuetype(klass) ? " struct" : "");
    Out("  fields:\n");
    DumpFields(klass, !isEnum, follow);
    if (isEnum) return;
    // Inherited instance fields, up to the first engine/system base type.
    for (void* p = parent; p && IsBeyond(p); p = il.il2cpp_class_get_parent(p)) {
        Out("  inherited from %s:\n", FullName(p).c_str());
        DumpFields(p, true, follow);
    }
    Out("  methods:\n");
    DumpMethods(klass);
}
// Explicit targets and how many levels of field types to expand from each.
struct Target { const char* ns; const char* name; int follow; };
const Target kTargets[] = {
    {"Beyond.Gameplay.Core", "AbilitySystem", 2}, {"Beyond.Gameplay.Core", "Buff", 2},
    {"Beyond.Gameplay.Core", "Skill", 2}, {"Beyond.Gameplay.Core", "BattleManager", 2},
    {"Beyond.Gameplay.Core", "Attributes", 2}, {"Beyond.Gameplay.Core", "SquadManager", 1},
    {"Beyond.Gameplay.Core", "EntityManager", 1}, {"Beyond.Gameplay.Core", "Entity", 1},
    {"Beyond.Gameplay.Core", "BuffData", 1}, {"Beyond.Gameplay.Core", "BuffIconConfig", 0},
    {"Beyond.Gameplay.Core", "FactionIndex", 0}, {"Beyond.Gameplay", "SkillSetting", 1},
    {"Beyond.UI", "UIHeadBar", 1}, {"Beyond.UI", "HeadBarCtrl", 1}, {"Beyond.UI", "SquadIcon", 1},
    {"Beyond.UI", "AtbNode", 1}, {"Beyond.UI", "MainCharHpBar", 0}, {"Beyond.UI", "GPUIBuffNode", 0},
    {"Beyond.UI", "UIBuffNode", 0}, {"Beyond.UI", "UIBuffCell", 0}, {"Beyond.UI", "GPUIBuffCell", 0},
    {"Beyond.UI", "SkillButton", 0}, {"Beyond.UI", "ComboSkillPanel", 0},
};
// Static fields / static getters that look like a way in to live objects.
bool LooksLikeRoot(const char* member, const std::string& type) {
    static const char* const kWords[] = {"instance", "Instance", "manager", "Manager", "world",
        "World", "current", "Current", "main", "Main", "s_inst", "singleton", "Singleton"};
    for (const char* w : kWords) if (strstr(member, w) || type.find(w) != std::string::npos) return true;
    return false;
}
struct Root { void* klass; std::string what; void* type; };
// Class-name keywords (Beyond.* only) for the compact index.
const char* const kIndexWords[] = {
    "Buff", "HpBar", "HPBar", "HeadBar", "HeadInfo", "Enemy", "Boss", "Monster",
    "Team", "Squad", "Battle", "SkillPanel", "SkillButton", "UltimateSp", "Energy",
};
// Class-name keywords (Beyond.UI) that get a full dump.
const char* const kUiDetailWords[] = {"HpBar", "HPBar", "HeadBar", "Boss", "BuffNode", "BuffCell"};
struct Hit { void* klass; std::string what; };
// Static members of Beyond.Gameplay* classes typed as Beyond classes.
void ScanRoots(void* klass, std::vector<Root>& roots) {
    if (strncmp(OuterNamespace(klass), "Beyond.Gameplay", 15)) return;
    void* it = nullptr;
    while (void* f = il.il2cpp_class_get_fields(klass, &it)) {
        const int flags = il.il2cpp_field_get_flags(f);
        const char* n = il.il2cpp_field_get_name(f);
        void* type = il.il2cpp_field_get_type(f);
        if (!n || !type || !(flags & FIELD_STATIC) || (flags & FIELD_LITERAL)) continue;
        const int t = il.il2cpp_type_get_type(type);
        if (t != IL2CPP_TYPE_CLASS && t != IL2CPP_TYPE_GENERICINST) continue;
        const std::string tn = TypeName(type);
        if (tn.compare(0, 7, "Beyond.") || !LooksLikeRoot(n, tn)) continue;
        roots.push_back({klass, "field " + tn + " " + n, type});
    }
    it = nullptr;
    while (void* m = il.il2cpp_class_get_methods(klass, &it)) {
        uint32_t impl = 0;
        const char* n = il.il2cpp_method_get_name(m);
        if (!n || strncmp(n, "get_", 4) || il.il2cpp_method_get_param_count(m) ||
            !(il.il2cpp_method_get_flags(m, &impl) & METHOD_STATIC)) continue;
        void* type = il.il2cpp_method_get_return_type(m);
        const std::string tn = TypeName(type);
        if (tn.compare(0, 7, "Beyond.") || !LooksLikeRoot(n, tn)) continue;
        roots.push_back({klass, "static " + tn + " " + n + "()", type});
    }
}
void ScanClass(void* klass, std::vector<Hit>& hits, std::vector<void*>& index, std::vector<Root>& roots) {
    const char* name = il.il2cpp_class_get_name(klass);
    if (!name || !IsBeyond(klass)) return;
    ScanRoots(klass, roots);
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
            for (const char* k : kOuter) if (!strcmp(o, k)) { QueueDetail(klass, 1); break; }
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
    std::vector<Root> roots;
    size_t classes = 0;
    Out("# EndfieldDataBridge metadata dump\n# GameAssembly base=%p\n", g_gameAssembly);
    for (size_t a = 0; a < count; ++a) {
        void* image = il.il2cpp_assembly_get_image(assemblies[a]);
        if (!image) continue;
        const char* imageName = il.il2cpp_image_get_name(image);
        for (const auto& t : kTargets)
            if (void* k = il.il2cpp_class_from_name(image, t.ns, t.name)) QueueDetail(k, t.follow);
        const size_t n = il.il2cpp_image_get_class_count(image);
        Out("# image %s classes=%zu\n", imageName ? imageName : "?", n);
        for (size_t c = 0; c < n; ++c) {
            if ((c & 255) == 0 && !WorkOpen()) return false;
            if (void* k = il.il2cpp_image_get_class(image, c)) { ScanClass(k, hits, index, roots); ++classes; }
        }
    }

    Out("\n######## SP/ATB hits (%zu)\n", hits.size());
    for (const auto& h : hits) Out("%s :: %s\n", FullName(h.klass).c_str(), h.what.c_str());
    Out("\n######## Class index (%zu)\n", index.size());
    for (void* k : index) {
        void* parent = il.il2cpp_class_get_parent(k);
        Out("%s : %s\n", FullName(k).c_str(), parent ? FullName(parent).c_str() : "-");
    }
    Out("\n######## Static roots (%zu)\n", roots.size());
    for (const auto& r : roots) Out("%s :: %s\n", FullName(r.klass).c_str(), r.what.c_str());
    Out("\n######## Details\n");
    // Dumping queues field types; iterate by index. Targets expand first,
    // then the static roots' types, so roots cannot starve the targets.
    size_t i = 0;
    auto drain = [&] {
        for (; i < g_detail.size(); ++i) {
            if (!WorkOpen()) return false;
            const Detail d = g_detail[i];
            DumpClass(d.klass, d.follow);
        }
        return true;
    };
    if (!drain()) return false;
    for (const auto& r : roots)
        if (void* k = il.il2cpp_class_from_type(r.type)) QueueDetail(k, 1);
    if (!drain()) return false;
    if (g_detailSkipped) Out("\n!! detail cap %zu reached, %zu classes skipped\n", MAX_DETAIL, g_detailSkipped);
    LogF("scanned %zu classes, %zu hits, %zu indexed, %zu roots, %zu detailed, %zu skipped", classes,
        hits.size(), index.size(), roots.size(), g_detail.size(), g_detailSkipped);
    Out("\n# END\n");
    return true;
}
void ClearDump() {
    g_detail.clear();
    g_detailIndex.clear();
    g_detailSkipped = 0;
}
bool DumpAllGuarded(void** assemblies, size_t count) {
    g_inMetadata.fetch_add(1);
    bool ok = false;
    __try { ok = DumpAll(assemblies, count); }
    __except (EXCEPTION_EXECUTE_HANDLER) { LogF("dump faulted: 0x%08lX", GetExceptionCode()); }
    g_inMetadata.fetch_sub(1);
    return ok;
}
} // namespace bridge
