#include <windows.h>
#include <cstdio>

#include "applepie_client.h"

#if !defined(_M_X64)
#error EndfieldDataBridge requires an x64 MSVC build.
#endif

namespace {
applepie::Client g_client;

void Log(const char* event, uint64_t epoch, APH_Result result) noexcept {
    char line[256]{};
    std::snprintf(line, sizeof(line),
        "[EndfieldDataBridge] %s owner=%llu epoch=%llu result=%ld\n",
        event, static_cast<unsigned long long>(g_client.owner()),
        static_cast<unsigned long long>(epoch), static_cast<long>(result));
    OutputDebugStringA(line);
}

void APH_CALL Disabling(APH_Owner owner, uint64_t epoch, void*) {
    if (owner == g_client.owner()) Log("disabling", epoch, APH_OK);
}

APH_Result APH_CALL Restore(APH_Owner owner, const APH_Cleanup* cleanup, void*) {
    if (owner != g_client.owner() || !cleanup || cleanup->size < sizeof(*cleanup))
        return APH_E_ARGUMENT;

    // No game hooks, workers or owned resources have been introduced yet.
    Log("restored", 0, APH_OK);
    return APH_OK;
}
} // namespace

extern "C" __declspec(dllexport) const APH_Manifest APH_PluginManifest = {
    sizeof(APH_Manifest), APH_ABI_MAJOR, APH_ABI_MINOR, APH_ARCH_X64,
    APH_PLUGIN_LIFECYCLE | APH_PLUGIN_LATE_LOAD,
    APH_CAP_CORE, 0,
    "endfield.data_bridge", "Endfield Data Bridge"
};

extern "C" __declspec(dllexport) APH_Result APH_CALL APH_Attach(
    const APH_AttachArgs* args) {
    const APH_Result attached = g_client.Attach(args);
    if (attached != APH_OK) return attached;
    if (g_client.lifecycle()) return APH_OK;

    const APH_LifecycleDesc lifecycle{
        sizeof(APH_LifecycleDesc), 10000, Disabling, Restore, nullptr
    };
    const APH_Result result = g_client.RegisterLifecycle(lifecycle);
    Log("attached", 0, result);
    return result;
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

    Log("enabled", epoch, APH_OK);
    return APH_OK;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
