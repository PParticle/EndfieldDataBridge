// Offline data-path tests: the plugin source is compiled into this exe so the
// tracker runs against fake Buff / AbilitySystem objects laid out at known
// offsets, with a fake frame counter and clock. No IL2CPP or game needed.
#include "../src/plugin.cpp"
#include "../include/endfield_bridge_reader.h"

#define CHECK(expression) do { \
    if (!(expression)) { \
        std::printf("[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        ExitProcess(1); \
    } \
} while (false)

namespace test {
int32_t g_frame = 1;
int64_t g_now = 1;
int32_t Frame() { return g_frame; }
int64_t Clock() { return g_now; }
void Advance(double seconds) { g_now += int64_t(seconds * double(g_qpcFreq)); }

// Managed System.String: length at +0x10, UTF-16 chars at +0x14.
struct FakeString { alignas(8) char bytes[0x14 + 2 * 64]{}; };
void SetString(FakeString& s, const char* text) {
    memset(s.bytes, 0, sizeof(s.bytes));
    const int32_t n = int32_t(strlen(text));
    memcpy(s.bytes + 0x10, &n, 4);
    for (int i = 0; i < n; ++i) reinterpret_cast<wchar_t*>(s.bytes + 0x14)[i] = wchar_t(text[i]);
}
struct FakeAbility { alignas(8) char bytes[0x40]{}; };
struct FakeBuff { alignas(8) char bytes[0x80]{}; FakeString id; };
template<class T> void Put(void* obj, int offset, T value) { memcpy(static_cast<char*>(obj) + offset, &value, sizeof(T)); }

void SetUpOffsets() {
    off = Offsets{};
    off.buffId = 0x10; off.buffDuration = 0x18; off.buffLifeTime = 0x1C; off.buffPassed = 0x20;
    off.buffEnhance = 0x24; off.buffOwner = 0x28; off.buffSource = 0x38; off.buffUid = 0x48;
    off.buffLifeType = 0x4C; off.buffData = 0x50; off.buffFinished = 0x58;
    off.asUid = 0x10;
}
void MakeAbility(FakeAbility& a, uint32_t uid) { Put<uint32_t>(a.bytes, off.asUid, uid); }
void MakeBuff(FakeBuff& b, const char* id, uint32_t uid, FakeAbility& owner, uint32_t ownerUid,
    FakeAbility& source, uint32_t sourceUid, float duration) {
    memset(b.bytes, 0, sizeof(b.bytes));
    SetString(b.id, id);
    Put<void*>(b.bytes, off.buffId, &b.id);
    Put<float>(b.bytes, off.buffDuration, duration);
    Put<float>(b.bytes, off.buffLifeTime, duration);
    Put<float>(b.bytes, off.buffPassed, 0.0f);
    Put<int32_t>(b.bytes, off.buffEnhance, 1);
    Put<ObjPtr>(b.bytes, off.buffOwner, ObjPtr{owner.bytes, ownerUid, 0});
    Put<ObjPtr>(b.bytes, off.buffSource, ObjPtr{source.bytes, sourceUid, 0});
    Put<uint32_t>(b.bytes, off.buffUid, uid);
}
ObjPtr Ref(FakeBuff& b) { return ObjPtr{b.bytes, At<uint32_t>(b.bytes, off.buffUid), 0}; }
void Report(FakeBuff& b, bool applied, int32_t reason = EFB_FINISH_OTHER) {
    ProbeBuff("test", Ref(b), applied, reason);
}
void NextFrame() { ++g_frame; Advance(1.0 / 60); Tick(); }

uint64_t g_seen = 0;
std::vector<EFB_Event> NewEvents() {
    std::vector<EFB_Event> out;
    for (uint64_t n = g_seen + 1; n <= g_view->event_head; ++n) out.push_back(g_view->events[(n - 1) % EFB_MAX_EVENTS]);
    g_seen = g_view->event_head;
    return out;
}
const EFB_Buff* FindKey(uint32_t key) {
    for (const auto& b : g_view->buffs) if (b.key == key) return &b;
    return nullptr;
}
// Each test starts like a first enable.
void Reset() {
    g_sessionsStarted = 0;
    StartSession();
    g_seen = g_view->event_head;
}
void TestAddRefreshFinish() {
    Reset();
    FakeAbility ikut{}, enemy{};
    MakeAbility(ikut, 11); MakeAbility(enemy, 22);
    FakeBuff a{}, a2{}, a3{};
    MakeBuff(a, "buff_wpn_sword_0012_atk_up", 100, ikut, 11, ikut, 11, 20);
    Report(a, true);
    CHECK(NewEvents().empty()); // settled on the next frame
    NextFrame();
    auto ev = NewEvents();
    CHECK(ev.size() == 1 && ev[0].type == EFB_EVENT_BUFF_ADDED && ev[0].buff_slot >= 0 && ev[0].frame == g_frame - 1);
    const uint32_t key = ev[0].key;
    CHECK(key && FindKey(key) && FindKey(key)->instance_uid == 100 && g_view->buff_count == 1);
    CHECK(!strcmp(FindKey(key)->buff_id, "buff_wpn_sword_0012_atk_up"));

    // Refresh as finish(old) + add(new) in one frame keeps the key.
    MakeBuff(a2, "buff_wpn_sword_0012_atk_up", 101, ikut, 11, ikut, 11, 20);
    Report(a, false);
    Report(a2, true);
    CHECK(FindKey(key)); // still visible while the frame settles
    NextFrame();
    ev = NewEvents();
    CHECK(ev.size() == 1 && ev[0].type == EFB_EVENT_BUFF_REFRESHED && ev[0].key == key);
    CHECK(FindKey(key)->instance_uid == 101 && g_view->buff_count == 1);

    // Same, in the other order.
    MakeBuff(a3, "buff_wpn_sword_0012_atk_up", 102, ikut, 11, ikut, 11, 20);
    Report(a3, true);
    Report(a2, false);
    NextFrame();
    ev = NewEvents();
    CHECK(ev.size() == 1 && ev[0].type == EFB_EVENT_BUFF_REFRESHED && ev[0].key == key);

    // A different source is a different buff, not a refresh.
    FakeBuff b{};
    MakeBuff(b, "buff_wpn_sword_0012_atk_up", 200, ikut, 11, enemy, 22, 20);
    Report(b, true);
    NextFrame();
    ev = NewEvents();
    CHECK(ev.size() == 1 && ev[0].type == EFB_EVENT_BUFF_ADDED && ev[0].key != key);
    const uint32_t keyB = ev[0].key;

    // Duplicate reports from a second panel produce nothing.
    Report(b, true);
    NextFrame();
    CHECK(NewEvents().empty());

    // In-place refresh: passed_time drops on the same object.
    Put<float>(a3.bytes, off.buffPassed, 5.0f);
    NextFrame();
    CHECK(NewEvents().empty());
    Put<float>(a3.bytes, off.buffPassed, 0.0f);
    NextFrame();
    ev = NewEvents();
    CHECK(ev.size() == 1 && ev[0].type == EFB_EVENT_BUFF_REFRESHED && ev[0].key == key);

    // Every stack change is an event.
    Put<int32_t>(a3.bytes, off.buffEnhance, 2);
    NextFrame();
    Put<int32_t>(a3.bytes, off.buffEnhance, 3);
    NextFrame();
    ev = NewEvents();
    CHECK(ev.size() == 2 && ev[0].type == EFB_EVENT_BUFF_ENHANCED && ev[0].enhance_cnt == 2 &&
        ev[1].enhance_cnt == 3 && FindKey(key)->enhance_cnt == 3);

    // Finish: reported reason, at the frame the game reported it.
    const int32_t finishFrame = g_frame;
    Report(a3, false, EFB_FINISH_LIFETIME);
    Report(a3, false, EFB_FINISH_LIFETIME); // second panel
    NextFrame();
    ev = NewEvents();
    CHECK(ev.size() == 1 && ev[0].type == EFB_EVENT_BUFF_FINISHED && ev[0].key == key &&
        ev[0].finish_reason == EFB_FINISH_LIFETIME && ev[0].frame == finishFrame);
    CHECK(!FindKey(key) && FindKey(keyB) && g_view->buff_count == 1);
    // A late duplicate after settlement does not mark the table partial.
    Report(a3, false);
    NextFrame();
    CHECK(!(g_view->flags & EFB_FLAG_BUFFS_PARTIAL) && NewEvents().empty());
    std::puts("[OK] Add, finish+add refresh (both orders), in-place refresh, stacks, finish, duplicates");
}

void TestSameFrameAndLost() {
    Reset();
    FakeAbility ikut{};
    MakeAbility(ikut, 11);
    FakeBuff flash{}, reused{}, orphan{};
    MakeBuff(flash, "buff_common_fire_fire_burning_triggered", 300, ikut, 11, ikut, 11, 0);
    Report(flash, true);
    Report(flash, false, EFB_FINISH_EARLY);
    NextFrame();
    auto ev = NewEvents();
    CHECK(ev.size() == 2 && ev[0].type == EFB_EVENT_BUFF_ADDED && ev[1].type == EFB_EVENT_BUFF_FINISHED &&
        ev[0].key == ev[1].key && ev[0].buff_slot == -1 && ev[1].finish_reason == EFB_FINISH_EARLY);
    CHECK(g_view->buff_count == 0);

    // The game reused the object for another buff: finished as LOST.
    MakeBuff(reused, "buff_a", 310, ikut, 11, ikut, 11, 10);
    Report(reused, true);
    NextFrame();
    NewEvents();
    Put<uint32_t>(reused.bytes, off.buffUid, 999);
    NextFrame();
    NextFrame();
    ev = NewEvents();
    CHECK(ev.size() == 1 && ev[0].type == EFB_EVENT_BUFF_FINISHED && ev[0].finish_reason == EFB_FINISH_LOST);

    // The owner despawned.
    FakeAbility enemy{};
    MakeAbility(enemy, 22);
    MakeBuff(orphan, "buff_b", 320, enemy, 22, ikut, 11, 10);
    Report(orphan, true);
    NextFrame();
    NewEvents();
    MakeAbility(enemy, 0);
    NextFrame();
    NextFrame();
    ev = NewEvents();
    CHECK(ev.size() == 1 && ev[0].type == EFB_EVENT_BUFF_FINISHED && ev[0].finish_reason == EFB_FINISH_LOST);

    // A finish for a buff never seen added: tracking started late.
    FakeBuff unknown{};
    MakeBuff(unknown, "buff_c", 330, ikut, 11, ikut, 11, 10);
    Report(unknown, false);
    NextFrame();
    CHECK((g_view->flags & EFB_FLAG_BUFFS_PARTIAL) && NewEvents().empty());
    std::puts("[OK] Same-frame add+finish, reused object, despawned owner, late-start detection");
}

void TestDilation() {
    Reset();
    FakeAbility ikut{};
    MakeAbility(ikut, 11);
    FakeBuff b{};
    MakeBuff(b, "buff_d", 400, ikut, 11, ikut, 11, 30);
    Report(b, true);
    NextFrame();
    const uint32_t key = NewEvents()[0].key;
    float passed = 0;
    for (int i = 0; i < 30; ++i) { passed += 0.5f / 60; Put<float>(b.bytes, off.buffPassed, passed); NextFrame(); }
    const float slow = FindKey(key)->time_dilation;
    CHECK(slow > 0.4f && slow < 0.6f);
    for (int i = 0; i < 30; ++i) NextFrame();
    CHECK(FindKey(key)->time_dilation == 0);
    for (int i = 0; i < 30; ++i) { passed += 1.0f / 60; Put<float>(b.bytes, off.buffPassed, passed); NextFrame(); }
    CHECK(FindKey(key)->time_dilation == 1);
    CHECK(FindKey(key)->sampled_at == g_now);
    CHECK(NewEvents().empty()); // timers moving is not an event
    std::puts("[OK] Time dilation follows game time (0.5x, frozen, 1x) without events");
}

void TestOverflow() {
    Reset();
    FakeAbility ikut{};
    MakeAbility(ikut, 11);
    static FakeBuff many[EFB_MAX_BUFFS + 3];
    for (int i = 0; i < EFB_MAX_BUFFS + 3; ++i) {
        MakeBuff(many[i], "buff_many", 1000 + i, ikut, 11, ikut, 11, 10);
        Report(many[i], true);
    }
    NextFrame();
    CHECK(g_view->buff_count == EFB_MAX_BUFFS && g_view->buffs_dropped == 3 &&
        (g_view->flags & EFB_FLAG_BUFFS_DROPPED));
    // A finish for a dropped buff is not a missed add.
    Report(many[EFB_MAX_BUFFS + 1], false);
    NextFrame();
    CHECK(!(g_view->flags & EFB_FLAG_BUFFS_PARTIAL));
    // More events than the ring holds: the reader can detect the gap.
    for (int i = 0; i < EFB_MAX_BUFFS; ++i) Report(many[i], false);
    NextFrame();
    Report(many[EFB_MAX_BUFFS], true);
    NextFrame();
    CHECK(g_view->buff_count == 1 && g_view->event_head - g_seen > EFB_MAX_EVENTS);
    std::puts("[OK] Table overflow counted and flagged; event ring overrun detectable");
}

void TestSessionAndLiveness() {
    Reset();
    const uint32_t session = g_view->session;
    CHECK(g_view->active && !(g_view->flags & EFB_FLAG_BUFFS_PARTIAL)); // first enable of the process
    StartSession();
    CHECK(g_view->session == session + 1 && (g_view->flags & EFB_FLAG_BUFFS_PARTIAL)); // re-enable mid-game
    EFB_Shared copy{};
    CHECK(efb::Snapshot(g_view, copy) && efb::LayoutMatches(copy));
    CHECK(efb::WriterAlive(copy));
    copy.writer_start_time += 1; // same pid, different process
    CHECK(!efb::WriterAlive(copy));
    CHECK(efb::Snapshot(g_view, copy));
    CHECK(!efb::TeamFresh(copy, efb::Now()) && efb::Check(copy, efb::Now(), true) == efb::Status::Stale);
    EndSession();
    CHECK(efb::Snapshot(g_view, copy) && efb::Check(copy, efb::Now(), true) == efb::Status::Disabled);
    std::puts("[OK] Session restart, partial flag on re-enable, writer identity, freshness, disabled");
}

// A reader thread must never see a torn snapshot while the game thread writes.
volatile LONG g_stopReader = 0;
DWORD WINAPI TornReader(void* arg) {
    auto* failures = static_cast<volatile LONG*>(arg);
    static EFB_Shared copy;
    while (!g_stopReader) {
        if (!efb::Snapshot(g_view, copy)) continue;
        // Writer invariant: every live entry carries enhance_cnt == buff_count.
        for (const auto& b : copy.buffs)
            if (b.key && uint32_t(b.enhance_cnt) != copy.buff_count) InterlockedIncrement(failures);
        uint32_t n = 0;
        for (const auto& b : copy.buffs) n += b.key != 0;
        if (n != copy.buff_count) InterlockedIncrement(failures);
    }
    return 0;
}
// Timers follow the configured interval; adds and finishes still settle every frame.
void TestSampleInterval() {
    Reset();
    g_sampleIntervalMs = 100;
    FakeAbility ikut{};
    MakeAbility(ikut, 11);
    FakeBuff b{};
    MakeBuff(b, "buff_e", 500, ikut, 11, ikut, 11, 30);
    NextFrame(); // first sample of the session
    Report(b, true);
    NextFrame();
    auto ev = NewEvents();
    CHECK(ev.size() == 1 && ev[0].type == EFB_EVENT_BUFF_ADDED); // not delayed by the interval
    const uint32_t key = ev[0].key;
    const int64_t first = FindKey(key)->sampled_at;
    Put<int32_t>(b.bytes, off.buffEnhance, 2);
    int frames = 0;
    while (FindKey(key)->sampled_at == first) { NextFrame(); ++frames; CHECK(frames < 20); }
    // 100 ms at 60 fps: the next read lands on the 5th-7th frame.
    CHECK(frames >= 5 && frames <= 7 && FindKey(key)->enhance_cnt == 2);
    ev = NewEvents();
    CHECK(ev.size() == 1 && ev[0].type == EFB_EVENT_BUFF_ENHANCED);
    CHECK(g_view->sample_interval_ms == 100);
    Report(b, false, EFB_FINISH_EARLY);
    NextFrame();
    ev = NewEvents();
    CHECK(ev.size() == 1 && ev[0].type == EFB_EVENT_BUFF_FINISHED);
    g_sampleIntervalMs = 0;
    std::puts("[OK] Sample interval paces timer reads; adds and finishes stay per-frame");
}

void TestSeqlock() {
    Reset();
    g_clock = RealQpc; // the reader thread uses real time
    FakeAbility ikut{};
    MakeAbility(ikut, 11);
    static FakeBuff pool[64];
    volatile LONG failures = 0;
    HANDLE reader = CreateThread(nullptr, 0, TornReader, (void*)&failures, 0, nullptr);
    CHECK(reader);
    uint32_t uid = 5000;
    for (int round = 0; round < 3000; ++round) {
        const int count = 1 + round % 64;
        for (int i = 0; i < 64; ++i) {
            if (i < count) {
                MakeBuff(pool[i], "buff_seq", ++uid, ikut, 11, ikut, 11, 10);
                Put<int32_t>(pool[i].bytes, off.buffEnhance, count);
                Report(pool[i], true);
            }
        }
        ++g_frame;
        Tick();
        for (int i = 0; i < count; ++i) Report(pool[i], false);
        ++g_frame;
        Tick();
    }
    InterlockedExchange(&g_stopReader, 1);
    WaitForSingleObject(reader, INFINITE);
    CloseHandle(reader);
    g_clock = Clock;
    CHECK(failures == 0);
    std::puts("[OK] Concurrent reader never observes a torn snapshot");
}
} // namespace test

int main() {
    using namespace test;
    wchar_t name[64];
    swprintf_s(name, L"Local\\EndfieldDataBridge.test.%lu", GetCurrentProcessId());
    g_mappingName = name;
    g_frameClock = Frame;
    g_clock = Clock;
    CHECK(OpenShared());
    SetUpOffsets();
    g_sampleIntervalMs = 0; // per-frame reads, so each step is observable

    TestAddRefreshFinish();
    TestSameFrameAndLost();
    TestDilation();
    TestOverflow();
    TestSessionAndLiveness();
    TestSampleInterval();
    TestSeqlock();
    std::puts("[PASS] EndfieldDataBridge offline data verification");
    return 0;
}
