#include "lobby/lobby_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace {

volatile sig_atomic_t s_stop = 0;

void OnSignal(int)
{
    s_stop = 1;
}

void Log(const char *format, ...)
{
    char stamp[32] = "";
    time_t now = time(nullptr);
    struct tm utc;
    if (gmtime_r(&now, &utc) != nullptr)
        strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    va_list args;
    va_start(args, format);
    fprintf(stderr, "%s ", stamp);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

void Usage(FILE *out)
{
    fprintf(out,
            "usage: opencmr2-lobby [--bind ADDR] [--port N] [--relay-ports FIRST-LAST] [--max-sessions N]\n"
            "                      [--max-guest-relays N] [--max-host-relays N] [--verbose] [--help]\n"
            "\n"
            "  --bind ADDR               IPv4 address to listen on (default 0.0.0.0)\n"
            "  --port N                  UDP port for the lobby (default 28630)\n"
            "  --relay-ports FIRST-LAST  UDP ports for relays (default 28631-28694)\n"
            "  --max-sessions N          most sessions at once, up to 65536 (default 4096)\n"
            "  --max-guest-relays N      most relays at once requested from one IP address (default 2)\n"
            "  --max-host-relays N       most relays at once to hosts on one IP address (default 7)\n"
            "  --verbose                 log endpoints and codes\n"
            "  --help                    show this help\n");
}

bool ParseNumber(const char *text, unsigned long min, unsigned long max, unsigned long &value)
{
    if (text == nullptr || *text < '0' || *text > '9')
        return false;
    errno = 0;
    char *end = nullptr;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < min || parsed > max)
        return false;
    value = parsed;
    return true;
}

bool ParseRange(const char *text, uint16_t &first, uint16_t &count)
{
    if (text == nullptr)
        return false;
    const char *dash = strchr(text, '-');
    if (dash == nullptr || dash == text || strlen(text) > 11)
        return false;
    char head[12] = {};
    memcpy(head, text, (size_t)(dash - text));
    unsigned long low = 0, high = 0;
    if (!ParseNumber(head, 1, 65535, low) || !ParseNumber(dash + 1, 1, 65535, high) || high < low)
        return false;
    first = (uint16_t)low;
    count = (uint16_t)(high - low + 1);
    return true;
}

}

int main(int argc, char **argv)
{
    lobby::Options options;
    options.log = Log;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value = i + 1 < argc ? argv[i + 1] : nullptr;
        unsigned long number = 0;
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            Usage(stdout);
            return 0;
        } else if (strcmp(arg, "--verbose") == 0) {
            options.verbose = true;
        } else if (strcmp(arg, "--bind") == 0) {
            in_addr address;
            if (value == nullptr || inet_pton(AF_INET, value, &address) != 1) {
                fprintf(stderr, "opencmr2-lobby: --bind needs an IPv4 address\n");
                return 2;
            }
            options.bindAddress = address.s_addr;
            i++;
        } else if (strcmp(arg, "--port") == 0) {
            if (!ParseNumber(value, 1, 65535, number)) {
                fprintf(stderr, "opencmr2-lobby: --port needs a number from 1 to 65535\n");
                return 2;
            }
            options.port = (uint16_t)number;
            i++;
        } else if (strcmp(arg, "--relay-ports") == 0) {
            if (!ParseRange(value, options.relayFirstPort, options.relayPortCount)) {
                fprintf(stderr, "opencmr2-lobby: --relay-ports needs FIRST-LAST, 1 <= FIRST <= LAST <= 65535\n");
                return 2;
            }
            i++;
        } else if (strcmp(arg, "--max-sessions") == 0) {
            if (!ParseNumber(value, 1, lobby::kMaxSessionsLimit, number)) {
                fprintf(stderr, "opencmr2-lobby: --max-sessions needs a number from 1 to %zu\n", lobby::kMaxSessionsLimit);
                return 2;
            }
            options.maxSessions = (size_t)number;
            i++;
        } else if (strcmp(arg, "--max-guest-relays") == 0 || strcmp(arg, "--max-host-relays") == 0) {
            if (!ParseNumber(value, 1, 65535, number)) {
                fprintf(stderr, "opencmr2-lobby: %s needs a number from 1 to 65535\n", arg);
                return 2;
            }
            if (strcmp(arg, "--max-guest-relays") == 0)
                options.maxRelaysPerGuestAddress = (size_t)number;
            else
                options.maxRelaysPerHostAddress = (size_t)number;
            i++;
        } else {
            fprintf(stderr, "opencmr2-lobby: unknown option %s\n", arg);
            Usage(stderr);
            return 2;
        }
    }
    if (options.port >= options.relayFirstPort &&
        (uint32_t)options.port < (uint32_t)options.relayFirstPort + options.relayPortCount) {
        fprintf(stderr, "opencmr2-lobby: the lobby port is inside the relay port range\n");
        return 2;
    }

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = OnSignal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    signal(SIGPIPE, SIG_IGN);

    lobby::Server server(options);
    if (!server.Start())
        return 1;
    while (!s_stop)
        server.Poll(1000);
    Log("stopping: %zu sessions, %zu relays", server.SessionCount(), server.RelayCount());
    return 0;
}
