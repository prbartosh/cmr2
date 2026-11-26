#ifndef OPENCMR2_NET_LOBBY_WIRE_H
#define OPENCMR2_NET_LOBBY_WIRE_H

#include "net/net_wire.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <array>
#include <vector>

namespace net {
namespace lobby {

constexpr uint8_t kMagic[4] = { 'C', 'M', 'R', '2' };
constexpr uint8_t kVersion = 1;
constexpr uint16_t kDefaultPort = 28630;
constexpr size_t kHeaderSize = 6;
constexpr size_t kMinRequest = 64;
constexpr size_t kMaxDatagram = 4096;
constexpr size_t kCodeLength = 8;
constexpr size_t kKeyLength = 16;

enum Kind : uint8_t {
    KIND_REGISTER = 0x10,
    KIND_REGISTERED,
    KIND_REFRESH,
    KIND_REFRESHED,
    KIND_UNREGISTER,
    KIND_RESOLVE,
    KIND_RESOLVED,
    KIND_INTRODUCE,
    KIND_RELAY,
    KIND_RELAYED,
    KIND_RELAY_OFFER,
    KIND_BIND,
    KIND_BOUND,
    KIND_PUNCH,
    KIND_FAILURE,
    KIND_CHALLENGE,
};

enum Failure : uint8_t {
    FAILURE_NOT_FOUND = 1,
    FAILURE_RATE_LIMITED,
    FAILURE_FULL,
    FAILURE_UNKNOWN_SESSION,
    FAILURE_VERSION,
};

using Code = std::array<char, kCodeLength>;
using Key = std::array<uint8_t, kKeyLength>;
using Guid = std::array<uint8_t, 16>;

struct Endpoint {
    uint32_t address = 0;
    uint16_t port = 0;

    bool Valid() const { return address != 0 && port != 0; }
    bool operator==(const Endpoint &o) const { return address == o.address && port == o.port; }
    bool operator!=(const Endpoint &o) const { return !(*this == o); }
    bool operator<(const Endpoint &o) const { return address != o.address ? address < o.address : port < o.port; }
};

inline void WriteEndpoint(Writer &w, const Endpoint &e)
{
    w.U32(e.address);
    w.U16(e.port);
}

inline Endpoint ReadEndpoint(Reader &r)
{
    Endpoint e;
    e.address = r.U32();
    e.port = r.U16();
    return e;
}

template <typename T, size_t N>
inline void WriteArray(Writer &w, const std::array<T, N> &a)
{
    w.Bytes(a.data(), sizeof(T) * N);
}

template <typename T, size_t N>
inline void ReadArray(Reader &r, std::array<T, N> &a)
{
    r.Bytes(a.data(), sizeof(T) * N);
}

struct Register {
    static constexpr Kind kKind = KIND_REGISTER;
    static constexpr bool kRequest = true;
    uint32_t nonce = 0;
    Key cookie = {};
    Guid instance = {};
    Endpoint local;

    void Write(Writer &w) const
    {
        w.U32(nonce);
        WriteArray(w, cookie);
        WriteArray(w, instance);
        WriteEndpoint(w, local);
    }
    void Read(Reader &r)
    {
        nonce = r.U32();
        ReadArray(r, cookie);
        ReadArray(r, instance);
        local = ReadEndpoint(r);
    }
};

struct Registered {
    static constexpr Kind kKind = KIND_REGISTERED;
    static constexpr bool kRequest = false;
    uint32_t nonce = 0;
    Code code = {};
    Key secret = {};

    void Write(Writer &w) const
    {
        w.U32(nonce);
        WriteArray(w, code);
        WriteArray(w, secret);
    }
    void Read(Reader &r)
    {
        nonce = r.U32();
        ReadArray(r, code);
        ReadArray(r, secret);
    }
};

struct Refresh {
    static constexpr Kind kKind = KIND_REFRESH;
    static constexpr bool kRequest = true;
    uint32_t nonce = 0;
    Key cookie = {};
    Code code = {};
    Key secret = {};
    Endpoint local;

    void Write(Writer &w) const
    {
        w.U32(nonce);
        WriteArray(w, cookie);
        WriteArray(w, code);
        WriteArray(w, secret);
        WriteEndpoint(w, local);
    }
    void Read(Reader &r)
    {
        nonce = r.U32();
        ReadArray(r, cookie);
        ReadArray(r, code);
        ReadArray(r, secret);
        local = ReadEndpoint(r);
    }
};

struct Refreshed {
    static constexpr Kind kKind = KIND_REFRESHED;
    static constexpr bool kRequest = false;
    uint32_t nonce = 0;

    void Write(Writer &w) const { w.U32(nonce); }
    void Read(Reader &r) { nonce = r.U32(); }
};

struct Unregister {
    static constexpr Kind kKind = KIND_UNREGISTER;
    static constexpr bool kRequest = true;
    Code code = {};
    Key secret = {};

    void Write(Writer &w) const
    {
        WriteArray(w, code);
        WriteArray(w, secret);
    }
    void Read(Reader &r)
    {
        ReadArray(r, code);
        ReadArray(r, secret);
    }
};

struct Resolve {
    static constexpr Kind kKind = KIND_RESOLVE;
    static constexpr bool kRequest = true;
    uint32_t nonce = 0;
    Key cookie = {};
    Code code = {};
    Endpoint local;

    void Write(Writer &w) const
    {
        w.U32(nonce);
        WriteArray(w, cookie);
        WriteArray(w, code);
        WriteEndpoint(w, local);
    }
    void Read(Reader &r)
    {
        nonce = r.U32();
        ReadArray(r, cookie);
        ReadArray(r, code);
        local = ReadEndpoint(r);
    }
};

struct Resolved {
    static constexpr Kind kKind = KIND_RESOLVED;
    static constexpr bool kRequest = false;
    uint32_t nonce = 0;
    Guid instance = {};
    Endpoint hostPublic;
    Endpoint hostLocal;

    void Write(Writer &w) const
    {
        w.U32(nonce);
        WriteArray(w, instance);
        WriteEndpoint(w, hostPublic);
        WriteEndpoint(w, hostLocal);
    }
    void Read(Reader &r)
    {
        nonce = r.U32();
        ReadArray(r, instance);
        hostPublic = ReadEndpoint(r);
        hostLocal = ReadEndpoint(r);
    }
};

struct Introduce {
    static constexpr Kind kKind = KIND_INTRODUCE;
    static constexpr bool kRequest = false;
    Key secret = {};
    Endpoint guestPublic;
    Endpoint guestLocal;

    void Write(Writer &w) const
    {
        WriteArray(w, secret);
        WriteEndpoint(w, guestPublic);
        WriteEndpoint(w, guestLocal);
    }
    void Read(Reader &r)
    {
        ReadArray(r, secret);
        guestPublic = ReadEndpoint(r);
        guestLocal = ReadEndpoint(r);
    }
};

struct Relay {
    static constexpr Kind kKind = KIND_RELAY;
    static constexpr bool kRequest = true;
    uint32_t nonce = 0;
    Key cookie = {};
    Code code = {};

    void Write(Writer &w) const
    {
        w.U32(nonce);
        WriteArray(w, cookie);
        WriteArray(w, code);
    }
    void Read(Reader &r)
    {
        nonce = r.U32();
        ReadArray(r, cookie);
        ReadArray(r, code);
    }
};

struct Relayed {
    static constexpr Kind kKind = KIND_RELAYED;
    static constexpr bool kRequest = false;
    uint32_t nonce = 0;
    uint16_t port = 0;
    Key token = {};

    void Write(Writer &w) const
    {
        w.U32(nonce);
        w.U16(port);
        WriteArray(w, token);
    }
    void Read(Reader &r)
    {
        nonce = r.U32();
        port = r.U16();
        ReadArray(r, token);
    }
};

struct RelayOffer {
    static constexpr Kind kKind = KIND_RELAY_OFFER;
    static constexpr bool kRequest = false;
    Key secret = {};
    uint16_t port = 0;
    Key token = {};

    void Write(Writer &w) const
    {
        WriteArray(w, secret);
        w.U16(port);
        WriteArray(w, token);
    }
    void Read(Reader &r)
    {
        ReadArray(r, secret);
        port = r.U16();
        ReadArray(r, token);
    }
};

struct Bind {
    static constexpr Kind kKind = KIND_BIND;
    static constexpr bool kRequest = true;
    Key token = {};

    void Write(Writer &w) const { WriteArray(w, token); }
    void Read(Reader &r) { ReadArray(r, token); }
};

struct Bound {
    static constexpr Kind kKind = KIND_BOUND;
    static constexpr bool kRequest = false;
    Key token = {};

    void Write(Writer &w) const { WriteArray(w, token); }
    void Read(Reader &r) { ReadArray(r, token); }
};

struct Punch {
    static constexpr Kind kKind = KIND_PUNCH;
    static constexpr bool kRequest = false;
    uint32_t nonce = 0;

    void Write(Writer &w) const { w.U32(nonce); }
    void Read(Reader &r) { nonce = r.U32(); }
};

struct Failed {
    static constexpr Kind kKind = KIND_FAILURE;
    static constexpr bool kRequest = false;
    uint32_t nonce = 0;
    Failure reason = FAILURE_NOT_FOUND;

    void Write(Writer &w) const
    {
        w.U32(nonce);
        w.U8(reason);
    }
    void Read(Reader &r)
    {
        nonce = r.U32();
        reason = (Failure)r.U8();
    }
};

struct Challenge {
    static constexpr Kind kKind = KIND_CHALLENGE;
    static constexpr bool kRequest = false;
    uint32_t nonce = 0;
    Key cookie = {};

    void Write(Writer &w) const
    {
        w.U32(nonce);
        WriteArray(w, cookie);
    }
    void Read(Reader &r)
    {
        nonce = r.U32();
        ReadArray(r, cookie);
    }
};

inline bool Peek(const uint8_t *data, size_t size, Kind &kind)
{
    if (data == nullptr || size < kHeaderSize || size > kMaxDatagram || memcmp(data, kMagic, sizeof(kMagic)) != 0)
        return false;
    if (data[4] < KIND_REGISTER || data[4] > KIND_CHALLENGE)
        return false;
    kind = (Kind)data[4];
    return true;
}

template <typename Message>
inline std::vector<uint8_t> Encode(const Message &message)
{
    Writer w;
    w.Bytes(kMagic, sizeof(kMagic));
    w.U8(Message::kKind);
    w.U8(kVersion);
    message.Write(w);
    if (Message::kRequest && w.data.size() < kMinRequest)
        w.data.resize(kMinRequest, 0);
    return w.data;
}

template <typename Message>
inline bool Decode(const uint8_t *data, size_t size, Message &message)
{
    Kind kind;
    if (!Peek(data, size, kind) || kind != Message::kKind || data[5] != kVersion)
        return false;
    if (Message::kRequest && size < kMinRequest)
        return false;
    Reader r(data + kHeaderSize, size - kHeaderSize);
    message.Read(r);
    return r.ok();
}

}
}

#endif
