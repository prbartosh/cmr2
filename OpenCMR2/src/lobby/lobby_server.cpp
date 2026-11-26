#include "lobby/lobby_server.h"

#include "net/join_code.h"
#include "net/lobby_wire.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <vector>

namespace lobby {

namespace {

namespace wire = net::lobby;

constexpr int64_t kLookupCost = 3000;
constexpr int64_t kLookupCapacity = 10 * kLookupCost;
constexpr int64_t kRegisterCost = 10000;
constexpr int64_t kRegisterCapacity = 4 * kRegisterCost;
constexpr int64_t kBucketIdleMs = std::max(kLookupCapacity, kRegisterCapacity);
constexpr int64_t kBucketPruneMs = 10000;
constexpr int64_t kBucketFullPruneMs = 1000;
constexpr int64_t kSweepMs = 1000;
constexpr int64_t kStatsMs = 60000;
constexpr int kIntroducePerSecond = 16;
constexpr int kCodeAttempts = 32;
constexpr int kDrainLimit = 4096;
constexpr int kSocketBuffer = 1 << 20;

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool ReadUrandom(void *out, size_t size)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    uint8_t *p = (uint8_t *)out;
    size_t done = 0;
    while (done < size) {
        ssize_t n = read(fd, p + done, size - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        done += (size_t)n;
    }
    close(fd);
    return done == size;
}

bool RandomBytes(void *out, size_t size)
{
    uint8_t *p = (uint8_t *)out;
    size_t done = 0;
    while (done < size) {
        size_t chunk = std::min<size_t>(size - done, 256);
        if (getentropy(p + done, chunk) != 0)
            return ReadUrandom(out, size);
        done += chunk;
    }
    return true;
}

uint64_t Rotl(uint64_t x, int bits)
{
    return (x << bits) | (x >> (64 - bits));
}

void SipRound(uint64_t &v0, uint64_t &v1, uint64_t &v2, uint64_t &v3)
{
    v0 += v1;
    v1 = Rotl(v1, 13);
    v1 ^= v0;
    v0 = Rotl(v0, 32);
    v2 += v3;
    v3 = Rotl(v3, 16);
    v3 ^= v2;
    v0 += v3;
    v3 = Rotl(v3, 21);
    v3 ^= v0;
    v2 += v1;
    v1 = Rotl(v1, 17);
    v1 ^= v2;
    v2 = Rotl(v2, 32);
}

uint64_t LoadLe64(const uint8_t *p)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; i++)
        value |= (uint64_t)p[i] << (8 * i);
    return value;
}

void StoreLe64(uint8_t *p, uint64_t value)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(value >> (8 * i));
}

bool KeysEqual(const wire::Key &a, const wire::Key &b)
{
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); i++)
        diff = (uint8_t)(diff | (a[i] ^ b[i]));
    return diff == 0;
}

uint64_t CodeKey(const wire::Code &code)
{
    uint64_t key = 0;
    for (size_t i = 0; i < code.size(); i++)
        key |= (uint64_t)(uint8_t)code[i] << (8 * i);
    return key;
}

bool IsPrivate(const wire::Endpoint &e)
{
    if (e.port == 0)
        return false;
    uint32_t a = ntohl(e.address);
    return (a >> 24) == 10 || (a >> 20) == ((172u << 4) | 1) || (a >> 16) == ((192u << 8) | 168);
}

wire::Endpoint LocalFor(const wire::Endpoint &local, bool sameNat)
{
    return sameNat && IsPrivate(local) ? local : wire::Endpoint();
}

bool IsRequestKind(uint8_t kind)
{
    switch (kind) {
    case wire::KIND_REGISTER:
    case wire::KIND_REFRESH:
    case wire::KIND_UNREGISTER:
    case wire::KIND_RESOLVE:
    case wire::KIND_RELAY:
    case wire::KIND_BIND:
        return true;
    default:
        return false;
    }
}

bool HasNonce(uint8_t kind)
{
    return kind == wire::KIND_REGISTER || kind == wire::KIND_REFRESH || kind == wire::KIND_RESOLVE ||
           kind == wire::KIND_RELAY;
}

sockaddr_in ToSockaddr(const wire::Endpoint &e)
{
    sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = e.address;
    sa.sin_port = htons(e.port);
    return sa;
}

const char *FormatEndpoint(const wire::Endpoint &e, char *buffer, size_t size)
{
    char address[INET_ADDRSTRLEN] = {};
    in_addr in;
    in.s_addr = e.address;
    inet_ntop(AF_INET, &in, address, sizeof(address));
    snprintf(buffer, size, "%s:%u", address, (unsigned)e.port);
    return buffer;
}

int OpenSocket(uint32_t address, uint16_t port, uint16_t &boundPort)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        close(fd);
        return -1;
    }
    int buffer = kSocketBuffer;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
    wire::Endpoint local;
    local.address = address;
    local.port = port;
    sockaddr_in sa = ToSockaddr(local);
    if (bind(fd, (const sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    sockaddr_in bound;
    socklen_t length = sizeof(bound);
    if (getsockname(fd, (sockaddr *)&bound, &length) != 0) {
        close(fd);
        return -1;
    }
    boundPort = ntohs(bound.sin_port);
    return fd;
}

struct Session {
    wire::Code code = {};
    wire::Key secret = {};
    wire::Guid instance = {};
    uint32_t registerNonce = 0;
    wire::Endpoint hostPublic;
    wire::Endpoint hostLocal;
    int64_t expiresAt = 0;
    int64_t introduceWindow = 0;
    int introduceCount = 0;
    std::vector<uint64_t> relays;
};

struct Relay {
    uint64_t session = 0;
    int fd = -1;
    uint16_t port = 0;
    size_t slot = 0;
    wire::Key hostToken = {};
    wire::Key guestToken = {};
    wire::Endpoint host;
    wire::Endpoint guest;
    uint32_t hostAddress = 0;
    uint32_t guestAddress = 0;
    wire::Endpoint requester;
    uint32_t requestNonce = 0;
    bool hostBound = false;
    bool guestBound = false;
    int64_t createdAt = 0;
    int64_t lastActivity = 0;
    int64_t credit = 0;
    int64_t creditUpdated = 0;
    uint64_t forwardedBytes = 0;
    uint64_t droppedBytes = 0;
};

struct Bucket {
    int64_t lookup = kLookupCapacity;
    int64_t registration = kRegisterCapacity;
    int64_t updated = 0;
};

struct Stats {
    uint64_t registered = 0;
    uint64_t resolved = 0;
    uint64_t relayed = 0;
    uint64_t limited = 0;
    uint64_t failed = 0;
    uint64_t challenged = 0;
};

}

void SipHash128(const uint8_t key[16], const void *data, size_t size, uint8_t out[16])
{
    const uint8_t *in = (const uint8_t *)data;
    uint64_t k0 = LoadLe64(key);
    uint64_t k1 = LoadLe64(key + 8);
    uint64_t v0 = k0 ^ 0x736f6d6570736575ull;
    uint64_t v1 = k1 ^ 0x646f72616e646f6dull ^ 0xee;
    uint64_t v2 = k0 ^ 0x6c7967656e657261ull;
    uint64_t v3 = k1 ^ 0x7465646279746573ull;
    size_t blocks = size - size % 8;
    for (size_t i = 0; i < blocks; i += 8) {
        uint64_t m = LoadLe64(in + i);
        v3 ^= m;
        SipRound(v0, v1, v2, v3);
        SipRound(v0, v1, v2, v3);
        v0 ^= m;
    }
    uint64_t last = (uint64_t)size << 56;
    for (size_t i = 0; i < size % 8; i++)
        last |= (uint64_t)in[blocks + i] << (8 * i);
    v3 ^= last;
    SipRound(v0, v1, v2, v3);
    SipRound(v0, v1, v2, v3);
    v0 ^= last;
    v2 ^= 0xee;
    for (int i = 0; i < 4; i++)
        SipRound(v0, v1, v2, v3);
    StoreLe64(out, v0 ^ v1 ^ v2 ^ v3);
    v1 ^= 0xdd;
    for (int i = 0; i < 4; i++)
        SipRound(v0, v1, v2, v3);
    StoreLe64(out + 8, v0 ^ v1 ^ v2 ^ v3);
}

struct Server::State {
    Options options;
    int mainFd = -1;
    uint16_t port = 0;
    std::unordered_map<uint64_t, Session> sessions;
    std::unordered_map<uint32_t, std::vector<uint64_t>> byAddress;
    std::unordered_map<uint64_t, Relay> relays;
    std::vector<bool> relaySlots;
    size_t relayCursor = 0;
    uint64_t nextRelayId = 1;
    std::unordered_map<uint32_t, Bucket> buckets;
    wire::Key cookieKey = {};
    std::vector<pollfd> pollFds;
    std::vector<uint64_t> pollRelays;
    std::vector<uint8_t> buffer;
    int64_t now = 0;
    int64_t nextSweep = 0;
    int64_t nextPrune = 0;
    int64_t nextFullPrune = 0;
    int64_t nextStats = 0;
    Stats stats;
    bool statsDirty = false;
    std::atomic<size_t> sessionCount{ 0 };
    std::atomic<size_t> relayCount{ 0 };

    explicit State(const Options &o) : options(o), buffer(wire::kMaxDatagram + 1) {}

    ~State()
    {
        for (auto &entry : relays)
            close(entry.second.fd);
        if (mainFd >= 0)
            close(mainFd);
    }

    __attribute__((format(printf, 2, 3))) void Log(const char *format, ...)
    {
        if (options.log == nullptr)
            return;
        char text[512];
        va_list args;
        va_start(args, format);
        vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        options.log("%s", text);
    }

    void UpdateCounts()
    {
        sessionCount.store(sessions.size());
        relayCount.store(relays.size());
        statsDirty = true;
    }

    size_t RelayCapacity() const { return options.relayPortCount; }

    template <typename Message>
    void Send(int fd, const wire::Endpoint &to, const Message &message, size_t limit)
    {
        std::vector<uint8_t> data = wire::Encode(message);
        if (data.size() > limit)
            return;
        sockaddr_in sa = ToSockaddr(to);
        sendto(fd, data.data(), data.size(), 0, (const sockaddr *)&sa, sizeof(sa));
    }

    uint64_t CookieWindow() const { return (uint64_t)now / options.cookieWindowMs; }

    wire::Key MakeCookie(const wire::Endpoint &from, uint64_t window) const
    {
        uint8_t input[14];
        memcpy(input, &from.address, sizeof(from.address));
        input[4] = (uint8_t)(from.port >> 8);
        input[5] = (uint8_t)from.port;
        for (int i = 0; i < 8; i++)
            input[6 + i] = (uint8_t)(window >> (8 * i));
        wire::Key cookie;
        SipHash128(cookieKey.data(), input, sizeof(input), cookie.data());
        return cookie;
    }

    bool CookieValid(const wire::Endpoint &from, const wire::Key &cookie) const
    {
        uint64_t window = CookieWindow();
        bool current = KeysEqual(cookie, MakeCookie(from, window));
        bool previous = KeysEqual(cookie, MakeCookie(from, window - 1));
        return current | previous;
    }

    void SendChallenge(const wire::Endpoint &to, uint32_t nonce, size_t limit)
    {
        wire::Challenge challenge;
        challenge.nonce = nonce;
        challenge.cookie = MakeCookie(to, CookieWindow());
        Send(mainFd, to, challenge, limit);
        stats.challenged++;
        statsDirty = true;
    }

    void Fail(const wire::Endpoint &to, uint32_t nonce, wire::Failure reason, size_t limit)
    {
        wire::Failed failed;
        failed.nonce = nonce;
        failed.reason = reason;
        Send(mainFd, to, failed, limit);
        if (reason == wire::FAILURE_RATE_LIMITED)
            stats.limited++;
        else
            stats.failed++;
        statsDirty = true;
    }

    Bucket *BucketFor(uint32_t address)
    {
        auto it = buckets.find(address);
        if (it == buckets.end()) {
            if (buckets.size() >= options.maxBuckets && now >= nextFullPrune) {
                PruneBuckets();
                nextFullPrune = now + kBucketFullPruneMs;
            }
            if (buckets.size() >= options.maxBuckets)
                return nullptr;
            it = buckets.emplace(address, Bucket()).first;
            it->second.updated = now;
            return &it->second;
        }
        Bucket &bucket = it->second;
        int64_t elapsed = std::max<int64_t>(0, now - bucket.updated);
        bucket.lookup = std::min(kLookupCapacity, bucket.lookup + elapsed);
        bucket.registration = std::min(kRegisterCapacity, bucket.registration + elapsed);
        bucket.updated = now;
        return &bucket;
    }

    bool TakeLookup(uint32_t address)
    {
        Bucket *bucket = BucketFor(address);
        if (bucket == nullptr || bucket->lookup < kLookupCost)
            return false;
        bucket->lookup -= kLookupCost;
        return true;
    }

    bool TakeRegistration(uint32_t address)
    {
        Bucket *bucket = BucketFor(address);
        if (bucket == nullptr || bucket->registration < kRegisterCost)
            return false;
        bucket->registration -= kRegisterCost;
        return true;
    }

    void PruneBuckets()
    {
        for (auto it = buckets.begin(); it != buckets.end();) {
            if (now - it->second.updated >= kBucketIdleMs)
                it = buckets.erase(it);
            else
                ++it;
        }
    }

    void IndexAdd(uint32_t address, uint64_t key) { byAddress[address].push_back(key); }

    void IndexRemove(uint32_t address, uint64_t key)
    {
        auto it = byAddress.find(address);
        if (it == byAddress.end())
            return;
        std::vector<uint64_t> &keys = it->second;
        keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
        if (keys.empty())
            byAddress.erase(it);
    }

    size_t SessionsAt(uint32_t address) const
    {
        auto it = byAddress.find(address);
        return it == byAddress.end() ? 0 : it->second.size();
    }

    Session *FindRegistration(const wire::Endpoint &from, const wire::Register &request)
    {
        auto it = byAddress.find(from.address);
        if (it == byAddress.end())
            return nullptr;
        for (uint64_t key : it->second) {
            auto found = sessions.find(key);
            if (found == sessions.end())
                continue;
            Session &session = found->second;
            if (session.hostPublic == from && session.instance == request.instance &&
                session.registerNonce == request.nonce)
                return &session;
        }
        return nullptr;
    }

    Session *FindSession(const wire::Code &code)
    {
        auto it = sessions.find(CodeKey(code));
        return it == sessions.end() || now >= it->second.expiresAt ? nullptr : &it->second;
    }

    size_t RelaysForGuest(uint32_t address) const
    {
        size_t count = 0;
        for (const auto &entry : relays)
            count += entry.second.guestAddress == address;
        return count;
    }

    size_t RelaysForHost(uint32_t address) const
    {
        size_t count = 0;
        for (const auto &entry : relays)
            count += entry.second.hostAddress == address;
        return count;
    }

    void CloseRelay(uint64_t id, bool detach)
    {
        auto it = relays.find(id);
        if (it == relays.end())
            return;
        Relay &relay = it->second;
        if (detach) {
            auto session = sessions.find(relay.session);
            if (session != sessions.end()) {
                std::vector<uint64_t> &ids = session->second.relays;
                ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
            }
        }
        close(relay.fd);
        if (options.relayFirstPort != 0 && relay.slot < relaySlots.size())
            relaySlots[relay.slot] = false;
        Log("relay closed: %llu bytes forwarded, %llu bytes dropped", (unsigned long long)relay.forwardedBytes,
            (unsigned long long)relay.droppedBytes);
        relays.erase(it);
        UpdateCounts();
    }

    void RemoveSession(uint64_t key)
    {
        auto it = sessions.find(key);
        if (it == sessions.end())
            return;
        std::vector<uint64_t> ids = it->second.relays;
        for (uint64_t id : ids)
            CloseRelay(id, false);
        if (options.verbose) {
            char code[net::joincode::kTextSize + 1];
            snprintf(code, sizeof(code), "%s", net::joincode::Format(it->second.code).c_str());
            Log("session %s closed", code);
        }
        IndexRemove(it->second.hostPublic.address, key);
        sessions.erase(it);
        UpdateCounts();
    }

    bool OpenRelay(Relay &relay)
    {
        if (relays.size() >= RelayCapacity())
            return false;
        if (options.relayFirstPort == 0) {
            relay.fd = OpenSocket(options.bindAddress, 0, relay.port);
            return relay.fd >= 0;
        }
        size_t count = relaySlots.size();
        for (size_t i = 0; i < count; i++) {
            size_t slot = (relayCursor + i) % count;
            if (relaySlots[slot])
                continue;
            uint16_t port = (uint16_t)(options.relayFirstPort + slot);
            int fd = OpenSocket(options.bindAddress, port, relay.port);
            if (fd < 0)
                continue;
            relaySlots[slot] = true;
            relayCursor = (slot + 1) % count;
            relay.fd = fd;
            relay.slot = slot;
            return true;
        }
        return false;
    }

    void HandleRegister(const wire::Endpoint &from, const uint8_t *data, size_t size)
    {
        wire::Register request;
        if (!wire::Decode(data, size, request))
            return;
        if (!CookieValid(from, request.cookie)) {
            SendChallenge(from, request.nonce, size);
            return;
        }
        Session *existing = FindRegistration(from, request);
        if (existing == nullptr) {
            if (sessions.size() >= options.maxSessions || SessionsAt(from.address) >= options.maxSessionsPerAddress) {
                Fail(from, request.nonce, wire::FAILURE_FULL, size);
                return;
            }
            if (!TakeRegistration(from.address)) {
                Fail(from, request.nonce, wire::FAILURE_RATE_LIMITED, size);
                return;
            }
            Session session;
            uint64_t key = 0;
            bool unique = false;
            for (int attempt = 0; attempt < kCodeAttempts && !unique; attempt++) {
                uint8_t bits[5];
                if (!RandomBytes(bits, sizeof(bits))) {
                    Log("error: no random source");
                    return;
                }
                uint64_t value = 0;
                for (uint8_t b : bits)
                    value = (value << 8) | b;
                session.code = net::joincode::FromBits(value);
                key = CodeKey(session.code);
                unique = sessions.find(key) == sessions.end();
            }
            if (!unique || !RandomBytes(session.secret.data(), session.secret.size())) {
                Fail(from, request.nonce, wire::FAILURE_FULL, size);
                return;
            }
            session.instance = request.instance;
            session.registerNonce = request.nonce;
            session.hostPublic = from;
            session.hostLocal = request.local;
            existing = &sessions.emplace(key, session).first->second;
            IndexAdd(from.address, key);
            stats.registered++;
            UpdateCounts();
            if (options.verbose) {
                char endpoint[32];
                Log("session %s registered by %s", net::joincode::Format(session.code).c_str(),
                    FormatEndpoint(from, endpoint, sizeof(endpoint)));
            }
        }
        existing->expiresAt = now + options.sessionTtlMs;
        wire::Registered reply;
        reply.nonce = request.nonce;
        reply.code = existing->code;
        reply.secret = existing->secret;
        Send(mainFd, from, reply, size);
    }

    void HandleRefresh(const wire::Endpoint &from, const uint8_t *data, size_t size)
    {
        wire::Refresh request;
        if (!wire::Decode(data, size, request))
            return;
        uint64_t key = CodeKey(request.code);
        Session *found = FindSession(request.code);
        if (found == nullptr || !KeysEqual(found->secret, request.secret)) {
            Fail(from, request.nonce, wire::FAILURE_UNKNOWN_SESSION, size);
            return;
        }
        Session &session = *found;
        if (session.hostPublic != from) {
            if (!CookieValid(from, request.cookie)) {
                SendChallenge(from, request.nonce, size);
                return;
            }
            if (session.hostPublic.address != from.address) {
                IndexRemove(session.hostPublic.address, key);
                IndexAdd(from.address, key);
                for (uint64_t id : session.relays) {
                    auto relay = relays.find(id);
                    if (relay != relays.end())
                        relay->second.hostAddress = from.address;
                }
            }
            if (options.verbose) {
                char endpoint[32];
                Log("session %s moved to %s", net::joincode::Format(session.code).c_str(),
                    FormatEndpoint(from, endpoint, sizeof(endpoint)));
            }
        }
        session.hostPublic = from;
        session.hostLocal = request.local;
        session.expiresAt = now + options.sessionTtlMs;
        wire::Refreshed reply;
        reply.nonce = request.nonce;
        Send(mainFd, from, reply, size);
    }

    void HandleUnregister(const uint8_t *data, size_t size)
    {
        wire::Unregister request;
        if (!wire::Decode(data, size, request))
            return;
        Session *session = FindSession(request.code);
        if (session == nullptr || !KeysEqual(session->secret, request.secret))
            return;
        RemoveSession(CodeKey(request.code));
    }

    void HandleResolve(const wire::Endpoint &from, const uint8_t *data, size_t size)
    {
        wire::Resolve request;
        if (!wire::Decode(data, size, request))
            return;
        if (!CookieValid(from, request.cookie)) {
            SendChallenge(from, request.nonce, size);
            return;
        }
        if (!TakeLookup(from.address)) {
            Fail(from, request.nonce, wire::FAILURE_RATE_LIMITED, size);
            return;
        }
        Session *session = FindSession(request.code);
        if (session == nullptr) {
            Fail(from, request.nonce, wire::FAILURE_NOT_FOUND, size);
            return;
        }
        if (now - session->introduceWindow >= 1000) {
            session->introduceWindow = now;
            session->introduceCount = 0;
        }
        if (session->introduceCount >= kIntroducePerSecond) {
            Fail(from, request.nonce, wire::FAILURE_RATE_LIMITED, size);
            return;
        }
        session->introduceCount++;
        bool sameNat = from.address == session->hostPublic.address;
        wire::Resolved reply;
        reply.nonce = request.nonce;
        reply.instance = session->instance;
        reply.hostPublic = session->hostPublic;
        reply.hostLocal = LocalFor(session->hostLocal, sameNat);
        wire::Introduce introduce;
        introduce.secret = session->secret;
        introduce.guestPublic = from;
        introduce.guestLocal = LocalFor(request.local, sameNat);
        Send(mainFd, from, reply, size);
        Send(mainFd, session->hostPublic, introduce, wire::kMinRequest);
        stats.resolved++;
        statsDirty = true;
        if (options.verbose) {
            char guest[32], host[32];
            Log("session %s resolved by %s for %s", net::joincode::Format(session->code).c_str(),
                FormatEndpoint(from, guest, sizeof(guest)), FormatEndpoint(session->hostPublic, host, sizeof(host)));
        }
    }

    void OfferRelay(const Session &session, const Relay &relay, const wire::Endpoint &to, uint32_t nonce, size_t size,
                    bool offerHost)
    {
        wire::Relayed reply;
        reply.nonce = nonce;
        reply.port = relay.port;
        reply.token = relay.guestToken;
        Send(mainFd, to, reply, size);
        if (!offerHost)
            return;
        wire::RelayOffer offer;
        offer.secret = session.secret;
        offer.port = relay.port;
        offer.token = relay.hostToken;
        Send(mainFd, session.hostPublic, offer, wire::kMinRequest);
    }

    void HandleRelay(const wire::Endpoint &from, const uint8_t *data, size_t size)
    {
        wire::Relay request;
        if (!wire::Decode(data, size, request))
            return;
        if (!CookieValid(from, request.cookie)) {
            SendChallenge(from, request.nonce, size);
            return;
        }
        Session *found = FindSession(request.code);
        if (found != nullptr) {
            for (uint64_t id : found->relays) {
                auto existing = relays.find(id);
                if (existing != relays.end() && existing->second.requester == from &&
                    existing->second.requestNonce == request.nonce) {
                    OfferRelay(*found, existing->second, from, request.nonce, size, !existing->second.hostBound);
                    return;
                }
            }
        }
        if (!TakeLookup(from.address)) {
            Fail(from, request.nonce, wire::FAILURE_RATE_LIMITED, size);
            return;
        }
        if (found == nullptr) {
            Fail(from, request.nonce, wire::FAILURE_NOT_FOUND, size);
            return;
        }
        Session &session = *found;
        Relay relay;
        relay.session = CodeKey(request.code);
        relay.requester = from;
        relay.requestNonce = request.nonce;
        relay.guestAddress = from.address;
        relay.hostAddress = session.hostPublic.address;
        if (session.relays.size() >= options.maxRelaysPerSession ||
            RelaysForGuest(relay.guestAddress) >= options.maxRelaysPerGuestAddress ||
            RelaysForHost(relay.hostAddress) >= options.maxRelaysPerHostAddress ||
            !RandomBytes(relay.hostToken.data(), relay.hostToken.size()) ||
            !RandomBytes(relay.guestToken.data(), relay.guestToken.size()) || !OpenRelay(relay)) {
            Fail(from, request.nonce, wire::FAILURE_FULL, size);
            return;
        }
        relay.createdAt = now;
        relay.lastActivity = now;
        relay.creditUpdated = now;
        relay.credit = 2 * (int64_t)options.relayBytesPerSecond * 1000;
        uint64_t id = nextRelayId++;
        relays.emplace(id, relay);
        session.relays.push_back(id);
        stats.relayed++;
        UpdateCounts();
        OfferRelay(session, relay, from, request.nonce, size, true);
        if (options.verbose) {
            char guest[32];
            Log("session %s relay on port %u for %s", net::joincode::Format(session.code).c_str(),
                (unsigned)relay.port, FormatEndpoint(from, guest, sizeof(guest)));
        }
    }

    void HandleMain(const wire::Endpoint &from, const uint8_t *data, size_t size)
    {
        if (size < wire::kHeaderSize || memcmp(data, wire::kMagic, sizeof(wire::kMagic)) != 0)
            return;
        uint8_t kind = data[4];
        if (!IsRequestKind(kind) || size < wire::kMinRequest)
            return;
        if (data[5] != wire::kVersion) {
            uint32_t nonce = 0;
            if (HasNonce(kind)) {
                net::Reader reader(data + wire::kHeaderSize, size - wire::kHeaderSize);
                nonce = reader.U32();
            }
            Fail(from, nonce, wire::FAILURE_VERSION, size);
            return;
        }
        switch (kind) {
        case wire::KIND_REGISTER:
            HandleRegister(from, data, size);
            break;
        case wire::KIND_REFRESH:
            HandleRefresh(from, data, size);
            break;
        case wire::KIND_UNREGISTER:
            HandleUnregister(data, size);
            break;
        case wire::KIND_RESOLVE:
            HandleResolve(from, data, size);
            break;
        case wire::KIND_RELAY:
            HandleRelay(from, data, size);
            break;
        default:
            break;
        }
    }

    bool SpendRelayCredit(Relay &relay, size_t size)
    {
        int64_t rate = options.relayBytesPerSecond;
        int64_t elapsed = std::max<int64_t>(0, now - relay.creditUpdated);
        relay.credit = std::min(2 * rate * 1000, relay.credit + elapsed * rate);
        relay.creditUpdated = now;
        int64_t cost = (int64_t)size * 1000;
        if (relay.credit < cost)
            return false;
        relay.credit -= cost;
        return true;
    }

    void HandleBind(Relay &relay, const wire::Endpoint &from, const uint8_t *data, size_t size)
    {
        if (size < wire::kMinRequest)
            return;
        if (data[5] != wire::kVersion) {
            wire::Failed failed;
            failed.reason = wire::FAILURE_VERSION;
            Send(relay.fd, from, failed, size);
            return;
        }
        wire::Bind request;
        if (!wire::Decode(data, size, request))
            return;
        bool hostToken = KeysEqual(request.token, relay.hostToken);
        bool guestToken = KeysEqual(request.token, relay.guestToken);
        if (hostToken && from.address == relay.hostAddress) {
            relay.host = from;
            relay.hostBound = true;
        } else if (guestToken && from.address == relay.guestAddress) {
            relay.guest = from;
            relay.guestBound = true;
        } else {
            return;
        }
        wire::Bound reply;
        reply.token = request.token;
        Send(relay.fd, from, reply, size);
    }

    void HandleRelayDatagram(Relay &relay, const wire::Endpoint &from, const uint8_t *data, size_t size)
    {
        wire::Kind kind;
        if (wire::Peek(data, size, kind) && kind == wire::KIND_BIND) {
            HandleBind(relay, from, data, size);
            return;
        }
        if (!relay.hostBound || !relay.guestBound)
            return;
        const wire::Endpoint *to = nullptr;
        if (from == relay.host)
            to = &relay.guest;
        else if (from == relay.guest)
            to = &relay.host;
        else
            return;
        if (!SpendRelayCredit(relay, size)) {
            relay.droppedBytes += size;
            return;
        }
        sockaddr_in sa = ToSockaddr(*to);
        if (sendto(relay.fd, data, size, 0, (const sockaddr *)&sa, sizeof(sa)) == (ssize_t)size) {
            relay.forwardedBytes += size;
            relay.lastActivity = now;
        } else {
            relay.droppedBytes += size;
        }
    }

    template <typename Handler>
    void Drain(int fd, Handler handler)
    {
        for (int i = 0; i < kDrainLimit; i++) {
            sockaddr_in sa;
            socklen_t length = sizeof(sa);
            ssize_t n = recvfrom(fd, buffer.data(), buffer.size(), 0, (sockaddr *)&sa, &length);
            if (n < 0) {
                if (errno == EINTR || errno == ECONNREFUSED)
                    continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                    Log("error: recvfrom: %s", strerror(errno));
                return;
            }
            if ((size_t)n > wire::kMaxDatagram || length < sizeof(sa) || sa.sin_family != AF_INET)
                continue;
            wire::Endpoint from;
            from.address = sa.sin_addr.s_addr;
            from.port = ntohs(sa.sin_port);
            if (!from.Valid())
                continue;
            if (!handler(from, (size_t)n))
                return;
        }
    }

    void Sweep()
    {
        std::vector<uint64_t> expired;
        for (auto &entry : sessions) {
            if (now >= entry.second.expiresAt)
                expired.push_back(entry.first);
        }
        for (uint64_t key : expired)
            RemoveSession(key);
        std::vector<uint64_t> idle;
        for (auto &entry : relays) {
            const Relay &relay = entry.second;
            bool unbound =
                !(relay.hostBound && relay.guestBound) && now - relay.createdAt >= (int64_t)options.relayBindMs;
            if (unbound || now - relay.lastActivity >= (int64_t)options.relayIdleMs)
                idle.push_back(entry.first);
        }
        for (uint64_t id : idle)
            CloseRelay(id, true);
    }

    void Maintain()
    {
        if (now >= nextSweep) {
            Sweep();
            nextSweep = now + kSweepMs;
        }
        if (now >= nextPrune) {
            PruneBuckets();
            nextPrune = now + kBucketPruneMs;
        }
        if (now >= nextStats) {
            if (statsDirty) {
                Log("%zu sessions, %zu relays; registered %llu, resolved %llu, relayed %llu, rate limited %llu, "
                    "failed %llu, challenged %llu",
                    sessions.size(), relays.size(), (unsigned long long)stats.registered,
                    (unsigned long long)stats.resolved, (unsigned long long)stats.relayed,
                    (unsigned long long)stats.limited, (unsigned long long)stats.failed,
                    (unsigned long long)stats.challenged);
                stats = Stats();
                statsDirty = false;
            }
            nextStats = now + kStatsMs;
        }
    }
};

Server::Server(const Options &options) : state(new State(options)) {}

Server::~Server() = default;

bool Server::Start()
{
    State &s = *state;
    if (s.mainFd >= 0)
        return true;
    const Options &o = s.options;
    if (o.maxSessions == 0 || o.maxSessions > kMaxSessionsLimit) {
        s.Log("error: max sessions must be from 1 to %zu", kMaxSessionsLimit);
        return false;
    }
    if (o.cookieWindowMs == 0) {
        s.Log("error: invalid cookie window");
        return false;
    }
    if (o.relayFirstPort != 0) {
        uint32_t last = (uint32_t)o.relayFirstPort + o.relayPortCount;
        if (o.relayPortCount == 0 || last - 1 > 65535 || (o.port >= o.relayFirstPort && o.port < last)) {
            s.Log("error: invalid relay port range");
            return false;
        }
        s.relaySlots.assign(o.relayPortCount, false);
    }
    if (!RandomBytes(s.cookieKey.data(), s.cookieKey.size())) {
        s.Log("error: no random source");
        return false;
    }
    s.mainFd = OpenSocket(o.bindAddress, o.port, s.port);
    if (s.mainFd < 0) {
        s.Log("error: cannot bind UDP port %u: %s", (unsigned)o.port, strerror(errno));
        return false;
    }
    char endpoint[32];
    wire::Endpoint bound;
    bound.address = o.bindAddress;
    bound.port = s.port;
    if (o.relayFirstPort != 0)
        s.Log("listening on %s, relay ports %u-%u, max sessions %zu (%zu per address), verbose %s",
              FormatEndpoint(bound, endpoint, sizeof(endpoint)), (unsigned)o.relayFirstPort,
              (unsigned)(o.relayFirstPort + o.relayPortCount - 1), o.maxSessions, o.maxSessionsPerAddress,
              o.verbose ? "on" : "off");
    else
        s.Log("listening on %s, %u relays on ephemeral ports, max sessions %zu (%zu per address), verbose %s",
              FormatEndpoint(bound, endpoint, sizeof(endpoint)), (unsigned)o.relayPortCount, o.maxSessions,
              o.maxSessionsPerAddress, o.verbose ? "on" : "off");
    s.now = NowMs();
    s.nextPrune = s.now + kBucketPruneMs;
    s.nextStats = s.now + kStatsMs;
    return true;
}

uint16_t Server::Port() const
{
    return state->port;
}

void Server::Poll(int timeoutMs)
{
    State &s = *state;
    if (s.mainFd < 0)
        return;
    s.pollFds.clear();
    s.pollRelays.clear();
    s.pollFds.push_back({ s.mainFd, POLLIN, 0 });
    for (auto &entry : s.relays) {
        s.pollFds.push_back({ entry.second.fd, POLLIN, 0 });
        s.pollRelays.push_back(entry.first);
    }
    int ready = poll(s.pollFds.data(), (nfds_t)s.pollFds.size(), timeoutMs);
    if (ready < 0 && errno != EINTR)
        s.Log("error: poll: %s", strerror(errno));
    s.now = NowMs();
    if (ready > 0) {
        if (s.pollFds[0].revents != 0) {
            s.Drain(s.mainFd, [&s](const wire::Endpoint &from, size_t size) {
                s.HandleMain(from, s.buffer.data(), size);
                return true;
            });
        }
        for (size_t i = 1; i < s.pollFds.size(); i++) {
            if (s.pollFds[i].revents == 0)
                continue;
            uint64_t id = s.pollRelays[i - 1];
            if (s.relays.find(id) == s.relays.end())
                continue;
            s.Drain(s.pollFds[i].fd, [&s, id](const wire::Endpoint &from, size_t size) {
                auto it = s.relays.find(id);
                if (it == s.relays.end())
                    return false;
                s.HandleRelayDatagram(it->second, from, s.buffer.data(), size);
                return true;
            });
        }
    }
    s.Maintain();
}

size_t Server::SessionCount() const
{
    return state->sessionCount.load();
}

size_t Server::RelayCount() const
{
    return state->relayCount.load();
}

}
