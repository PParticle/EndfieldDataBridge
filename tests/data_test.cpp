// Offline data-path tests. The plugin source is compiled into this exe so the
// tracker runs against fake Buff / AbilitySystem objects laid out at known
// offsets, with a fake frame counter and clock. Every result is checked
// through the reference reader (include/endfield_bridge_reader.h), the same
// code a consumer uses. No IL2CPP or game needed.
#include "../src/plugin.cpp"
#include "../include/endfield_bridge_reader.h"

#include <algorithm>

#define CHECK(expression) do { \
    if (!(expression)) { \
        std::printf("[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        std::fflush(stdout); /* ExitProcess skips stdio flushing */ \
        ExitProcess(1); \
    } \
} while (false)

namespace test {
int32_t g_frame = 1;
int64_t g_now = 1;
int32_t Frame() { return g_frame; }
int64_t Clock() { return g_now; }
void Advance(double seconds) { g_now += int64_t(seconds * double(g_qpcFreq)); }

// ---- Fake game objects ----
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
template<class T> void Put(void* obj, size_t offset, T value) { memcpy(static_cast<char*>(obj) + offset, &value, sizeof(T)); }

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

// ---- Consumer side: everything below reads through efb:: only ----
efb::Mapping g_reader;
efb::EventCursor g_cursor;
struct View {
    efb::Header header;
    EFB_BuffsHeader buffsHead{};
    std::vector<EFB_Buff> buffs; // live entries
    EFB_Team team{};
    const EFB_Buff* Find(uint32_t key) const {
        for (const auto& b : buffs) if (b.key == key) return &b;
        return nullptr;
    }
};
View Read() {
    View v;
    for (;;) {
        CHECK(efb::ReadHeader(g_reader, v.header) == efb::Result::Ok);
        efb::Section buffs, team;
        const efb::Result rb = efb::ReadSection(g_reader, v.header, EFB_SECTION_BUFFS, buffs);
        const efb::Result rt = rb == efb::Result::Ok ? efb::ReadSection(g_reader, v.header, EFB_SECTION_TEAM, team) : rb;
        if (rt == efb::Result::Retry) continue;
        CHECK(rt == efb::Result::Ok);
        v.buffsHead = buffs.Head<EFB_BuffsHeader>();
        v.buffs.clear();
        for (uint32_t i = 0; i < buffs.capacity(); ++i) {
            const EFB_Buff b = buffs.Element<EFB_Buff>(i);
            if (b.key) v.buffs.push_back(b);
        }
        v.team = team.Element<EFB_Team>(0);
        return v;
    }
}
struct Ev { uint32_t type; int32_t frame; EFB_BuffEventPayload p; };
std::vector<Ev> NewEvents(efb::EventBatch* batchOut = nullptr) {
    efb::Header h;
    CHECK(efb::ReadHeader(g_reader, h) == efb::Result::Ok);
    efb::EventBatch batch;
    CHECK(g_cursor.Poll(g_reader, h, batch) == efb::Result::Ok);
    std::vector<Ev> out;
    for (const auto& e : batch.events) out.push_back({e.type, e.frame, efb::Payload<EFB_BuffEventPayload>(e)});
    if (batchOut) *batchOut = batch;
    return out;
}
// Each test starts like a first enable.
void Reset() {
    g_sessionsStarted = 0;
    StartSession();
    efb::EventBatch batch;
    NewEvents(&batch);
    CHECK(batch.resync && batch.events.empty());
}
void TestAddRefreshFinish() {
    Reset();
    FakeAbility ikut{}, enemy{};
    MakeAbility(ikut, 11); MakeAbility(enemy, 22);
    FakeBuff a{}, a2{}, a3{}, b{};
    MakeBuff(a, "buff_wpn_sword_0012_atk_up", 100, ikut, 11, ikut, 11, 20);
    Report(a, true);
    CHECK(NewEvents().empty()); // settled on the next frame
    NextFrame();
    auto ev = NewEvents();
    CHECK(ev.size() == 1 && ev[0].type == EFB_EVENT_BUFF_ADDED && ev[0].p.buff_slot >= 0 && ev[0].frame == g_frame - 1);
    const uint32_t key = ev[0].p.key;
    View v = Read();
    CHECK(key && v.Find(key) && v.Find(key)->instance_uid == 100 && v.buffsHead.count == 1);
    CHECK(!strcmp(v.Find(key)->buff_id, "buff_wpn_sword_0012_atk_up") && !strcmp(ev[0].p.buff_id, v.Find(key)->buff_id));

    // Refresh as finish(old) + add(new) in one frame keeps the key.
    MakeBuff(a2, "buff_wpn_sword_0012_atk_up", 101, ikut, 11, ikut, 11, 20);
    Report(a, false);
    Report(a2, true);
    CHECK(Read().Find(key)); // still visible while the frame settles
    NextFrame();
    ev = NewEvents();
    CHECK(ev.size() == 1 && ev[0].type == EFB_EVENT_BUFF_REFRESHED && ev[0].p.key == key && ev[0].p.instance_uid == 101);
    v = Read();
    CHECK(v.Find(key)->instance_uid == 101 && v.buffsHead.count == 1);

    // Same, in the other order.
    MakeBuff(a3, "buff_wpn_sword_0012_atk_up", 102, ikut, 11, ikut, 11, 20);
    Report(a3, true);
    Report(a2, false);
    NextFrame();
    ev = NewEvents();
    CHECK(ev.size() == 1 && ev[0].type == EFB_EVENT_BUFF_REFRESHED && ev[0].p.key == key);

    // A different source is a different buff, not a refresh.
    MakeBuff(b, "buff_wpn_sword_0012_atk_up", 200, ikut, 11, enemy, 22, 20);
    Report(b, true);
    NextFrame();
    ev = NewEvents();
    CHECK(ev.size() == 1 && ev[0].type == EFB_EVENT_BUFF_ADDED && ev[0].p.key != key);
    const uint32_t keyB = ev[0].p.key;

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
    CHECK(ev.size() == 1 && ev[0].type == EFB_EVENT_BUFF_REFRESHED && ev[0].p.key == key);

    // Every stack change is an event.
    Put<int32_t>(a3.bytes, off.buffEnhance, 2);
    NextFrame();
    Put<int32_t>(a3.bytes, off.buffEnhance, 3);
    NextFrame();
    ev = NewEvents();
    CHECK(ev.size() == 2 && ev[0].type == EFB_EVENT_BUFF_ENHANCED && ev[0].p.enhance_cnt == 2 &&
        ev[1].p.enhance_cnt == 3 && Read().Find(key)->enhance_cnt == 3);

    // Finish: reported reason, at the frame the game reported it.
    const int32_t finishFrame = g_frame;
    Report(a3, false, EFB_FINISH_LIFETIME);
    Report(a3, false, EFB_FINISH_LIFETIME); // second panel
    NextFrame();
    ev = NewEvents();
    CHECK(ev.size() == 1 && ev[0].type == EFB_EVENT_BUFF_FINISHED && ev[0].p.key == key &&
        ev[0].p.finish_reason == EFB_FINISH_LIFETIME && ev[0].frame == finishFrame);
    v = Read();
    CHECK(!v.Find(key) && v.Find(keyB) && v.buffsHead.count == 1);
    // A late duplicate after settlement does not mark the table partial.
    Report(a3, false);
    NextFrame();
    CHECK(!(Read().buffsHead.flags & EFB_BUFFS_PARTIAL) && NewEvents().empty());
    std::puts("[OK] Add, finish+add refresh (both orders), in-place refresh, stacks, finish, duplicates");
}

void TestSameFrameAndLost() {
    Reset();
    FakeAbility ikut{}, enemy{};
    MakeAbility(ikut, 11); MakeAbility(enemy, 22);
    FakeBuff flash{}, reused{}, orphan{}, unknown{};
    MakeBuff(flash, "buff_common_fire_fire_burning_triggered", 300, ikut, 11, ikut, 11, 0);
    Report(flash, true);
    Report(flash, false, EFB_FINISH_EARLY);
    NextFrame();
    auto ev = NewEvents();
    CHECK(ev.size() == 2 && ev[0].type == EFB_EVENT_BUFF_ADDED && ev[1].type == EFB_EVENT_BUFF_FINISHED &&
        ev[0].p.key == ev[1].p.key && ev[0].p.buff_slot == -1 && ev[1].p.finish_reason == EFB_FINISH_EARLY);
    CHECK(Read().buffsHead.count == 0);

    // The game reused the object for another buff: finished as LOST.
    MakeBuff(reused, "buff_a", 310, ikut, 11, ikut, 11, 10);
    Report(reused, true);
    NextFrame();
    NewEvents();
    Put<uint32_t>(reused.bytes, off.buffUid, 999);
    NextFrame();
    NextFrame();
    ev = NewEvents();
    CHECK(ev.size() == 1 && ev[0].type == EFB_EVENT_BUFF_FINISHED && ev[0].p.finish_reason == EFB_FINISH_LOST);

    // The owner despawned.
    MakeBuff(orphan, "buff_b", 320, enemy, 22, ikut, 11, 10);
    Report(orphan, true);
    NextFrame();
    NewEvents();
    MakeAbility(enemy, 0);
    NextFrame();
    NextFrame();
    ev = NewEvents();
    CHECK(ev.size() == 1 && ev[0].type == EFB_EVENT_BUFF_FINISHED && ev[0].p.finish_reason == EFB_FINISH_LOST);

    // A finish for a buff never seen added: tracking started late.
    MakeBuff(unknown, "buff_c", 330, ikut, 11, ikut, 11, 10);
    Report(unknown, false);
    NextFrame();
    CHECK((Read().buffsHead.flags & EFB_BUFFS_PARTIAL) && NewEvents().empty());
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
    const uint32_t key = NewEvents()[0].p.key;
    float passed = 0;
    for (int i = 0; i < 30; ++i) { passed += 0.5f / 60; Put<float>(b.bytes, off.buffPassed, passed); NextFrame(); }
    const float slow = Read().Find(key)->time_dilation;
    CHECK(slow > 0.4f && slow < 0.6f);
    for (int i = 0; i < 30; ++i) NextFrame();
    CHECK(Read().Find(key)->time_dilation == 0);
    for (int i = 0; i < 30; ++i) { passed += 1.0f / 60; Put<float>(b.bytes, off.buffPassed, passed); NextFrame(); }
    const View v = Read();
    CHECK(v.Find(key)->time_dilation == 1 && v.Find(key)->sampled_at == g_now);
    CHECK(NewEvents().empty()); // timers moving is not an event
    std::puts("[OK] Time dilation follows game time (0.5x, frozen, 1x) without events");
}

void TestOverflow() {
    Reset();
    FakeAbility ikut{};
    MakeAbility(ikut, 11);
    static FakeBuff many[BUFF_SLOTS + 3];
    for (uint32_t i = 0; i < BUFF_SLOTS + 3; ++i) {
        MakeBuff(many[i], "buff_many", 1000 + i, ikut, 11, ikut, 11, 10);
        Report(many[i], true);
    }
    NextFrame();
    View v = Read();
    CHECK(v.buffsHead.count == BUFF_SLOTS && v.buffsHead.dropped == 3 && (v.buffsHead.flags & EFB_BUFFS_DROPPED));
    // A finish for a dropped buff is not a missed add.
    Report(many[BUFF_SLOTS + 1], false);
    NextFrame();
    CHECK(!(Read().buffsHead.flags & EFB_BUFFS_PARTIAL));
    // More events than the ring holds: the cursor reports the loss and asks for a resync.
    for (uint32_t i = 0; i < BUFF_SLOTS; ++i) Report(many[i], false);
    NextFrame();
    Report(many[BUFF_SLOTS], true);
    NextFrame();
    efb::EventBatch batch;
    NewEvents(&batch);
    CHECK(batch.resync && batch.lost == 2 * BUFF_SLOTS + 1 - EVENT_SLOTS && batch.events.size() == EVENT_SLOTS);
    CHECK(Read().buffsHead.count == 1);
    std::puts("[OK] Table overflow counted and flagged; event ring overrun reported to the cursor");
}
void TestSessionAndLiveness() {
    Reset();
    View v = Read();
    const uint32_t session = v.header.fields.session;
    CHECK(v.header.fields.active && !(v.buffsHead.flags & EFB_BUFFS_PARTIAL)); // first enable of the process
    StartSession(); // re-enable mid-game
    efb::EventBatch batch;
    NewEvents(&batch);
    v = Read();
    CHECK(batch.resync && v.header.fields.session == session + 1 && (v.buffsHead.flags & EFB_BUFFS_PARTIAL));
    CHECK(efb::WriterAlive(v.header.fields));
    EFB_Header other = v.header.fields;
    other.writer_start_time += 1; // same pid, different process
    CHECK(!efb::WriterAlive(other));
    CHECK(!v.team.sampled_at && efb::Check(v.header.fields, true, v.team.sampled_at, g_now) == efb::Status::Stale);
    CHECK(efb::Check(v.header.fields, true, g_now, g_now) == efb::Status::Online);
    EndSession();
    CHECK(efb::Check(Read().header.fields, true, g_now, g_now) == efb::Status::Disabled);
    std::puts("[OK] Session restart, partial flag on re-enable, writer identity, freshness, disabled");
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
    const uint32_t key = ev[0].p.key;
    const int64_t first = Read().Find(key)->sampled_at;
    Put<int32_t>(b.bytes, off.buffEnhance, 2);
    int frames = 0;
    while (Read().Find(key)->sampled_at == first) { NextFrame(); ++frames; CHECK(frames < 20); }
    // 100 ms at 60 fps: the next read lands on the 5th-7th frame.
    const View v = Read();
    CHECK(frames >= 5 && frames <= 7 && v.Find(key)->enhance_cnt == 2 && v.header.fields.sample_interval_ms == 100);
    ev = NewEvents();
    CHECK(ev.size() == 1 && ev[0].type == EFB_EVENT_BUFF_ENHANCED);
    Report(b, false, EFB_FINISH_EARLY);
    NextFrame();
    ev = NewEvents();
    CHECK(ev.size() == 1 && ev[0].type == EFB_EVENT_BUFF_FINISHED);
    g_sampleIntervalMs = 0;
    std::puts("[OK] Sample interval paces timer reads; adds and finishes stay per-frame");
}

void TestSectionIndependence() {
    Reset();
    efb::Header h;
    efb::Section s;
    CHECK(efb::ReadHeader(g_reader, h) == efb::Result::Ok);
    // The writer holding one section busy does not block the others.
    AcquireSRWLockExclusive(&g_dataLock);
    g_layout->teamHead.seq |= 1;
    CHECK(efb::ReadSection(g_reader, h, EFB_SECTION_TEAM, s) == efb::Result::Busy);
    CHECK(efb::ReadSection(g_reader, h, EFB_SECTION_BUFFS, s) == efb::Result::Ok);
    CHECK(efb::ReadSection(g_reader, h, EFB_SECTION_EVENTS, s) == efb::Result::Ok);
    g_layout->teamHead.seq += 1;
    ReleaseSRWLockExclusive(&g_dataLock);
    CHECK(efb::ReadSection(g_reader, h, EFB_SECTION_TEAM, s) == efb::Result::Ok);
    // A section read under a header that changed meanwhile must be redone.
    Write(W_HEADER, [] {});
    CHECK(efb::ReadSection(g_reader, h, EFB_SECTION_TEAM, s) == efb::Result::Retry);
    CHECK(efb::ReadHeader(g_reader, h) == efb::Result::Ok && efb::ReadSection(g_reader, h, EFB_SECTION_TEAM, s) == efb::Result::Ok);
    // Settling a buff publishes its event and the table change in one write
    // per section (seq + 2), so no reader can see the event without the buff.
    FakeAbility ikut{};
    MakeAbility(ikut, 11);
    FakeBuff b{};
    MakeBuff(b, "buff_f", 600, ikut, 11, ikut, 11, 10);
    Report(b, true);
    uint32_t buffsSeq = g_layout->buffsHead.common.seq, eventsSeq = g_layout->eventsHead.common.seq;
    NextFrame();
    CHECK(g_layout->buffsHead.common.seq == buffsSeq + 2 && g_layout->eventsHead.common.seq == eventsSeq + 2);
    // The same when the next UI callback settles it before the HUD ticks.
    FakeBuff c{}, d{};
    MakeBuff(c, "buff_g", 601, ikut, 11, ikut, 11, 10);
    MakeBuff(d, "buff_h", 602, ikut, 11, ikut, 11, 10);
    Report(c, true);
    ++g_frame;
    buffsSeq = g_layout->buffsHead.common.seq;
    eventsSeq = g_layout->eventsHead.common.seq;
    Report(d, true);
    CHECK(g_layout->buffsHead.common.seq == buffsSeq + 2 && g_layout->eventsHead.common.seq == eventsSeq + 2);
    CHECK(Read().buffsHead.count == 2);
    std::puts("[OK] Per-section seqlocks: one busy section does not block others; header change forces a re-read;"
        " events and the buff table publish together");
}

// A hand-built mapping from a hypothetical newer minor version (bigger
// header, directory entries, section headers and elements, an unknown
// section, an unknown event type, an unknown flag) and an older one
// (smaller operator element). The 1.0 reader must read what it knows.
void TestCompatibility() {
    alignas(8) static char buf[8192];
    memset(buf, 0, sizeof(buf));
    EFB_Header h{};
    h.magic = EFB_MAGIC;
    h.abi_major = EFB_ABI_MAJOR;
    h.abi_minor = EFB_ABI_MINOR + 1;
    h.header_size = sizeof(EFB_Header) + 8;
    h.total_size = sizeof(buf);
    h.directory_offset = 128;
    h.section_count = 5;
    h.section_entry_size = sizeof(EFB_Section) + 8;
    h.session = 7;
    h.active = 1;
    h.qpc_frequency = g_qpcFreq;
    Put(buf, 0, h);
    Put<uint64_t>(buf, sizeof(EFB_Header), 0x1111111111111111ull); // a header field from the future
    const EFB_Section sections[] = {
        {99, 16, 256, 272, 8, 4},                                                       // unknown section
        {EFB_SECTION_OPERATORS, 16, 320, 336, uint32_t(offsetof(EFB_Operator, sampled_at)), 2}, // older: no sampled_at
        {EFB_SECTION_BUFFS, sizeof(EFB_BuffsHeader) + 8, 512, 552, sizeof(EFB_Buff) + 16, 3},
        {EFB_SECTION_EVENTS, sizeof(EFB_EventsHeader) + 8, 4096, 4128, sizeof(EFB_Event) + 64, 4},
        {EFB_SECTION_TEAM, 16, 8000, 8016, sizeof(EFB_Team), 8},                        // runs past total_size
    };
    for (uint32_t i = 0; i < 5; ++i) {
        Put(buf, 128 + i * h.section_entry_size, sections[i]);
        Put<uint64_t>(buf, 128 + i * h.section_entry_size + sizeof(EFB_Section), ~0ull);
    }
    EFB_Operator op{};
    op.present = 1;
    op.ultimate_sp = 42;
    Put(buf, 336 + 88, op); // element 1 of the older operator section (overlaps nothing past 336 + 176)
    EFB_BuffsHeader bh{};
    bh.count = 1;
    bh.flags = EFB_BUFFS_DROPPED | 0x80000000u;
    bh.dropped = 2;
    Put(buf, 512, bh);
    EFB_Buff b{};
    b.key = 7;
    b.enhance_cnt = 3;
    strcpy_s(b.buff_id, "buff_future");
    const size_t slot1 = 552 + sizeof(EFB_Buff) + 16;
    Put(buf, slot1, b);
    memset(buf + slot1 + sizeof(EFB_Buff), 0xFF, 16); // element fields from the future

    efb::Mapping m;
    m.Attach(buf, sizeof(buf));
    efb::Header rh;
    CHECK(efb::ReadHeader(m, rh) == efb::Result::Ok && rh.fields.session == 7 && rh.fields.abi_minor == EFB_ABI_MINOR + 1);
    CHECK(!rh.Find(EFB_SECTION_TEAM)); // out of bounds: treated as absent
    efb::Section s;
    CHECK(efb::ReadSection(m, rh, EFB_SECTION_TEAM, s) == efb::Result::Missing);
    CHECK(efb::ReadSection(m, rh, EFB_SECTION_OPERATORS, s) == efb::Result::Ok && s.capacity() == 2);
    const EFB_Operator ro = s.Element<EFB_Operator>(1);
    CHECK(ro.present == 1 && ro.ultimate_sp == 42 && ro.sampled_at == 0); // missing trailing field reads as 0
    CHECK(efb::ReadSection(m, rh, EFB_SECTION_BUFFS, s) == efb::Result::Ok && s.capacity() == 3);
    const auto rbh = s.Head<EFB_BuffsHeader>();
    CHECK(rbh.count == 1 && rbh.dropped == 2 && (rbh.flags & EFB_BUFFS_DROPPED));
    CHECK(s.Element<EFB_Buff>(0).key == 0);
    const EFB_Buff rb = s.Element<EFB_Buff>(1);
    CHECK(rb.key == 7 && rb.enhance_cnt == 3 && !strcmp(rb.buff_id, "buff_future"));

    // Events: start the cursor, then publish an unknown type and a buff event with a longer payload.
    efb::EventCursor cursor;
    efb::EventBatch batch;
    CHECK(cursor.Poll(m, rh, batch) == efb::Result::Ok && batch.resync && batch.events.empty());
    EFB_Event e{};
    e.serial = 1;
    e.type = 99;
    e.payload_size = EFB_EVENT_PAYLOAD;
    Put(buf, 4128, e);
    e = EFB_Event{};
    e.serial = 2;
    e.type = EFB_EVENT_BUFF_ADDED;
    e.payload_size = sizeof(EFB_BuffEventPayload) + 8;
    EFB_BuffEventPayload p{};
    p.key = 7;
    p.buff_slot = 1;
    strcpy_s(p.buff_id, "buff_future");
    memcpy(e.payload, &p, sizeof(p));
    Put(buf, 4128 + sizeof(EFB_Event) + 64, e);
    Put<uint64_t>(buf, 4096 + offsetof(EFB_EventsHeader, head), 2);
    CHECK(cursor.Poll(m, rh, batch) == efb::Result::Ok && !batch.resync && batch.events.size() == 2);
    CHECK(batch.events[0].type == 99); // delivered; consumers skip types they do not know
    const auto rp = efb::Payload<EFB_BuffEventPayload>(batch.events[1]);
    CHECK(batch.events[1].type == EFB_EVENT_BUFF_ADDED && rp.key == 7 && rp.buff_slot == 1 && !strcmp(rp.buff_id, "buff_future"));

    // Not this ABI.
    h.abi_major = EFB_ABI_MAJOR + 1;
    Put(buf, 0, h);
    CHECK(efb::ReadHeader(m, rh) == efb::Result::Incompatible);
    h.abi_major = EFB_ABI_MAJOR;
    h.magic = 0;
    Put(buf, 0, h);
    CHECK(efb::ReadHeader(m, rh) == efb::Result::Incompatible);
    std::puts("[OK] Reader handles newer and older minor layouts; rejects other ABIs");
}
// ---- Units and skill casts ----
// Fake AbilitySystem fields behind fake getters (MethodInfo: function pointer first).
constexpr size_t kFaction = 0x14, kHp = 0x18, kMaxHp = 0x20, kPoise = 0x28, kMaxPoise = 0x2C, kHasPoise = 0x30,
    kZeroPoise = 0x31, kInRecovery = 0x32, kProgress = 0x34, kRecTime = 0x38, kSumRecTime = 0x3C;
template<class T, size_t Offset> T FakeGetter(void* self, void*) { return At<T>(self, int(Offset)); }
struct FakeMethod { void* fn; };
FakeMethod g_fakeFaction{reinterpret_cast<void*>(&FakeGetter<int32_t, kFaction>)};
FakeMethod g_fakeHp{reinterpret_cast<void*>(&FakeGetter<double, kHp>)};
FakeMethod g_fakeMaxHp{reinterpret_cast<void*>(&FakeGetter<double, kMaxHp>)};
FakeMethod g_fakePoise{reinterpret_cast<void*>(&FakeGetter<float, kPoise>)};
FakeMethod g_fakeMaxPoise{reinterpret_cast<void*>(&FakeGetter<float, kMaxPoise>)};
FakeMethod g_fakeHasPoise{reinterpret_cast<void*>(&FakeGetter<bool, kHasPoise>)};
FakeMethod g_fakeZeroPoise{reinterpret_cast<void*>(&FakeGetter<bool, kZeroPoise>)};
FakeMethod g_fakeInRecovery{reinterpret_cast<void*>(&FakeGetter<bool, kInRecovery>)};
FakeMethod g_fakeProgress{reinterpret_cast<void*>(&FakeGetter<float, kProgress>)};
FakeMethod g_fakeRecTime{reinterpret_cast<void*>(&FakeGetter<float, kRecTime>)};
FakeMethod g_fakeSumRecTime{reinterpret_cast<void*>(&FakeGetter<float, kSumRecTime>)};
// Fake Skill: type 0x10, owner 0x18 (ObjectPtr), id 0x28, casting 0x30, castId 0x34, instId 0x38.
FakeMethod g_fakeIsCasting{reinterpret_cast<void*>(&FakeGetter<bool, 0x30>)};
FakeMethod g_fakeCastId{reinterpret_cast<void*>(&FakeGetter<uint32_t, 0x34>)};
FakeMethod g_fakeInstId{reinterpret_cast<void*>(&FakeGetter<uint64_t, 0x38>)};
struct FakeUnit { alignas(8) char bytes[0x40]{}; };
struct FakeSkill { alignas(8) char bytes[0x40]{}; FakeString id; };
void SetUpUnitGetters() {
    g_mFaction = &g_fakeFaction;
    g_mHp = &g_fakeHp; g_mMaxHp = &g_fakeMaxHp; g_mPoise = &g_fakePoise; g_mMaxPoise = &g_fakeMaxPoise;
    g_mHasPoise = &g_fakeHasPoise; g_mZeroPoise = &g_fakeZeroPoise; g_mInPoiseRecovery = &g_fakeInRecovery;
    g_mPoiseProgress = &g_fakeProgress; g_mPoiseRecTime = &g_fakeRecTime; g_mSumPoiseRecTime = &g_fakeSumRecTime;
    g_mIsCasting = &g_fakeIsCasting; g_mSkillCastId = &g_fakeCastId; g_mSkillInstId = &g_fakeInstId;
    off.skillType = 0x10; off.skillOwner = 0x18; off.skillId = 0x28;
    off.sqIndex = 0x10; off.sqAbility = 0x18; off.sqEmpty = 0x28; off.sqMain = 0x29;
    off.hbTarget = 0x10;
}
void MakeUnit(FakeUnit& u, uint32_t uid, int32_t faction, double hp, double maxHp, float poise, float maxPoise) {
    memset(u.bytes, 0, sizeof(u.bytes));
    Put<uint32_t>(u.bytes, off.asUid, uid);
    Put<int32_t>(u.bytes, kFaction, faction);
    Put<double>(u.bytes, kHp, hp);
    Put<double>(u.bytes, kMaxHp, maxHp);
    Put<float>(u.bytes, kPoise, poise);
    Put<float>(u.bytes, kMaxPoise, maxPoise);
    Put<bool>(u.bytes, kHasPoise, maxPoise > 0);
}
std::vector<EFB_UnitState> Units() {
    for (;;) {
        efb::Header h;
        efb::Section s;
        CHECK(efb::ReadHeader(g_reader, h) == efb::Result::Ok);
        const efb::Result r = efb::ReadSection(g_reader, h, EFB_SECTION_UNITS, s);
        if (r == efb::Result::Retry) continue;
        CHECK(r == efb::Result::Ok && s.Head<EFB_UnitsHeader>().count <= s.capacity());
        std::vector<EFB_UnitState> out;
        for (uint32_t i = 0; i < s.capacity(); ++i) {
            const EFB_UnitState u = s.Element<EFB_UnitState>(i);
            if (u.instance_uid) out.push_back(u);
        }
        CHECK(out.size() == s.Head<EFB_UnitsHeader>().count);
        return out;
    }
}
const EFB_UnitState* FindUnit(const std::vector<EFB_UnitState>& units, uint32_t uid) {
    for (const auto& u : units) if (u.instance_uid == uid) return &u;
    return nullptr;
}

void TestUnits() {
    Reset();
    FakeUnit ikut{}, boss{};
    MakeUnit(ikut, 11, EFB_FACTION_GOOD, 900, 1000, 0, 0);
    MakeUnit(boss, 77, EFB_FACTION_BAD, 50000, 60000, 40, 100);
    alignas(8) char icon[0x30]{}, bar[0x20]{};
    Put<int32_t>(icon, off.sqIndex, 2);
    Put<ObjPtr>(icon, off.sqAbility, ObjPtr{ikut.bytes, 11, 0});
    Put<ObjPtr>(bar, off.hbTarget, ObjPtr{boss.bytes, 77, 0});
    ProbeSquad(icon);
    ProbeHeadBar(bar);
    NextFrame();
    auto units = Units();
    CHECK(units.size() == 2);
    const EFB_UnitState* a = FindUnit(units, 11);
    const EFB_UnitState* b = FindUnit(units, 77);
    CHECK(a && a->char_index == 2 && !a->on_head_bar && a->hp == 900 && a->max_hp == 1000 && !a->has_poise &&
        a->faction == EFB_FACTION_GOOD && a->sampled_at);
    CHECK(b && b->char_index == -1 && b->on_head_bar && b->hp == 50000 && b->max_hp == 60000 && b->has_poise &&
        b->poise == 40 && b->max_poise == 100 && b->faction == EFB_FACTION_BAD);

    // Broken, then recovering: raw fields as the game reports them.
    Put<float>(boss.bytes, kPoise, 0);
    Put<bool>(boss.bytes, kZeroPoise, true);
    Put<bool>(boss.bytes, kInRecovery, true);
    Put<float>(boss.bytes, kProgress, 0.25f);
    Put<float>(boss.bytes, kRecTime, 1.5f);
    Put<float>(boss.bytes, kSumRecTime, 6.0f);
    Put<double>(boss.bytes, kHp, 42000);
    ProbeHeadBar(bar);
    NextFrame();
    b = FindUnit(units = Units(), 77);
    CHECK(b && b->is_zero_poise && b->in_poise_recovery && b->poise_recovery_progress == 0.25f &&
        b->poise_rec_time == 1.5f && b->sum_poise_rec_time == 6.0f && b->hp == 42000);

    // The head bar goes away: the unit stays a short while, then leaves; the squad member stays.
    for (int i = 0; i < 30; ++i) { ProbeSquad(icon); NextFrame(); }
    CHECK(FindUnit(Units(), 77));
    for (int i = 0; i < 40; ++i) { ProbeSquad(icon); NextFrame(); }
    units = Units();
    CHECK(!FindUnit(units, 77) && FindUnit(units, 11));
    // The object is reused for another unit: the old one leaves at once.
    ProbeHeadBar(bar);
    NextFrame();
    CHECK(FindUnit(Units(), 77));
    Put<uint32_t>(boss.bytes, off.asUid, 78);
    NextFrame();
    CHECK(!FindUnit(Units(), 77));
    // A new session starts empty.
    Reset();
    CHECK(Units().empty());
    std::puts("[OK] Units: squad and head-bar units with HP and poise, removal when gone");
}

void TestSkillCasts() {
    Reset();
    g_provides = EFB_PROVIDES_SKILL_CASTS;
    FakeUnit enemy{};
    MakeUnit(enemy, 77, EFB_FACTION_BAD, 100, 100, 0, 0);
    FakeSkill attack{}, ult{};
    auto make = [](FakeSkill& s, int32_t type, FakeUnit& owner, uint32_t ownerUid, const char* id, uint32_t castId) {
        memset(s.bytes, 0, sizeof(s.bytes));
        SetString(s.id, id);
        Put<int32_t>(s.bytes, off.skillType, type);
        Put<ObjPtr>(s.bytes, off.skillOwner, ObjPtr{owner.bytes, ownerUid, 0});
        Put<void*>(s.bytes, off.skillId, &s.id);
        Put<uint32_t>(s.bytes, 0x34, castId);
        Put<uint64_t>(s.bytes, 0x38, 9000 + castId);
    };
    make(attack, EFB_SKILL_ATTACK, enemy, 77, "eny_attack", 1);
    make(ult, EFB_SKILL_ULTIMATE, enemy, 77, "eny_ultimate", 2);
    // Normal attacks are not reported.
    Put<bool>(attack.bytes, 0x30, true);
    ProbeCastStart(attack.bytes);
    ProbeCastEnd(attack.bytes, 0, 0);
    CHECK(NewEvents().empty());
    // DoCast that did not start casting reports nothing.
    ProbeCastStart(ult.bytes);
    CHECK(NewEvents().empty());
    Put<bool>(ult.bytes, 0x30, true);
    ProbeCastStart(ult.bytes);
    ProbeCastEnd(ult.bytes, 3, 5);
    Put<bool>(ult.bytes, 0x30, false);
    ProbeCastEnd(ult.bytes, 3, 5); // a second end after the cast finished
    efb::Header h;
    CHECK(efb::ReadHeader(g_reader, h) == efb::Result::Ok);
    efb::EventBatch batch;
    CHECK(g_cursor.Poll(g_reader, h, batch) == efb::Result::Ok && batch.events.size() == 2);
    const auto s0 = efb::Payload<EFB_SkillEventPayload>(batch.events[0]);
    const auto s1 = efb::Payload<EFB_SkillEventPayload>(batch.events[1]);
    CHECK(batch.events[0].type == EFB_EVENT_SKILL_CAST_STARTED && batch.events[1].type == EFB_EVENT_SKILL_CAST_ENDED);
    CHECK(batch.events[0].subject_uid == 77 && s0.caster_uid == 77 && s0.char_index == -1 && s0.faction == EFB_FACTION_BAD &&
        s0.skill_type == EFB_SKILL_ULTIMATE && s0.skill_cast_id == 2 && s0.skill_inst_id == 9002 &&
        !strcmp(s0.skill_id, "eny_ultimate"));
    CHECK(s1.skill_cast_id == s0.skill_cast_id && s1.finish_type == 3 && s1.interrupt_reason == 5);
    // Not installed: nothing.
    g_provides = 0;
    Put<bool>(ult.bytes, 0x30, true);
    ProbeCastStart(ult.bytes);
    CHECK(NewEvents().empty());
    std::puts("[OK] Skill casts: start/end paired by cast id, normal attacks and non-starts not reported");
}

// A reader thread, via the reference reader, while the game thread writes:
//  - a buffs copy is never torn: every live entry has enhance_cnt == count;
//  - events never run ahead of buffs: a key the events say is present is in
//    the buffs read that follows, unless a later poll shows it finished.
volatile LONG g_stopReader = 0;
struct ReaderStats { volatile LONG failures, checks, orderChecks; };
DWORD WINAPI ConcurrentReader(void* arg) {
    auto* stats = static_cast<ReaderStats*>(arg);
    volatile LONG* failures = &stats->failures;
    efb::Mapping m;
    if (!m.Open(g_mappingName)) { InterlockedIncrement(failures); return 0; }
    efb::EventCursor cursor;
    std::vector<uint32_t> present; // keys alive according to events
    auto apply = [&](const efb::EventBatch& batch) {
        if (batch.resync) present.clear();
        for (const auto& e : batch.events) {
            const uint32_t key = efb::Payload<EFB_BuffEventPayload>(e).key;
            if (e.type == EFB_EVENT_BUFF_ADDED && efb::Payload<EFB_BuffEventPayload>(e).buff_slot >= 0) present.push_back(key);
            if (e.type == EFB_EVENT_BUFF_FINISHED) present.erase(std::remove(present.begin(), present.end(), key), present.end());
        }
    };
    while (!g_stopReader) {
        efb::Header h;
        efb::EventBatch first, second;
        efb::Section buffs;
        if (efb::ReadHeader(m, h) != efb::Result::Ok || cursor.Poll(m, h, first) != efb::Result::Ok) continue;
        apply(first);
        if (efb::ReadSection(m, h, EFB_SECTION_BUFFS, buffs) != efb::Result::Ok) continue;
        const uint32_t count = buffs.Head<EFB_BuffsHeader>().count;
        std::vector<uint32_t> inTable;
        for (uint32_t i = 0; i < buffs.capacity(); ++i) {
            const EFB_Buff b = buffs.Element<EFB_Buff>(i);
            if (!b.key) continue;
            inTable.push_back(b.key);
            if (uint32_t(b.enhance_cnt) != count) InterlockedIncrement(failures);
        }
        if (inTable.size() != count) InterlockedIncrement(failures);
        InterlockedIncrement(&stats->checks);
        if (first.resync) continue; // events lost: nothing to compare against
        InterlockedIncrement(&stats->orderChecks);
        std::vector<uint32_t> missing;
        for (uint32_t key : present)
            if (std::find(inTable.begin(), inTable.end(), key) == inTable.end()) missing.push_back(key);
        if (missing.empty() || cursor.Poll(m, h, second) != efb::Result::Ok) continue;
        apply(second);
        if (second.resync) continue;
        for (uint32_t key : missing)
            if (std::find(present.begin(), present.end(), key) != present.end()) InterlockedIncrement(failures);
    }
    return 0;
}
void TestConcurrentReader() {
    Reset();
    g_clock = RealQpc; // the reader thread uses real time
    FakeAbility ikut{};
    MakeAbility(ikut, 11);
    static FakeBuff pool[24];
    ReaderStats stats{};
    HANDLE reader = CreateThread(nullptr, 0, ConcurrentReader, &stats, 0, nullptr);
    CHECK(reader);
    uint32_t uid = 5000;
    // Run until the reader has checked enough (bounded), yielding each round
    // so it can keep up with the event ring.
    const ULONGLONG deadline = GetTickCount64() + 20000;
    for (int round = 0; round < 3000 || stats.checks < 500 || stats.orderChecks < 200; ++round) {
        CHECK(GetTickCount64() < deadline);
        const int count = 1 + (round * 7) % 24;
        for (int i = 0; i < count; ++i) {
            MakeBuff(pool[i], "buff_seq", ++uid, ikut, 11, ikut, 11, 10);
            Put<int32_t>(pool[i].bytes, off.buffEnhance, count);
            Report(pool[i], true);
        }
        ++g_frame;
        Tick();
        for (int i = 0; i < count; ++i) Report(pool[i], false);
        ++g_frame;
        Tick();
        SwitchToThread();
    }
    InterlockedExchange(&g_stopReader, 1);
    WaitForSingleObject(reader, INFINITE);
    CloseHandle(reader);
    g_clock = Clock;
    CHECK(stats.failures == 0);
    std::printf("[OK] Concurrent reader: no torn section, events never ahead of the buff table "
        "(%ld reads, %ld ordering checks)\n", stats.checks, stats.orderChecks);
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
    CHECK(g_reader.Open(name) && g_reader.size() >= EFB_MAPPING_SIZE);
    SetUpOffsets();
    g_sampleIntervalMs = 0; // per-frame reads, so each step is observable
    TestAddRefreshFinish();
    TestSameFrameAndLost();
    TestDilation();
    TestOverflow();
    TestSessionAndLiveness();
    TestSampleInterval();
    TestSectionIndependence();
    TestCompatibility();
    SetUpUnitGetters();
    TestUnits();
    TestSkillCasts();
    TestConcurrentReader();
    std::puts("[PASS] EndfieldDataBridge offline data verification");
    return 0;
}
