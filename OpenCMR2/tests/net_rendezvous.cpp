#include "net/join_code.h"
#include "net/lobby_wire.h"
#include "net/net_session.h"
#include "net/net_wire.h"
#include "net/rendezvous.h"

#include "net/enet_platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace lobby = net::lobby;
using lobby::Endpoint;

int s_failures;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            s_failures++;                                                              \
        }                                                                              \
    } while (0)

void Log(const char *format, ...)
{
    if (getenv("NET_TEST_VERBOSE") == nullptr)
        return;
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

Endpoint At(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port)
{
    uint8_t bytes[4] = { a, b, c, d };
    Endpoint e;
    memcpy(&e.address, bytes, sizeof(bytes));
    e.port = port;
    return e;
}

lobby::Key MakeKey(uint8_t seed)
{
    lobby::Key key;
    for (size_t i = 0; i < key.size(); i++)
        key[i] = (uint8_t)(seed + i);
    return key;
}

const Endpoint kLobby = At(203, 0, 113, 5, 28630);
const Endpoint kLocal = At(192, 168, 1, 10, 28620);
const Endpoint kHostPublic = At(198, 51, 100, 9, 28620);
const Endpoint kHostLocal = At(192, 168, 5, 5, 28620);
const lobby::Guid kInstance = { { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 } };
const lobby::Code kCode = { { '7', 'K', '3', 'M', '9', 'Q', 'X', '2' } };

struct Harness {
    std::mt19937_64 rng;
    std::vector<std::pair<Endpoint, std::vector<uint8_t>>> sent;
    net::Rendezvous rv;

    explicit Harness(net::RelayMode mode = net::RelayMode::Auto)
        : rng(7),
          rv([this](const Endpoint &to, const std::vector<uint8_t> &data) { sent.push_back({ to, data }); }, mode,
             rng)
    {
    }

    template <typename T>
    bool Deliver(const T &message, const Endpoint &from, uint32_t now)
    {
        std::vector<uint8_t> data = lobby::Encode(message);
        return rv.Handle(data.data(), data.size(), from, now);
    }

    template <typename T>
    std::vector<std::pair<Endpoint, T>> Take()
    {
        std::vector<std::pair<Endpoint, T>> out;
        for (auto it = sent.begin(); it != sent.end();) {
            T message;
            if (lobby::Decode(it->second.data(), it->second.size(), message)) {
                out.push_back({ it->first, message });
                it = sent.erase(it);
            } else {
                ++it;
            }
        }
        return out;
    }

    std::set<Endpoint> PunchTargets(uint32_t now)
    {
        rv.Tick(now);
        std::set<Endpoint> targets;
        for (const auto &p : Take<lobby::Punch>())
            targets.insert(p.first);
        return targets;
    }

    void Register(uint32_t now, const lobby::Code &code, const lobby::Key &secret)
    {
        rv.Tick(now);
        auto registers = Take<lobby::Register>();
        CHECK(!registers.empty());
        if (registers.empty())
            return;
        lobby::Registered reply;
        reply.nonce = registers.back().second.nonce;
        reply.code = code;
        reply.secret = secret;
        CHECK(Deliver(reply, kLobby, now));
    }

    void Resolve(uint32_t now, const Endpoint &hostPublic, const Endpoint &hostLocal)
    {
        rv.Tick(now);
        auto resolves = Take<lobby::Resolve>();
        CHECK(resolves.size() == 1);
        if (resolves.empty())
            return;
        lobby::Resolved reply;
        reply.nonce = resolves.back().second.nonce;
        reply.instance = kInstance;
        reply.hostPublic = hostPublic;
        reply.hostLocal = hostLocal;
        CHECK(Deliver(reply, kLobby, now));
    }
};

NetCodeStatus HostStatus(Harness &h, lobby::Code *code = nullptr)
{
    return h.rv.HostStatus(code);
}

NetCodeStatus FindStatus(Harness &h, bool *relayed = nullptr)
{
    lobby::Guid instance = {};
    NetCodeStatus status = h.rv.FindStatus(&instance, relayed);
    if (status == NET_CODE_FOUND)
        CHECK(instance == kInstance);
    return status;
}

void TestRegister()
{
    Harness h;
    h.rv.SetLobby(kLobby, kLocal);
    CHECK(HostStatus(h) == NET_CODE_IDLE);
    CHECK(!h.rv.NeedsLobby());
    h.rv.Host(kInstance);
    CHECK(HostStatus(h) == NET_CODE_CONTACTING);
    CHECK(h.rv.NeedsLobby());
    h.rv.Tick(1000);
    auto registers = h.Take<lobby::Register>();
    CHECK(registers.size() == 1);
    if (registers.size() != 1)
        return;
    CHECK(registers[0].first == kLobby);
    CHECK(registers[0].second.instance == kInstance);
    CHECK(registers[0].second.local == kLocal);
    uint32_t firstNonce = registers[0].second.nonce;

    lobby::Registered reply;
    reply.nonce = firstNonce;
    reply.code = kCode;
    reply.secret = MakeKey(1);
    CHECK(!h.Deliver(reply, At(198, 51, 100, 1, 28630), 1010));
    CHECK(!h.Deliver(reply, At(203, 0, 113, 5, 28631), 1010));
    lobby::Registered wrongNonce = reply;
    wrongNonce.nonce ^= 2;
    CHECK(!h.Deliver(wrongNonce, kLobby, 1010));
    lobby::Registered badCode = reply;
    badCode.code[0] = 'U';
    CHECK(!h.Deliver(badCode, kLobby, 1010));
    CHECK(HostStatus(h) == NET_CODE_CONTACTING);
    CHECK(h.Deliver(reply, kLobby, 1010));
    lobby::Code code = {};
    CHECK(HostStatus(h, &code) == NET_CODE_READY);
    CHECK(code == kCode);
    CHECK(net::joincode::Format(code) == "7K3M-9QX2");
    CHECK(!h.Deliver(reply, kLobby, 1020));

    h.rv.Tick(11009);
    CHECK(h.Take<lobby::Refresh>().empty());
    h.rv.Tick(11010);
    auto refreshes = h.Take<lobby::Refresh>();
    CHECK(refreshes.size() == 1);
    if (refreshes.size() != 1)
        return;
    CHECK(refreshes[0].first == kLobby);
    CHECK(refreshes[0].second.code == kCode);
    CHECK(refreshes[0].second.secret == MakeKey(1));
    CHECK(refreshes[0].second.local == kLocal);
    lobby::Refreshed refreshed;
    refreshed.nonce = refreshes[0].second.nonce ^ 2;
    CHECK(!h.Deliver(refreshed, kLobby, 11020));
    refreshed.nonce = refreshes[0].second.nonce;
    CHECK(!h.Deliver(refreshed, kHostPublic, 11020));
    CHECK(h.Deliver(refreshed, kLobby, 11020));
    CHECK(!h.Deliver(refreshed, kLobby, 11030));

    h.rv.Tick(21010);
    refreshes = h.Take<lobby::Refresh>();
    CHECK(refreshes.size() == 1);
    if (refreshes.size() != 1)
        return;
    lobby::Failed unknown;
    unknown.nonce = refreshes[0].second.nonce;
    unknown.reason = lobby::FAILURE_UNKNOWN_SESSION;
    CHECK(h.Deliver(unknown, kLobby, 21020));
    CHECK(HostStatus(h) == NET_CODE_CONTACTING);
    h.rv.Tick(21020);
    registers = h.Take<lobby::Register>();
    CHECK(registers.size() == 1);
    if (registers.size() != 1)
        return;
    CHECK(registers[0].second.nonce != firstNonce);
    lobby::Code second = { { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H' } };
    reply.nonce = registers[0].second.nonce;
    reply.code = second;
    reply.secret = MakeKey(50);
    CHECK(h.Deliver(reply, kLobby, 21030));
    CHECK(HostStatus(h, &code) == NET_CODE_READY);
    CHECK(code == second);

    h.sent.clear();
    h.rv.StopHosting(true);
    auto unregisters = h.Take<lobby::Unregister>();
    CHECK(unregisters.size() == 1);
    if (unregisters.size() == 1) {
        CHECK(unregisters[0].first == kLobby);
        CHECK(unregisters[0].second.code == second);
        CHECK(unregisters[0].second.secret == MakeKey(50));
    }
    CHECK(HostStatus(h) == NET_CODE_IDLE);
    CHECK(!h.rv.NeedsLobby());

    h.rv.Host(kInstance);
    h.Register(30000, kCode, MakeKey(1));
    CHECK(HostStatus(h) == NET_CODE_READY);
    h.sent.clear();
    h.rv.StopHosting(false);
    CHECK(h.sent.empty());
    CHECK(HostStatus(h) == NET_CODE_IDLE);
}

void TestLobbyUnreachable()
{
    Harness h;
    h.rv.Host(kInstance);
    for (uint32_t t = 1000; t <= 30000; t += 1000)
        h.rv.Tick(t);
    CHECK(h.sent.empty());
    CHECK(HostStatus(h) == NET_CODE_CONTACTING);
    h.rv.LobbyUnresolved();
    CHECK(HostStatus(h) == NET_CODE_LOBBY_UNREACHABLE);
    CHECK(h.rv.LobbyUnreachable());
    h.rv.Tick(31000);
    CHECK(h.sent.empty());

    h.rv.SetLobby(kLobby, kLocal);
    CHECK(HostStatus(h) == NET_CODE_CONTACTING);
    std::set<uint32_t> nonces;
    size_t count = 0;
    for (uint32_t t = 31001; t <= 36001; t += 1000) {
        h.rv.Tick(t);
        for (const auto &r : h.Take<lobby::Register>()) {
            nonces.insert(r.second.nonce);
            count++;
        }
        h.rv.LobbyUnresolved();
    }
    CHECK(count == 6);
    CHECK(nonces.size() == 1);
    CHECK(HostStatus(h) == NET_CODE_LOBBY_UNREACHABLE);
    h.rv.Tick(41000);
    CHECK(h.Take<lobby::Register>().empty());
    h.rv.Tick(41001);
    auto registers = h.Take<lobby::Register>();
    CHECK(registers.size() == 1);
    if (registers.size() == 1) {
        lobby::Registered reply;
        reply.nonce = registers[0].second.nonce;
        reply.code = kCode;
        reply.secret = MakeKey(3);
        CHECK(h.Deliver(reply, kLobby, 41050));
        CHECK(HostStatus(h) == NET_CODE_READY);
        CHECK(!h.rv.LobbyUnreachable());
    }

    Harness full;
    full.rv.SetLobby(kLobby, kLocal);
    full.rv.Host(kInstance);
    full.rv.Tick(1000);
    registers = full.Take<lobby::Register>();
    CHECK(registers.size() == 1);
    if (registers.size() == 1) {
        lobby::Failed failed;
        failed.nonce = registers[0].second.nonce;
        failed.reason = lobby::FAILURE_FULL;
        CHECK(full.Deliver(failed, kLobby, 1010));
        CHECK(HostStatus(full) == NET_CODE_LOBBY_UNREACHABLE);
    }
}

void TestIntroduce()
{
    Harness h;
    h.rv.SetLobby(kLobby, kLocal);
    h.rv.Host(kInstance);
    h.Register(1000, kCode, MakeKey(1));
    h.sent.clear();
    const Endpoint guestPublic = At(198, 51, 100, 7, 40000);
    const Endpoint guestLocal = At(10, 0, 0, 5, 28620);
    lobby::Introduce m;
    m.secret = MakeKey(1);
    m.guestPublic = guestPublic;
    m.guestLocal = guestLocal;
    lobby::Introduce wrongSecret = m;
    wrongSecret.secret[0] ^= 1;
    CHECK(!h.Deliver(wrongSecret, kLobby, 1100));
    CHECK(!h.Deliver(m, guestPublic, 1100));
    CHECK(h.PunchTargets(1100).empty());
    CHECK(h.Deliver(m, kLobby, 1100));
    std::map<Endpoint, int> counts;
    std::map<Endpoint, std::set<uint32_t>> nonces;
    for (uint32_t t = 1100; t <= 6000; t += 50) {
        h.rv.Tick(t);
        for (const auto &p : h.Take<lobby::Punch>()) {
            counts[p.first]++;
            nonces[p.first].insert(p.second.nonce);
        }
    }
    CHECK(counts.size() == 2);
    CHECK(counts[guestPublic] == 15);
    CHECK(counts[guestLocal] == 15);
    CHECK(nonces[guestPublic].size() == 1);
}

void TestCandidateFilter()
{
    CHECK(net::IsPrivateAddress(At(10, 1, 2, 3, 0).address));
    CHECK(net::IsPrivateAddress(At(172, 16, 0, 1, 0).address));
    CHECK(net::IsPrivateAddress(At(172, 31, 255, 255, 0).address));
    CHECK(!net::IsPrivateAddress(At(172, 15, 0, 1, 0).address));
    CHECK(!net::IsPrivateAddress(At(172, 32, 0, 1, 0).address));
    CHECK(net::IsPrivateAddress(At(192, 168, 0, 1, 0).address));
    CHECK(!net::IsPrivateAddress(At(192, 169, 0, 1, 0).address));
    CHECK(!net::IsPrivateAddress(At(8, 8, 8, 8, 0).address));
    CHECK(!net::IsPrivateAddress(At(127, 0, 0, 1, 0).address));
    CHECK(net::IsLoopbackAddress(At(127, 0, 0, 1, 0).address));
    CHECK(!net::IsLoopbackAddress(At(10, 0, 0, 1, 0).address));
    CHECK(net::IsPublicAddress(At(8, 8, 8, 8, 0).address));
    CHECK(net::IsPublicAddress(At(203, 0, 113, 5, 0).address));
    CHECK(!net::IsPublicAddress(At(10, 1, 2, 3, 0).address));
    CHECK(!net::IsPublicAddress(At(172, 20, 0, 1, 0).address));
    CHECK(net::IsPublicAddress(At(172, 32, 0, 1, 0).address));
    CHECK(!net::IsPublicAddress(At(192, 168, 1, 1, 0).address));
    CHECK(!net::IsPublicAddress(At(127, 0, 0, 1, 0).address));
    CHECK(!net::IsPublicAddress(At(169, 254, 3, 4, 0).address));
    CHECK(net::IsPublicAddress(At(169, 253, 3, 4, 0).address));
    CHECK(!net::IsPublicAddress(At(100, 64, 0, 1, 0).address));
    CHECK(!net::IsPublicAddress(At(100, 101, 102, 103, 0).address));
    CHECK(!net::IsPublicAddress(At(100, 127, 255, 255, 0).address));
    CHECK(net::IsPublicAddress(At(100, 63, 255, 255, 0).address));
    CHECK(net::IsPublicAddress(At(100, 128, 0, 0, 0).address));

    Harness h;
    h.rv.SetLobby(kLobby, kLocal);
    h.rv.Host(kInstance);
    h.Register(1000, kCode, MakeKey(1));
    h.sent.clear();
    const Endpoint guestPublic = At(198, 51, 100, 7, 40000);
    struct Case {
        Endpoint local;
        size_t targets;
    } cases[] = {
        { At(8, 8, 8, 8, 28620), 1 },
        { At(10, 0, 0, 5, 0), 1 },
        { At(0, 0, 0, 0, 0), 1 },
        { At(172, 32, 0, 1, 28620), 1 },
        { At(172, 20, 0, 1, 28620), 2 },
    };
    uint32_t now = 2000;
    for (const Case &c : cases) {
        lobby::Introduce m;
        m.secret = MakeKey(1);
        m.guestPublic = guestPublic;
        m.guestLocal = c.local;
        CHECK(h.Deliver(m, kLobby, now));
        std::set<Endpoint> targets = h.PunchTargets(now);
        CHECK(targets.size() == c.targets);
        CHECK(targets.count(guestPublic) == 1);
        for (uint32_t t = now; t < now + 4000; t += 200)
            h.PunchTargets(t);
        now += 5000;
    }

    lobby::Introduce same;
    same.secret = MakeKey(1);
    same.guestPublic = At(10, 0, 0, 9, 5000);
    same.guestLocal = same.guestPublic;
    CHECK(h.Deliver(same, kLobby, now));
    CHECK(h.PunchTargets(now).size() == 1);

    Harness g;
    g.rv.SetLobby(kLobby, kLocal);
    g.rv.Find(kCode);
    g.Resolve(1000, kHostPublic, At(8, 8, 4, 4, 28620));
    std::vector<Endpoint> targets = g.rv.QueryTargets();
    CHECK(targets.size() == 1 && targets[0] == kHostPublic);
    g.rv.Find(kCode);
    g.Resolve(2000, kHostPublic, At(192, 168, 5, 5, 0));
    CHECK(g.rv.QueryTargets().size() == 1);
    g.rv.Find(kCode);
    g.Resolve(3000, kHostLocal, kHostLocal);
    CHECK(g.rv.QueryTargets().size() == 1);
}

void TestPunchBounds()
{
    Harness h;
    h.rv.SetLobby(kLobby, kLocal);
    h.rv.Host(kInstance);
    h.Register(1000, kCode, MakeKey(1));
    h.sent.clear();
    std::set<Endpoint> first, second;
    for (int i = 0; i < 5; i++) {
        lobby::Introduce m;
        m.secret = MakeKey(1);
        m.guestPublic = At(198, 51, 100, (uint8_t)(10 + i), 40000);
        m.guestLocal = At(10, 0, 0, (uint8_t)(10 + i), 28620);
        bool accepted = h.Deliver(m, kLobby, 2000);
        CHECK(accepted == (i < 4));
        if (accepted) {
            first.insert(m.guestPublic);
            first.insert(m.guestLocal);
        }
    }
    CHECK(h.PunchTargets(2000) == first);
    lobby::Introduce late;
    late.secret = MakeKey(1);
    late.guestPublic = At(198, 51, 100, 99, 40000);
    CHECK(!h.Deliver(late, kLobby, 2999));
    for (int i = 0; i < 4; i++) {
        lobby::Introduce m;
        m.secret = MakeKey(1);
        m.guestPublic = At(198, 51, 100, (uint8_t)(50 + i), 40000);
        m.guestLocal = At(10, 0, 0, (uint8_t)(50 + i), 28620);
        CHECK(h.Deliver(m, kLobby, 3000));
        second.insert(m.guestPublic);
        second.insert(m.guestLocal);
    }
    CHECK(h.PunchTargets(3000) == second);
    size_t total = 0;
    for (uint32_t t = 3000; t <= 8000; t += 100) {
        h.rv.Tick(t);
        auto punches = h.Take<lobby::Punch>();
        CHECK(punches.size() <= 8);
        total += punches.size();
        for (const auto &p : punches)
            CHECK(second.count(p.first) == 1);
    }
    CHECK(total == 8 * 14);
}

void TestHostRelay()
{
    Harness h;
    h.rv.SetLobby(kLobby, kLocal);
    h.rv.Host(kInstance);
    h.Register(1000, kCode, MakeKey(1));
    h.sent.clear();
    const Endpoint relay = At(203, 0, 113, 5, 40001);
    lobby::RelayOffer offer;
    offer.secret = MakeKey(1);
    offer.port = relay.port;
    offer.token = MakeKey(100);
    lobby::RelayOffer wrongSecret = offer;
    wrongSecret.secret[3] ^= 8;
    CHECK(!h.Deliver(wrongSecret, kLobby, 1500));
    CHECK(!h.Deliver(offer, relay, 1500));
    CHECK(!h.rv.IsRelay(relay));
    CHECK(h.Deliver(offer, kLobby, 1500));
    CHECK(h.rv.IsRelay(relay));

    h.rv.Tick(2000);
    auto binds = h.Take<lobby::Bind>();
    CHECK(binds.size() == 1);
    if (binds.size() == 1) {
        CHECK(binds[0].first == relay);
        CHECK(binds[0].second.token == offer.token);
    }
    h.rv.Tick(2499);
    CHECK(h.Take<lobby::Bind>().empty());
    h.rv.Tick(2500);
    CHECK(h.Take<lobby::Bind>().size() == 1);
    lobby::Bound bound;
    bound.token = offer.token;
    CHECK(!h.Deliver(bound, kLobby, 2600));
    lobby::Bound wrongToken;
    wrongToken.token = MakeKey(101);
    CHECK(!h.Deliver(wrongToken, relay, 2600));
    CHECK(h.Deliver(bound, relay, 2600));
    CHECK(!h.Deliver(bound, relay, 2610));
    for (uint32_t t = 2700; t <= 8000; t += 100)
        h.rv.Tick(t);
    CHECK(h.Take<lobby::Bind>().empty());
    CHECK(h.rv.IsRelay(relay));

    const Endpoint lost = At(203, 0, 113, 5, 40002);
    offer.port = lost.port;
    offer.token = MakeKey(120);
    CHECK(h.Deliver(offer, kLobby, 8000));
    size_t count = 0;
    for (uint32_t t = 8000; t <= 20000; t += 100) {
        h.rv.Tick(t);
        for (const auto &b : h.Take<lobby::Bind>()) {
            CHECK(b.first == lost);
            count++;
        }
    }
    CHECK(count == 10);
    CHECK(!h.rv.IsRelay(lost));

    for (uint16_t i = 0; i < 9; i++) {
        offer.port = (uint16_t)(41000 + i);
        CHECK(h.Deliver(offer, kLobby, 20000));
    }
    CHECK(!h.rv.IsRelay(At(203, 0, 113, 5, 41000)));
    CHECK(h.rv.IsRelay(At(203, 0, 113, 5, 41001)));
    CHECK(h.rv.IsRelay(At(203, 0, 113, 5, 41008)));
    h.rv.Tick(20000);
    CHECK(h.Take<lobby::Bind>().size() == 8);
}

void TestFindDirect()
{
    Harness h;
    h.rv.SetLobby(kLobby, kLocal);
    CHECK(FindStatus(h) == NET_CODE_IDLE);
    h.rv.Find(kCode);
    CHECK(FindStatus(h) == NET_CODE_CONTACTING);
    CHECK(h.rv.NeedsLobby());
    CHECK(!h.rv.Finding());
    h.rv.Tick(1000);
    auto resolves = h.Take<lobby::Resolve>();
    CHECK(resolves.size() == 1);
    if (resolves.size() != 1)
        return;
    CHECK(resolves[0].first == kLobby);
    CHECK(resolves[0].second.code == kCode);
    CHECK(resolves[0].second.local == kLocal);
    lobby::Resolved reply;
    reply.nonce = resolves[0].second.nonce;
    reply.instance = kInstance;
    reply.hostPublic = kHostPublic;
    reply.hostLocal = kHostLocal;
    CHECK(!h.Deliver(reply, kHostPublic, 1100));
    lobby::Resolved wrongNonce = reply;
    wrongNonce.nonce ^= 4;
    CHECK(!h.Deliver(wrongNonce, kLobby, 1100));
    CHECK(FindStatus(h) == NET_CODE_CONTACTING);
    CHECK(h.Deliver(reply, kLobby, 1100));
    CHECK(FindStatus(h) == NET_CODE_CONNECTING);
    CHECK(h.rv.Finding());
    std::vector<Endpoint> targets = h.rv.QueryTargets();
    CHECK(targets.size() == 2);
    CHECK(targets.size() == 2 && targets[0] == kHostPublic && targets[1] == kHostLocal);

    lobby::Guid other = kInstance;
    other[0] ^= 1;
    h.rv.OnSessionReply(At(198, 51, 100, 10, 28620), kInstance);
    CHECK(FindStatus(h) == NET_CODE_CONNECTING);
    h.rv.OnSessionReply(kHostPublic, other);
    CHECK(FindStatus(h) == NET_CODE_CONNECTING);
    h.rv.OnSessionReply(kHostLocal, kInstance);
    bool relayed = true;
    CHECK(FindStatus(h, &relayed) == NET_CODE_FOUND);
    CHECK(!relayed);
    targets = h.rv.QueryTargets();
    CHECK(targets.size() == 1 && targets[0] == kHostLocal);
    CHECK(!h.rv.NeedsLobby());
    CHECK(!h.rv.Finding());
    h.rv.OnSessionReply(kHostPublic, kInstance);
    targets = h.rv.QueryTargets();
    CHECK(targets.size() == 1 && targets[0] == kHostLocal);
    CHECK(!h.Deliver(reply, kLobby, 1200));
    h.sent.clear();
    for (uint32_t t = 1200; t <= 30000; t += 500)
        h.rv.Tick(t);
    CHECK(h.sent.empty());
    CHECK(FindStatus(h) == NET_CODE_FOUND);
}

void TestFindFailures()
{
    Harness h;
    h.rv.SetLobby(kLobby, kLocal);
    h.rv.Find(kCode);
    for (uint32_t t = 1000; t <= 5000; t += 1000)
        h.rv.Tick(t);
    CHECK(h.Take<lobby::Resolve>().size() == 5);
    CHECK(FindStatus(h) == NET_CODE_CONTACTING);
    h.rv.Tick(6000);
    CHECK(FindStatus(h) == NET_CODE_LOBBY_UNREACHABLE);
    for (uint32_t t = 6000; t <= 20000; t += 1000)
        h.rv.Tick(t);
    CHECK(h.sent.empty());

    struct Case {
        lobby::Failure reason;
        NetCodeStatus status;
    } cases[] = {
        { lobby::FAILURE_NOT_FOUND, NET_CODE_NOT_FOUND },
        { lobby::FAILURE_RATE_LIMITED, NET_CODE_RATE_LIMITED },
        { lobby::FAILURE_VERSION, NET_CODE_LOBBY_UNREACHABLE },
    };
    for (const Case &c : cases) {
        h.rv.Find(kCode);
        h.rv.Tick(30000);
        auto resolves = h.Take<lobby::Resolve>();
        CHECK(resolves.size() == 1);
        if (resolves.size() != 1)
            continue;
        lobby::Failed failed;
        failed.nonce = resolves[0].second.nonce ^ 2;
        failed.reason = c.reason;
        CHECK(!h.Deliver(failed, kLobby, 30010));
        failed.nonce = resolves[0].second.nonce;
        CHECK(!h.Deliver(failed, kHostPublic, 30010));
        CHECK(FindStatus(h) == NET_CODE_CONTACTING);
        CHECK(h.Deliver(failed, kLobby, 30010));
        CHECK(FindStatus(h) == c.status);
        CHECK(!h.rv.NeedsLobby());
    }

    h.rv.Find(kCode);
    h.rv.Tick(40000);
    auto resolves = h.Take<lobby::Resolve>();
    CHECK(resolves.size() == 1);
    h.rv.CancelFind();
    CHECK(FindStatus(h) == NET_CODE_IDLE);
    CHECK(!h.rv.NeedsLobby());
    if (resolves.size() == 1) {
        lobby::Resolved late;
        late.nonce = resolves[0].second.nonce;
        late.instance = kInstance;
        late.hostPublic = kHostPublic;
        CHECK(!h.Deliver(late, kLobby, 40010));
    }
    CHECK(FindStatus(h) == NET_CODE_IDLE);
    CHECK(h.rv.QueryTargets().empty());
    h.rv.Tick(41000);
    CHECK(h.sent.empty());
}

void TestAutoRelay()
{
    Harness h(net::RelayMode::Auto);
    h.rv.SetLobby(kLobby, kLocal);
    h.rv.Find(kCode);
    h.Resolve(1100, kHostPublic, Endpoint());
    CHECK(FindStatus(h) == NET_CODE_CONNECTING);
    h.rv.Tick(4099);
    CHECK(h.Take<lobby::Relay>().empty());
    h.rv.Tick(4100);
    auto relays = h.Take<lobby::Relay>();
    CHECK(relays.size() == 1);
    if (relays.size() != 1)
        return;
    CHECK(relays[0].first == kLobby);
    CHECK(relays[0].second.code == kCode);
    CHECK(FindStatus(h) == NET_CODE_RELAYING);
    CHECK(h.rv.Finding());

    const Endpoint relay = At(203, 0, 113, 5, 40005);
    lobby::Relayed assigned;
    assigned.nonce = relays[0].second.nonce;
    assigned.port = relay.port;
    assigned.token = MakeKey(70);
    CHECK(!h.Deliver(assigned, relay, 4150));
    lobby::Relayed wrongNonce = assigned;
    wrongNonce.nonce ^= 2;
    CHECK(!h.Deliver(wrongNonce, kLobby, 4150));
    CHECK(h.Deliver(assigned, kLobby, 4150));
    CHECK(h.rv.IsRelay(relay));
    std::vector<Endpoint> targets = h.rv.QueryTargets();
    CHECK(targets.size() == 1 && targets[0] == kHostPublic);
    h.rv.Tick(4200);
    auto binds = h.Take<lobby::Bind>();
    CHECK(binds.size() == 1 && binds[0].first == relay && binds[0].second.token == assigned.token);
    h.rv.Tick(4699);
    CHECK(h.Take<lobby::Bind>().empty());
    h.rv.Tick(4700);
    CHECK(h.Take<lobby::Bind>().size() == 1);

    lobby::Bound bound;
    bound.token = assigned.token;
    CHECK(!h.Deliver(bound, kLobby, 4800));
    lobby::Bound wrongToken;
    wrongToken.token = MakeKey(71);
    CHECK(!h.Deliver(wrongToken, relay, 4800));
    CHECK(h.Deliver(bound, relay, 4800));
    targets = h.rv.QueryTargets();
    CHECK(targets.size() == 2 && targets[0] == kHostPublic && targets[1] == relay);
    h.rv.Tick(5300);
    CHECK(h.Take<lobby::Bind>().empty());

    h.rv.OnSessionReply(relay, kInstance);
    bool relayed = false;
    CHECK(FindStatus(h, &relayed) == NET_CODE_FOUND);
    CHECK(relayed);
    targets = h.rv.QueryTargets();
    CHECK(targets.size() == 1 && targets[0] == relay);
    h.rv.OnSessionReply(kHostPublic, kInstance);
    CHECK(FindStatus(h, &relayed) == NET_CODE_FOUND);
    CHECK(!relayed);
    targets = h.rv.QueryTargets();
    CHECK(targets.size() == 1 && targets[0] == kHostPublic);
    h.rv.OnSessionReply(relay, kInstance);
    CHECK(FindStatus(h, &relayed) == NET_CODE_FOUND);
    CHECK(!relayed);
}

void TestRelayAlways()
{
    Harness h(net::RelayMode::Always);
    h.rv.SetLobby(kLobby, kLocal);
    h.rv.Find(kCode);
    h.Resolve(1100, kHostPublic, kHostLocal);
    CHECK(FindStatus(h) == NET_CODE_RELAYING);
    auto relays = h.Take<lobby::Relay>();
    CHECK(relays.size() == 1);
    if (relays.size() != 1)
        return;
    CHECK(h.rv.QueryTargets().empty());
    const Endpoint relay = At(203, 0, 113, 5, 40009);
    lobby::Relayed assigned;
    assigned.nonce = relays[0].second.nonce;
    assigned.port = relay.port;
    assigned.token = MakeKey(90);
    CHECK(h.Deliver(assigned, kLobby, 1200));
    h.rv.Tick(1200);
    CHECK(h.Take<lobby::Bind>().size() == 1);
    lobby::Bound bound;
    bound.token = assigned.token;
    CHECK(h.Deliver(bound, relay, 1250));
    std::vector<Endpoint> targets = h.rv.QueryTargets();
    CHECK(targets.size() == 1 && targets[0] == relay);
    h.rv.OnSessionReply(kHostPublic, kInstance);
    h.rv.OnSessionReply(kHostLocal, kInstance);
    CHECK(FindStatus(h) == NET_CODE_RELAYING);
    h.rv.OnSessionReply(relay, kInstance);
    bool relayed = false;
    CHECK(FindStatus(h, &relayed) == NET_CODE_FOUND);
    CHECK(relayed);

    Harness refused(net::RelayMode::Always);
    refused.rv.SetLobby(kLobby, kLocal);
    refused.rv.Find(kCode);
    refused.Resolve(1100, kHostPublic, Endpoint());
    relays = refused.Take<lobby::Relay>();
    CHECK(relays.size() == 1);
    if (relays.size() == 1) {
        lobby::Failed failed;
        failed.nonce = relays[0].second.nonce;
        failed.reason = lobby::FAILURE_FULL;
        CHECK(refused.Deliver(failed, kLobby, 1200));
        CHECK(FindStatus(refused) == NET_CODE_HOST_UNREACHABLE);
    }
}

void TestRelayNever()
{
    Harness h(net::RelayMode::Never);
    h.rv.SetLobby(kLobby, kLocal);
    h.rv.Find(kCode);
    h.Resolve(1100, kHostPublic, Endpoint());
    for (uint32_t t = 1100; t < 4100; t += 100) {
        h.rv.Tick(t);
        CHECK(FindStatus(h) == NET_CODE_CONNECTING);
    }
    h.rv.Tick(4100);
    CHECK(FindStatus(h) == NET_CODE_HOST_UNREACHABLE);
    for (uint32_t t = 4100; t < 30000; t += 100)
        h.rv.Tick(t);
    CHECK(h.sent.empty());
    CHECK(h.rv.QueryTargets().empty());
}

void TestHostUnreachable()
{
    Harness h(net::RelayMode::Auto);
    h.rv.SetLobby(kLobby, kLocal);
    h.rv.Find(kCode);
    h.Resolve(1100, kHostPublic, Endpoint());
    size_t relays = 0;
    std::set<uint32_t> nonces;
    for (uint32_t t = 1100; t < 16100; t += 100) {
        h.rv.Tick(t);
        for (const auto &r : h.Take<lobby::Relay>()) {
            relays++;
            nonces.insert(r.second.nonce);
        }
        if (t >= 9100)
            CHECK(FindStatus(h) == NET_CODE_CONNECTING);
    }
    CHECK(relays == 5);
    CHECK(nonces.size() == 1);
    h.rv.Tick(16100);
    CHECK(FindStatus(h) == NET_CODE_HOST_UNREACHABLE);
    CHECK(h.rv.QueryTargets().empty());

    Harness b(net::RelayMode::Auto);
    b.rv.SetLobby(kLobby, kLocal);
    b.rv.Find(kCode);
    b.Resolve(1100, kHostPublic, Endpoint());
    b.rv.Tick(4100);
    auto requested = b.Take<lobby::Relay>();
    CHECK(requested.size() == 1);
    if (requested.size() != 1)
        return;
    lobby::Relayed assigned;
    assigned.nonce = requested[0].second.nonce;
    assigned.port = 40100;
    assigned.token = MakeKey(10);
    CHECK(b.Deliver(assigned, kLobby, 4150));
    size_t binds = 0;
    for (uint32_t t = 4200; t <= 9200; t += 100) {
        b.rv.Tick(t);
        binds += b.Take<lobby::Bind>().size();
    }
    CHECK(binds == 10);
    CHECK(FindStatus(b) == NET_CODE_CONNECTING);
    for (uint32_t t = 9300; t < 16100; t += 100)
        b.rv.Tick(t);
    CHECK(b.Take<lobby::Relay>().empty());
    CHECK(b.Take<lobby::Bind>().empty());
    b.rv.Tick(16100);
    CHECK(FindStatus(b) == NET_CODE_HOST_UNREACHABLE);
}

void TestGarbage()
{
    Harness h;
    h.rv.SetLobby(kLobby, kLocal);
    h.rv.Host(kInstance);
    h.Register(1000, kCode, MakeKey(1));
    h.rv.Find(kCode);
    std::mt19937 rng(99);
    for (int i = 0; i < 20000; i++) {
        uint8_t data[96];
        size_t n = rng() % sizeof(data);
        for (size_t j = 0; j < n; j++)
            data[j] = (uint8_t)rng();
        if (n >= 6) {
            memcpy(data, lobby::kMagic, 4);
            data[4] = (uint8_t)(lobby::KIND_REGISTER + rng() % 16);
            data[5] = lobby::kVersion;
        }
        h.rv.Handle(data, n, i % 2 ? kLobby : At(203, 0, 113, 5, (uint16_t)(40000 + i % 4)), 1100);
    }
    lobby::Register request;
    request.nonce = 1;
    CHECK(!h.Deliver(request, kLobby, 1100));
    CHECK(HostStatus(h) == NET_CODE_READY);
    CHECK(FindStatus(h) == NET_CODE_CONTACTING);
    h.rv.Tick(1200);
    CHECK(h.Take<lobby::Punch>().empty());
    CHECK(h.Take<lobby::Bind>().empty());
}

void TestElapsed()
{
    CHECK(net::Elapsed(1000, 0, 1000));
    CHECK(!net::Elapsed(999, 0, 1000));
    CHECK(net::Elapsed(1000, 1000, 0));
    CHECK(net::Elapsed(5, 0xFFFFFFF0u, 21));
    CHECK(!net::Elapsed(5, 0xFFFFFFF0u, 22));
    CHECK(!net::Elapsed(0xFFFFFFF0u, 5, 10));
    CHECK(!net::Elapsed(1000, 1001, 0));
    CHECK(!net::Elapsed(1000, 1001, 15000));
}

void TestStaleNow()
{
    Harness g;
    g.rv.SetLobby(kLobby, kLocal);
    g.rv.Find(kCode);
    g.rv.Tick(1000);
    auto resolves = g.Take<lobby::Resolve>();
    CHECK(resolves.size() == 1);
    if (resolves.size() == 1) {
        lobby::Resolved reply;
        reply.nonce = resolves[0].second.nonce;
        reply.instance = kInstance;
        reply.hostPublic = kHostPublic;
        CHECK(g.Deliver(reply, kLobby, 1001));
        g.rv.Tick(1000);
        CHECK(FindStatus(g) == NET_CODE_CONNECTING);
        g.rv.Tick(1001);
        CHECK(FindStatus(g) == NET_CODE_CONNECTING);
        CHECK(g.Take<lobby::Relay>().empty());
    }

    Harness r(net::RelayMode::Always);
    r.rv.SetLobby(kLobby, kLocal);
    r.rv.Find(kCode);
    r.rv.Tick(1000);
    resolves = r.Take<lobby::Resolve>();
    CHECK(resolves.size() == 1);
    if (resolves.size() == 1) {
        lobby::Resolved reply;
        reply.nonce = resolves[0].second.nonce;
        reply.instance = kInstance;
        reply.hostPublic = kHostPublic;
        CHECK(r.Deliver(reply, kLobby, 1001));
        CHECK(r.Take<lobby::Relay>().size() == 1);
        r.rv.Tick(1000);
        CHECK(r.Take<lobby::Relay>().empty());
        CHECK(FindStatus(r) == NET_CODE_RELAYING);
    }

    Harness h;
    h.rv.SetLobby(kLobby, kLocal);
    h.rv.Host(kInstance);
    h.rv.Tick(1000);
    auto registers = h.Take<lobby::Register>();
    CHECK(registers.size() == 1);
    if (registers.size() == 1) {
        lobby::Registered reply;
        reply.nonce = registers[0].second.nonce;
        reply.code = kCode;
        reply.secret = MakeKey(1);
        CHECK(h.Deliver(reply, kLobby, 1001));
        h.rv.Tick(1000);
        CHECK(h.Take<lobby::Refresh>().empty());
        CHECK(HostStatus(h) == NET_CODE_READY);
    }
}

void TestChallenge()
{
    Harness h;
    h.rv.SetLobby(kLobby, kLocal);
    h.rv.Host(kInstance);
    h.rv.Tick(1000);
    auto registers = h.Take<lobby::Register>();
    CHECK(registers.size() == 1);
    if (registers.size() != 1)
        return;
    CHECK(registers[0].second.cookie == lobby::Key{});
    uint32_t nonce = registers[0].second.nonce;
    lobby::Challenge challenge;
    challenge.nonce = nonce;
    challenge.cookie = MakeKey(30);
    CHECK(!h.Deliver(challenge, kHostPublic, 1010));
    CHECK(!h.Deliver(challenge, At(203, 0, 113, 5, 28631), 1010));
    lobby::Challenge wrongNonce = challenge;
    wrongNonce.nonce ^= 2;
    CHECK(!h.Deliver(wrongNonce, kLobby, 1010));
    lobby::Challenge zero = challenge;
    zero.nonce = 0;
    CHECK(!h.Deliver(zero, kLobby, 1010));
    CHECK(h.sent.empty());
    for (int i = 0; i < 3; i++) {
        challenge.cookie = MakeKey((uint8_t)(30 + i));
        CHECK(h.Deliver(challenge, kLobby, (uint32_t)(1010 + i)));
        registers = h.Take<lobby::Register>();
        CHECK(registers.size() == 1);
        if (registers.size() == 1) {
            CHECK(registers[0].second.nonce == nonce);
            CHECK(registers[0].second.cookie == MakeKey((uint8_t)(30 + i)));
        }
        CHECK(HostStatus(h) == NET_CODE_CONTACTING);
    }
    challenge.cookie = MakeKey(40);
    CHECK(h.Deliver(challenge, kLobby, 1020));
    CHECK(h.sent.empty());
    CHECK(HostStatus(h) == NET_CODE_LOBBY_UNREACHABLE);
    h.rv.Tick(7000);
    registers = h.Take<lobby::Register>();
    CHECK(registers.size() == 1);
    if (registers.size() != 1)
        return;
    CHECK(registers[0].second.nonce == nonce);
    CHECK(registers[0].second.cookie == MakeKey(40));
    challenge.cookie = MakeKey(41);
    CHECK(h.Deliver(challenge, kLobby, 7010));
    registers = h.Take<lobby::Register>();
    CHECK(registers.size() == 1 && registers[0].second.cookie == MakeKey(41));
    lobby::Registered registered;
    registered.nonce = nonce;
    registered.code = kCode;
    registered.secret = MakeKey(1);
    CHECK(h.Deliver(registered, kLobby, 7020));
    CHECK(HostStatus(h) == NET_CODE_READY);
    CHECK(!h.Deliver(challenge, kLobby, 7030));

    h.rv.Tick(17020);
    auto refreshes = h.Take<lobby::Refresh>();
    CHECK(refreshes.size() == 1);
    if (refreshes.size() != 1)
        return;
    CHECK(refreshes[0].second.cookie == MakeKey(41));
    lobby::Challenge refreshChallenge;
    refreshChallenge.nonce = refreshes[0].second.nonce;
    refreshChallenge.cookie = MakeKey(50);
    CHECK(h.Deliver(refreshChallenge, kLobby, 17030));
    auto again = h.Take<lobby::Refresh>();
    CHECK(again.size() == 1);
    if (again.size() == 1) {
        CHECK(again[0].second.nonce == refreshes[0].second.nonce);
        CHECK(again[0].second.cookie == MakeKey(50));
    }
    lobby::Refreshed refreshed;
    refreshed.nonce = refreshes[0].second.nonce;
    CHECK(h.Deliver(refreshed, kLobby, 17040));
    for (uint32_t t = 27020; t <= 57020; t += 10000)
        h.rv.Tick(t);
    CHECK(HostStatus(h) == NET_CODE_LOBBY_UNREACHABLE);

    Harness g(net::RelayMode::Always);
    g.rv.SetLobby(kLobby, kLocal);
    g.rv.Find(kCode);
    g.rv.Tick(1000);
    auto resolves = g.Take<lobby::Resolve>();
    CHECK(resolves.size() == 1);
    if (resolves.size() != 1)
        return;
    CHECK(resolves[0].second.cookie == lobby::Key{});
    challenge.nonce = resolves[0].second.nonce;
    challenge.cookie = MakeKey(60);
    CHECK(g.Deliver(challenge, kLobby, 1010));
    resolves = g.Take<lobby::Resolve>();
    CHECK(resolves.size() == 1);
    if (resolves.size() != 1)
        return;
    CHECK(resolves[0].second.nonce == challenge.nonce);
    CHECK(resolves[0].second.cookie == MakeKey(60));
    lobby::Resolved reply;
    reply.nonce = resolves[0].second.nonce;
    reply.instance = kInstance;
    reply.hostPublic = kHostPublic;
    CHECK(g.Deliver(reply, kLobby, 1020));
    auto relays = g.Take<lobby::Relay>();
    CHECK(relays.size() == 1);
    if (relays.size() != 1)
        return;
    CHECK(relays[0].second.cookie == MakeKey(60));
    challenge.nonce = relays[0].second.nonce;
    for (int i = 0; i < 3; i++) {
        challenge.cookie = MakeKey((uint8_t)(70 + i));
        CHECK(g.Deliver(challenge, kLobby, 1030));
        relays = g.Take<lobby::Relay>();
        CHECK(relays.size() == 1 && relays[0].second.nonce == challenge.nonce &&
              relays[0].second.cookie == MakeKey((uint8_t)(70 + i)));
    }
    CHECK(FindStatus(g) == NET_CODE_RELAYING);
    CHECK(g.Deliver(challenge, kLobby, 1040));
    CHECK(g.Take<lobby::Relay>().empty());
    CHECK(FindStatus(g) == NET_CODE_HOST_UNREACHABLE);

    Harness f;
    f.rv.SetLobby(kLobby, kLocal);
    f.rv.Find(kCode);
    f.rv.Tick(1000);
    resolves = f.Take<lobby::Resolve>();
    CHECK(resolves.size() == 1);
    if (resolves.size() != 1)
        return;
    challenge.nonce = resolves[0].second.nonce;
    for (int i = 0; i < 3; i++) {
        CHECK(f.Deliver(challenge, kLobby, 1010));
        CHECK(f.Take<lobby::Resolve>().size() == 1);
    }
    CHECK(f.Deliver(challenge, kLobby, 1020));
    CHECK(f.sent.empty());
    CHECK(FindStatus(f) == NET_CODE_LOBBY_UNREACHABLE);
}

void TestRefreshMisses()
{
    Harness h;
    h.rv.SetLobby(kLobby, kLocal);
    h.rv.Host(kInstance);
    h.Register(1000, kCode, MakeKey(1));
    lobby::Code code = {};
    std::vector<uint32_t> nonces;
    for (uint32_t t = 11000; t <= 31000; t += 10000) {
        h.rv.Tick(t);
        auto refreshes = h.Take<lobby::Refresh>();
        CHECK(refreshes.size() == 1);
        if (refreshes.size() == 1)
            nonces.push_back(refreshes[0].second.nonce);
        CHECK(HostStatus(h, &code) == NET_CODE_READY);
        CHECK(!h.rv.LobbyUnreachable());
    }
    h.rv.Tick(41000);
    CHECK(h.Take<lobby::Refresh>().size() == 1);
    code = {};
    CHECK(HostStatus(h, &code) == NET_CODE_LOBBY_UNREACHABLE);
    CHECK(code == lobby::Code{});
    CHECK(h.rv.LobbyUnreachable());
    h.rv.Tick(51000);
    auto refreshes = h.Take<lobby::Refresh>();
    CHECK(refreshes.size() == 1);
    if (refreshes.size() != 1)
        return;
    CHECK(refreshes[0].second.code == kCode);
    CHECK(refreshes[0].second.secret == MakeKey(1));
    CHECK(HostStatus(h) == NET_CODE_LOBBY_UNREACHABLE);
    lobby::Refreshed stale;
    stale.nonce = nonces[0];
    CHECK(!h.Deliver(stale, kLobby, 51010));
    CHECK(HostStatus(h) == NET_CODE_LOBBY_UNREACHABLE);
    lobby::Refreshed refreshed;
    refreshed.nonce = refreshes[0].second.nonce;
    CHECK(h.Deliver(refreshed, kLobby, 51010));
    CHECK(HostStatus(h, &code) == NET_CODE_READY);
    CHECK(code == kCode);

    h.rv.Tick(61000);
    refreshes = h.Take<lobby::Refresh>();
    CHECK(refreshes.size() == 1);
    if (refreshes.size() != 1)
        return;
    lobby::Challenge challenge;
    challenge.nonce = refreshes[0].second.nonce;
    challenge.cookie = MakeKey(9);
    for (int i = 0; i < 5; i++)
        CHECK(h.Deliver(challenge, kLobby, 61010));
    CHECK(h.Take<lobby::Refresh>().size() == 3);
    CHECK(HostStatus(h) == NET_CODE_READY);
    lobby::Failed rateLimited;
    rateLimited.nonce = refreshes[0].second.nonce;
    rateLimited.reason = lobby::FAILURE_RATE_LIMITED;
    CHECK(h.Deliver(rateLimited, kLobby, 61020));
    for (uint32_t t = 71000; t <= 91000; t += 10000)
        h.rv.Tick(t);
    CHECK(HostStatus(h) == NET_CODE_READY);
}

void TestLobbySwitch()
{
    const Endpoint other = At(203, 0, 113, 6, 28630);
    Harness h;
    h.rv.SetLobby(kLobby, kLocal);
    h.rv.Host(kInstance);
    h.Register(1000, kCode, MakeKey(1));
    h.rv.SetLobby(other, kLocal);
    h.rv.Tick(11000);
    auto refreshes = h.Take<lobby::Refresh>();
    CHECK(refreshes.size() == 1 && refreshes[0].first == kLobby);
    lobby::Introduce introduce;
    introduce.secret = MakeKey(1);
    introduce.guestPublic = At(198, 51, 100, 7, 40000);
    CHECK(!h.Deliver(introduce, other, 11010));
    CHECK(h.Deliver(introduce, kLobby, 11010));
    lobby::RelayOffer offer;
    offer.secret = MakeKey(1);
    offer.port = 40001;
    offer.token = MakeKey(100);
    CHECK(!h.Deliver(offer, other, 11010));
    CHECK(h.Deliver(offer, kLobby, 11010));
    h.sent.clear();
    for (uint32_t t = 21000; t <= 41000; t += 10000) {
        h.rv.Tick(t);
        for (const auto &r : h.Take<lobby::Refresh>())
            CHECK(r.first == kLobby);
    }
    CHECK(HostStatus(h) == NET_CODE_LOBBY_UNREACHABLE);
    h.sent.clear();
    h.rv.Tick(51000);
    refreshes = h.Take<lobby::Refresh>();
    CHECK(refreshes.size() == 1);
    if (refreshes.size() != 1)
        return;
    CHECK(refreshes[0].first == other);
    CHECK(refreshes[0].second.code == kCode);
    CHECK(refreshes[0].second.cookie == lobby::Key{});
    lobby::Refreshed refreshed;
    refreshed.nonce = refreshes[0].second.nonce;
    CHECK(!h.Deliver(refreshed, kLobby, 51010));
    CHECK(h.Deliver(refreshed, other, 51010));
    lobby::Code code = {};
    CHECK(HostStatus(h, &code) == NET_CODE_READY);
    CHECK(code == kCode);

    Harness g(net::RelayMode::Auto);
    g.rv.SetLobby(kLobby, kLocal);
    g.rv.Find(kCode);
    g.Resolve(1000, kHostPublic, Endpoint());
    g.rv.SetLobby(other, kLocal);
    g.rv.Tick(4000);
    auto relays = g.Take<lobby::Relay>();
    CHECK(relays.size() == 1 && relays[0].first == kLobby);
    if (relays.size() != 1)
        return;
    lobby::Relayed assigned;
    assigned.nonce = relays[0].second.nonce;
    assigned.port = 40005;
    assigned.token = MakeKey(70);
    CHECK(!g.Deliver(assigned, other, 4010));
    CHECK(g.Deliver(assigned, kLobby, 4010));
    CHECK(g.rv.IsRelay(At(203, 0, 113, 5, 40005)));
    g.rv.CancelFind();
    g.rv.Tick(5000);
    g.sent.clear();
    g.rv.Find(kCode);
    g.rv.Tick(5001);
    auto resolves = g.Take<lobby::Resolve>();
    CHECK(resolves.size() == 1 && resolves[0].first == other);

    Harness c;
    c.rv.SetLobby(kLobby, kLocal);
    c.rv.Find(kCode);
    c.rv.Tick(1000);
    resolves = c.Take<lobby::Resolve>();
    CHECK(resolves.size() == 1 && resolves[0].first == kLobby);
    c.rv.SetLobby(other, kLocal);
    c.rv.Tick(1001);
    resolves = c.Take<lobby::Resolve>();
    CHECK(resolves.size() == 1 && resolves[0].first == other);

    Harness u;
    u.rv.SetLobby(kLobby, kLocal);
    u.rv.Host(kInstance);
    u.rv.Tick(1000);
    CHECK(u.Take<lobby::Register>().size() == 1);
    u.rv.SetLobby(other, kLocal);
    u.rv.Tick(1001);
    auto registers = u.Take<lobby::Register>();
    CHECK(registers.size() == 1 && registers[0].first == other);
}

void TestNoLobby()
{
    Harness g;
    g.rv.Find(kCode);
    CHECK(g.rv.NeedsLobby());
    for (uint32_t t = 1000; t <= 10999; t += 1000) {
        g.rv.Tick(t);
        CHECK(FindStatus(g) == NET_CODE_CONTACTING);
    }
    CHECK(g.sent.empty());
    g.rv.Tick(11000);
    CHECK(FindStatus(g) == NET_CODE_LOBBY_UNREACHABLE);
    CHECK(g.rv.LobbyUnreachable());
    CHECK(!g.rv.NeedsLobby());

    Harness f;
    f.rv.Find(kCode);
    f.rv.Tick(1000);
    CHECK(FindStatus(f) == NET_CODE_CONTACTING);
    f.rv.LobbyUnresolved();
    CHECK(FindStatus(f) == NET_CODE_LOBBY_UNREACHABLE);
    CHECK(f.sent.empty());

    Harness s;
    s.rv.Find(kCode);
    for (uint32_t t = 1000; t <= 7000; t += 1000)
        s.rv.Tick(t);
    CHECK(s.sent.empty());
    s.rv.SetLobby(kLobby, kLocal);
    size_t resolves = 0;
    for (uint32_t t = 7000; t <= 10999; t += 500) {
        s.rv.Tick(t);
        resolves += s.Take<lobby::Resolve>().size();
        CHECK(FindStatus(s) == NET_CODE_CONTACTING);
    }
    CHECK(resolves == 4);
    s.rv.Tick(11000);
    CHECK(FindStatus(s) == NET_CODE_LOBBY_UNREACHABLE);

    Harness v;
    v.rv.SetLobby(kLobby, kLocal);
    v.rv.Find(kCode);
    v.rv.Tick(1000);
    v.rv.LobbyUnresolved();
    CHECK(FindStatus(v) == NET_CODE_CONTACTING);
    v.rv.Host(kInstance);
    v.rv.LobbyUnresolved();
    CHECK(HostStatus(v) == NET_CODE_CONTACTING);
}

void TestPathPinning()
{
    CHECK(net::ReplacesPath(false, true, false));
    CHECK(!net::ReplacesPath(false, true, true));
    CHECK(net::ReplacesPath(false, false, true));
    CHECK(net::ReplacesPath(true, true, false));
    CHECK(net::ReplacesPath(true, true, true));
    CHECK(net::ReplacesPath(true, false, false));
}

bool WaitFor(const std::function<bool()> &condition, int ms = 5000)
{
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (condition())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return condition();
}

class FakeLobby {
public:
    std::atomic<int> registers{0}, unregisters{0}, resolves{0}, relays{0}, forwarded{0}, forwardedEnet{0},
        forwardedRaw{0}, challenges{0};

    explicit FakeLobby(bool challenging = false) : challenging(challenging)
    {
        lobbySocket = Open(&lobbyPort);
        relaySocket = Open(&relayPort);
        running = true;
        thread = std::thread(&FakeLobby::Run, this);
    }

    ~FakeLobby()
    {
        running = false;
        thread.join();
        enet_socket_destroy(lobbySocket);
        enet_socket_destroy(relaySocket);
    }

    uint16_t Port() const { return lobbyPort; }

private:
    static ENetSocket Open(uint16_t *port)
    {
        ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        ENetAddress address;
        enet_address_set_host_ip(&address, "127.0.0.1");
        address.port = 0;
        CHECK(enet_socket_bind(s, &address) == 0);
        enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
        ENetAddress bound;
        CHECK(enet_socket_get_address(s, &bound) == 0);
        *port = bound.port;
        return s;
    }

    static void SendTo(ENetSocket s, const Endpoint &to, const std::vector<uint8_t> &data)
    {
        ENetAddress address;
        address.host = to.address;
        address.port = to.port;
        ENetBuffer buffer;
        buffer.data = (void *)data.data();
        buffer.dataLength = data.size();
        enet_socket_send(s, &address, &buffer, 1);
    }

    void Run()
    {
        while (running) {
            bool any = Poll(lobbySocket, false);
            any = Poll(relaySocket, true) || any;
            if (!any)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    bool Poll(ENetSocket s, bool relay)
    {
        uint8_t data[4096];
        ENetBuffer buffer;
        buffer.data = data;
        buffer.dataLength = sizeof(data);
        ENetAddress address;
        int n = enet_socket_receive(s, &address, &buffer, 1);
        if (n <= 0)
            return false;
        Endpoint from;
        from.address = address.host;
        from.port = address.port;
        if (relay)
            Relay(from, data, (size_t)n);
        else
            Lobby(from, data, (size_t)n);
        return true;
    }

    static lobby::Key CookieFor(const Endpoint &from)
    {
        lobby::Key key = MakeKey(77);
        key[0] ^= (uint8_t)from.port;
        key[1] ^= (uint8_t)(from.port >> 8);
        return key;
    }

    bool Challenged(const Endpoint &from, uint32_t nonce, const lobby::Key &cookie)
    {
        if (!challenging || cookie == CookieFor(from))
            return false;
        challenges++;
        lobby::Challenge reply;
        reply.nonce = nonce;
        reply.cookie = CookieFor(from);
        SendTo(lobbySocket, from, lobby::Encode(reply));
        return true;
    }

    void Lobby(const Endpoint &from, const uint8_t *data, size_t size)
    {
        lobby::Register reg;
        lobby::Refresh refresh;
        lobby::Unregister unreg;
        lobby::Resolve resolve;
        lobby::Relay relay;
        if (lobby::Decode(data, size, reg)) {
            if (Challenged(from, reg.nonce, reg.cookie))
                return;
            registers++;
            host = from;
            instance = reg.instance;
            lobby::Registered reply;
            reply.nonce = reg.nonce;
            reply.code = kCode;
            reply.secret = MakeKey(1);
            SendTo(lobbySocket, from, lobby::Encode(reply));
        } else if (lobby::Decode(data, size, refresh)) {
            lobby::Refreshed reply;
            reply.nonce = refresh.nonce;
            SendTo(lobbySocket, from, lobby::Encode(reply));
        } else if (lobby::Decode(data, size, unreg)) {
            if (unreg.code == kCode && unreg.secret == MakeKey(1))
                unregisters++;
        } else if (lobby::Decode(data, size, resolve)) {
            if (Challenged(from, resolve.nonce, resolve.cookie))
                return;
            resolves++;
            if (!host.Valid() || resolve.code != kCode) {
                lobby::Failed reply;
                reply.nonce = resolve.nonce;
                reply.reason = lobby::FAILURE_NOT_FOUND;
                SendTo(lobbySocket, from, lobby::Encode(reply));
                return;
            }
            lobby::Resolved reply;
            reply.nonce = resolve.nonce;
            reply.instance = instance;
            reply.hostPublic = host;
            SendTo(lobbySocket, from, lobby::Encode(reply));
            lobby::Introduce introduce;
            introduce.secret = MakeKey(1);
            introduce.guestPublic = from;
            introduce.guestLocal = resolve.local;
            SendTo(lobbySocket, host, lobby::Encode(introduce));
        } else if (lobby::Decode(data, size, relay)) {
            if (Challenged(from, relay.nonce, relay.cookie))
                return;
            if (!host.Valid() || relay.code != kCode)
                return;
            relays++;
            lobby::Relayed reply;
            reply.nonce = relay.nonce;
            reply.port = relayPort;
            reply.token = MakeKey(200);
            SendTo(lobbySocket, from, lobby::Encode(reply));
            lobby::RelayOffer offer;
            offer.secret = MakeKey(1);
            offer.port = relayPort;
            offer.token = MakeKey(200);
            SendTo(lobbySocket, host, lobby::Encode(offer));
        }
    }

    void Relay(const Endpoint &from, const uint8_t *data, size_t size)
    {
        lobby::Bind bind;
        if (lobby::Decode(data, size, bind)) {
            if (bind.token != MakeKey(200))
                return;
            if (std::find(bound.begin(), bound.end(), from) == bound.end() && bound.size() < 2)
                bound.push_back(from);
            lobby::Bound reply;
            reply.token = bind.token;
            SendTo(relaySocket, from, lobby::Encode(reply));
            return;
        }
        if (bound.size() != 2)
            return;
        const Endpoint *to = nullptr;
        if (from == bound[0])
            to = &bound[1];
        else if (from == bound[1])
            to = &bound[0];
        if (to == nullptr)
            return;
        forwarded++;
        if (size < 4 || memcmp(data, lobby::kMagic, 4) != 0)
            forwardedEnet++;
        else
            forwardedRaw++;
        SendTo(relaySocket, *to, std::vector<uint8_t>(data, data + size));
    }

    bool challenging;
    ENetSocket lobbySocket, relaySocket;
    uint16_t lobbyPort = 0, relayPort = 0;
    std::atomic<bool> running{false};
    std::thread thread;
    Endpoint host;
    lobby::Guid instance = {};
    std::vector<Endpoint> bound;
};

const NetGuid kApp = { { 0x43, 0x4d, 0x52, 0x32, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2 } };

std::unique_ptr<net::Session> MakeSession(uint16_t lobbyPort, net::RelayMode mode, uint16_t peerPort = 0)
{
    net::Config config;
    config.port = 0;
    config.broadcast = false;
    config.timeoutMs = 1500;
    config.joinTimeoutMs = 3000;
    if (lobbyPort != 0)
        config.lobby = "127.0.0.1:" + std::to_string(lobbyPort);
    config.relay = mode;
    if (peerPort != 0)
        config.peers.push_back("127.0.0.1:" + std::to_string(peerPort));
    config.log = Log;
    std::unique_ptr<net::Session> session(new net::Session(config));
    CHECK(session->Start() == NET_OK);
    return session;
}

bool Listed(net::Session &session, const NetGuid &instance)
{
    struct Context {
        NetGuid wanted;
        bool seen;
    } context = { instance, false };
    session.EnumSessions(&kApp,
                         [](const NetSessionDesc *d, void *p) -> BOOL {
                             Context *c = (Context *)p;
                             if (memcmp(&d->guidInstance, &c->wanted, sizeof(NetGuid)) == 0)
                                 c->seen = true;
                             return TRUE;
                         },
                         &context);
    return context.seen;
}

bool Receives(net::Session &session, NetPlayerID from)
{
    for (;;) {
        NetPlayerID sender = 0, to = 0;
        uint8_t data[256];
        DWORD size = sizeof(data);
        if (session.Receive(&sender, &to, data, &size) != NET_OK)
            return false;
        if (sender == from && size == 2 && memcmp(data, "hi", 2) == 0)
            return true;
    }
}

void JoinByCode(net::RelayMode guestMode, bool challenging)
{
    FakeLobby fake(challenging);
    std::unique_ptr<net::Session> host = MakeSession(fake.Port(), net::RelayMode::Auto);
    std::unique_ptr<net::Session> guest = MakeSession(fake.Port(), guestMode);

    std::string code;
    CHECK(host->HostCode(&code) == NET_CODE_IDLE);
    NetSessionDesc desc = {};
    desc.dwSize = sizeof(desc);
    desc.guidApplication = kApp;
    desc.dwMaxPlayers = 4;
    desc.lpszSessionNameA = (char *)"Coded";
    desc.lpszPasswordA = (char *)"pw";
    CHECK(host->Open(&desc, NET_OPEN_CREATE) == NET_OK);
    CHECK(WaitFor([&] { return host->HostCode(&code) == NET_CODE_READY; }));
    CHECK(code == "7K3M-9QX2");
    NetSessionDesc hosted;
    CHECK(host->GetSessionDesc(&hosted) == NET_OK);

    CHECK(guest->FindByCode("7K3M-9QX") == NET_CODE_INVALID);
    CHECK(guest->FindByCode("7k3m 9qx2") == NET_CODE_CONTACTING);
    NetGuid instance = {};
    bool relayed = !(guestMode == net::RelayMode::Always);
    CHECK(WaitFor([&] {
        Listed(*guest, hosted.guidInstance);
        return guest->CodeSession(&instance, &relayed) == NET_CODE_FOUND;
    }));
    CHECK(memcmp(&instance, &hosted.guidInstance, sizeof(instance)) == 0);
    CHECK(relayed == (guestMode == net::RelayMode::Always));
    CHECK(WaitFor([&] { return Listed(*guest, hosted.guidInstance); }));

    NetSessionDesc join = {};
    join.dwSize = sizeof(join);
    join.guidInstance = instance;
    join.lpszPasswordA = (char *)"pw";
    CHECK(guest->Open(&join, NET_OPEN_JOIN) == NET_OK);
    CHECK(guest->FindByCode("7K3M-9QX2") == NET_CODE_IDLE);
    CHECK(guest->CodeSession(nullptr, nullptr) == NET_CODE_IDLE);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    int raw = fake.forwardedRaw;
    for (int i = 0; i < 6; i++) {
        Listed(*guest, hosted.guidInstance);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    CHECK(fake.forwardedRaw == raw);
    CHECK(challenging == (fake.challenges > 0));

    NetName hostName = { sizeof(NetName), 0, (char *)"host", (char *)"host" };
    NetName guestName = { sizeof(NetName), 0, (char *)"guest", (char *)"guest" };
    NetPlayerID hostPlayer = 0, guestPlayer = 0;
    CHECK(host->CreatePlayer(&hostPlayer, &hostName, nullptr, 0) == NET_OK);
    CHECK(guest->CreatePlayer(&guestPlayer, &guestName, nullptr, 0) == NET_OK);
    CHECK(WaitFor([&] {
        guest->Send(guestPlayer, hostPlayer, TRUE, "hi", 2);
        return Receives(*host, guestPlayer);
    }));
    CHECK(WaitFor([&] {
        host->Send(hostPlayer, NET_ID_ALL, TRUE, "hi", 2);
        return Receives(*guest, hostPlayer);
    }));
    if (guestMode == net::RelayMode::Always) {
        CHECK(fake.forwarded > 0);
        CHECK(fake.forwardedEnet > 0);
    } else {
        CHECK(fake.relays == 0);
        CHECK(fake.forwardedEnet == 0);
    }

    CHECK(guest->Close() == NET_OK);
    CHECK(host->Close() == NET_OK);
    CHECK(WaitFor([&] { return fake.unregisters == 1; }));
    CHECK(host->HostCode(&code) == NET_CODE_IDLE);
    CHECK(code.empty());
}

NetSessionDesc Host(net::Session &session)
{
    NetSessionDesc desc = {};
    desc.dwSize = sizeof(desc);
    desc.guidApplication = kApp;
    desc.dwMaxPlayers = 4;
    desc.lpszSessionNameA = (char *)"Coded";
    desc.lpszPasswordA = (char *)"";
    CHECK(session.Open(&desc, NET_OPEN_CREATE) == NET_OK);
    NetSessionDesc hosted = {};
    CHECK(session.GetSessionDesc(&hosted) == NET_OK);
    return hosted;
}

HRESULT Join(net::Session &session, const NetGuid &instance)
{
    NetSessionDesc join = {};
    join.dwSize = sizeof(join);
    join.guidInstance = instance;
    join.lpszPasswordA = (char *)"";
    return session.Open(&join, NET_OPEN_JOIN);
}

void JoinPinned()
{
    FakeLobby fake;
    std::unique_ptr<net::Session> host = MakeSession(fake.Port(), net::RelayMode::Auto);
    NetSessionDesc hosted = Host(*host);
    std::string code;
    CHECK(WaitFor([&] { return host->HostCode(&code) == NET_CODE_READY; }));
    std::unique_ptr<net::Session> guest = MakeSession(fake.Port(), net::RelayMode::Always, host->BoundPort());
    CHECK(WaitFor([&] { return Listed(*guest, hosted.guidInstance); }));
    CHECK(guest->FindByCode(code.c_str()) == NET_CODE_CONTACTING);
    bool relayed = false;
    CHECK(WaitFor([&] {
        Listed(*guest, hosted.guidInstance);
        return guest->CodeSession(nullptr, &relayed) == NET_CODE_FOUND;
    }));
    CHECK(relayed);
    CHECK(fake.forwarded > 0);
    for (int i = 0; i < 10; i++) {
        Listed(*guest, hosted.guidInstance);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(Join(*guest, hosted.guidInstance) == NET_OK);
    NetName name = { sizeof(NetName), 0, (char *)"p", (char *)"p" };
    NetPlayerID hostPlayer = 0, guestPlayer = 0;
    CHECK(host->CreatePlayer(&hostPlayer, &name, nullptr, 0) == NET_OK);
    CHECK(guest->CreatePlayer(&guestPlayer, &name, nullptr, 0) == NET_OK);
    CHECK(WaitFor([&] {
        guest->Send(guestPlayer, hostPlayer, TRUE, "hi", 2);
        return Receives(*host, guestPlayer);
    }));
    CHECK(fake.forwardedEnet == 0);
}

void AbandonIsSilent()
{
    FakeLobby fake;
    std::unique_ptr<net::Session> host = MakeSession(fake.Port(), net::RelayMode::Auto);
    Host(*host);
    std::string code;
    CHECK(WaitFor([&] { return host->HostCode(&code) == NET_CODE_READY; }));
    host->Abandon();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(fake.unregisters == 0);
    CHECK(host->HostCode(&code) == NET_CODE_IDLE);
}

void MigrationRegisters()
{
    FakeLobby fake;
    std::unique_ptr<net::Session> host = MakeSession(fake.Port(), net::RelayMode::Auto);
    NetSessionDesc hosted = Host(*host);
    std::string code;
    CHECK(WaitFor([&] { return host->HostCode(&code) == NET_CODE_READY; }));
    std::unique_ptr<net::Session> a = MakeSession(fake.Port(), net::RelayMode::Auto, host->BoundPort());
    std::unique_ptr<net::Session> b = MakeSession(fake.Port(), net::RelayMode::Auto, host->BoundPort());
    CHECK(WaitFor([&] { return Listed(*a, hosted.guidInstance); }));
    CHECK(Join(*a, hosted.guidInstance) == NET_OK);
    CHECK(WaitFor([&] { return Listed(*b, hosted.guidInstance); }));
    CHECK(Join(*b, hosted.guidInstance) == NET_OK);
    CHECK(a->HostCode(&code) == NET_CODE_IDLE);
    CHECK(host->Close() == NET_OK);
    CHECK(WaitFor([&] { return a->IsHost(); }));
    CHECK(WaitFor([&] { return a->HostCode(&code) == NET_CODE_READY; }));
    CHECK(fake.registers == 2);
    CHECK(fake.unregisters == 1);
    CHECK(!b->IsHost());
    CHECK(b->HostCode(&code) == NET_CODE_IDLE);
}

void SessionWithoutLobby()
{
    std::unique_ptr<net::Session> session = MakeSession(0, net::RelayMode::Auto);
    std::string code;
    CHECK(session->HostCode(&code) == NET_CODE_NO_LOBBY);
    CHECK(session->FindByCode("7K3M-9QX2") == NET_CODE_NO_LOBBY);
    bool relayed = false;
    CHECK(session->CodeSession(nullptr, &relayed) == NET_CODE_IDLE);
    session->CancelFind();
}

void SessionUnknownCode()
{
    FakeLobby fake;
    std::unique_ptr<net::Session> guest = MakeSession(fake.Port(), net::RelayMode::Auto);
    Listed(*guest, NetGuid());
    CHECK(guest->FindByCode("ZZZZ-ZZZZ") == NET_CODE_CONTACTING);
    CHECK(WaitFor([&] { return guest->CodeSession(nullptr, nullptr) == NET_CODE_NOT_FOUND; }));
    CHECK(guest->FindByCode("ABCD-EFGH") == NET_CODE_CONTACTING);
    guest->CancelFind();
    CHECK(guest->CodeSession(nullptr, nullptr) == NET_CODE_IDLE);
    CHECK(guest->FindByCode("ABCD-EFGH") == NET_CODE_CONTACTING);
    Host(*guest);
    CHECK(guest->CodeSession(nullptr, nullptr) == NET_CODE_IDLE);
}

void FindWaitsForApplication()
{
    FakeLobby fake;
    std::unique_ptr<net::Session> host = MakeSession(fake.Port(), net::RelayMode::Auto);
    NetSessionDesc hosted = Host(*host);
    std::string code;
    CHECK(WaitFor([&] { return host->HostCode(&code) == NET_CODE_READY; }));
    std::unique_ptr<net::Session> guest = MakeSession(fake.Port(), net::RelayMode::Auto);
    CHECK(guest->FindByCode(code.c_str()) == NET_CODE_CONTACTING);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(fake.resolves == 0);
    CHECK(guest->CodeSession(nullptr, nullptr) == NET_CODE_CONTACTING);
    guest->CancelFind();
    CHECK(guest->CodeSession(nullptr, nullptr) == NET_CODE_IDLE);
    CHECK(guest->FindByCode(code.c_str()) == NET_CODE_CONTACTING);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(fake.resolves == 0);
    CHECK(WaitFor([&] {
        Listed(*guest, hosted.guidInstance);
        return guest->CodeSession(nullptr, nullptr) == NET_CODE_FOUND;
    }));
    CHECK(fake.resolves == 1);
    CHECK(WaitFor([&] { return Listed(*guest, hosted.guidInstance); }));
    CHECK(Join(*guest, hosted.guidInstance) == NET_OK);
}

ENetSocket OpenRaw(uint16_t *port)
{
    ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    ENetAddress address;
    enet_address_set_host_ip(&address, "127.0.0.1");
    address.port = 0;
    CHECK(enet_socket_bind(s, &address) == 0);
    enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
    ENetAddress bound;
    CHECK(enet_socket_get_address(s, &bound) == 0);
    *port = bound.port;
    return s;
}

void SendRawTo(ENetSocket s, const ENetAddress &to, const std::vector<uint8_t> &data)
{
    ENetBuffer buffer;
    buffer.data = (void *)data.data();
    buffer.dataLength = data.size();
    enet_socket_send(s, &to, &buffer, 1);
}

std::vector<uint8_t> ReceiveRaw(ENetSocket s, ENetAddress *from, const std::function<bool(const std::vector<uint8_t> &)> &wanted,
                                int ms = 3000)
{
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    uint8_t data[4096];
    while (std::chrono::steady_clock::now() < end) {
        ENetBuffer buffer;
        buffer.data = data;
        buffer.dataLength = sizeof(data);
        int n = enet_socket_receive(s, from, &buffer, 1);
        if (n > 0) {
            std::vector<uint8_t> packet(data, data + n);
            if (wanted(packet))
                return packet;
        } else {
            enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
            enet_socket_wait(s, &condition, 20);
        }
    }
    return std::vector<uint8_t>();
}

bool IsQuery(const std::vector<uint8_t> &p)
{
    return p.size() >= 26 && memcmp(p.data(), lobby::kMagic, 4) == 0 && p[4] == 1 && p[5] == 1;
}

bool IsReply(const std::vector<uint8_t> &p)
{
    return p.size() >= 10 && memcmp(p.data(), lobby::kMagic, 4) == 0 && p[4] == 2 && p[5] == 1;
}

void StaleClockBurst()
{
    uint16_t lobbyPort = 0;
    ENetSocket s = OpenRaw(&lobbyPort);
    int early = 0;
    for (int trial = 0; trial < 10; trial++) {
        std::unique_ptr<net::Session> guest = MakeSession(lobbyPort, net::RelayMode::Auto);
        Listed(*guest, NetGuid());
        CHECK(guest->FindByCode("7K3M-9QX2") == NET_CODE_CONTACTING);
        ENetAddress from;
        lobby::Resolve request;
        std::vector<uint8_t> packet = ReceiveRaw(s, &from, [&](const std::vector<uint8_t> &p) {
            return lobby::Decode(p.data(), p.size(), request);
        });
        CHECK(!packet.empty());
        if (packet.empty())
            continue;
        std::vector<uint8_t> junk = { 'C', 'M', 'R', '2', 2, 1, 0, 0, 0, 0 };
        for (int i = 0; i < 200; i++)
            SendRawTo(s, from, junk);
        lobby::Resolved reply;
        reply.nonce = request.nonce;
        reply.instance = kInstance;
        reply.hostPublic = At(127, 0, 0, 1, 9);
        SendRawTo(s, from, lobby::Encode(reply));
        auto stop = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
        bool connecting = false;
        while (std::chrono::steady_clock::now() < stop) {
            NetCodeStatus status = guest->CodeSession(nullptr, nullptr);
            connecting = connecting || status == NET_CODE_CONNECTING;
            if (status == NET_CODE_HOST_UNREACHABLE) {
                early++;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(connecting);
    }
    CHECK(early == 0);
    enet_socket_destroy(s);
}

std::vector<uint8_t> FakeReply(uint32_t nonce, uint8_t id)
{
    NetSessionDesc d = {};
    d.guidApplication = kApp;
    d.guidInstance.bytes[0] = 0xEE;
    d.guidInstance.bytes[1] = id;
    d.dwMaxPlayers = 4;
    net::Writer w;
    w.Bytes(lobby::kMagic, 4);
    w.U8(2);
    w.U8(1);
    w.U32(nonce);
    w.U32(d.dwFlags);
    w.Bytes(d.guidInstance.bytes, 16);
    w.Bytes(d.guidApplication.bytes, 16);
    w.U32(d.dwMaxPlayers);
    w.U32(0);
    for (int i = 0; i < 6; i++)
        w.U32(0);
    w.Str("flood");
    w.Str("");
    w.U8(0);
    return w.data;
}

size_t ListedCount(net::Session &session, uint8_t id, bool *seen)
{
    struct Context {
        uint8_t id;
        bool seen;
        size_t count;
    } context = { id, false, 0 };
    session.EnumSessions(&kApp,
                         [](const NetSessionDesc *d, void *p) -> BOOL {
                             Context *c = (Context *)p;
                             c->count++;
                             if (d->guidInstance.bytes[0] == 0xEE && d->guidInstance.bytes[1] == c->id)
                                 c->seen = true;
                             return TRUE;
                         },
                         &context);
    *seen = context.seen;
    return context.count;
}

void FoundIsBounded()
{
    uint16_t peerPort = 0;
    ENetSocket s = OpenRaw(&peerPort);
    std::unique_ptr<net::Session> guest = MakeSession(0, net::RelayMode::Auto, peerPort);
    bool seen = false;
    ListedCount(*guest, 0, &seen);
    ENetAddress from;
    std::vector<uint8_t> query = ReceiveRaw(s, &from, IsQuery);
    CHECK(!query.empty());
    if (query.empty()) {
        enet_socket_destroy(s);
        return;
    }
    uint32_t nonce = (uint32_t)query[6] | (uint32_t)query[7] << 8 | (uint32_t)query[8] << 16 | (uint32_t)query[9] << 24;
    for (int i = 0; i < 80; i++)
        SendRawTo(s, from, FakeReply(nonce, (uint8_t)i));
    CHECK(WaitFor([&] { return ListedCount(*guest, 0, &seen) == 64; }, 2000));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(ListedCount(*guest, 79, &seen) == 64);
    CHECK(!seen);
    ListedCount(*guest, 0, &seen);
    CHECK(seen);
    SendRawTo(s, from, FakeReply(nonce, 0));
    SendRawTo(s, from, FakeReply(nonce, 100));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(ListedCount(*guest, 100, &seen) == 64);
    CHECK(!seen);
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(4500);
    while (std::chrono::steady_clock::now() < end) {
        ListedCount(*guest, 0, &seen);
        for (;;) {
            query = ReceiveRaw(s, &from, IsQuery, 1);
            if (query.empty())
                break;
            nonce = (uint32_t)query[6] | (uint32_t)query[7] << 8 | (uint32_t)query[8] << 16 | (uint32_t)query[9] << 24;
            SendRawTo(s, from, FakeReply(nonce, 0));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(ListedCount(*guest, 0, &seen) == 1);
    CHECK(seen);
    enet_socket_destroy(s);
}

void QueryPadding()
{
    std::unique_ptr<net::Session> host = MakeSession(0, net::RelayMode::Auto);
    NetSessionDesc desc = {};
    desc.dwSize = sizeof(desc);
    desc.guidApplication = kApp;
    desc.dwMaxPlayers = 4;
    std::string name(300, 'n');
    desc.lpszSessionNameA = (char *)name.c_str();
    desc.lpszPasswordA = (char *)"pw";
    CHECK(host->Open(&desc, NET_OPEN_CREATE) == NET_OK);

    uint16_t rawPort = 0;
    ENetSocket s = OpenRaw(&rawPort);
    net::Writer w;
    w.Bytes(lobby::kMagic, 4);
    w.U8(1);
    w.U8(1);
    w.U32(0x12345679);
    w.Bytes(kApp.bytes, 16);
    CHECK(w.data.size() == 26);
    ENetAddress to;
    enet_address_set_host_ip(&to, "127.0.0.1");
    to.port = host->BoundPort();
    SendRawTo(s, to, w.data);
    ENetAddress from;
    std::vector<uint8_t> reply = ReceiveRaw(s, &from, IsReply);
    CHECK(reply.size() > 300);

    std::unique_ptr<net::Session> guest = MakeSession(0, net::RelayMode::Auto, rawPort);
    bool seen = false;
    ListedCount(*guest, 0, &seen);
    std::vector<uint8_t> query = ReceiveRaw(s, &from, IsQuery);
    CHECK(!query.empty());
    CHECK(query.size() >= reply.size());
    enet_socket_destroy(s);
}

std::atomic<bool> s_resolverBlocked{false};
std::atomic<bool> s_resolverRelease{false};
std::atomic<int> s_resolverCalls{0};

bool BlockingResolve(const char *, uint32_t *)
{
    s_resolverCalls++;
    s_resolverBlocked = true;
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!s_resolverRelease && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    s_resolverBlocked = false;
    return false;
}

bool LiteralResolve(const char *, uint32_t *)
{
    s_resolverCalls++;
    return false;
}

std::unique_ptr<net::Session> NamedLobbySession(const char *lobbyName, bool (*resolve)(const char *, uint32_t *))
{
    net::Config config;
    config.port = 0;
    config.broadcast = false;
    config.timeoutMs = 1500;
    config.joinTimeoutMs = 3000;
    config.lobby = lobbyName;
    config.resolveHost = resolve;
    config.log = Log;
    std::unique_ptr<net::Session> session(new net::Session(config));
    CHECK(session->Start() == NET_OK);
    return session;
}

void ServesWhileLobbyResolves(const char *lobbyName, bool (*resolve)(const char *, uint32_t *))
{
    std::unique_ptr<net::Session> host = NamedLobbySession(lobbyName, resolve);
    NetSessionDesc hosted = Host(*host);
    if (resolve != nullptr)
        CHECK(WaitFor([] { return s_resolverBlocked.load(); }));
    std::unique_ptr<net::Session> guest = MakeSession(0, net::RelayMode::Auto, host->BoundPort());
    CHECK(WaitFor([&] { return Listed(*guest, hosted.guidInstance); }));
    CHECK(Join(*guest, hosted.guidInstance) == NET_OK);
    NetName name = { sizeof(NetName), 0, (char *)"p", (char *)"p" };
    NetPlayerID hostPlayer = 0, guestPlayer = 0;
    CHECK(host->CreatePlayer(&hostPlayer, &name, nullptr, 0) == NET_OK);
    CHECK(guest->CreatePlayer(&guestPlayer, &name, nullptr, 0) == NET_OK);
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    int exchanges = 0;
    while (std::chrono::steady_clock::now() < end) {
        bool up = WaitFor([&] {
            guest->Send(guestPlayer, hostPlayer, TRUE, "hi", 2);
            return Receives(*host, guestPlayer);
        });
        bool down = WaitFor([&] {
            host->Send(hostPlayer, NET_ID_ALL, TRUE, "hi", 2);
            return Receives(*guest, hostPlayer);
        });
        CHECK(up && down);
        if (!up || !down)
            break;
        exchanges++;
    }
    CHECK(exchanges > 0);
    CHECK(host->IsHost());
    CHECK(!guest->IsHost());
    std::string code;
    NetCodeStatus status = host->HostCode(&code);
    CHECK(status == NET_CODE_CONTACTING || status == NET_CODE_LOBBY_UNREACHABLE);
    if (resolve != nullptr)
        CHECK(s_resolverBlocked);
    s_resolverRelease = true;
    CHECK(WaitFor([] { return !s_resolverBlocked; }));
}

void SlowLobbyResolution()
{
    s_resolverRelease = false;
    s_resolverCalls = 0;
    ServesWhileLobbyResolves("lobby.example:28630", BlockingResolve);
    CHECK(s_resolverCalls >= 1);
    ServesWhileLobbyResolves("nonexistent.invalid", nullptr);
}

void ReleaseResolverSoon(std::thread &release)
{
    release = std::thread([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        s_resolverRelease = true;
    });
}

void ShutdownWhileResolving()
{
    s_resolverRelease = false;
    s_resolverCalls = 0;
    std::unique_ptr<net::Session> host = NamedLobbySession("lobby.example", BlockingResolve);
    Host(*host);
    CHECK(WaitFor([] { return s_resolverBlocked.load(); }));
    std::thread release;
    ReleaseResolverSoon(release);
    host.reset();
    release.join();
    CHECK(!s_resolverBlocked);
    CHECK(s_resolverCalls == 1);
}

void AbandonWhileResolving()
{
    s_resolverRelease = false;
    s_resolverCalls = 0;
    std::unique_ptr<net::Session> host = NamedLobbySession("lobby.example", BlockingResolve);
    Host(*host);
    CHECK(WaitFor([] { return s_resolverBlocked.load(); }));
    std::thread release;
    ReleaseResolverSoon(release);
    host->Abandon();
    release.join();
    CHECK(!s_resolverBlocked);
    CHECK(s_resolverCalls == 1);
    CHECK(host->Start() == NET_OK);
    Host(*host);
    CHECK(WaitFor([] { return s_resolverCalls >= 2; }));
    std::string code;
    NetCodeStatus status = host->HostCode(&code);
    CHECK(status == NET_CODE_CONTACTING || status == NET_CODE_LOBBY_UNREACHABLE);
}

void LiteralLobbyAddress()
{
    FakeLobby fake;
    s_resolverCalls = 0;
    std::string lobbyName = "127.0.0.1:" + std::to_string(fake.Port());
    std::unique_ptr<net::Session> host = NamedLobbySession(lobbyName.c_str(), LiteralResolve);
    Host(*host);
    std::string code;
    CHECK(WaitFor([&] { return host->HostCode(&code) == NET_CODE_READY; }));
    CHECK(code == "7K3M-9QX2");
    CHECK(s_resolverCalls == 0);
}

void InvalidLobbyPort()
{
    FakeLobby fake;
    std::string port = std::to_string(fake.Port());
    const std::string settings[] = {
        "127.0.0.1:" + std::to_string(fake.Port() + 65536),
        "127.0.0.1:+" + port,
        "127.0.0.1: " + port,
        "127.0.0.1:" + port + " ",
        "127.0.0.1:" + port + "x",
        "127.0.0.1:-" + port,
        "127.0.0.1:",
        "127.0.0.1:0",
        ":" + port,
    };
    for (const std::string &setting : settings) {
        std::unique_ptr<net::Session> session = NamedLobbySession(setting.c_str(), nullptr);
        Host(*session);
        std::string code;
        CHECK(WaitFor([&] { return session->HostCode(&code) == NET_CODE_LOBBY_UNREACHABLE; }, 2000));
        session->Close();
        Listed(*session, NetGuid());
        CHECK(session->FindByCode("7K3M-9QX2") == NET_CODE_CONTACTING);
        CHECK(WaitFor([&] { return session->CodeSession(nullptr, nullptr) == NET_CODE_LOBBY_UNREACHABLE; }, 2000));
    }
    CHECK(fake.registers == 0);
    CHECK(fake.resolves == 0);
}

void UnresolvedLobbyFailsFast()
{
    s_resolverCalls = 0;
    std::unique_ptr<net::Session> guest = NamedLobbySession("lobby.example", LiteralResolve);
    Listed(*guest, NetGuid());
    auto start = std::chrono::steady_clock::now();
    CHECK(guest->FindByCode("7K3M-9QX2") == NET_CODE_CONTACTING);
    CHECK(WaitFor([&] { return guest->CodeSession(nullptr, nullptr) == NET_CODE_LOBBY_UNREACHABLE; }, 3000));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1500));
    int calls = s_resolverCalls;
    CHECK(calls >= 1);
    CHECK(guest->FindByCode("7K3M-9QX2") == NET_CODE_CONTACTING);
    CHECK(WaitFor([&] { return guest->CodeSession(nullptr, nullptr) == NET_CODE_LOBBY_UNREACHABLE; }, 3000));
    CHECK(s_resolverCalls > calls);

    std::unique_ptr<net::Session> host = NamedLobbySession("lobby.example", LiteralResolve);
    start = std::chrono::steady_clock::now();
    Host(*host);
    std::string code;
    CHECK(WaitFor([&] { return host->HostCode(&code) == NET_CODE_LOBBY_UNREACHABLE; }, 3000));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1500));
}

}

int main()
{
    TestRegister();
    TestLobbyUnreachable();
    TestIntroduce();
    TestCandidateFilter();
    TestPunchBounds();
    TestHostRelay();
    TestFindDirect();
    TestFindFailures();
    TestAutoRelay();
    TestRelayAlways();
    TestRelayNever();
    TestHostUnreachable();
    TestGarbage();
    TestPathPinning();
    TestElapsed();
    TestStaleNow();
    TestChallenge();
    TestRefreshMisses();
    TestLobbySwitch();
    TestNoLobby();

    if (enet_initialize() != 0) {
        fprintf(stderr, "net_rendezvous: enet_initialize failed\n");
        return 1;
    }
    SessionWithoutLobby();
    SessionUnknownCode();
    JoinByCode(net::RelayMode::Auto, false);
    JoinByCode(net::RelayMode::Always, false);
    JoinByCode(net::RelayMode::Auto, true);
    JoinByCode(net::RelayMode::Always, true);
    JoinPinned();
    FindWaitsForApplication();
    StaleClockBurst();
    FoundIsBounded();
    QueryPadding();
    AbandonIsSilent();
    MigrationRegisters();
    SlowLobbyResolution();
    ShutdownWhileResolving();
    AbandonWhileResolving();
    LiteralLobbyAddress();
    InvalidLobbyPort();
    UnresolvedLobbyFailsFast();

    if (s_failures != 0) {
        fprintf(stderr, "net_rendezvous: %d failures\n", s_failures);
        return 1;
    }
    printf("net_rendezvous: ok\n");
    return 0;
}
