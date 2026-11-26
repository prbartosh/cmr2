#include "net/rendezvous.h"
#include "net/join_code.h"

#include <string.h>

#include <algorithm>
#include <utility>

namespace net {

namespace {

const uint32_t kRetryMs = 1000;
const uint32_t kSlowRetryMs = 5000;
const int kMaxAttempts = 5;
const uint32_t kRefreshMs = 10000;
const uint32_t kPunchIntervalMs = 200;
const int kPunchCount = 15;
const size_t kMaxPunchJobs = 8;
const size_t kMaxIntroducesPerWindow = 4;
const uint32_t kIntroduceWindowMs = 1000;
const uint32_t kDirectWindowMs = 3000;
const uint32_t kFindTimeoutMs = 15000;
const uint32_t kBindIntervalMs = 500;
const int kMaxBindTries = 10;
const size_t kMaxHostRelays = 8;

bool Elapsed(uint32_t now, uint32_t since, uint32_t ms)
{
    return (uint32_t)(now - since) >= ms;
}

bool UsableLocal(const lobby::Endpoint &candidate, const lobby::Endpoint &observed)
{
    return candidate.port != 0 && IsPrivateAddress(candidate.address) && candidate != observed;
}

}

bool IsPrivateAddress(uint32_t address)
{
    uint8_t b[4];
    memcpy(b, &address, sizeof(b));
    return b[0] == 10 || (b[0] == 172 && (b[1] & 0xF0) == 16) || (b[0] == 192 && b[1] == 168);
}

bool IsLoopbackAddress(uint32_t address)
{
    uint8_t b[4];
    memcpy(b, &address, sizeof(b));
    return b[0] == 127;
}

bool ReplacesPath(bool currentRelayed, bool currentFresh, bool incomingRelayed)
{
    return !incomingRelayed || currentRelayed || !currentFresh;
}

Rendezvous::Rendezvous(Sender sender, RelayMode relayMode, std::mt19937_64 &rng)
    : sender(std::move(sender)), relayMode(relayMode), rng(rng)
{
}

void Rendezvous::Send(const lobby::Endpoint &to, const std::vector<uint8_t> &data)
{
    if (to.Valid() && sender)
        sender(to, data);
}

uint32_t Rendezvous::NewNonce()
{
    return (uint32_t)rng() | 1;
}

void Rendezvous::SetLobby(const lobby::Endpoint &lobby, const lobby::Endpoint &local)
{
    bool changed = lobby != lobbyEndpoint;
    lobbyEndpoint = lobby;
    localEndpoint = local;
    if (!changed || !lobby.Valid())
        return;
    if (hosting && !registered) {
        registerAttempts = 0;
        hostUnreachable = false;
        registerDue = true;
    }
    if (findStatus == NET_CODE_CONTACTING) {
        resolveAttempts = 0;
        resolveDue = true;
    }
}

bool Rendezvous::NeedsLobby() const
{
    return hosting || findStatus == NET_CODE_CONTACTING || Finding();
}

void Rendezvous::Host(const lobby::Guid &instance)
{
    if (hosting)
        StopHosting(true);
    hosting = true;
    hostInstance = instance;
    registered = false;
    hostUnreachable = false;
    registerNonce = NewNonce();
    registerAttempts = 0;
    registerDue = true;
}

void Rendezvous::StopHosting(bool notifyLobby)
{
    if (hosting && registered && notifyLobby) {
        lobby::Unregister message;
        message.code = hostCode;
        message.secret = hostSecret;
        Send(lobbyEndpoint, lobby::Encode(message));
    }
    hosting = false;
    registered = false;
    hostUnreachable = false;
    registerNonce = 0;
    registerAttempts = 0;
    registerDue = false;
    refreshNonce = 0;
    hostInstance = {};
    hostCode = {};
    hostSecret = {};
    introduceTimes.clear();
    punches.clear();
    hostRelays.clear();
}

NetCodeStatus Rendezvous::HostStatus(lobby::Code *code) const
{
    if (!hosting)
        return NET_CODE_IDLE;
    if (registered) {
        if (code != nullptr)
            *code = hostCode;
        return NET_CODE_READY;
    }
    return hostUnreachable ? NET_CODE_LOBBY_UNREACHABLE : NET_CODE_CONTACTING;
}

void Rendezvous::Find(const lobby::Code &code)
{
    CancelFind();
    findStatus = NET_CODE_CONTACTING;
    findCode = code;
    resolveNonce = NewNonce();
    resolveAttempts = 0;
    resolveDue = true;
}

void Rendezvous::CancelFind()
{
    findStatus = NET_CODE_IDLE;
    findCode = {};
    resolveNonce = 0;
    resolveAttempts = 0;
    resolveDue = false;
    findInstance = {};
    direct.clear();
    relayPhase = RelayPhase::None;
    relayNonce = 0;
    relayAttempts = 0;
    guestRelay = RelayLink();
    foundAt = lobby::Endpoint();
    foundRelayed = false;
}

NetCodeStatus Rendezvous::FindStatus(lobby::Guid *instance, bool *relayed) const
{
    if (findStatus == NET_CODE_FOUND) {
        if (instance != nullptr)
            *instance = findInstance;
        if (relayed != nullptr)
            *relayed = foundRelayed;
    }
    return findStatus;
}

bool Rendezvous::Finding() const
{
    return findStatus == NET_CODE_CONNECTING || findStatus == NET_CODE_RELAYING;
}

void Rendezvous::FinishFind(NetCodeStatus status)
{
    findStatus = status;
    resolveNonce = 0;
    relayNonce = 0;
}

void Rendezvous::Tick(uint32_t now)
{
    TickHost(now);
    TickFind(now);
}

void Rendezvous::TickHost(uint32_t now)
{
    if (!hosting)
        return;
    if (!registered) {
        uint32_t interval = hostUnreachable ? kSlowRetryMs : kRetryMs;
        if (registerDue || Elapsed(now, registerSentAt, interval)) {
            if (registerAttempts >= kMaxAttempts)
                hostUnreachable = true;
            lobby::Register message;
            message.nonce = registerNonce;
            message.instance = hostInstance;
            message.local = localEndpoint;
            Send(lobbyEndpoint, lobby::Encode(message));
            registerAttempts++;
            registerSentAt = now;
            registerDue = false;
        }
    } else if (Elapsed(now, refreshSentAt, kRefreshMs)) {
        lobby::Refresh message;
        refreshNonce = NewNonce();
        message.nonce = refreshNonce;
        message.code = hostCode;
        message.secret = hostSecret;
        message.local = localEndpoint;
        Send(lobbyEndpoint, lobby::Encode(message));
        refreshSentAt = now;
    }

    for (auto it = punches.begin(); it != punches.end();) {
        if (it->left == kPunchCount || Elapsed(now, it->sentAt, kPunchIntervalMs)) {
            lobby::Punch message;
            message.nonce = it->nonce;
            Send(it->to, lobby::Encode(message));
            it->left--;
            it->sentAt = now;
        }
        if (it->left <= 0)
            it = punches.erase(it);
        else
            ++it;
    }
    for (auto it = hostRelays.begin(); it != hostRelays.end();) {
        if (TickBind(*it, now))
            ++it;
        else
            it = hostRelays.erase(it);
    }
}

bool Rendezvous::TickBind(RelayLink &link, uint32_t now)
{
    if (link.bound)
        return true;
    if (link.tries > 0 && !Elapsed(now, link.sentAt, kBindIntervalMs))
        return true;
    if (link.tries >= kMaxBindTries)
        return false;
    lobby::Bind message;
    message.token = link.token;
    Send(link.endpoint, lobby::Encode(message));
    link.tries++;
    link.sentAt = now;
    return true;
}

void Rendezvous::TickFind(uint32_t now)
{
    if (findStatus == NET_CODE_CONTACTING) {
        if (resolveDue || Elapsed(now, resolveSentAt, kRetryMs)) {
            if (resolveAttempts >= kMaxAttempts) {
                FinishFind(NET_CODE_LOBBY_UNREACHABLE);
                return;
            }
            lobby::Resolve message;
            message.nonce = resolveNonce;
            message.code = findCode;
            message.local = localEndpoint;
            Send(lobbyEndpoint, lobby::Encode(message));
            resolveAttempts++;
            resolveSentAt = now;
            resolveDue = false;
        }
        return;
    }
    if (!Finding())
        return;
    uint32_t limit = relayMode == RelayMode::Never ? kDirectWindowMs : kFindTimeoutMs;
    if (Elapsed(now, resolvedAt, limit)) {
        FinishFind(NET_CODE_HOST_UNREACHABLE);
        return;
    }
    if (relayMode == RelayMode::Auto && relayPhase == RelayPhase::None && Elapsed(now, resolvedAt, kDirectWindowMs))
        RequestRelay(now);
    if (relayPhase == RelayPhase::Requested && Elapsed(now, relaySentAt, kRetryMs)) {
        if (relayAttempts >= kMaxAttempts) {
            AbandonRelay();
        } else {
            lobby::Relay message;
            message.nonce = relayNonce;
            message.code = findCode;
            Send(lobbyEndpoint, lobby::Encode(message));
            relayAttempts++;
            relaySentAt = now;
        }
    } else if (relayPhase == RelayPhase::Assigned && !TickBind(guestRelay, now)) {
        AbandonRelay();
    }
}

void Rendezvous::RequestRelay(uint32_t now)
{
    relayPhase = RelayPhase::Requested;
    relayNonce = NewNonce();
    findStatus = NET_CODE_RELAYING;
    lobby::Relay message;
    message.nonce = relayNonce;
    message.code = findCode;
    Send(lobbyEndpoint, lobby::Encode(message));
    relayAttempts = 1;
    relaySentAt = now;
}

void Rendezvous::AbandonRelay()
{
    relayPhase = RelayPhase::Failed;
    relayNonce = 0;
    if (relayMode == RelayMode::Always)
        FinishFind(NET_CODE_HOST_UNREACHABLE);
    else
        findStatus = NET_CODE_CONNECTING;
}

bool Rendezvous::Handle(const uint8_t *data, size_t size, const lobby::Endpoint &from, uint32_t now)
{
    lobby::Kind kind;
    if (!lobby::Peek(data, size, kind))
        return false;
    if (kind == lobby::KIND_BOUND)
        return HandleBound(data, size, from);
    if (!lobbyEndpoint.Valid() || from != lobbyEndpoint)
        return false;
    return HandleLobby(kind, data, size, now);
}

bool Rendezvous::HandleLobby(lobby::Kind kind, const uint8_t *data, size_t size, uint32_t now)
{
    switch (kind) {
    case lobby::KIND_REGISTERED: {
        lobby::Registered m;
        if (!lobby::Decode(data, size, m) || !hosting || registered || registerNonce == 0 ||
            m.nonce != registerNonce || !joincode::IsCanonical(m.code))
            return false;
        registered = true;
        hostUnreachable = false;
        hostCode = m.code;
        hostSecret = m.secret;
        registerNonce = 0;
        refreshSentAt = now;
        return true;
    }
    case lobby::KIND_REFRESHED: {
        lobby::Refreshed m;
        if (!lobby::Decode(data, size, m) || !registered || refreshNonce == 0 || m.nonce != refreshNonce)
            return false;
        refreshNonce = 0;
        return true;
    }
    case lobby::KIND_FAILURE: {
        lobby::Failed m;
        return lobby::Decode(data, size, m) && m.nonce != 0 && HandleFailure(m);
    }
    case lobby::KIND_RESOLVED: {
        lobby::Resolved m;
        if (!lobby::Decode(data, size, m) || findStatus != NET_CODE_CONTACTING || resolveNonce == 0 ||
            m.nonce != resolveNonce)
            return false;
        Resolved(m, now);
        return true;
    }
    case lobby::KIND_INTRODUCE: {
        lobby::Introduce m;
        if (!lobby::Decode(data, size, m) || !registered || m.secret != hostSecret)
            return false;
        return Introduce(m, now);
    }
    case lobby::KIND_RELAYED: {
        lobby::Relayed m;
        if (!lobby::Decode(data, size, m) || relayPhase != RelayPhase::Requested || relayNonce == 0 ||
            m.nonce != relayNonce || !Finding())
            return false;
        if (m.port == 0) {
            AbandonRelay();
            return true;
        }
        relayPhase = RelayPhase::Assigned;
        relayNonce = 0;
        guestRelay = RelayLink();
        guestRelay.endpoint.address = lobbyEndpoint.address;
        guestRelay.endpoint.port = m.port;
        guestRelay.token = m.token;
        return true;
    }
    case lobby::KIND_RELAY_OFFER: {
        lobby::RelayOffer m;
        if (!lobby::Decode(data, size, m) || !registered || m.secret != hostSecret)
            return false;
        return OfferRelay(m);
    }
    default:
        return false;
    }
}

bool Rendezvous::HandleFailure(const lobby::Failed &m)
{
    if (hosting && !registered && m.nonce == registerNonce) {
        hostUnreachable = true;
        return true;
    }
    if (registered && m.nonce == refreshNonce) {
        refreshNonce = 0;
        if (m.reason == lobby::FAILURE_UNKNOWN_SESSION)
            Reregister();
        return true;
    }
    if (findStatus == NET_CODE_CONTACTING && m.nonce == resolveNonce) {
        if (m.reason == lobby::FAILURE_NOT_FOUND)
            FinishFind(NET_CODE_NOT_FOUND);
        else if (m.reason == lobby::FAILURE_RATE_LIMITED)
            FinishFind(NET_CODE_RATE_LIMITED);
        else
            FinishFind(NET_CODE_LOBBY_UNREACHABLE);
        return true;
    }
    if (relayPhase == RelayPhase::Requested && m.nonce == relayNonce && Finding()) {
        AbandonRelay();
        return true;
    }
    return false;
}

void Rendezvous::Reregister()
{
    registered = false;
    hostUnreachable = false;
    hostCode = {};
    hostSecret = {};
    registerNonce = NewNonce();
    registerAttempts = 0;
    registerDue = true;
    introduceTimes.clear();
    punches.clear();
    hostRelays.clear();
}

void Rendezvous::Resolved(const lobby::Resolved &m, uint32_t now)
{
    findStatus = NET_CODE_CONNECTING;
    resolveNonce = 0;
    findInstance = m.instance;
    resolvedAt = now;
    direct.clear();
    if (m.hostPublic.Valid())
        direct.push_back(m.hostPublic);
    if (UsableLocal(m.hostLocal, m.hostPublic))
        direct.push_back(m.hostLocal);
    if (relayMode == RelayMode::Always || (relayMode == RelayMode::Auto && direct.empty()))
        RequestRelay(now);
}

bool Rendezvous::Introduce(const lobby::Introduce &m, uint32_t now)
{
    while (!introduceTimes.empty() && Elapsed(now, introduceTimes.front(), kIntroduceWindowMs))
        introduceTimes.pop_front();
    if (introduceTimes.size() >= kMaxIntroducesPerWindow)
        return false;
    introduceTimes.push_back(now);
    if (m.guestPublic.Valid())
        AddPunch(m.guestPublic);
    if (UsableLocal(m.guestLocal, m.guestPublic))
        AddPunch(m.guestLocal);
    return true;
}

void Rendezvous::AddPunch(const lobby::Endpoint &to)
{
    punches.erase(std::remove_if(punches.begin(), punches.end(), [&](const PunchJob &job) { return job.to == to; }),
                  punches.end());
    while (punches.size() >= kMaxPunchJobs)
        punches.pop_front();
    PunchJob job;
    job.to = to;
    job.nonce = NewNonce();
    job.left = kPunchCount;
    punches.push_back(job);
}

bool Rendezvous::OfferRelay(const lobby::RelayOffer &m)
{
    if (m.port == 0 || !lobbyEndpoint.Valid())
        return false;
    lobby::Endpoint endpoint;
    endpoint.address = lobbyEndpoint.address;
    endpoint.port = m.port;
    hostRelays.erase(std::remove_if(hostRelays.begin(), hostRelays.end(),
                                    [&](const RelayLink &link) { return link.endpoint == endpoint; }),
                     hostRelays.end());
    while (hostRelays.size() >= kMaxHostRelays)
        hostRelays.pop_front();
    RelayLink link;
    link.endpoint = endpoint;
    link.token = m.token;
    hostRelays.push_back(link);
    return true;
}

bool Rendezvous::HandleBound(const uint8_t *data, size_t size, const lobby::Endpoint &from)
{
    lobby::Bound m;
    if (!lobby::Decode(data, size, m))
        return false;
    if (relayPhase == RelayPhase::Assigned && Finding() && !guestRelay.bound && from == guestRelay.endpoint &&
        m.token == guestRelay.token) {
        guestRelay.bound = true;
        return true;
    }
    for (RelayLink &link : hostRelays) {
        if (!link.bound && from == link.endpoint && m.token == link.token) {
            link.bound = true;
            return true;
        }
    }
    return false;
}

void Rendezvous::OnSessionReply(const lobby::Endpoint &from, const lobby::Guid &instance)
{
    if ((!Finding() && findStatus != NET_CODE_FOUND) || instance != findInstance)
        return;
    bool viaDirect = relayMode != RelayMode::Always && std::find(direct.begin(), direct.end(), from) != direct.end();
    bool viaRelay = relayPhase == RelayPhase::Assigned && guestRelay.bound && from == guestRelay.endpoint;
    if (viaDirect && (findStatus != NET_CODE_FOUND || foundRelayed)) {
        FinishFind(NET_CODE_FOUND);
        foundAt = from;
        foundRelayed = false;
    } else if (viaRelay && findStatus != NET_CODE_FOUND) {
        FinishFind(NET_CODE_FOUND);
        foundAt = from;
        foundRelayed = true;
    }
}

std::vector<lobby::Endpoint> Rendezvous::QueryTargets() const
{
    std::vector<lobby::Endpoint> targets;
    if (findStatus == NET_CODE_FOUND) {
        targets.push_back(foundAt);
        return targets;
    }
    if (!Finding())
        return targets;
    if (relayMode != RelayMode::Always)
        targets = direct;
    if (relayPhase == RelayPhase::Assigned && guestRelay.bound)
        targets.push_back(guestRelay.endpoint);
    return targets;
}

bool Rendezvous::IsRelay(const lobby::Endpoint &endpoint) const
{
    if (!endpoint.Valid())
        return false;
    if (guestRelay.endpoint == endpoint)
        return true;
    for (const RelayLink &link : hostRelays)
        if (link.endpoint == endpoint)
            return true;
    return false;
}

}
