#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "applepie_client.h"
#include "hub_core.h"

#define CHECK(expression) do { \
    if (!(expression)) { \
        std::printf("[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        ExitProcess(1); \
    } \
} while (false)

namespace {
APH_OwnerStatus State(applepie::Client& client, uint32_t expected) {
    const ULONGLONG deadline = GetTickCount64() + 5000;
    APH_OwnerStatus status{};
    for (;;) {
        CHECK(client.Status(status) == APH_OK);
        if (status.state == expected) return status;
        if (APH_OwnerHasFault(status.state) || GetTickCount64() >= deadline) {
            std::printf("[FAIL] state=%u expected=%u result=%ld\n",
                status.state, expected, static_cast<long>(status.result));
            ExitProcess(1);
        }
        // The SDK exposes status queries, but no lifecycle-completion event.
        // Only this offline host waits with a bounded status check.
        Sleep(1);
    }
}

bool WaitFor(bool (*ready)(const wchar_t*), const wchar_t* path) {
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (!ready(path)) {
        if (GetTickCount64() >= deadline) return false;
        Sleep(1); // saves complete on a Manager worker; no completion event here
    }
    return true;
}
bool Exists(const wchar_t* path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }
// Deletable means the plugin no longer holds the log open.
bool Deleted(const wchar_t* path) { return DeleteFileW(path) || GetLastError() == ERROR_FILE_NOT_FOUND; }
// Saves debug and sample_interval; interval nullptr keeps the stored one.
void SaveConfig(applepie::Client& client, const APH_ConfigApi& config, bool debug, const char* interval) {
    APH_ConfigSnapshot state{};
    APH_ConfigValue values[2]{};
    const ULONGLONG deadline = GetTickCount64() + 5000;
    for (;;) {
        CHECK(config.snapshot(client.owner(), sizeof(state), &state, 2, values) == APH_OK);
        if (!state.pending) break;
        CHECK(GetTickCount64() < deadline);
        Sleep(1);
    }
    CHECK(state.field_count == 2 && values[0].type == APH_CONFIG_BOOL && values[1].type == APH_CONFIG_ENUM);
    values[0].integer = debug;
    if (interval) strcpy_s(values[1].text, interval);
    const APH_Result r = config.save(client.owner(), state.revision, 2, values);
    CHECK(r == APH_PENDING || r == APH_OK);
}

uint64_t Enable(applepie::Client& client, APH_EnableFn enable) {
    const uint64_t epoch = hub::begin_enable(client.owner());
    CHECK(epoch != 0);
    const APH_Result result = hub::enable(client.owner(), enable, epoch);
    CHECK(result == APH_OK);
    CHECK(hub::finish_enable(client.owner(), result, epoch) == APH_OK);
    CHECK(State(client, APH_OWNER_ACTIVE).activation_epoch == epoch);
    return epoch;
}
} // namespace

int main() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    CHECK(length > 0 && length < MAX_PATH);
    wchar_t* filename = std::wcsrchr(path, L'\\');
    CHECK(filename);
    ++filename;
    CHECK(wcscpy_s(filename, MAX_PATH - (filename - path),
        L"EndfieldDataBridge.dll") == 0);

    // Settings and log live next to the DLL; start from defaults.
    wchar_t log[MAX_PATH]{}, ini[MAX_PATH]{};
    wcscpy_s(log, path);
    wcscpy_s(ini, path);
    wcscpy_s(std::wcsrchr(log, L'\\') + 1, MAX_PATH - (std::wcsrchr(log, L'\\') + 1 - log), L"EndfieldDataBridge_log.txt");
    wcscpy_s(std::wcsrchr(ini, L'\\') + 1, MAX_PATH - (std::wcsrchr(ini, L'\\') + 1 - ini),
        L"config\\applepie.endfield.data_bridge.ini");
    DeleteFileW(ini);
    CHECK(Deleted(log));

    // Keep the DLL resident: Core retains its registered lifecycle callbacks.
    const HMODULE dll = LoadLibraryW(path);
    CHECK(dll);
    const auto* manifest = reinterpret_cast<const APH_Manifest*>(
        GetProcAddress(dll, "APH_PluginManifest"));
    const auto attach = reinterpret_cast<APH_AttachFn>(
        GetProcAddress(dll, "APH_Attach"));
    const auto enable = reinterpret_cast<APH_EnableFn>(
        GetProcAddress(dll, "APH_Enable"));
    CHECK(manifest && attach && enable);

    MEMORY_BASIC_INFORMATION memory{};
    CHECK(VirtualQuery(manifest, &memory, sizeof(memory)) == sizeof(memory));
    CHECK((memory.Protect & 0xff) == PAGE_READONLY);
    CHECK(manifest->size == sizeof(*manifest));
    CHECK(manifest->abi_major == APH_ABI_MAJOR);
    CHECK(manifest->required_minor == APH_ABI_MINOR);
    CHECK(manifest->architecture == APH_ARCH_X64);
    CHECK(manifest->flags == (APH_PLUGIN_LIFECYCLE | APH_PLUGIN_LATE_LOAD |
        APH_PLUGIN_MODULE_OBSERVER));
    CHECK(manifest->required_capabilities == (APH_CAP_CORE | APH_CAP_HOOKS));
    CHECK(manifest->optional_capabilities == 0);
    CHECK(std::strcmp(manifest->id, "endfield.data_bridge") == 0);
    CHECK(attach(nullptr) == APH_E_ARGUMENT);
    CHECK(enable(1, 1) == APH_E_ARGUMENT);
    std::puts("[OK] Read-only manifest, entrypoints and pre-attach guards");

    CHECK(hub::initialize() == APH_OK);
    const APH_Owner owner = hub::add_owner(*manifest, dll);
    CHECK(owner);
    CHECK(hub::attach(owner, attach) == APH_OK);

    const APH_AttachArgs args{sizeof(APH_AttachArgs), 0, owner, hub::api()};
    CHECK(attach(&args) == APH_OK);
    applepie::Client observer;
    CHECK(observer.Attach(&args) == APH_OK);
    APH_LifecycleApi lifecycle{};
    CHECK(observer.Service(lifecycle) == APH_OK);
    State(observer, APH_OWNER_ATTACHED);
    CHECK(enable(owner, 0) == APH_E_ARGUMENT);
    CHECK(enable(owner + 1, 1) == APH_E_ARGUMENT);
    CHECK(enable(owner, 1) == APH_E_STALE);
    std::puts("[OK] Official Core attach, idempotent attach and owner/epoch guards");

    APH_ConfigApi config{};
    CHECK(observer.Service(config) == APH_OK);
    APH_ConfigField field{};
    CHECK(config.field(owner, 0, sizeof(field), &field) == APH_OK);
    CHECK(!std::strcmp(field.key, "debug") && field.type == APH_CONFIG_BOOL && field.initial.integer == 0);
    CHECK(config.field(owner, 1, sizeof(field), &field) == APH_OK);
    CHECK(!std::strcmp(field.key, "sample_interval") && field.type == APH_CONFIG_ENUM &&
        field.option_count == 4 && !std::strcmp(field.initial.text, "50ms"));
    const char* intervals[] = {"50ms", "100ms", "150ms", "200ms"};
    for (uint32_t i = 0; i < 4; ++i) {
        APH_ConfigOption option{};
        CHECK(config.option(owner, 1, i, sizeof(option), &option) == APH_OK && !std::strcmp(option.value, intervals[i]));
    }
    CHECK(!Exists(log)); // debug defaults to off: no file at all
    SaveConfig(observer, config, true, nullptr);
    CHECK(WaitFor(Exists, log));
    SaveConfig(observer, config, false, nullptr);
    CHECK(WaitFor(Deleted, log));
    {
        // An unlisted interval is rejected by the host before it reaches the plugin.
        APH_ConfigSnapshot state{};
        APH_ConfigValue values[2]{};
        CHECK(config.snapshot(owner, sizeof(state), &state, 2, values) == APH_OK && !state.pending);
        strcpy_s(values[1].text, "10ms");
        CHECK(config.save(owner, state.revision, 2, values) == APH_E_ARGUMENT);
    }
    std::puts("[OK] Debug and sample-interval settings registered; debug toggles the log file at runtime");

    const uint64_t first = Enable(observer, enable);
    CHECK(enable(owner, first) == APH_E_STALE);
    CHECK(lifecycle.disable(owner) == APH_PENDING);
    State(observer, APH_OWNER_DISABLED);
    CHECK(enable(owner, first) == APH_E_STALE);
    std::puts("[OK] Enable -> active -> disable -> restore -> disabled");

    const uint64_t second = Enable(observer, enable);
    CHECK(second > first);
    CHECK(lifecycle.complete_enable(owner, first, APH_OK) == APH_E_STALE);
    CHECK(enable(owner, first) == APH_E_STALE);
    CHECK(lifecycle.disable(owner) == APH_PENDING);
    State(observer, APH_OWNER_DISABLED);
    std::puts("[OK] Re-enable with a new epoch, reject stale epoch, disable again");
    std::puts("[PASS] EndfieldDataBridge offline lifecycle verification");
    return 0;
}
