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
    uint32_t sessionTtlMs = 35000;
    uint32_t relayIdleMs = 30000;
    uint32_t relayBytesPerSecond = 262144;
    bool verbose = false;
    void (*log)(const char *format, ...) = nullptr;
};

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

} // namespace lobby

#endif
