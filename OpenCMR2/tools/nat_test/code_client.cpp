#include "net/net_session.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

const NetGuid kApp = { { 0x43, 0x4d, 0x52, 0x32, 0x4e, 0x41, 0x54, 0x54, 1, 2, 3, 4, 5, 6, 7, 8 } };
const size_t kPayload = 13;

Clock::time_point g_start = Clock::now();

double SinceStartMs()
{
    return std::chrono::duration<double, std::milli>(Clock::now() - g_start).count();
}

uint64_t NowUs()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - g_start).count();
}

void Log(const char *format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    fprintf(stderr, "[%9.1f] %s\n", SinceStartMs(), text);
    fflush(stderr);
}

const char *StatusName(NetCodeStatus status)
{
    switch (status) {
    case NET_CODE_IDLE: return "IDLE";
    case NET_CODE_NO_LOBBY: return "NO_LOBBY";
    case NET_CODE_INVALID: return "INVALID";
    case NET_CODE_CONTACTING: return "CONTACTING";
    case NET_CODE_READY: return "READY";
    case NET_CODE_CONNECTING: return "CONNECTING";
    case NET_CODE_RELAYING: return "RELAYING";
    case NET_CODE_FOUND: return "FOUND";
    case NET_CODE_NOT_FOUND: return "NOT_FOUND";
    case NET_CODE_LOBBY_UNREACHABLE: return "LOBBY_UNREACHABLE";
    case NET_CODE_HOST_UNREACHABLE: return "HOST_UNREACHABLE";
    case NET_CODE_RATE_LIMITED: return "RATE_LIMITED";
    }
    return "UNKNOWN";
}

bool Terminal(NetCodeStatus status)
{
    return status == NET_CODE_IDLE || status == NET_CODE_NO_LOBBY || status == NET_CODE_INVALID ||
           status == NET_CODE_NOT_FOUND || status == NET_CODE_LOBBY_UNREACHABLE ||
           status == NET_CODE_HOST_UNREACHABLE || status == NET_CODE_RATE_LIMITED;
}

struct Args {
    std::string mode;
    std::string code;
    std::string lobby;
    std::string codeFile;
    net::RelayMode relay = net::RelayMode::Auto;
    std::string relayName = "auto";
    int timeoutS = 30;
    int serveS = 120;
    int count = 200;
    double minUnguaranteed = 0.9;
    uint16_t port = 28620;
};

void Usage()
{
    fprintf(stderr,
            "usage: code_client host --lobby IP:PORT [--relay auto|always|never] [--code-file PATH] [--timeout S]\n"
            "                        [--serve S] [--port N]\n"
            "       code_client join CODE --lobby IP:PORT [--relay auto|always|never] [--timeout S] [--count N]\n"
            "                        [--min-unguaranteed FRACTION] [--port N]\n");
}

bool Parse(int argc, char **argv, Args &args)
{
    if (argc < 2)
        return false;
    args.mode = argv[1];
    int i = 2;
    if (args.mode == "join") {
        if (argc < 3)
            return false;
        args.code = argv[2];
        i = 3;
    } else if (args.mode != "host") {
        return false;
    }
    for (; i < argc; i++) {
        std::string arg = argv[i];
        if (i + 1 >= argc)
            return false;
        std::string value = argv[++i];
        if (arg == "--lobby") {
            args.lobby = value;
        } else if (arg == "--relay") {
            args.relayName = value;
            if (value == "auto")
                args.relay = net::RelayMode::Auto;
            else if (value == "always")
                args.relay = net::RelayMode::Always;
            else if (value == "never")
                args.relay = net::RelayMode::Never;
            else
                return false;
        } else if (arg == "--code-file") {
            args.codeFile = value;
        } else if (arg == "--timeout") {
            args.timeoutS = atoi(value.c_str());
        } else if (arg == "--serve") {
            args.serveS = atoi(value.c_str());
        } else if (arg == "--count") {
            args.count = atoi(value.c_str());
        } else if (arg == "--min-unguaranteed") {
            args.minUnguaranteed = atof(value.c_str());
        } else if (arg == "--port") {
            args.port = (uint16_t)atoi(value.c_str());
        } else {
            return false;
        }
    }
    return !args.lobby.empty() && args.timeoutS > 0 && args.serveS > 0 && args.count > 0;
}

net::Config MakeConfig(const Args &args)
{
    net::Config config;
    config.port = args.port;
    config.broadcast = false;
    config.lobby = args.lobby;
    config.relay = args.relay;
    config.log = Log;
    return config;
}

std::vector<uint8_t> Payload(char kind, uint32_t seq)
{
    std::vector<uint8_t> data(kPayload);
    uint64_t sent = NowUs();
    data[0] = (uint8_t)kind;
    memcpy(&data[1], &seq, sizeof(seq));
    memcpy(&data[5], &sent, sizeof(sent));
    return data;
}

struct Message {
    NetPlayerID from = 0;
    std::vector<uint8_t> data;
};

bool ReceiveOne(net::Session &session, std::vector<uint8_t> &buffer, Message &out)
{
    for (;;) {
        NetPlayerID from = 0, to = 0;
        DWORD size = (DWORD)buffer.size();
        HRESULT hr = session.Receive(&from, &to, buffer.data(), &size);
        if (hr == NET_ERR_BUFFERTOOSMALL) {
            buffer.resize(size);
            continue;
        }
        if (hr != NET_OK)
            return false;
        if (from == NET_ID_SYSTEM) {
            DWORD type = 0;
            if (size >= sizeof(type))
                memcpy(&type, buffer.data(), sizeof(type));
            Log("code_client: system message 0x%04x", (unsigned)type);
            continue;
        }
        out.from = from;
        out.data.assign(buffer.begin(), buffer.begin() + size);
        return true;
    }
}

void WriteCodeFile(const std::string &path, const std::string &code)
{
    if (path.empty())
        return;
    std::string temp = path + ".tmp";
    FILE *f = fopen(temp.c_str(), "w");
    if (f == nullptr)
        return;
    fprintf(f, "%s\n", code.c_str());
    fclose(f);
    rename(temp.c_str(), path.c_str());
}

int RunHost(const Args &args)
{
    net::Session session(MakeConfig(args));
    if (session.Start() != NET_OK) {
        printf("{\"mode\":\"host\",\"error\":\"start\"}\n");
        return 1;
    }
    NetSessionDesc desc = {};
    desc.dwSize = sizeof(desc);
    desc.guidApplication = kApp;
    desc.dwMaxPlayers = 4;
    desc.lpszSessionNameA = (char *)"nat-test";
    desc.lpszPasswordA = (char *)"";
    HRESULT hr = session.Open(&desc, NET_OPEN_CREATE);
    if (hr != NET_OK) {
        printf("{\"mode\":\"host\",\"error\":\"open 0x%08x\"}\n", (unsigned)hr);
        return 1;
    }
    NetName name = { sizeof(NetName), 0, (char *)"host", (char *)"host" };
    NetPlayerID self = 0;
    hr = session.CreatePlayer(&self, &name, nullptr, 0);
    if (hr != NET_OK) {
        printf("{\"mode\":\"host\",\"error\":\"createplayer 0x%08x\"}\n", (unsigned)hr);
        return 1;
    }
    Clock::time_point deadline = Clock::now() + std::chrono::seconds(args.timeoutS);
    std::string code;
    NetCodeStatus status = NET_CODE_IDLE;
    for (;;) {
        status = session.HostCode(&code);
        if (status == NET_CODE_READY)
            break;
        if (Clock::now() >= deadline) {
            printf("{\"mode\":\"host\",\"status\":\"%s\",\"error\":\"no code\"}\n", StatusName(status));
            return 3;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    Log("code_client: host code %s after %.0f ms", code.c_str(), SinceStartMs());
    printf("CODE %s\n", code.c_str());
    fflush(stdout);
    WriteCodeFile(args.codeFile, code);

    Clock::time_point serveUntil = Clock::now() + std::chrono::seconds(args.serveS);
    std::vector<uint8_t> buffer(65536);
    unsigned echoedG = 0, echoedU = 0, other = 0;
    bool bye = false;
    while (!bye && Clock::now() < serveUntil) {
        Message m;
        if (!ReceiveOne(session, buffer, m)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (m.data.size() != kPayload) {
            other++;
            continue;
        }
        char kind = (char)m.data[0];
        bool guaranteed = kind != 'U';
        hr = session.Send(self, m.from, guaranteed ? TRUE : FALSE, m.data.data(), (DWORD)m.data.size());
        if (hr != NET_OK)
            Log("code_client: echo failed 0x%08x", (unsigned)hr);
        if (kind == 'U')
            echoedU++;
        else
            echoedG++;
        if (kind == 'B')
            bye = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    session.Close();
    printf("{\"mode\":\"host\",\"code\":\"%s\",\"echoed_guaranteed\":%u,\"echoed_unguaranteed\":%u,\"other\":%u,"
           "\"bye\":%s}\n",
           code.c_str(), echoedG, echoedU, other, bye ? "true" : "false");
    fflush(stdout);
    return bye ? 0 : 6;
}

struct JoinResult {
    std::string status = "IDLE";
    std::string error;
    bool found = false;
    bool relayed = false;
    bool joined = false;
    double findMs = -1;
    double rttMs = -1;
    int guaranteedSent = 0, guaranteedOk = 0;
    int unguaranteedSent = 0, unguaranteedOk = 0;
    bool byeEchoed = false;
};

void PrintJoin(const JoinResult &r)
{
    printf("{\"mode\":\"join\",\"status\":\"%s\",\"found\":%s,\"relayed\":%s,\"joined\":%s,\"find_ms\":%.0f,"
           "\"rtt_ms\":%.2f,\"guaranteed_sent\":%d,\"guaranteed_ok\":%d,\"unguaranteed_sent\":%d,"
           "\"unguaranteed_ok\":%d,\"bye_echoed\":%s,\"error\":\"%s\"}\n",
           r.status.c_str(), r.found ? "true" : "false", r.relayed ? "true" : "false", r.joined ? "true" : "false",
           r.findMs, r.rttMs, r.guaranteedSent, r.guaranteedOk, r.unguaranteedSent, r.unguaranteedOk,
           r.byeEchoed ? "true" : "false", r.error.c_str());
    fflush(stdout);
}

bool Listed(net::Session &session, const NetGuid *instance)
{
    struct Context {
        const NetGuid *wanted;
        bool seen;
    } context = { instance, false };
    session.EnumSessions(&kApp,
                         [](const NetSessionDesc *d, void *p) -> BOOL {
                             Context *c = (Context *)p;
                             if (c->wanted != nullptr &&
                                 memcmp(&d->guidInstance, c->wanted, sizeof(NetGuid)) == 0)
                                 c->seen = true;
                             return TRUE;
                         },
                         &context);
    return context.seen;
}

int RunJoin(const Args &args)
{
    JoinResult result;
    net::Session session(MakeConfig(args));
    if (session.Start() != NET_OK) {
        result.error = "start";
        PrintJoin(result);
        return 1;
    }
    Clock::time_point began = Clock::now();
    Clock::time_point deadline = began + std::chrono::seconds(args.timeoutS);
    NetCodeStatus status = session.FindByCode(args.code.c_str());
    result.status = StatusName(status);
    if (status != NET_CODE_CONTACTING) {
        result.error = "findbycode";
        PrintJoin(result);
        return 3;
    }
    NetGuid instance = {};
    bool relayed = false;
    NetCodeStatus last = status;
    for (;;) {
        Listed(session, nullptr);
        status = session.CodeSession(&instance, &relayed);
        if (status != last) {
            Log("code_client: code status %s", StatusName(status));
            last = status;
        }
        result.status = StatusName(status);
        if (status == NET_CODE_FOUND)
            break;
        if (Terminal(status)) {
            PrintJoin(result);
            return 3;
        }
        if (Clock::now() >= deadline) {
            result.error = "timeout";
            PrintJoin(result);
            return 3;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    result.found = true;
    result.relayed = relayed;
    result.findMs = std::chrono::duration<double, std::milli>(Clock::now() - began).count();
    Log("code_client: found relayed=%d after %.0f ms", relayed ? 1 : 0, result.findMs);

    while (!Listed(session, &instance)) {
        if (Clock::now() >= deadline) {
            result.error = "not listed";
            PrintJoin(result);
            return 4;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    NetSessionDesc join = {};
    join.dwSize = sizeof(join);
    join.guidInstance = instance;
    join.lpszPasswordA = (char *)"";
    HRESULT hr = session.Open(&join, NET_OPEN_JOIN);
    if (hr != NET_OK) {
        char text[32];
        snprintf(text, sizeof(text), "join 0x%08x", (unsigned)hr);
        result.error = text;
        PrintJoin(result);
        return 4;
    }
    result.joined = true;
    NetName name = { sizeof(NetName), 0, (char *)"guest", (char *)"guest" };
    NetPlayerID self = 0;
    hr = session.CreatePlayer(&self, &name, nullptr, 0);
    if (hr != NET_OK) {
        result.error = "createplayer";
        PrintJoin(result);
        return 4;
    }
    NetPlayerID peer = 0;
    Clock::time_point playerDeadline = Clock::now() + std::chrono::seconds(10);
    while (peer == 0 && Clock::now() < playerDeadline) {
        struct Context {
            NetPlayerID self, peer;
        } context = { self, 0 };
        session.EnumPlayers(
            [](NetPlayerID id, const NetName *, void *p) -> BOOL {
                Context *c = (Context *)p;
                if (id != c->self)
                    c->peer = id;
                return TRUE;
            },
            &context);
        peer = context.peer;
        if (peer == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (peer == 0) {
        result.error = "no host player";
        PrintJoin(result);
        return 4;
    }

    std::vector<uint8_t> buffer(65536);
    std::vector<bool> gotG(args.count, false), gotU(args.count, false);
    std::vector<double> rtts;
    bool byeBack = false;
    int pingWanted = -1;
    bool pingBack = false;
    auto pump = [&]() {
        Message m;
        while (ReceiveOne(session, buffer, m)) {
            if (m.data.size() != kPayload || m.from != peer)
                continue;
            uint32_t seq = 0;
            uint64_t sent = 0;
            memcpy(&seq, &m.data[1], sizeof(seq));
            memcpy(&sent, &m.data[5], sizeof(sent));
            char kind = (char)m.data[0];
            if (kind == 'P' && (int)seq == pingWanted) {
                rtts.push_back((double)(NowUs() - sent) / 1000.0);
                pingBack = true;
            } else if (kind == 'G' && seq < gotG.size()) {
                gotG[seq] = true;
            } else if (kind == 'U' && seq < gotU.size()) {
                gotU[seq] = true;
            } else if (kind == 'B') {
                byeBack = true;
            }
        }
    };

    for (int i = 0; i < 20; i++) {
        pingWanted = i;
        pingBack = false;
        std::vector<uint8_t> data = Payload('P', (uint32_t)i);
        session.Send(self, peer, TRUE, data.data(), (DWORD)data.size());
        Clock::time_point pingDeadline = Clock::now() + std::chrono::seconds(3);
        while (!pingBack && Clock::now() < pingDeadline) {
            pump();
            if (!pingBack)
                std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    if (!rtts.empty()) {
        std::sort(rtts.begin(), rtts.end());
        result.rttMs = rtts[rtts.size() / 2];
    }

    for (int i = 0; i < args.count; i++) {
        std::vector<uint8_t> data = Payload('G', (uint32_t)i);
        if (session.Send(self, peer, TRUE, data.data(), (DWORD)data.size()) == NET_OK)
            result.guaranteedSent++;
    }
    for (int i = 0; i < args.count; i++) {
        std::vector<uint8_t> data = Payload('U', (uint32_t)i);
        if (session.Send(self, peer, FALSE, data.data(), (DWORD)data.size()) == NET_OK)
            result.unguaranteedSent++;
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    auto count = [](const std::vector<bool> &v) { return (int)std::count(v.begin(), v.end(), true); };
    Clock::time_point echoDeadline = Clock::now() + std::chrono::seconds(15);
    while (count(gotG) < args.count && Clock::now() < echoDeadline) {
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Clock::time_point linger = Clock::now() + std::chrono::milliseconds(500);
    while (Clock::now() < linger) {
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    result.guaranteedOk = count(gotG);
    result.unguaranteedOk = count(gotU);

    std::vector<uint8_t> bye = Payload('B', 0);
    session.Send(self, peer, TRUE, bye.data(), (DWORD)bye.size());
    Clock::time_point byeDeadline = Clock::now() + std::chrono::seconds(5);
    while (!byeBack && Clock::now() < byeDeadline) {
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    result.byeEchoed = byeBack;
    session.Close();

    int minU = (int)std::ceil(args.minUnguaranteed * args.count);
    bool ok = result.guaranteedOk == args.count && result.unguaranteedOk >= minU;
    if (!ok)
        result.error = "messages";
    PrintJoin(result);
    return ok ? 0 : 5;
}

}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    Args args;
    if (!Parse(argc, argv, args)) {
        Usage();
        return 2;
    }
    Log("code_client: %s lobby=%s relay=%s port=%u", args.mode.c_str(), args.lobby.c_str(), args.relayName.c_str(),
        (unsigned)args.port);
    return args.mode == "host" ? RunHost(args) : RunJoin(args);
}
