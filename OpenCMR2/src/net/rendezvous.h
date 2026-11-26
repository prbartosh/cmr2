#ifndef OPENCMR2_NET_RENDEZVOUS_H
#define OPENCMR2_NET_RENDEZVOUS_H

#include "net/lobby_wire.h"
#include "port/net.h"

#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <functional>
#include <random>
#include <vector>

namespace net {

enum class RelayMode { Auto, Always, Never };

bool IsPrivateAddress(uint32_t address);
bool IsLoopbackAddress(uint32_t address);
bool ReplacesPath(bool currentRelayed, bool currentFresh, bool incomingRelayed);

class Rendezvous {
public:
    using Sender = std::function<void(const lobby::Endpoint &to, const std::vector<uint8_t> &data)>;

    Rendezvous(Sender sender, RelayMode relayMode, std::mt19937_64 &rng);

    void SetLobby(const lobby::Endpoint &lobby, const lobby::Endpoint &local);
    bool NeedsLobby() const;

    void Host(const lobby::Guid &instance);
    void StopHosting(bool notifyLobby);
    NetCodeStatus HostStatus(lobby::Code *code) const;

    void Find(const lobby::Code &code);
    void CancelFind();
    NetCodeStatus FindStatus(lobby::Guid *instance, bool *relayed) const;
    bool Finding() const;

    void Tick(uint32_t now);
    bool Handle(const uint8_t *data, size_t size, const lobby::Endpoint &from, uint32_t now);
    void OnSessionReply(const lobby::Endpoint &from, const lobby::Guid &instance);
    std::vector<lobby::Endpoint> QueryTargets() const;
    bool IsRelay(const lobby::Endpoint &endpoint) const;

private:
    enum class RelayPhase { None, Requested, Assigned, Failed };

    struct PunchJob {
        lobby::Endpoint to;
        uint32_t nonce = 0;
        int left = 0;
        uint32_t sentAt = 0;
    };
    struct RelayLink {
        lobby::Endpoint endpoint;
        lobby::Key token = {};
        bool bound = false;
        int tries = 0;
        uint32_t sentAt = 0;
    };

    void Send(const lobby::Endpoint &to, const std::vector<uint8_t> &data);
    uint32_t NewNonce();
    void TickHost(uint32_t now);
    void TickFind(uint32_t now);
    bool TickBind(RelayLink &link, uint32_t now);
    bool HandleLobby(lobby::Kind kind, const uint8_t *data, size_t size, uint32_t now);
    bool HandleFailure(const lobby::Failed &message);
    bool HandleBound(const uint8_t *data, size_t size, const lobby::Endpoint &from);
    bool Introduce(const lobby::Introduce &message, uint32_t now);
    bool OfferRelay(const lobby::RelayOffer &message);
    void Resolved(const lobby::Resolved &message, uint32_t now);
    void Reregister();
    void RequestRelay(uint32_t now);
    void AbandonRelay();
    void FinishFind(NetCodeStatus status);
    void AddPunch(const lobby::Endpoint &to);

    Sender sender;
    RelayMode relayMode;
    std::mt19937_64 &rng;
    lobby::Endpoint lobbyEndpoint;
    lobby::Endpoint localEndpoint;

    bool hosting = false;
    lobby::Guid hostInstance = {};
    bool registered = false;
    bool hostUnreachable = false;
    uint32_t registerNonce = 0;
    int registerAttempts = 0;
    bool registerDue = false;
    uint32_t registerSentAt = 0;
    lobby::Code hostCode = {};
    lobby::Key hostSecret = {};
    uint32_t refreshNonce = 0;
    uint32_t refreshSentAt = 0;
    std::deque<uint32_t> introduceTimes;
    std::deque<PunchJob> punches;
    std::deque<RelayLink> hostRelays;

    NetCodeStatus findStatus = NET_CODE_IDLE;
    lobby::Code findCode = {};
    uint32_t resolveNonce = 0;
    int resolveAttempts = 0;
    bool resolveDue = false;
    uint32_t resolveSentAt = 0;
    lobby::Guid findInstance = {};
    std::vector<lobby::Endpoint> direct;
    uint32_t resolvedAt = 0;
    RelayPhase relayPhase = RelayPhase::None;
    uint32_t relayNonce = 0;
    int relayAttempts = 0;
    uint32_t relaySentAt = 0;
    RelayLink guestRelay;
    lobby::Endpoint foundAt;
    bool foundRelayed = false;
};

} // namespace net

#endif
