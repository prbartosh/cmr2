FROM debian:bookworm-slim@sha256:e67eeead736ff7bbe258ef088bff683635c561b1fcfefad0806c1df4ab49ce0e AS build
RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ cmake ninja-build \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY net/net_wire.h net/lobby_wire.h net/join_code.h net/
COPY lobby/ lobby/
RUN cmake -S lobby -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPENCMR2_BUILD_TESTS=OFF \
    && cmake --build /build \
    && cmake --install /build --prefix /opt/opencmr2-lobby --strip

FROM gcr.io/distroless/cc-debian12:nonroot@sha256:9dac0a79194e45a7da0158a9c6da57b217585af0786db3845d1f0ec1a0dd182f
COPY --from=build /opt/opencmr2-lobby/bin/opencmr2-lobby /usr/local/bin/opencmr2-lobby
USER nonroot
EXPOSE 28630/udp
EXPOSE 28631-28694/udp
ENTRYPOINT ["/usr/local/bin/opencmr2-lobby", "--relay-ports", "28631-28694"]
