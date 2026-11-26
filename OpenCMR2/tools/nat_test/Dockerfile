FROM debian:bookworm-slim@sha256:e67eeead736ff7bbe258ef088bff683635c561b1fcfefad0806c1df4ab49ce0e
RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ iproute2 iptables conntrack tcpdump procps \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY third_party/enet third_party/enet
COPY src/net src/net
COPY src/port/net.h src/port/types.h src/port/
COPY src/lobby src/lobby
COPY tools/nat_test/code_client.cpp tools/nat_test/router.sh tools/nat_test/
RUN set -e; mkdir /obj; \
    for f in callbacks compress host list packet peer protocol unix; do \
        gcc -O2 -c -Ithird_party/enet/include -DHAS_FCNTL=1 -DHAS_POLL=1 -DHAS_GETADDRINFO=1 -DHAS_GETNAMEINFO=1 \
            -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 -DHAS_MSGHDR_FLAGS=1 -DHAS_SOCKLEN_T=1 \
            third_party/enet/$f.c -o /obj/$f.o; \
    done; \
    g++ -std=c++17 -O2 -Wall -Wextra -pthread -include src/port/types.h -Isrc -Ithird_party/enet/include \
        src/net/net_session.cpp src/net/rendezvous.cpp tools/nat_test/code_client.cpp /obj/*.o \
        -o /usr/local/bin/code_client; \
    g++ -std=c++17 -O2 -Wall -Wextra -Isrc src/lobby/lobby_server.cpp src/lobby/main.cpp \
        -o /usr/local/bin/opencmr2-lobby; \
    install -m 755 tools/nat_test/router.sh /usr/local/bin/nat-router
