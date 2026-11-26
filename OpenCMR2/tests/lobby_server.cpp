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

constexpr size_t kCookieOffset = wire::kHeaderSize + 4;

bool HasCookie(uint8_t kind)
{
    return kind == wire::KIND_REGISTER || kind == wire::KIND_REFRESH || kind == wire::KIND_RESOLVE ||
           kind == wire::KIND_RELAY;
}

uint32_t NonceOf(const std::vector<uint8_t> &data)
{
    if (data.size() < wire::kHeaderSize + 4)
        return 0;
    net::Reader reader(data.data() + wire::kHeaderSize, data.size() - wire::kHeaderSize);
    return reader.U32();
}

class Client {
public:
    explicit Client(uint32_t address = kLoopback, uint16_t bindPort = 0)
    {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = address;
        sa.sin_port = htons(bindPort);
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
        lastData = data;
        lastTo = to;
        if (useCookie && lastData.size() >= kCookieOffset + cookie.size() &&
            memcmp(lastData.data(), wire::kMagic, sizeof(wire::kMagic)) == 0 && HasCookie(lastData[4]))
            memcpy(lastData.data() + kCookieOffset, cookie.data(), cookie.size());
        lastRequest = lastData.size();
        SendRaw(to, lastData.data(), lastData.size());
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
        int rounds = 0;
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
            CHECK(d.data.size() <= lastRequest);
            Message decoded;
            if (wire::Decode(d.data.data(), d.data.size(), decoded)) {
                message = decoded;
                return true;
            }
            wire::Challenge challenge;
            if (wire::Decode(d.data.data(), d.data.size(), challenge)) {
                CHECK(d.data.size() == 26);
                if (!answerChallenges || rounds >= 3 || challenge.nonce != NonceOf(lastData))
                    return false;
                rounds++;
                challenges++;
                cookie = challenge.cookie;
                Send(lastTo, lastData);
                continue;
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

    void Manual()
    {
        useCookie = false;
        answerChallenges = false;
    }

    void Automatic()
    {
        useCookie = true;
        answerChallenges = true;
    }

    int fd = -1;
    uint16_t port = 0;
    size_t lastRequest = 0;
    std::vector<uint8_t> lastData;
    uint16_t lastTo = 0;
    wire::Key cookie = {};
    bool useCookie = true;
    bool answerChallenges = true;
    int challenges = 0;
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
    refresh.nonce = 30;
    refresh.code = hosted.code;
    refresh.secret = hosted.secret;
    refresh.local = MakeEndpoint("192.168.1.11", 28620);
    moved.Manual();
    moved.Send(port, refresh);
    wire::Challenge challenge;
    CHECK(moved.AwaitReply(port, challenge));
    CHECK(challenge.nonce == 30);
    wire::Refresh stay = refresh;
    stay.nonce = 34;
    stay.local = hostLocal;
    host.Manual();
    host.Send(port, stay);
    wire::Refreshed refreshed;
    CHECK(host.AwaitReply(port, refreshed));
    CHECK(refreshed.nonce == 34);
    host.Automatic();

    moved.Automatic();
    refresh.nonce = 31;
    moved.Send(port, refresh);
    CHECK(moved.AwaitReply(port, refreshed));
    CHECK(refreshed.nonce == 31);
    CHECK(moved.challenges == 1);

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
    options.sessionTtlMs = 600;
    Running lobby(options);
    CHECK(lobby.started);
    if (!lobby.started)
        return;
    uint16_t port = lobby.Port();
    Client host;
    Hosted hosted;
    CHECK(RegisterHost(host, port, 1, MakeGuid(3), wire::Endpoint(), hosted));
    SleepMs(300);
    wire::Refresh refresh;
    refresh.nonce = 2;
    refresh.code = hosted.code;
    refresh.secret = hosted.secret;
    host.Send(port, refresh);
    wire::Refreshed refreshed;
    CHECK(host.AwaitReply(port, refreshed));
    SleepMs(300);
    CHECK(lobby.server.SessionCount() == 1);
    SleepMs(350);
    Client guest;
    guest.Send(port, MakeResolve(3, hosted.code, wire::Endpoint()));
    CHECK(guest.AwaitFailure(port, 3, wire::FAILURE_NOT_FOUND));
    CHECK(WaitFor([&] { return lobby.server.SessionCount() == 0; }, 2500));
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

        for (uint32_t i = 0; i < 5; i++) {
            guest.Send(port, MakeResolve(100 + i, a.code, wire::Endpoint()));
            wire::Resolved resolved;
            CHECK(guest.AwaitReply(port, resolved));
            CHECK(resolved.nonce == 100 + i);
        }
        int introductions = 0;
        wire::Introduce introduce;
        while (host.Await(port, introduce, 150))
            introductions++;
        CHECK(introductions == 5);

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
        wire::Relay request;
        request.nonce = 3;
        request.code = hosted.code;
        guest.Send(port, request);
        CHECK(guest.AwaitFailure(port, 3, wire::FAILURE_FULL));

        std::vector<uint8_t> chunk = Payload(3000, 9);
        for (int i = 0; i < 6; i++)
            host.Send(relay.relayPort, chunk);
        int received = 0;
        Datagram d;
        while (guest.Receive(d, 150))
            received++;
        CHECK(received >= 1 && received <= 3);
        CHECK(WaitFor([&] { return lobby.server.RelayCount() == 0; }, 3000));
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
    fuzz.Manual();
    fuzz.Send(port, MakeResolve(3, hosted.code, wire::Endpoint()));
    wire::Challenge challenge;
    CHECK(fuzz.AwaitReply(port, challenge));
    wire::Key fuzzCookie = challenge.cookie;
    size_t codeOffset = kCookieOffset + fuzzCookie.size();

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
            data[4] = (uint8_t)(wire::KIND_REGISTER + rng() % 16);
            uint32_t version = rng() % 4;
            data[5] = version < 2 ? 1 : (uint8_t)version;
            if (size >= codeOffset && (rng() % 2) == 0)
                memcpy(data.data() + kCookieOffset, fuzzCookie.data(), fuzzCookie.size());
            if (size >= codeOffset + hosted.code.size() && (rng() % 3) == 0)
                memcpy(data.data() + codeOffset, hosted.code.data(), hosted.code.size());
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

void TestSipHash()
{
    static const uint8_t kExpected[][16] = {
        { 0xa3, 0x81, 0x7f, 0x04, 0xba, 0x25, 0xa8, 0xe6, 0x6d, 0xf6, 0x72, 0x14, 0xc7, 0x55, 0x02, 0x93 },
        { 0xda, 0x87, 0xc1, 0xd8, 0x6b, 0x99, 0xaf, 0x44, 0x34, 0x76, 0x59, 0x11, 0x9b, 0x22, 0xfc, 0x45 },
        { 0x81, 0x77, 0x22, 0x8d, 0xa4, 0xa4, 0x5d, 0xc7, 0xfc, 0xa3, 0x8b, 0xde, 0xf6, 0x0a, 0xff, 0xe4 },
        { 0x9c, 0x70, 0xb6, 0x0c, 0x52, 0x67, 0xa9, 0x4e, 0x5f, 0x33, 0xb6, 0xb0, 0x29, 0x85, 0xed, 0x51 },
        { 0xf8, 0x81, 0x64, 0xc1, 0x2d, 0x9c, 0x8f, 0xaf, 0x7d, 0x0f, 0x6e, 0x7c, 0x7b, 0xcd, 0x55, 0x79 },
        { 0x13, 0x68, 0x87, 0x59, 0x80, 0x77, 0x6f, 0x88, 0x54, 0x52, 0x7a, 0x07, 0x69, 0x0e, 0x96, 0x27 },
        { 0x14, 0xee, 0xca, 0x33, 0x8b, 0x20, 0x86, 0x13, 0x48, 0x5e, 0xa0, 0x30, 0x8f, 0xd7, 0xa1, 0x5e },
        { 0xa1, 0xf1, 0xeb, 0xbe, 0xd8, 0xdb, 0xc1, 0x53, 0xc0, 0xb8, 0x4a, 0xa6, 0x1f, 0xf0, 0x82, 0x39 },
        { 0x3b, 0x62, 0xa9, 0xba, 0x62, 0x58, 0xf5, 0x61, 0x0f, 0x83, 0xe2, 0x64, 0xf3, 0x14, 0x97, 0xb4 },
        { 0x26, 0x44, 0x99, 0x06, 0x0a, 0xd9, 0xba, 0xab, 0xc4, 0x7f, 0x8b, 0x02, 0xbb, 0x6d, 0x71, 0xed },
        { 0x00, 0x11, 0x0d, 0xc3, 0x78, 0x14, 0x69, 0x56, 0xc9, 0x54, 0x47, 0xd3, 0xf3, 0xd0, 0xfb, 0xba },
        { 0x01, 0x51, 0xc5, 0x68, 0x38, 0x6b, 0x66, 0x77, 0xa2, 0xb4, 0xdc, 0x6f, 0x81, 0xe5, 0xdc, 0x18 },
        { 0xd6, 0x26, 0xb2, 0x66, 0x90, 0x5e, 0xf3, 0x58, 0x82, 0x63, 0x4d, 0xf6, 0x85, 0x32, 0xc1, 0x25 },
        { 0x98, 0x69, 0xe2, 0x47, 0xe9, 0xc0, 0x8b, 0x10, 0xd0, 0x29, 0x93, 0x4f, 0xc4, 0xb9, 0x52, 0xf7 },
        { 0x31, 0xfc, 0xef, 0xac, 0x66, 0xd7, 0xde, 0x9c, 0x7e, 0xc7, 0x48, 0x5f, 0xe4, 0x49, 0x49, 0x02 },
        { 0x54, 0x93, 0xe9, 0x99, 0x33, 0xb0, 0xa8, 0x11, 0x7e, 0x08, 0xec, 0x0f, 0x97, 0xcf, 0xc3, 0xd9 },
        { 0x6e, 0xe2, 0xa4, 0xca, 0x67, 0xb0, 0x54, 0xbb, 0xfd, 0x33, 0x15, 0xbf, 0x85, 0x23, 0x05, 0x77 },
    };
    static const uint8_t kExpected63[16] = { 0x51, 0x50, 0xd1, 0x77, 0x2f, 0x50, 0x83, 0x4a,
                                             0x50, 0x3e, 0x06, 0x9a, 0x97, 0x3f, 0xbd, 0x7c };
    uint8_t key[16];
    uint8_t message[64];
    for (size_t i = 0; i < sizeof(key); i++)
        key[i] = (uint8_t)i;
    for (size_t i = 0; i < sizeof(message); i++)
        message[i] = (uint8_t)i;
    uint8_t out[16];
    for (size_t length = 0; length < sizeof(kExpected) / sizeof(kExpected[0]); length++) {
        lobby::SipHash128(key, message, length, out);
        CHECK(memcmp(out, kExpected[length], sizeof(out)) == 0);
    }
    lobby::SipHash128(key, message, 63, out);
    CHECK(memcmp(out, kExpected63, sizeof(out)) == 0);
}

void TestCookies()
{
    Running lobby(TestOptions());
    CHECK(lobby.started);
    if (!lobby.started)
        return;
    uint16_t port = lobby.Port();
    Client host, guest;
    Hosted hosted;
    CHECK(RegisterHost(host, port, 1, MakeGuid(50), wire::Endpoint(), hosted));
    CHECK(host.challenges == 1);
    CHECK(lobby.server.SessionCount() == 1);

    std::mt19937 rng(7);
    wire::Key garbage;
    for (uint8_t &b : garbage)
        b = (uint8_t)(rng() & 0xff);
    const wire::Key cookies[3] = { wire::Key(), garbage, host.cookie };
    guest.Manual();
    wire::Key guestCookie = {};
    for (uint32_t k = 0; k < 3; k++) {
        uint32_t base = 100 + 10 * k;
        wire::Challenge challenge;

        wire::Register reg;
        reg.nonce = base;
        reg.cookie = cookies[k];
        reg.instance = MakeGuid(51);
        guest.Send(port, reg);
        CHECK(guest.AwaitReply(port, challenge));
        CHECK(challenge.nonce == base);
        CHECK(!KeyIsZero(challenge.cookie));
        CHECK(challenge.cookie != host.cookie);
        guestCookie = challenge.cookie;
        CHECK(lobby.server.SessionCount() == 1);

        wire::Resolve resolve = MakeResolve(base + 1, hosted.code, wire::Endpoint());
        resolve.cookie = cookies[k];
        guest.Send(port, resolve);
        CHECK(guest.AwaitReply(port, challenge));
        CHECK(challenge.nonce == base + 1);
        CHECK(challenge.cookie == guestCookie);

        wire::Relay relay;
        relay.nonce = base + 2;
        relay.cookie = cookies[k];
        relay.code = hosted.code;
        guest.Send(port, relay);
        CHECK(guest.AwaitReply(port, challenge));
        CHECK(challenge.nonce == base + 2);
        CHECK(lobby.server.RelayCount() == 0);

        wire::Refresh refresh;
        refresh.nonce = base + 3;
        refresh.cookie = cookies[k];
        refresh.code = hosted.code;
        refresh.secret = hosted.secret;
        guest.Send(port, refresh);
        CHECK(guest.AwaitReply(port, challenge));
        CHECK(challenge.nonce == base + 3);

        CHECK(host.Silent(100));
        CHECK(lobby.server.SessionCount() == 1);
        CHECK(lobby.server.RelayCount() == 0);
    }

    wire::Refresh stay;
    stay.nonce = 140;
    stay.code = hosted.code;
    stay.secret = hosted.secret;
    host.Manual();
    host.Send(port, stay);
    wire::Refreshed refreshed;
    CHECK(host.AwaitReply(port, refreshed));
    CHECK(refreshed.nonce == 140);
    host.Automatic();

    wire::Resolve resolve = MakeResolve(141, hosted.code, wire::Endpoint());
    resolve.cookie = guestCookie;
    guest.Send(port, resolve);
    wire::Resolved resolved;
    CHECK(guest.AwaitReply(port, resolved));
    CHECK(resolved.hostPublic == host.self);
    wire::Introduce introduce;
    CHECK(host.Await(port, introduce));
    CHECK(introduce.guestPublic == guest.self);

    Client remote(htonl(0x7f000002));
    if (remote.Ok()) {
        Client twin(kLoopback, remote.port);
        if (twin.Ok()) {
            remote.Manual();
            remote.Send(port, MakeResolve(150, hosted.code, wire::Endpoint()));
            wire::Challenge challenge;
            CHECK(remote.AwaitReply(port, challenge));
            twin.Manual();
            wire::Resolve stolen = MakeResolve(151, hosted.code, wire::Endpoint());
            stolen.cookie = challenge.cookie;
            twin.Send(port, stolen);
            wire::Challenge again;
            CHECK(twin.AwaitReply(port, again));
            CHECK(again.nonce == 151);
            CHECK(again.cookie != challenge.cookie);
            CHECK(host.Silent(100));
        } else {
            printf("lobby_server: 127.0.0.1:%u not available, cookie address case skipped\n", (unsigned)remote.port);
        }
    } else {
        printf("lobby_server: 127.0.0.2 not available, cookie address case skipped\n");
    }
}

void TestCookieWindow()
{
    lobby::Options options = TestOptions();
    options.cookieWindowMs = 400;
    Running lobby(options);
    CHECK(lobby.started);
    if (!lobby.started)
        return;
    uint16_t port = lobby.Port();
    Client client;
    client.Manual();
    wire::Code unknown = net::joincode::FromBits(0x123456789ull);

    auto fresh = [&](uint32_t nonce, wire::Key &out) {
        client.Send(port, MakeResolve(nonce, unknown, wire::Endpoint()));
        wire::Challenge challenge;
        if (!client.AwaitReply(port, challenge) || challenge.nonce != nonce)
            return false;
        out = challenge.cookie;
        return true;
    };
    auto accepted = [&](uint32_t nonce, const wire::Key &cookie) {
        wire::Resolve request = MakeResolve(nonce, unknown, wire::Endpoint());
        request.cookie = cookie;
        client.Send(port, request);
        return client.AwaitFailure(port, nonce, wire::FAILURE_NOT_FOUND);
    };
    auto next = [&](const wire::Key &current, wire::Key &out) {
        for (uint32_t i = 0; i < 100; i++) {
            if (!fresh(1000 + i, out))
                return false;
            if (out != current)
                return true;
            SleepMs(10);
        }
        return false;
    };

    wire::Key first, second, third;
    CHECK(fresh(1, first));
    CHECK(accepted(2, first));
    CHECK(next(first, second));
    CHECK(accepted(3, first));
    CHECK(accepted(4, second));
    CHECK(next(second, third));
    wire::Resolve stale = MakeResolve(5, unknown, wire::Endpoint());
    stale.cookie = first;
    client.Send(port, stale);
    wire::Challenge challenge;
    CHECK(client.AwaitReply(port, challenge));
    CHECK(challenge.nonce == 5);
    CHECK(challenge.cookie == third);
    CHECK(accepted(6, second));
    CHECK(accepted(7, third));
}

void TestIntroduceLimit()
{
    Running lobby(TestOptions());
    CHECK(lobby.started);
    if (!lobby.started)
        return;
    uint16_t port = lobby.Port();
    Client remote(htonl(0x7f000002));
    if (!remote.Ok()) {
        printf("lobby_server: 127.0.0.2 not available, introduction limit case skipped\n");
        return;
    }
    Client host, guest;
    Hosted hosted;
    CHECK(RegisterHost(host, port, 1, MakeGuid(60), wire::Endpoint(), hosted));
    wire::Resolved resolved;
    for (uint32_t i = 0; i < 10; i++) {
        guest.Send(port, MakeResolve(10 + i, hosted.code, wire::Endpoint()));
        CHECK(guest.AwaitReply(port, resolved));
    }
    for (uint32_t i = 0; i < 6; i++) {
        remote.Send(port, MakeResolve(30 + i, hosted.code, wire::Endpoint()));
        CHECK(remote.AwaitReply(port, resolved));
    }
    remote.Send(port, MakeResolve(36, hosted.code, wire::Endpoint()));
    CHECK(remote.AwaitFailure(port, 36, wire::FAILURE_RATE_LIMITED));
    int introductions = 0;
    wire::Introduce introduce;
    while (host.Await(port, introduce, 150))
        introductions++;
    CHECK(introductions == 16);
}

void TestRelayCaps()
{
    {
        lobby::Options options = TestOptions();
        options.relayPortCount = 8;
        options.maxRelaysPerSession = 7;
        options.maxRelaysPerGuestAddress = 2;
        options.maxRelaysPerHostAddress = 7;
        Running lobby(options);
        CHECK(lobby.started);
        if (!lobby.started)
            return;
        uint16_t port = lobby.Port();
        Client host, guest, neighbour;
        Hosted hosted;
        CHECK(RegisterHost(host, port, 1, MakeGuid(70), wire::Endpoint(), hosted));
        RelayPair a, b;
        CHECK(OpenRelay(host, guest, port, hosted, 2, a));
        CHECK(OpenRelay(host, guest, port, hosted, 3, b));
        wire::Relay request;
        request.nonce = 4;
        request.code = hosted.code;
        guest.Send(port, request);
        CHECK(guest.AwaitFailure(port, 4, wire::FAILURE_FULL));
        request.nonce = 5;
        neighbour.Send(port, request);
        CHECK(neighbour.AwaitFailure(port, 5, wire::FAILURE_FULL));
        CHECK(lobby.server.RelayCount() == 2);
        CHECK(host.Silent(100));
    }
    {
        lobby::Options options = TestOptions();
        options.relayPortCount = 8;
        options.maxRelaysPerSession = 2;
        options.maxRelaysPerGuestAddress = 7;
        options.maxRelaysPerHostAddress = 3;
        Running lobby(options);
        CHECK(lobby.started);
        if (!lobby.started)
            return;
        uint16_t port = lobby.Port();
        Client first, second, guest;
        Hosted one, two;
        CHECK(RegisterHost(first, port, 1, MakeGuid(71), wire::Endpoint(), one));
        CHECK(RegisterHost(second, port, 2, MakeGuid(72), wire::Endpoint(), two));
        RelayPair relay;
        CHECK(OpenRelay(first, guest, port, one, 3, relay));
        CHECK(OpenRelay(first, guest, port, one, 4, relay));
        wire::Relay request;
        request.nonce = 5;
        request.code = one.code;
        guest.Send(port, request);
        CHECK(guest.AwaitFailure(port, 5, wire::FAILURE_FULL));
        CHECK(OpenRelay(second, guest, port, two, 6, relay));
        request.nonce = 7;
        request.code = two.code;
        guest.Send(port, request);
        CHECK(guest.AwaitFailure(port, 7, wire::FAILURE_FULL));
        CHECK(lobby.server.RelayCount() == 3);
    }
}

void TestRelayTimers()
{
    {
        lobby::Options options = TestOptions();
        options.relayIdleMs = 300;
        Running lobby(options);
        CHECK(lobby.started);
        if (!lobby.started)
            return;
        uint16_t port = lobby.Port();
        Client host, guest;
        Hosted hosted;
        CHECK(RegisterHost(host, port, 1, MakeGuid(80), wire::Endpoint(), hosted));
        RelayPair relay;
        CHECK(OpenRelay(host, guest, port, hosted, 2, relay));
        CHECK(BindRelay(host, relay.relayPort, relay.hostToken));
        CHECK(BindRelay(guest, relay.relayPort, relay.guestToken));
        wire::Bind hostBind, guestBind;
        hostBind.token = relay.hostToken;
        guestBind.token = relay.guestToken;
        bool closed = false;
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        while (!closed && std::chrono::steady_clock::now() < end) {
            host.Send(relay.relayPort, hostBind);
            guest.Send(relay.relayPort, guestBind);
            SleepMs(50);
            closed = lobby.server.RelayCount() == 0;
        }
        CHECK(closed);
    }
    {
        lobby::Options options = TestOptions();
        options.relayBindMs = 300;
        options.relayIdleMs = 60000;
        Running lobby(options);
        CHECK(lobby.started);
        if (!lobby.started)
            return;
        uint16_t port = lobby.Port();
        Client host, guest;
        Hosted hosted;
        CHECK(RegisterHost(host, port, 1, MakeGuid(81), wire::Endpoint(), hosted));
        RelayPair half, full;
        CHECK(OpenRelay(host, guest, port, hosted, 2, half));
        CHECK(OpenRelay(host, guest, port, hosted, 3, full));
        CHECK(BindRelay(host, half.relayPort, half.hostToken));
        CHECK(BindRelay(host, full.relayPort, full.hostToken));
        CHECK(BindRelay(guest, full.relayPort, full.guestToken));
        CHECK(WaitFor([&] { return lobby.server.RelayCount() == 1; }, 3000));
        SleepMs(1100);
        CHECK(lobby.server.RelayCount() == 1);
        std::vector<uint8_t> hello = Payload(64, 11);
        host.Send(full.relayPort, hello);
        CHECK(Forwarded(guest, full.relayPort, hello));
    }
}

void TestBindAddress()
{
    Running lobby(TestOptions());
    CHECK(lobby.started);
    if (!lobby.started)
        return;
    uint16_t port = lobby.Port();
    Client remote(htonl(0x7f000002));
    if (!remote.Ok()) {
        printf("lobby_server: 127.0.0.2 not available, bind address case skipped\n");
        return;
    }
    Client host, guest;
    Hosted hosted;
    CHECK(RegisterHost(host, port, 1, MakeGuid(90), wire::Endpoint(), hosted));
    RelayPair relay;
    CHECK(OpenRelay(host, guest, port, hosted, 2, relay));
    wire::Bind bind;
    bind.token = relay.hostToken;
    remote.Send(relay.relayPort, bind);
    CHECK(remote.Silent(100));
    bind.token = relay.guestToken;
    remote.Send(relay.relayPort, bind);
    CHECK(remote.Silent(100));
    CHECK(BindRelay(host, relay.relayPort, relay.hostToken));
    CHECK(BindRelay(guest, relay.relayPort, relay.guestToken));
    remote.Send(relay.relayPort, bind);
    CHECK(remote.Silent(100));
    bind.token = relay.hostToken;
    remote.Send(relay.relayPort, bind);
    CHECK(remote.Silent(100));
    std::vector<uint8_t> hello = Payload(64, 12);
    host.Send(relay.relayPort, hello);
    CHECK(Forwarded(guest, relay.relayPort, hello));
    guest.Send(relay.relayPort, hello);
    CHECK(Forwarded(host, relay.relayPort, hello));
    CHECK(remote.Silent(50));
}

void TestBucketTable()
{
    lobby::Options options = TestOptions();
    options.maxBuckets = 1;
    Running lobby(options);
    CHECK(lobby.started);
    if (!lobby.started)
        return;
    uint16_t port = lobby.Port();
    Client remote(htonl(0x7f000002));
    if (!remote.Ok()) {
        printf("lobby_server: 127.0.0.2 not available, bucket table case skipped\n");
        return;
    }
    Client local;
    wire::Code unknown = net::joincode::FromBits(0x2468ace0ull);
    for (uint32_t i = 0; i < 10; i++) {
        local.Send(port, MakeResolve(10 + i, unknown, wire::Endpoint()));
        CHECK(local.AwaitFailure(port, 10 + i, wire::FAILURE_NOT_FOUND));
    }
    local.Send(port, MakeResolve(20, unknown, wire::Endpoint()));
    CHECK(local.AwaitFailure(port, 20, wire::FAILURE_RATE_LIMITED));
    remote.Send(port, MakeResolve(21, unknown, wire::Endpoint()));
    CHECK(remote.AwaitFailure(port, 21, wire::FAILURE_RATE_LIMITED));
    wire::Register request;
    request.nonce = 22;
    request.instance = MakeGuid(91);
    remote.Send(port, request);
    CHECK(remote.AwaitFailure(port, 22, wire::FAILURE_RATE_LIMITED));
    CHECK(lobby.server.SessionCount() == 0);
    local.Send(port, MakeResolve(23, unknown, wire::Endpoint()));
    CHECK(local.AwaitFailure(port, 23, wire::FAILURE_RATE_LIMITED));
}

}

int main()
{
    auto start = std::chrono::steady_clock::now();
    TestSipHash();
    TestRegisterResolve();
    TestExpiry();
    TestLimits();
    TestRelay();
    TestRelayLimits();
    TestCookies();
    TestCookieWindow();
    TestIntroduceLimit();
    TestRelayCaps();
    TestRelayTimers();
    TestBindAddress();
    TestBucketTable();
    TestFuzz();
    long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (s_failures != 0) {
        fprintf(stderr, "lobby_server: %d failures\n", s_failures);
        return 1;
    }
    printf("lobby_server: all passed in %ld ms\n", ms);
    return 0;
}
