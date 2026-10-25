// hoonbot.cpp - MoonBot-style SA-MP bot core (plain C API, used from Lua through LuaJIT FFI)
// Target: SA-MP 0.3.7 protocol, Android NDK, C++14. Needs RakSAMP's RakNet + samp_auth.
// The Lua side (hoonbot.lua) wraps this into the MoonBot-like API: require('hoonbot')

#include <android/log.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "RakClientInterface.h"
#include "RakNetworkFactory.h"
#include "BitStream.h"
#include "PacketEnumerations.h"
#include "samp_auth.h"   // extern char AuthKeyTable[512][2][128]

int gen_gpci(char buf[64], unsigned long factor);   // extracted from RakSAMP

#define HB_API extern "C" __attribute__((visibility("default")))
#define TAG "hoonbot"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static const int      MAX_BOTS     = 8;      // keep it small on purpose
static const int      SAMP_VERSION = 4057;   // 0.3.7
static const char*    SAMP_VER_STR = "0.3.7";

#pragma pack(push, 1)
struct OnFootSync {
    uint16_t lrAnalog, udAnalog, keys;
    float    pos[3];
    float    quat[4];
    uint8_t  health, armour, weapon, specialAction;
    float    moveSpeed[3];
    float    surfOffset[3];
    uint16_t surfId;
    int16_t  animId, animFlags;
};
#pragma pack(pop)

// ---------------- events (RakNet threads -> Lua thread) ----------------
struct Event { int bot = 0, kind = 0, id = 0; std::string data; };
// kind: 1 = packet, 2 = incoming RPC, 3 = disconnected (data = reason), 4 = connected (id = playerId)

static std::mutex g_evmu;
static std::deque<Event> g_events;

static void pushEvent(Event&& e) {
    std::lock_guard<std::mutex> l(g_evmu);
    if (g_events.size() > 4096) g_events.pop_front();
    g_events.push_back(std::move(e));
}

// ---------------- per-bot RPC thunks (so a callback knows which bot got it) ----------------
typedef void (*RpcFn)(RPCParameters*);

static std::atomic<int> g_spawnState[MAX_BOTS];   // 0 none, 1 server approved RequestSpawn, 2 server forced a spawn

static const char* rejectText(int r) {
    switch (r) {
    case 1: return "BAD_VERSION (server does not accept client version 0.3.7 / 4057)";
    case 2: return "BAD_NICKNAME (name rejected or already in use)";
    case 3: return "BAD_MOD";
    case 4: return "BAD_PLAYERID";
    default: return "unknown reason";
    }
}

static void onRpc(int bot, int id, RPCParameters* p) {
    size_t bytes = (p->numberOfBitsOfData + 7) / 8;
    if (id == 130 && bytes >= 1) {            // RPC_ConnectionRejected: why the server refused us
        int r = ((const unsigned char*)p->input)[0];
        char msg[160];
        snprintf(msg, sizeof(msg), "rejected by server: reason %d = %s", r, rejectText(r));
        LOGI("[%d] %s", bot + 1, msg);
        Event d; d.bot = bot + 1; d.kind = 5; d.data = msg;   // debug text
        pushEvent(std::move(d));
    }
    if (id == 129 && bytes >= 1 && bot >= 0 && bot < MAX_BOTS) {   // RequestSpawn reply
        int o = ((const unsigned char*)p->input)[0];
        if (o == 1 || o == 2) g_spawnState[bot] = o;
    }
    Event e;
    e.bot = bot + 1; e.kind = 2; e.id = id;
    e.data.assign((const char*)p->input, bytes);
    pushEvent(std::move(e));
}
template <int B, int R> static void thunk(RPCParameters* p) { onRpc(B, R, p); }

template <int B, int... Rs>
static std::array<RpcFn, 256> makeRow(std::integer_sequence<int, Rs...>) {
    return std::array<RpcFn, 256>{{ &thunk<B, Rs>... }};
}
template <int... Bs>
static std::array<std::array<RpcFn, 256>, MAX_BOTS> makeTable(std::integer_sequence<int, Bs...>) {
    return {{ makeRow<Bs>(std::make_integer_sequence<int, 256>{})... }};
}
static const std::array<std::array<RpcFn, 256>, MAX_BOTS> g_rpcTable =
    makeTable(std::make_integer_sequence<int, MAX_BOTS>{});

static int  g_rpcIds[256];
static bool g_rpcReg[256];
static bool g_pktReg[256];

// ---------------- bots ----------------
struct Bot {
    int slot = 0;
    std::string name;
    RakClientInterface* rak = nullptr;
    bool connected = false, spawned = false;
    uint16_t playerId = 0;
    uint32_t challenge = 0;
    float pos[3] = {0.f, 0.f, 3.f};
    uint64_t tick = 0, connectTick = 0;
    int dbgPackets = 0;
    bool warned = false;
    bool wantSpawn = false;
    uint64_t lastManual = 0;   // tick of the last sync packet sent by the script
    bool rpcDone[256];
    Bot() { memset(rpcDone, 0, sizeof(rpcDone)); }
};

static std::mutex g_mu;
static std::unique_ptr<Bot> g_bots[MAX_BOTS];
static std::atomic<bool> g_run{false};
static bool g_started = false;

static Bot* getBot(int idx) {
    if (idx < 1 || idx > MAX_BOTS) return nullptr;
    return g_bots[idx - 1].get();
}

static std::atomic<bool> g_debug{false};
static void dbg(Bot& b, const char* fmt, ...) {
    char buf[200];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    LOGI("[%d] %s", b.slot + 1, buf);
    if (!g_debug) return;
    Event e; e.bot = b.slot + 1; e.kind = 5; e.data = buf;   // kind 5 = debug text
    pushEvent(std::move(e));
}

static void rpcSend(Bot& b, int id, RakNet::BitStream& bs) {
    if (!b.rak) return;
    int rid = id;
    b.rak->RPC(&rid, &bs, HIGH_PRIORITY, RELIABLE, 0, false, UNASSIGNED_NETWORK_ID, NULL);
}

static void applyRpcRegistrations(Bot& b) {
    if (!b.rak) return;
    for (int i = 0; i < 256; i++) {
        if (g_rpcReg[i] && !b.rpcDone[i]) {
            b.rak->RegisterAsRemoteProcedureCall(&g_rpcIds[i], g_rpcTable[b.slot][i]);
            b.rpcDone[i] = true;
        }
    }
}

static void ensureRak(Bot& b) {
    if (b.rak) return;
    b.rak = RakNetworkFactory::GetRakClientInterface();
    b.rak->SetMTUSize(576);                 // RakSAMP does this too
    memset(b.rpcDone, 0, sizeof(b.rpcDone));
    applyRpcRegistrations(b);
}

static void destroyRak(Bot& b) {
    if (b.rak) {
        b.rak->Disconnect(300);
        RakNetworkFactory::DestroyRakClientInterface(b.rak);
        b.rak = nullptr;
    }
    b.connected = false; b.spawned = false; b.playerId = 0; b.wantSpawn = false;
    g_spawnState[b.slot] = 0;
}

static void dropped(Bot& b, const char* why) {
    b.connected = false; b.spawned = false;
    Event e; e.bot = b.slot + 1; e.kind = 3; e.data = why;
    pushEvent(std::move(e));
}

static void sendJoin(Bot& b) {
    RakNet::BitStream bs;
    char gpci[64] = {0};
    gen_gpci(gpci, 0x3e9);
    uint8_t mod = 1;
    uint8_t nl = (uint8_t)b.name.size();
    uint8_t al = (uint8_t)strlen(gpci);
    uint8_t vl = (uint8_t)strlen(SAMP_VER_STR);
    bs.Write((int32_t)SAMP_VERSION);
    bs.Write(mod);
    bs.Write(nl);  bs.Write(b.name.c_str(), nl);
    bs.Write(b.challenge);
    bs.Write(al);  bs.Write(gpci, al);
    bs.Write(vl);  bs.Write(SAMP_VER_STR, vl);
    rpcSend(b, 25, bs);   // ClientJoin
}

static void onAccepted(Bot& b, Packet* p) {
    RakNet::BitStream in(p->data + 1, p->length - 1, false);
    uint32_t ip; uint16_t port, pid; uint32_t cookie;
    in.Read(ip); in.Read(port); in.Read(pid); in.Read(cookie);
    b.playerId = pid;
    b.challenge = cookie ^ (uint32_t)SAMP_VERSION;
    b.connected = true;
    dbg(b, "accepted: playerId=%u challenge=%u", (unsigned)pid, (unsigned)b.challenge);
    sendJoin(b);
    Event e; e.bot = b.slot + 1; e.kind = 4; e.id = pid;
    pushEvent(std::move(e));
}

static void onAuthKey(Bot& b, Packet* p) {
    const char* chal = (const char*)p->data + 2;
    const char* key = nullptr;
    for (int i = 0; i < 512; i++) {
        if (!strcmp(chal, AuthKeyTable[i][0])) { key = AuthKeyTable[i][1]; break; }
    }
    if (!key) { dbg(b, "unknown AUTH_KEY challenge: %s", chal); return; }
    RakNet::BitStream bs;
    uint8_t kl = (uint8_t)strlen(key);
    bs.Write((uint8_t)ID_AUTH_KEY);
    bs.Write(kl);
    bs.Write(key, kl);
    b.rak->Send(&bs, SYSTEM_PRIORITY, RELIABLE, 0);
    dbg(b, "auth key sent for challenge %s", chal);
}

static void sendOnFoot(Bot& b) {
    OnFootSync s{};
    memcpy(s.pos, b.pos, sizeof(b.pos));
    s.quat[0] = 1.f;
    s.health = 100;
    RakNet::BitStream bs;
    bs.Write((uint8_t)ID_PLAYER_SYNC);
    bs.Write((char*)&s, sizeof(s));
    b.rak->Send(&bs, HIGH_PRIORITY, UNRELIABLE_SEQUENCED, 1);
}

static void stepBot(Bot& b) {
    if (!b.rak) return;
    while (Packet* p = b.rak->Receive()) {
        if (p->length > 0) {
            uint8_t id = p->data[0];
            if (b.dbgPackets < 80) { b.dbgPackets++; dbg(b, "packet id=%d len=%u", (int)id, (unsigned)p->length); }
            switch (id) {
            case ID_CONNECTION_REQUEST_ACCEPTED:     onAccepted(b, p); break;
            case ID_AUTH_KEY:                        onAuthKey(b, p);  break;
            case ID_DISCONNECTION_NOTIFICATION:      dropped(b, "disconnected"); break;
            case ID_CONNECTION_LOST:                 dropped(b, "connection lost"); break;
            case ID_CONNECTION_BANNED:               dropped(b, "banned"); break;
            case ID_NO_FREE_INCOMING_CONNECTIONS:    dropped(b, "server full"); break;
            default: break;
            }
            if (g_pktReg[id]) {
                Event e; e.bot = b.slot + 1; e.kind = 1; e.id = id;
                e.data.assign((const char*)p->data + 1, p->length - 1);
                pushEvent(std::move(e));
            }
        }
        b.rak->DeallocatePacket(p);
    }
    b.tick++;
    if (!b.connected && !b.warned && b.connectTick && b.tick - b.connectTick > 250) {
        b.warned = true;
        dbg(b, "no answer from the server after 5s (IsConnected=%d)", b.rak->IsConnected() ? 1 : 0);
    }
    int st = g_spawnState[b.slot];
    if (st == 2 || (st == 1 && b.wantSpawn)) {          // approved by the server -> now send Spawn (like a real client)
        g_spawnState[b.slot] = 0; b.wantSpawn = false;
        RakNet::BitStream s;
        rpcSend(b, 52, s);
        b.spawned = true;
        Event e; e.bot = b.slot + 1; e.kind = 6;       // spawned
        pushEvent(std::move(e));
    }
    // built-in idle sync (~10 Hz) only while the script is NOT sending its own sync data
    if (b.spawned && b.tick % 5 == 0 && b.tick - b.lastManual > 100) sendOnFoot(b);
}

static void workerMain() {
    while (g_run) {
        {
            std::lock_guard<std::mutex> l(g_mu);
            for (int i = 0; i < MAX_BOTS; i++) if (g_bots[i]) stepBot(*g_bots[i]);
        }
        usleep(20000);
    }
}

static void initOnce() {
    if (g_started) return;
    g_started = true;
    for (int i = 0; i < 256; i++) g_rpcIds[i] = i;
    g_pktReg[ID_CONNECTION_REQUEST_ACCEPTED]  = true;
    g_pktReg[ID_DISCONNECTION_NOTIFICATION]   = true;
    g_pktReg[ID_CONNECTION_LOST]              = true;
    g_pktReg[ID_CONNECTION_BANNED]            = true;
    g_pktReg[ID_NO_FREE_INCOMING_CONNECTIONS] = true;
    g_rpcReg[93] = true;    // ClientMessage (kick / server messages)
    g_rpcReg[129] = true;   // RequestSpawn reply
    g_rpcReg[130] = true;   // ConnectionRejected: needed to see why the server kicks us
    g_run = true;
    std::thread(workerMain).detach();
}

// ======================= C API (called from Lua via FFI) =======================
typedef struct { int bot; int kind; int id; int len; } HbEvent;

HB_API int hb_version(void) { return 3; }

HB_API int hb_add(const char* name) {
    std::lock_guard<std::mutex> l(g_mu);
    initOnce();
    if (!name || !*name || strlen(name) > 24) return 0;
    for (int i = 0; i < MAX_BOTS; i++) {
        if (g_bots[i]) continue;
        g_bots[i].reset(new Bot());
        g_bots[i]->slot = i;
        g_bots[i]->name = name;
        return i + 1;
    }
    return 0;   // all slots used
}

HB_API int hb_remove(int idx) {
    std::lock_guard<std::mutex> l(g_mu);
    Bot* b = getBot(idx);
    if (!b) return 0;
    destroyRak(*b);
    g_bots[idx - 1].reset();
    return 1;
}

HB_API int hb_connect(int idx, const char* ip, int port) {
    std::lock_guard<std::mutex> l(g_mu);
    Bot* b = getBot(idx);
    if (!b || !ip) return 0;
    if (b->rak) b->rak->Disconnect(100);
    ensureRak(*b);
    b->connected = false; b->spawned = false;
    bool ok = b->rak->Connect(ip, port, 0, 0, 5);
    b->connectTick = b->tick + 1; b->warned = false; b->dbgPackets = 0;
    dbg(*b, "Connect(%s:%d) returned %d", ip, port, ok ? 1 : 0);
    LOGI("bot %d (%s) connecting to %s:%d", idx, b->name.c_str(), ip, port);
    return 1;
}

HB_API int hb_disconnect(int idx) {
    std::lock_guard<std::mutex> l(g_mu);
    Bot* b = getBot(idx);
    if (!b) return 0;
    destroyRak(*b);
    return 1;
}

HB_API int hb_set_debug(int on) { g_debug = (on != 0); return 1; }

HB_API int hb_register_rpc(int id) {
    if (id < 0 || id > 255) return 0;
    std::lock_guard<std::mutex> l(g_mu);
    initOnce();
    g_rpcReg[id] = true;
    for (int i = 0; i < MAX_BOTS; i++) if (g_bots[i]) applyRpcRegistrations(*g_bots[i]);
    return 1;
}

HB_API int hb_register_packet(int id) {
    if (id < 0 || id > 255) return 0;
    std::lock_guard<std::mutex> l(g_mu);
    g_pktReg[id] = true;
    return 1;
}

HB_API int hb_poll(HbEvent* ev, unsigned char* buf, int cap) {
    std::lock_guard<std::mutex> l(g_evmu);
    if (g_events.empty()) return 0;
    Event& e = g_events.front();
    int n = (int)e.data.size();
    if (n > cap) n = cap;
    if (n > 0) memcpy(buf, e.data.data(), n);
    ev->bot = e.bot; ev->kind = e.kind; ev->id = e.id; ev->len = n;
    g_events.pop_front();
    return 1;
}

HB_API int hb_player_id(int idx) {
    std::lock_guard<std::mutex> l(g_mu);
    Bot* b = getBot(idx);
    return b ? (int)b->playerId : -1;
}

HB_API int hb_is_connected(int idx) {
    std::lock_guard<std::mutex> l(g_mu);
    Bot* b = getBot(idx);
    return (b && b->connected) ? 1 : 0;
}

HB_API int hb_chat(int idx, const char* text) {
    std::lock_guard<std::mutex> l(g_mu);
    Bot* b = getBot(idx);
    if (!b || !b->rak || !text) return 0;
    RakNet::BitStream bs;
    uint8_t n = (uint8_t)strlen(text);
    bs.Write(n); bs.Write(text, n);
    rpcSend(*b, 101, bs);
    return 1;
}

HB_API int hb_cmd(int idx, const char* text) {
    std::lock_guard<std::mutex> l(g_mu);
    Bot* b = getBot(idx);
    if (!b || !b->rak || !text) return 0;
    RakNet::BitStream bs;
    int32_t n = (int32_t)strlen(text);
    bs.Write(n); bs.Write(text, n);
    rpcSend(*b, 50, bs);
    return 1;
}

HB_API int hb_request_class(int idx, int cls) {
    std::lock_guard<std::mutex> l(g_mu);
    Bot* b = getBot(idx);
    if (!b || !b->rak) return 0;
    RakNet::BitStream a; int32_t c = cls; a.Write(c);
    rpcSend(*b, 128, a);
    return 1;
}

HB_API int hb_request_spawn(int idx) {
    std::lock_guard<std::mutex> l(g_mu);
    Bot* b = getBot(idx);
    if (!b || !b->rak) return 0;
    g_spawnState[b->slot] = 0;
    RakNet::BitStream r;
    rpcSend(*b, 129, r);
    return 1;
}

// Spawn RPC is sent only after the server approved RequestSpawn (a blind Spawn gets kicked)
HB_API int hb_send_spawn(int idx) {
    std::lock_guard<std::mutex> l(g_mu);
    Bot* b = getBot(idx);
    if (!b || !b->rak) return 0;
    b->wantSpawn = true;
    return 1;
}

HB_API int hb_spawn(int idx) {
    if (!hb_request_class(idx, 0)) return 0;
    hb_request_spawn(idx);
    return hb_send_spawn(idx);
}

HB_API int hb_is_spawned(int idx) {
    std::lock_guard<std::mutex> l(g_mu);
    Bot* b = getBot(idx);
    return (b && b->spawned) ? 1 : 0;
}

HB_API int hb_set_pos(int idx, float x, float y, float z) {
    std::lock_guard<std::mutex> l(g_mu);
    Bot* b = getBot(idx);
    if (!b) return 0;
    b->pos[0] = x; b->pos[1] = y; b->pos[2] = z;
    return 1;
}

HB_API int hb_dialog_response(int idx, int dialogId, int button, int listItem, const char* text) {
    std::lock_guard<std::mutex> l(g_mu);
    Bot* b = getBot(idx);
    if (!b || !b->rak) return 0;
    RakNet::BitStream bs;
    uint16_t did = (uint16_t)dialogId, li = (uint16_t)listItem;
    uint8_t btn = (uint8_t)button;
    const char* t = text ? text : "";
    uint8_t n = (uint8_t)strlen(t);
    bs.Write(did); bs.Write(btn); bs.Write(li); bs.Write(n); bs.Write(t, n);
    rpcSend(*b, 62, bs);
    return 1;
}

HB_API int hb_send_rpc(int idx, int id, const unsigned char* data, int len) {
    std::lock_guard<std::mutex> l(g_mu);
    Bot* b = getBot(idx);
    if (!b || !b->rak || id < 0 || id > 255) return 0;
    RakNet::BitStream bs;
    if (data && len > 0) bs.Write((const char*)data, len);
    rpcSend(*b, id, bs);
    return 1;
}

HB_API int hb_send_packet(int idx, const unsigned char* data, int len) {
    std::lock_guard<std::mutex> l(g_mu);
    Bot* b = getBot(idx);
    if (!b || !b->rak || !data || len <= 0) return 0;
    RakNet::BitStream bs;
    bs.Write((const char*)data, len);
    b->rak->Send(&bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0);
    return 1;
}

// priority: 0 system,1 high,2 medium,3 low | reliability: 0 unreliable,1 unreliable_seq,2 reliable,3 reliable_ordered,4 reliable_seq
HB_API int hb_send_packet_ex(int idx, const unsigned char* data, int len, int priority, int reliability, int channel) {
    std::lock_guard<std::mutex> l(g_mu);
    Bot* b = getBot(idx);
    if (!b || !b->rak || !data || len <= 0) return 0;
    static const PacketPriority pr[4] = {SYSTEM_PRIORITY, HIGH_PRIORITY, MEDIUM_PRIORITY, LOW_PRIORITY};
    static const PacketReliability rl[5] = {UNRELIABLE, UNRELIABLE_SEQUENCED, RELIABLE, RELIABLE_ORDERED, RELIABLE_SEQUENCED};
    if (priority < 0 || priority > 3 || reliability < 0 || reliability > 4) return 0;
    if (data[0] >= 200 && data[0] <= 212) b->lastManual = b->tick;   // script drives sync: pause the idle sync
    RakNet::BitStream bs;
    bs.Write((const char*)data, len);
    b->rak->Send(&bs, pr[priority], rl[reliability], (char)channel);
    return 1;
}

HB_API void hb_shutdown(void) {
    std::lock_guard<std::mutex> l(g_mu);
    for (int i = 0; i < MAX_BOTS; i++) if (g_bots[i]) { destroyRak(*g_bots[i]); g_bots[i].reset(); }
}
