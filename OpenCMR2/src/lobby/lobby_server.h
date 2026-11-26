#ifndef OPENCMR2_LOBBY_SERVER_H
#define OPENCMR2_LOBBY_SERVER_H

#include <stddef.h>
#include <stdint.h>

#include <memory>

namespace lobby {

struct Options {
    uint32_t bindAddress = 0;
    uint16_t port = 28630;
    uint16_t relayFirstPort = 28631;
    uint16_t relayPortCount = 64;
    size_t maxSessions = 4096;
    size_t maxSessionsPerAddress = 4;
    size_t maxRelaysPerSession = 7;
    size_t maxRelaysPerGuestAddress = 2;
    size_t maxRelaysPerHostAddress = 7;
    size_t maxBuckets = 65536;
    uint32_t sessionTtlMs = 35000;
    uint32_t relayIdleMs = 30000;
    uint32_t relayBindMs = 10000;
    uint32_t cookieWindowMs = 30000;
    uint32_t relayBytesPerSecond = 262144;
    bool verbose = false;
    void (*log)(const char *format, ...) = nullptr;
};

constexpr size_t kMaxSessionsLimit = 65536;

void SipHash128(const uint8_t key[16], const void *data, size_t size, uint8_t out[16]);

class Server {
public:
    explicit Server(const Options &options);
    ~Server();
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    bool Start();
    uint16_t Port() const;
    void Poll(int timeoutMs);
    size_t SessionCount() const;
    size_t RelayCount() const;

private:
    struct State;
    std::unique_ptr<State> state;
};

}

#endif
