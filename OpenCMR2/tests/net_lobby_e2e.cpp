#include "lobby/lobby_server.h"
#include "net/join_code.h"
#include "net/net_session.h"

#include "net/enet_platform.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>

namespace {

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

using Clock = std::chrono::steady_clock;

bool WaitFor(const std::function<bool()> &condition, int ms = 5000)
{
    auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end) {
        if (condition())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return condition();
}

int ElapsedMs(Clock::time_point since)
{
    return (int)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count();
}

class Lobby {
public:
    explicit Lobby(bool polled = true)
    {
        ::lobby::Options options;
        options.bindAddress = htonl(INADDR_LOOPBACK);
        options.port = 0;
        options.relayFirstPort = 0;
        options.log = Log;
        server.reset(new ::lobby::Server(options));
        CHECK(server->Start());
        if (polled) {
            running = true;
            thread = std::thread([this] {
                while (running)
                    server->Poll(5);
            });
        }
    }

    ~Lobby()
    {
        running = false;
        if (thread.joinable())
            thread.join();
    }

    std::string Address() const { return "127.0.0.1:" + std::to_string(server->Port()); }
    size_t Sessions() const { return server->SessionCount(); }
    size_t Relays() const { return server->RelayCount(); }

private:
    std::unique_ptr<::lobby::Server> server;
    std::atomic<bool> running{false};
    std::thread thread;
};

const NetGuid kApp = { { 0x43, 0x4d, 0x52, 0x32, 0xe2, 0xe0, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2 } };

std::unique_ptr<net::Session> MakeSession(const Lobby &lobby, net::RelayMode mode)
{
    net::Config config;
    config.port = 0;
    config.broadcast = false;
    config.timeoutMs = 3000;
    config.joinTimeoutMs = 3000;
    config.lobby = lobby.Address();
    config.relay = mode;
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

NetSessionDesc Host(net::Session &session)
{
    NetSessionDesc desc = {};
    desc.dwSize = sizeof(desc);
    desc.guidApplication = kApp;
    desc.dwMaxPlayers = 4;
    desc.lpszSessionNameA = (char *)"Lobby e2e";
    desc.lpszPasswordA = (char *)"";
    CHECK(session.Open(&desc, NET_OPEN_CREATE) == NET_OK);
    NetSessionDesc hosted = {};
    CHECK(session.GetSessionDesc(&hosted) == NET_OK);
    return hosted;
}

std::string HostCode(net::Session &session)
{
    std::string code;
    CHECK(WaitFor([&] { return session.HostCode(&code) == NET_CODE_READY; }));
    return code;
}

bool SameGuid(const NetGuid &a, const NetGuid &b)
{
    return memcmp(&a, &b, sizeof(a)) == 0;
}

bool IsCanonicalText(const std::string &code)
{
    net::lobby::Code parsed;
    return code.size() == net::joincode::kTextSize - 1 && net::joincode::Parse(code.c_str(), parsed) &&
           net::joincode::IsCanonical(parsed) && net::joincode::Format(parsed) == code;
}

NetCodeStatus Settle(net::Session &session, const NetGuid &instance)
{
    NetCodeStatus status = NET_CODE_IDLE;
    WaitFor([&] {
        Listed(session, instance);
        status = session.CodeSession(nullptr, nullptr);
        return status != NET_CODE_CONTACTING && status != NET_CODE_CONNECTING && status != NET_CODE_RELAYING;
    });
    return status;
}

bool Find(net::Session &guest, const std::string &text, const NetGuid &instance, bool *relayed)
{
    if (guest.FindByCode(text.c_str()) != NET_CODE_CONTACTING)
        return false;
    NetGuid found = {};
    bool viaRelay = false;
    bool ok = WaitFor([&] {
        Listed(guest, instance);
        return guest.CodeSession(&found, &viaRelay) == NET_CODE_FOUND;
    });
    if (relayed != nullptr)
        *relayed = viaRelay;
    return ok && SameGuid(found, instance) && WaitFor([&] { return Listed(guest, instance); });
}

HRESULT Join(net::Session &session, const NetGuid &instance)
{
    NetSessionDesc join = {};
    join.dwSize = sizeof(join);
    join.guidInstance = instance;
    join.lpszPasswordA = (char *)"";
    return session.Open(&join, NET_OPEN_JOIN);
}

NetPlayerID Player(net::Session &session, const char *name)
{
    NetName n = { sizeof(NetName), 0, (char *)name, (char *)name };
    NetPlayerID id = 0;
    CHECK(session.CreatePlayer(&id, &n, nullptr, 0) == NET_OK);
    return id;
}

bool Receives(net::Session &session, NetPlayerID from, const char *payload)
{
    size_t length = strlen(payload);
    for (;;) {
        NetPlayerID sender = 0, to = 0;
        uint8_t data[256];
        DWORD size = sizeof(data);
        if (session.Receive(&sender, &to, data, &size) != NET_OK)
            return false;
        if (sender == from && size == length && memcmp(data, payload, length) == 0)
            return true;
    }
}

bool Delivers(net::Session &from, NetPlayerID fromPlayer, NetPlayerID toPlayer, net::Session &to, BOOL guaranteed,
              const char *payload)
{
    return WaitFor([&] {
        from.Send(fromPlayer, toPlayer, guaranteed, payload, (DWORD)strlen(payload));
        return Receives(to, fromPlayer, payload);
    });
}

bool Talks(net::Session &a, NetPlayerID aPlayer, net::Session &b, NetPlayerID bPlayer)
{
    bool ok = Delivers(a, aPlayer, bPlayer, b, TRUE, "guaranteed a");
    ok = Delivers(a, aPlayer, bPlayer, b, FALSE, "unguaranteed a") && ok;
    ok = Delivers(b, bPlayer, aPlayer, a, TRUE, "guaranteed b") && ok;
    ok = Delivers(b, bPlayer, NET_ID_ALL, a, FALSE, "unguaranteed b") && ok;
    return ok;
}

void JoinByCode(net::RelayMode guestMode)
{
    bool relay = guestMode == net::RelayMode::Always;
    Lobby lobby;
    std::unique_ptr<net::Session> host = MakeSession(lobby, net::RelayMode::Auto);
    std::unique_ptr<net::Session> guest = MakeSession(lobby, guestMode);
    NetSessionDesc hosted = Host(*host);
    std::string code = HostCode(*host);
    CHECK(IsCanonicalText(code));
    CHECK(lobby.Sessions() == 1);

    CHECK(guest->FindByCode(code.c_str()) == NET_CODE_CONTACTING);
    NetCodeStatus pending = relay ? NET_CODE_RELAYING : NET_CODE_CONNECTING;
    CHECK(WaitFor([&] { return guest->CodeSession(nullptr, nullptr) == pending; }));
    NetGuid instance = {};
    bool relayed = !relay;
    CHECK(WaitFor([&] {
        Listed(*guest, hosted.guidInstance);
        return guest->CodeSession(&instance, &relayed) == NET_CODE_FOUND;
    }));
    CHECK(SameGuid(instance, hosted.guidInstance));
    CHECK(relayed == relay);
    CHECK(lobby.Relays() == (relay ? 1u : 0u));
    CHECK(WaitFor([&] { return Listed(*guest, hosted.guidInstance); }));

    CHECK(Join(*guest, instance) == NET_OK);
    NetPlayerID hostPlayer = Player(*host, "host");
    NetPlayerID guestPlayer = Player(*guest, "guest");
    CHECK(Talks(*host, hostPlayer, *guest, guestPlayer));
    CHECK(lobby.Relays() == (relay ? 1u : 0u));
    CHECK(!guest->IsHost());
}

void TwoGuests()
{
    Lobby lobby;
    std::unique_ptr<net::Session> host = MakeSession(lobby, net::RelayMode::Auto);
    std::unique_ptr<net::Session> direct = MakeSession(lobby, net::RelayMode::Auto);
    std::unique_ptr<net::Session> relayedGuest = MakeSession(lobby, net::RelayMode::Always);
    NetSessionDesc hosted = Host(*host);
    std::string code = HostCode(*host);
    NetPlayerID hostPlayer = Player(*host, "host");

    bool relayed = true;
    CHECK(Find(*direct, code, hosted.guidInstance, &relayed));
    CHECK(!relayed);
    CHECK(Join(*direct, hosted.guidInstance) == NET_OK);
    NetPlayerID directPlayer = Player(*direct, "direct");

    relayed = false;
    CHECK(Find(*relayedGuest, code, hosted.guidInstance, &relayed));
    CHECK(relayed);
    CHECK(lobby.Relays() == 1);
    CHECK(Join(*relayedGuest, hosted.guidInstance) == NET_OK);
    NetPlayerID relayedPlayer = Player(*relayedGuest, "relayed");

    CHECK(Talks(*direct, directPlayer, *relayedGuest, relayedPlayer));
    CHECK(Delivers(*host, hostPlayer, NET_ID_ALL, *direct, TRUE, "to all"));
    CHECK(Delivers(*host, hostPlayer, NET_ID_ALL, *relayedGuest, TRUE, "to all"));
    CHECK(host->IsHost());
    CHECK(!direct->IsHost());
    CHECK(!relayedGuest->IsHost());
}

std::string Substitute(const std::string &code)
{
    std::string text = code;
    for (char &c : text) {
        if (c == '0')
            c = 'O';
        else if (c == '1')
            c = 'l';
    }
    return text;
}

void CodeVariants()
{
    std::unique_ptr<Lobby> lobby;
    std::unique_ptr<net::Session> host;
    NetSessionDesc hosted = {};
    std::string code;
    for (int attempt = 0; attempt < 64; attempt++) {
        host.reset();
        lobby.reset(new Lobby());
        host = MakeSession(*lobby, net::RelayMode::Auto);
        hosted = Host(*host);
        code = HostCode(*host);
        if (code.find_first_of("01") != std::string::npos)
            break;
    }
    CHECK(code.find_first_of("01") != std::string::npos);
    std::unique_ptr<net::Session> guest = MakeSession(*lobby, net::RelayMode::Never);

    std::string lower = code;
    for (char &c : lower)
        c = (char)tolower((unsigned char)c);
    std::string dashed = code.substr(0, 2) + "-" + code.substr(2, 2) + " " + code.substr(5, 2) + "-" + code.substr(7, 2);
    std::string substituted = Substitute(code);
    CHECK(substituted != code);
    CHECK(Find(*guest, lower, hosted.guidInstance, nullptr));
    CHECK(Find(*guest, dashed, hosted.guidInstance, nullptr));
    CHECK(Find(*guest, substituted, hosted.guidInstance, nullptr));

    std::string wrong = code;
    const char *alphabet = net::joincode::kAlphabet;
    wrong[0] = alphabet[(strchr(alphabet, code[0]) - alphabet + 1) % 32];
    CHECK(guest->FindByCode(wrong.c_str()) == NET_CODE_CONTACTING);
    CHECK(Settle(*guest, hosted.guidInstance) == NET_CODE_NOT_FOUND);

    CHECK(guest->FindByCode("ABC") == NET_CODE_INVALID);
    CHECK(guest->FindByCode("ABCD-EFGU") == NET_CODE_INVALID);
    CHECK(guest->FindByCode((code + "Z").c_str()) == NET_CODE_INVALID);
    CHECK(guest->FindByCode("") == NET_CODE_INVALID);
    CHECK(guest->FindByCode(nullptr) == NET_CODE_INVALID);
    CHECK(guest->CodeSession(nullptr, nullptr) == NET_CODE_NOT_FOUND);
}

void Unregister()
{
    Lobby lobby;
    std::unique_ptr<net::Session> host = MakeSession(lobby, net::RelayMode::Auto);
    std::unique_ptr<net::Session> guest = MakeSession(lobby, net::RelayMode::Auto);
    NetSessionDesc hosted = Host(*host);
    std::string code = HostCode(*host);
    CHECK(lobby.Sessions() == 1);
    CHECK(host->Close() == NET_OK);
    CHECK(WaitFor([&] { return lobby.Sessions() == 0; }));
    std::string none;
    CHECK(host->HostCode(&none) == NET_CODE_IDLE);
    CHECK(none.empty());
    CHECK(guest->FindByCode(code.c_str()) == NET_CODE_CONTACTING);
    CHECK(Settle(*guest, hosted.guidInstance) == NET_CODE_NOT_FOUND);
}

void LobbyDown()
{
    Lobby lobby(false);
    std::unique_ptr<net::Session> host = MakeSession(lobby, net::RelayMode::Auto);
    std::unique_ptr<net::Session> guest = MakeSession(lobby, net::RelayMode::Auto);
    auto start = Clock::now();
    Host(*host);
    CHECK(guest->FindByCode("7K3M-9QX2") == NET_CODE_CONTACTING);
    std::string code;
    int hostMs = -1, guestMs = -1;
    WaitFor(
        [&] {
            if (hostMs < 0 && host->HostCode(&code) == NET_CODE_LOBBY_UNREACHABLE)
                hostMs = ElapsedMs(start);
            if (guestMs < 0 && guest->CodeSession(nullptr, nullptr) == NET_CODE_LOBBY_UNREACHABLE)
                guestMs = ElapsedMs(start);
            return hostMs >= 0 && guestMs >= 0;
        },
        8000);
    Log("lobby down: host %d ms, guest %d ms", hostMs, guestMs);
    CHECK(hostMs >= 0 && hostMs <= 6500);
    CHECK(guestMs >= 0 && guestMs <= 6500);
    CHECK(code.empty());
    CHECK(lobby.Sessions() == 0);
}

void RateLimited()
{
    Lobby lobby;
    std::unique_ptr<net::Session> guest = MakeSession(lobby, net::RelayMode::Auto);
    NetGuid none = {};
    std::set<NetCodeStatus> seen;
    int notFound = 0;
    for (int i = 0; i < 16 && seen.count(NET_CODE_RATE_LIMITED) == 0; i++) {
        std::string code = net::joincode::Format(net::joincode::FromBits(0x5A5A5A5A5Aull + (uint64_t)i * 7919));
        CHECK(guest->FindByCode(code.c_str()) == NET_CODE_CONTACTING);
        NetCodeStatus status = Settle(*guest, none);
        seen.insert(status);
        if (status == NET_CODE_NOT_FOUND)
            notFound++;
    }
    CHECK(seen.count(NET_CODE_RATE_LIMITED) == 1);
    CHECK(notFound >= 10);
    CHECK(seen.count(NET_CODE_LOBBY_UNREACHABLE) == 0);
}

}

int main()
{
    if (enet_initialize() != 0) {
        fprintf(stderr, "net_lobby_e2e: enet_initialize failed\n");
        return 1;
    }
    struct Scenario {
        const char *name;
        std::function<void()> run;
    } scenarios[] = {
        { "direct", [] { JoinByCode(net::RelayMode::Auto); } },
        { "relay", [] { JoinByCode(net::RelayMode::Always); } },
        { "two guests", TwoGuests },
        { "code variants", CodeVariants },
        { "unregister", Unregister },
        { "lobby down", LobbyDown },
        { "rate limited", RateLimited },
    };
    for (const Scenario &scenario : scenarios) {
        int before = s_failures;
        auto start = Clock::now();
        scenario.run();
        printf("net_lobby_e2e: %s %s (%d ms)\n", scenario.name, s_failures == before ? "ok" : "FAILED",
               ElapsedMs(start));
    }
    if (s_failures != 0) {
        fprintf(stderr, "net_lobby_e2e: %d failures\n", s_failures);
        return 1;
    }
    printf("net_lobby_e2e: ok\n");
    return 0;
}
