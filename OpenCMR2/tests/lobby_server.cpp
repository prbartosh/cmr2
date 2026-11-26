#include "lobby/lobby_server.h"

#include "net/join_code.h"
#include "net/lobby_wire.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <random>
#include <thread>
#include <vector>

namespace {

namespace wire = net::lobby;

int s_failures;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
            s_failures++;                                                                \
        }                                                                                \
    } while (0)

void Log(const char *format, ...)
{
    if (getenv("LOBBY_TEST_VERBOSE") == nullptr)
        return;
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

const uint32_t kLoopback = htonl(INADDR_LOOPBACK);

void SleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool WaitFor(const std::function<bool()> &condition, int ms = 3000)
{
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (condition())
            return true;
        SleepMs(5);
    }
    return condition();
}

wire::Endpoint MakeEndpoint(const char *address, uint16_t port)
{
    wire::Endpoint e;
    in_addr in;
    inet_pton(AF_INET, address, &in);
    e.address = in.s_addr;
    e.port = port;
    return e;
}

struct Datagram {
    std::vector<uint8_t> data;
    uint16_t port = 0;
    uint32_t address = 0;
};

class Client {
public:
    explicit Client(uint32_t address = kLoopback)
    {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = address;
        if (fd < 0 || bind(fd, (const sockaddr *)&sa, sizeof(sa)) != 0) {
            if (fd >= 0)
                close(fd);
            fd = -1;
            return;
        }
        int buffer = 1 << 20;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
        socklen_t length = sizeof(sa);
        getsockname(fd, (sockaddr *)&sa, &length);
        port = ntohs(sa.sin_port);
        self.address = address;
        self.port = port;
    }
    ~Client()
    {
        if (fd >= 0)
            close(fd);
    }
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    bool Ok() const { return fd >= 0; }

    void SendRaw(uint16_t to, const void *data, size_t size)
    {
        sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = kLoopback;
        sa.sin_port = htons(to);
        sendto(fd, data, size, 0, (const sockaddr *)&sa, sizeof(sa));
    }

    void Send(uint16_t to, const std::vector<uint8_t> &data)
    {
        lastRequest = data.size();
        SendRaw(to, data.data(), data.size());
    }

    template <typename Message>
    void Send(uint16_t to, const Message &message)
    {
        Send(to, wire::Encode(message));
    }

    bool Receive(Datagram &out, int timeoutMs)
    {
        pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, timeoutMs) <= 0)
            return false;
        out.data.resize(wire::kMaxDatagram + 64);
        sockaddr_in sa;
        socklen_t length = sizeof(sa);
        ssize_t n = recvfrom(fd, out.data.data(), out.data.size(), 0, (sockaddr *)&sa, &length);
        if (n < 0)
            return false;
        out.data.resize((size_t)n);
        out.port = ntohs(sa.sin_port);
        out.address = sa.sin_addr.s_addr;
        return true;
    }

    template <typename Message>
    bool Await(uint16_t from, Message &message, int timeoutMs = 1000)
    {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (true) {
            int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
            if (left < 0)
                return false;
            Datagram d;
            if (!Receive(d, left))
                return false;
            CHECK(d.data.size() <= wire::kMinRequest);
            if (d.port != from)
                continue;
            Message decoded;
            if (wire::Decode(d.data.data(), d.data.size(), decoded)) {
                message = decoded;
                return true;
            }
        }
    }

    template <typename Message>
    bool AwaitReply(uint16_t from, Message &message, int timeoutMs = 1000)
    {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (true) {
            int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
            if (left < 0)
                return false;
            Datagram d;
            if (!Receive(d, left))
                return false;
            CHECK(d.data.size() <= wire::kMinRequest);
            if (d.port != from)
                continue;
            Message decoded;
            if (wire::Decode(d.data.data(), d.data.size(), decoded)) {
                CHECK(d.data.size() <= lastRequest);
                message = decoded;
                return true;
            }
            wire::Kind kind;
            if (wire::Peek(d.data.data(), d.data.size(), kind) && kind == wire::KIND_FAILURE)
                return false;
        }
    }

    bool AwaitFailure(uint16_t from, uint32_t nonce, wire::Failure reason, int timeoutMs = 1000)
    {
        wire::Failed failed;
        if (!AwaitReply(from, failed, timeoutMs))
            return false;
        return failed.nonce == nonce && failed.reason == reason;
    }

    bool Silent(int ms = 150)
    {
        Datagram d;
        return !Receive(d, ms);
    }

    void Drain()
    {
        Datagram d;
        while (Receive(d, 0)) {
        }
    }

    int fd = -1;
    uint16_t port = 0;
    size_t lastRequest = 0;
    wire::Endpoint self;
};

class Running {
public:
    explicit Running(const lobby::Options &options) : server(options)
    {
        started = server.Start();
        if (started)
            thread = std::thread([this] {
                while (!stop.load())
                    server.Poll(5);
            });
    }
    ~Running()
    {
        stop.store(true);
        if (thread.joinable())
            thread.join();
    }
    uint16_t Port() const { return server.Port(); }

    lobby::Server server;
    std::thread thread;
    std::atomic<bool> stop{ false };
    bool started = false;
};

lobby::Options TestOptions()
{
    lobby::Options options;
    options.bindAddress = kLoopback;
    options.port = 0;
    options.relayFirstPort = 0;
    options.relayPortCount = 16;
    options.log = Log;
    options.verbose = getenv("LOBBY_TEST_VERBOSE") != nullptr;
    return options;
}

wire::Guid MakeGuid(uint8_t seed)
{
    wire::Guid guid;
    for (size_t i = 0; i < guid.size(); i++)
        guid[i] = (uint8_t)(seed + i * 7);
    return guid;
}

bool KeyIsZero(const wire::Key &key)
{
    for (uint8_t b : key) {
        if (b != 0)
            return false;
    }
    return true;
}

struct Hosted {
    wire::Code code = {};
    wire::Key secret = {};
};

bool RegisterHost(Client &host, uint16_t lobby, uint32_t nonce, const wire::Guid &instance, const wire::Endpoint &local,
                  Hosted &out)
{
    wire::Register request;
    request.nonce = nonce;
    request.instance = instance;
    request.local = local;
    host.Send(lobby, request);
    wire::Registered reply;
    if (!host.AwaitReply(lobby, reply))
        return false;
    if (reply.nonce != nonce)
        return false;
    out.code = reply.code;
    out.secret = reply.secret;
    return true;
}

wire::Resolve MakeResolve(uint32_t nonce, const wire::Code &code, const wire::Endpoint &local)
{
    wire::Resolve request;
    request.nonce = nonce;
    request.code = code;
    request.local = local;
    return request;
}

void TestRegisterResolve()
{
    Running lobby(TestOptions());
    CHECK(lobby.started);
    if (!lobby.started)
        return;
    uint16_t port = lobby.Port();
    CHECK(port != 0);
    Client host, guest;

    wire::Guid instance = MakeGuid(1);
    wire::Endpoint hostLocal = MakeEndpoint("192.168.1.10", 28620);
    Hosted hosted;
    CHECK(RegisterHost(host, port, 11, instance, hostLocal, hosted));
    CHECK(net::joincode::IsCanonical(hosted.code));
    CHECK(!KeyIsZero(hosted.secret));
    CHECK(lobby.server.SessionCount() == 1);

    Hosted again;
    CHECK(RegisterHost(host, port, 11, instance, hostLocal, again));
    CHECK(again.code == hosted.code);
    CHECK(again.secret == hosted.secret);
    CHECK(lobby.server.SessionCount() == 1);

    wire::Endpoint guestLocal = MakeEndpoint("192.168.1.20", 28620);
    guest.Send(port, MakeResolve(21, hosted.code, guestLocal));
    wire::Resolved resolved;
    CHECK(guest.AwaitReply(port, resolved));
    CHECK(resolved.nonce == 21);
    CHECK(resolved.instance == instance);
    CHECK(resolved.hostPublic == host.self);
    CHECK(resolved.hostLocal == hostLocal);
    wire::Introduce introduce;
    CHECK(host.Await(port, introduce));
    CHECK(introduce.secret == hosted.secret);
    CHECK(introduce.guestPublic == guest.self);
    CHECK(introduce.guestLocal == guestLocal);

    Client publicHost;
    Hosted second;
    CHECK(RegisterHost(publicHost, port, 12, MakeGuid(2), MakeEndpoint("8.8.8.8", 28620), second));
    CHECK(second.code != hosted.code);
    guest.Send(port, MakeResolve(22, second.code, MakeEndpoint("172.16.5.5", 0)));
    CHECK(guest.AwaitReply(port, resolved));
    CHECK(resolved.hostPublic == publicHost.self);
    CHECK(!resolved.hostLocal.Valid());
    CHECK(publicHost.Await(port, introduce));
    CHECK(!introduce.guestLocal.Valid());

    guest.Send(port, MakeResolve(23, second.code, MakeEndpoint("172.32.0.1", 5000)));
    CHECK(guest.AwaitReply(port, resolved));
    CHECK(publicHost.Await(port, introduce));
    CHECK(!introduce.guestLocal.Valid());

    guest.Send(port, MakeResolve(24, second.code, MakeEndpoint("10.1.2.3", 1)));
    CHECK(guest.AwaitReply(port, resolved));
    CHECK(publicHost.Await(port, introduce));
    CHECK(introduce.guestLocal == MakeEndpoint("10.1.2.3", 1));

    wire::Code unknown = hosted.code;
    unknown[0] = unknown[0] == '0' ? '1' : '0';
    if (unknown == second.code)
        unknown[1] = unknown[1] == '0' ? '1' : '0';
    guest.Send(port, MakeResolve(25, unknown, wire::Endpoint()));
    CHECK(guest.AwaitFailure(port, 25, wire::FAILURE_NOT_FOUND));

    std::vector<uint8_t> shortRequest = wire::Encode(MakeResolve(26, hosted.code, wire::Endpoint()));
    shortRequest.resize(wire::kMinRequest - 1);
    guest.Send(port, shortRequest);
    CHECK(guest.Silent());
    CHECK(host.Silent(0));

    std::vector<uint8_t> versioned = wire::Encode(MakeResolve(27, hosted.code, wire::Endpoint()));
    versioned[5] = 2;
    guest.Send(port, versioned);
    CHECK(guest.AwaitFailure(port, 27, wire::FAILURE_VERSION));
    versioned.resize(wire::kMinRequest - 1);
    guest.Send(port, versioned);
    CHECK(guest.Silent());

    wire::Unregister unregisterVersion;
    unregisterVersion.code = hosted.code;
    unregisterVersion.secret = hosted.secret;
    std::vector<uint8_t> unregisterData = wire::Encode(unregisterVersion);
    unregisterData[5] = 0;
    guest.Send(port, unregisterData);
    CHECK(guest.AwaitFailure(port, 0, wire::FAILURE_VERSION));
    CHECK(lobby.server.SessionCount() == 2);

    std::vector<uint8_t> reply = wire::Encode(wire::Registered());
    reply.resize(wire::kMinRequest);
    guest.Send(port, reply);
    CHECK(guest.Silent());

    Client moved;
    wire::Refresh refresh;
    refresh.nonce = 31;
    refresh.code = hosted.code;
    refresh.secret = hosted.secret;
    refresh.local = MakeEndpoint("192.168.1.11", 28620);
    moved.Send(port, refresh);
    wire::Refreshed refreshed;
    CHECK(moved.AwaitReply(port, refreshed));
    CHECK(refreshed.nonce == 31);

    guest.Send(port, MakeResolve(28, hosted.code, guestLocal));
    CHECK(guest.AwaitReply(port, resolved));
    CHECK(resolved.hostPublic == moved.self);
    CHECK(resolved.hostLocal == MakeEndpoint("192.168.1.11", 28620));
    CHECK(moved.Await(port, introduce));
    CHECK(host.Silent(50));

    refresh.nonce = 32;
    refresh.secret[0] ^= 1;
    moved.Send(port, refresh);
    CHECK(moved.AwaitFailure(port, 32, wire::FAILURE_UNKNOWN_SESSION));
    refresh.nonce = 33;
    refresh.code = unknown;
    refresh.secret = hosted.secret;
    moved.Send(port, refresh);
    CHECK(moved.AwaitFailure(port, 33, wire::FAILURE_UNKNOWN_SESSION));

    Client remote(htonl(0x7f000002));
    if (remote.Ok()) {
        remote.Send(port, MakeResolve(41, hosted.code, MakeEndpoint("192.168.1.30", 28620)));
        CHECK(remote.AwaitReply(port, resolved));
        CHECK(resolved.hostPublic == moved.self);
        CHECK(!resolved.hostLocal.Valid());
        CHECK(moved.Await(port, introduce));
        CHECK(introduce.guestPublic == remote.self);
        CHECK(!introduce.guestLocal.Valid());
    } else {
        printf("lobby_server: 127.0.0.2 not available, different-address case skipped\n");
    }

    wire::Unregister unregister;
    unregister.code = hosted.code;
    unregister.secret = hosted.secret;
    unregister.secret[3] ^= 0x80;
    moved.Send(port, unregister);
    CHECK(moved.Silent());
    CHECK(lobby.server.SessionCount() == 2);
    unregister.secret = hosted.secret;
    moved.Send(port, unregister);
    CHECK(moved.Silent());
    CHECK(WaitFor([&] { return lobby.server.SessionCount() == 1; }));
    guest.Send(port, MakeResolve(29, hosted.code, wire::Endpoint()));
    CHECK(guest.AwaitFailure(port, 29, wire::FAILURE_NOT_FOUND));
}

void TestExpiry()
{
    lobby::Options options = TestOptions();
    options.sessionTtlMs = 300;
    Running lobby(options);
    CHECK(lobby.started);
    if (!lobby.started)
        return;
    uint16_t port = lobby.Port();
    Client host;
    Hosted hosted;
    CHECK(RegisterHost(host, port, 1, MakeGuid(3), wire::Endpoint(), hosted));
    SleepMs(180);
    wire::Refresh refresh;
    refresh.nonce = 2;
    refresh.code = hosted.code;
    refresh.secret = hosted.secret;
    host.Send(port, refresh);
    wire::Refreshed refreshed;
    CHECK(host.AwaitReply(port, refreshed));
    SleepMs(180);
    CHECK(lobby.server.SessionCount() == 1);
    CHECK(WaitFor([&] { return lobby.server.SessionCount() == 0; }, 1000));
    Client guest;
    guest.Send(port, MakeResolve(3, hosted.code, wire::Endpoint()));
    CHECK(guest.AwaitFailure(port, 3, wire::FAILURE_NOT_FOUND));
    refresh.nonce = 4;
    host.Send(port, refresh);
    CHECK(host.AwaitFailure(port, 4, wire::FAILURE_UNKNOWN_SESSION));
}

void TestLimits()
{
    {
        lobby::Options options = TestOptions();
        options.maxSessionsPerAddress = 2;
        Running lobby(options);
        CHECK(lobby.started);
        if (!lobby.started)
            return;
        uint16_t port = lobby.Port();
        Client host, guest;
        Hosted a, b;
        CHECK(RegisterHost(host, port, 1, MakeGuid(4), wire::Endpoint(), a));
        CHECK(RegisterHost(host, port, 2, MakeGuid(5), wire::Endpoint(), b));
        wire::Register third;
        third.nonce = 3;
        third.instance = MakeGuid(6);
        host.Send(port, third);
        CHECK(host.AwaitFailure(port, 3, wire::FAILURE_FULL));
        CHECK(lobby.server.SessionCount() == 2);

        for (uint32_t i = 0; i < 4; i++) {
            guest.Send(port, MakeResolve(100 + i, a.code, wire::Endpoint()));
            wire::Resolved resolved;
            CHECK(guest.AwaitReply(port, resolved));
            CHECK(resolved.nonce == 100 + i);
        }
        guest.Send(port, MakeResolve(104, a.code, wire::Endpoint()));
        CHECK(guest.AwaitFailure(port, 104, wire::FAILURE_RATE_LIMITED));
        int introductions = 0;
        wire::Introduce introduce;
        while (host.Await(port, introduce, 150))
            introductions++;
        CHECK(introductions == 4);

        wire::Code unknown = a.code;
        unknown[7] = unknown[7] == '0' ? '1' : '0';
        if (unknown == b.code)
            unknown[6] = unknown[6] == '0' ? '1' : '0';
        for (uint32_t i = 0; i < 5; i++) {
            guest.Send(port, MakeResolve(200 + i, unknown, wire::Endpoint()));
            CHECK(guest.AwaitFailure(port, 200 + i, wire::FAILURE_NOT_FOUND));
        }
        guest.Send(port, MakeResolve(205, b.code, wire::Endpoint()));
        CHECK(guest.AwaitFailure(port, 205, wire::FAILURE_RATE_LIMITED));
        wire::Relay relay;
        relay.nonce = 206;
        relay.code = b.code;
        guest.Send(port, relay);
        CHECK(guest.AwaitFailure(port, 206, wire::FAILURE_RATE_LIMITED));
        CHECK(lobby.server.RelayCount() == 0);
    }
    {
        lobby::Options options = TestOptions();
        options.maxSessions = 3;
        options.maxSessionsPerAddress = 10;
        Running lobby(options);
        CHECK(lobby.started);
        if (!lobby.started)
            return;
        uint16_t port = lobby.Port();
        Client host;
        Hosted sessions[3];
        for (uint32_t i = 0; i < 3; i++)
            CHECK(RegisterHost(host, port, 1 + i, MakeGuid((uint8_t)(10 + i)), wire::Endpoint(), sessions[i]));
        wire::Register request;
        request.nonce = 4;
        request.instance = MakeGuid(13);
        host.Send(port, request);
        CHECK(host.AwaitFailure(port, 4, wire::FAILURE_FULL));

        wire::Unregister unregister;
        unregister.code = sessions[0].code;
        unregister.secret = sessions[0].secret;
        host.Send(port, unregister);
        CHECK(WaitFor([&] { return lobby.server.SessionCount() == 2; }));
        Hosted fourth;
        CHECK(RegisterHost(host, port, 5, MakeGuid(14), wire::Endpoint(), fourth));
        unregister.code = fourth.code;
        unregister.secret = fourth.secret;
        host.Send(port, unregister);
        CHECK(WaitFor([&] { return lobby.server.SessionCount() == 2; }));
        request.nonce = 6;
        request.instance = MakeGuid(15);
        host.Send(port, request);
        CHECK(host.AwaitFailure(port, 6, wire::FAILURE_RATE_LIMITED));
        CHECK(lobby.server.SessionCount() == 2);
    }
}

struct RelayPair {
    uint16_t relayPort = 0;
    wire::Key hostToken = {};
    wire::Key guestToken = {};
};

bool OpenRelay(Client &host, Client &guest, uint16_t port, const Hosted &hosted, uint32_t nonce, RelayPair &out)
{
    wire::Relay request;
    request.nonce = nonce;
    request.code = hosted.code;
    guest.Send(port, request);
    wire::Relayed relayed;
    if (!guest.AwaitReply(port, relayed) || relayed.nonce != nonce)
        return false;
    wire::RelayOffer offer;
    if (!host.Await(port, offer))
        return false;
    CHECK(offer.secret == hosted.secret);
    CHECK(offer.port == relayed.port);
    CHECK(offer.token != relayed.token);
    out.relayPort = relayed.port;
    out.hostToken = offer.token;
    out.guestToken = relayed.token;
    return relayed.port != 0 && relayed.port != port;
}

bool BindRelay(Client &client, uint16_t relayPort, const wire::Key &token)
{
    wire::Bind bind;
    bind.token = token;
    client.Send(relayPort, bind);
    wire::Bound bound;
    return client.AwaitReply(relayPort, bound) && bound.token == token;
}

bool Forwarded(Client &to, uint16_t relayPort, const std::vector<uint8_t> &expected)
{
    Datagram d;
    if (!to.Receive(d, 1000))
        return false;
    return d.port == relayPort && d.data == expected;
}

std::vector<uint8_t> Payload(size_t size, uint8_t seed)
{
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; i++)
        data[i] = (uint8_t)(seed + i * 13);
    return data;
}

void TestRelay()
{
    lobby::Options options = TestOptions();
    options.relayPortCount = 2;
    Running lobby(options);
    CHECK(lobby.started);
    if (!lobby.started)
        return;
    uint16_t port = lobby.Port();
    Client host, guest, stranger;
    Hosted hosted;
    CHECK(RegisterHost(host, port, 1, MakeGuid(20), wire::Endpoint(), hosted));
    RelayPair relay;
    CHECK(OpenRelay(host, guest, port, hosted, 2, relay));
    CHECK(lobby.server.RelayCount() == 1);
    uint16_t rp = relay.relayPort;

    std::vector<uint8_t> early = Payload(32, 1);
    host.Send(rp, early);
    CHECK(guest.Silent(100));

    wire::Key wrong = relay.hostToken;
    wrong[5] ^= 0x10;
    wire::Bind bind;
    bind.token = wrong;
    stranger.Send(rp, bind);
    CHECK(stranger.Silent(100));

    CHECK(BindRelay(host, rp, relay.hostToken));
    host.Send(rp, early);
    CHECK(guest.Silent(100));
    CHECK(BindRelay(guest, rp, relay.guestToken));
    CHECK(host.Silent(50));

    std::vector<uint8_t> hello = Payload(48, 2);
    host.Send(rp, hello);
    CHECK(Forwarded(guest, rp, hello));
    std::vector<uint8_t> large = Payload(4000, 3);
    guest.Send(rp, large);
    CHECK(Forwarded(host, rp, large));
    std::vector<uint8_t> search = Payload(40, 4);
    memcpy(search.data(), wire::kMagic, sizeof(wire::kMagic));
    search[4] = 1;
    search[5] = 1;
    guest.Send(rp, search);
    CHECK(Forwarded(host, rp, search));
    std::vector<uint8_t> maximum = Payload(wire::kMaxDatagram, 5);
    host.Send(rp, maximum);
    CHECK(Forwarded(guest, rp, maximum));
    std::vector<uint8_t> oversized = Payload(wire::kMaxDatagram + 1, 6);
    host.Send(rp, oversized);
    CHECK(guest.Silent(100));
    std::vector<uint8_t> tiny = Payload(1, 7);
    host.Send(rp, tiny);
    CHECK(Forwarded(guest, rp, tiny));

    stranger.Send(rp, hello);
    CHECK(host.Silent(100));
    CHECK(guest.Silent(0));

    Client rebound;
    CHECK(BindRelay(rebound, rp, relay.guestToken));
    CHECK(host.Silent(50));
    host.Send(rp, hello);
    CHECK(Forwarded(rebound, rp, hello));
    CHECK(guest.Silent(50));
    guest.Send(rp, hello);
    CHECK(host.Silent(100));
    rebound.Send(rp, large);
    CHECK(Forwarded(host, rp, large));

    Client otherHost, otherGuest;
    Hosted other;
    CHECK(RegisterHost(otherHost, port, 3, MakeGuid(21), wire::Endpoint(), other));
    RelayPair second;
    CHECK(OpenRelay(otherHost, otherGuest, port, other, 4, second));
    CHECK(second.relayPort != rp);
    CHECK(lobby.server.RelayCount() == 2);
    wire::Relay request;
    request.nonce = 5;
    request.code = hosted.code;
    guest.Send(port, request);
    CHECK(guest.AwaitFailure(port, 5, wire::FAILURE_FULL));

    wire::Unregister unregister;
    unregister.code = hosted.code;
    unregister.secret = hosted.secret;
    host.Send(port, unregister);
    CHECK(WaitFor([&] { return lobby.server.RelayCount() == 1; }));
    host.Send(rp, hello);
    CHECK(rebound.Silent(100));
    CHECK(lobby.server.SessionCount() == 1);
}

void TestRelayLimits()
{
    {
        lobby::Options options = TestOptions();
        options.relayPortCount = 4;
        options.maxRelaysPerSession = 1;
        Running lobby(options);
        CHECK(lobby.started);
        if (!lobby.started)
            return;
        uint16_t port = lobby.Port();
        Client host, guest;
        Hosted hosted;
        CHECK(RegisterHost(host, port, 1, MakeGuid(30), wire::Endpoint(), hosted));
        RelayPair relay;
        CHECK(OpenRelay(host, guest, port, hosted, 2, relay));
        wire::Relay request;
        request.nonce = 3;
        request.code = hosted.code;
        guest.Send(port, request);
        CHECK(guest.AwaitFailure(port, 3, wire::FAILURE_FULL));
        wire::Code unknown = hosted.code;
        unknown[2] = unknown[2] == '0' ? '1' : '0';
        request.nonce = 4;
        request.code = unknown;
        guest.Send(port, request);
        CHECK(guest.AwaitFailure(port, 4, wire::FAILURE_NOT_FOUND));
        CHECK(lobby.server.RelayCount() == 1);
    }
    {
        lobby::Options options = TestOptions();
        options.relayPortCount = 1;
        options.relayIdleMs = 300;
        options.relayBytesPerSecond = 4000;
        Running lobby(options);
        CHECK(lobby.started);
        if (!lobby.started)
            return;
        uint16_t port = lobby.Port();
        Client host, guest;
        Hosted hosted;
        CHECK(RegisterHost(host, port, 1, MakeGuid(31), wire::Endpoint(), hosted));
        RelayPair relay;
        CHECK(OpenRelay(host, guest, port, hosted, 2, relay));
        CHECK(BindRelay(host, relay.relayPort, relay.hostToken));
        CHECK(BindRelay(guest, relay.relayPort, relay.guestToken));
        std::vector<uint8_t> chunk = Payload(3000, 9);
        for (int i = 0; i < 6; i++)
            host.Send(relay.relayPort, chunk);
        int received = 0;
        Datagram d;
        while (guest.Receive(d, 150))
            received++;
        CHECK(received >= 1 && received <= 3);

        wire::Relay request;
        request.nonce = 3;
        request.code = hosted.code;
        guest.Send(port, request);
        CHECK(guest.AwaitFailure(port, 3, wire::FAILURE_FULL));
        CHECK(WaitFor([&] { return lobby.server.RelayCount() == 0; }, 2000));
        RelayPair again;
        CHECK(OpenRelay(host, guest, port, hosted, 4, again));
        CHECK(lobby.server.RelayCount() == 1);
    }
    {
        lobby::Options options = TestOptions();
        options.relayFirstPort = 1;
        options.relayPortCount = 0;
        Running lobby(options);
        CHECK(!lobby.started);
    }
}

void TestFuzz()
{
    lobby::Options options = TestOptions();
    Running lobby(options);
    CHECK(lobby.started);
    if (!lobby.started)
        return;
    uint16_t port = lobby.Port();
    Client host, guest, fuzz;
    Hosted hosted;
    CHECK(RegisterHost(host, port, 1, MakeGuid(40), wire::Endpoint(), hosted));
    RelayPair relay;
    CHECK(OpenRelay(host, guest, port, hosted, 2, relay));
    CHECK(BindRelay(host, relay.relayPort, relay.hostToken));
    CHECK(BindRelay(guest, relay.relayPort, relay.guestToken));

    std::mt19937 rng(12345);
    auto byte = [&rng] { return (uint8_t)(rng() & 0xff); };
    size_t largestReply = 0;
    size_t replies = 0;
    auto drain = [&] {
        Datagram d;
        while (fuzz.Receive(d, 0)) {
            replies++;
            largestReply = std::max(largestReply, d.data.size());
        }
    };
    auto make = [&](size_t size) {
        std::vector<uint8_t> data(size);
        for (uint8_t &b : data)
            b = byte();
        if (size >= wire::kHeaderSize && (rng() % 4) != 0) {
            memcpy(data.data(), wire::kMagic, sizeof(wire::kMagic));
            data[4] = (uint8_t)(wire::KIND_REGISTER + rng() % 15);
            uint32_t version = rng() % 4;
            data[5] = version < 2 ? 1 : (uint8_t)version;
            if (size >= 18 && (rng() % 3) == 0)
                memcpy(data.data() + 10, hosted.code.data(), hosted.code.size());
        }
        return data;
    };

    for (int i = 0; i < 10000; i++) {
        std::vector<uint8_t> data = make(rng() % wire::kMinRequest);
        fuzz.SendRaw((rng() % 8) == 0 ? relay.relayPort : port, data.data(), data.size());
        if ((i & 127) == 127) {
            SleepMs(1);
            drain();
        }
    }
    SleepMs(200);
    drain();
    CHECK(replies == 0);

    for (int i = 0; i < 10000; i++) {
        size_t size = (rng() % 4) == 0 ? wire::kMinRequest + rng() % 64 : rng() % (wire::kMaxDatagram + 128);
        std::vector<uint8_t> data = make(size);
        fuzz.SendRaw((rng() % 8) == 0 ? relay.relayPort : port, data.data(), data.size());
        if ((i & 127) == 127) {
            SleepMs(1);
            drain();
        }
    }
    SleepMs(200);
    drain();
    CHECK(largestReply <= wire::kMinRequest);
    Log("fuzz: %zu replies, largest %zu bytes", replies, largestReply);

    host.Drain();
    guest.Drain();
    wire::Refresh refresh;
    refresh.nonce = 3;
    refresh.code = hosted.code;
    refresh.secret = hosted.secret;
    host.Send(port, refresh);
    wire::Refreshed refreshed;
    CHECK(host.AwaitReply(port, refreshed));
    CHECK(refreshed.nonce == 3);
    host.Drain();
    std::vector<uint8_t> hello = Payload(100, 8);
    host.Send(relay.relayPort, hello);
    CHECK(Forwarded(guest, relay.relayPort, hello));
    guest.Send(relay.relayPort, hello);
    CHECK(Forwarded(host, relay.relayPort, hello));

    Client late;
    SleepMs(3100);
    late.Send(port, MakeResolve(4, hosted.code, wire::Endpoint()));
    wire::Resolved resolved;
    CHECK(late.AwaitReply(port, resolved));
    CHECK(resolved.hostPublic == host.self);
    CHECK(lobby.server.SessionCount() >= 1);
}

}

int main()
{
    auto start = std::chrono::steady_clock::now();
    TestRegisterResolve();
    TestExpiry();
    TestLimits();
    TestRelay();
    TestRelayLimits();
    TestFuzz();
    long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (s_failures != 0) {
        fprintf(stderr, "lobby_server: %d failures\n", s_failures);
        return 1;
    }
    printf("lobby_server: all passed in %ld ms\n", ms);
    return 0;
}
