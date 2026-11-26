#!/usr/bin/env bash
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
RUN_ID=${NAT_TEST_RUN_ID:-$$}
P=cmr2nat-$RUN_ID
LABEL=opencmr2.nat_test=$RUN_ID
IMG=opencmr2-nat-test:$RUN_ID
LOGS=${NAT_TEST_LOGS:-$(mktemp -d "${TMPDIR:-/tmp}/opencmr2-nat-test.XXXXXX")}
ONLY=${NAT_TEST_ONLY:-}

WAN_PREFIX=198.18.0.
LOBBY_IP=198.18.0.10
NAT_A_WAN=198.18.0.11
NAT_B_WAN=198.18.0.12
NAT_A_LAN=10.201.0.2
NAT_B_LAN=10.202.0.2
HOST_IP=10.201.0.10
GUEST_IP=10.202.0.10
LOBBY=$LOBBY_IP:28630

RESULTS=()
FAILED=0
START=$(date +%s)

log() { printf '[nat_test] %s\n' "$*" >&2; }

teardown_scenario() {
    local ids
    ids=$(docker ps -aq --filter "label=$LABEL")
    [ -n "$ids" ] && docker rm -f $ids >/dev/null 2>&1
    ids=$(docker network ls -q --filter "label=$LABEL")
    [ -n "$ids" ] && docker network rm $ids >/dev/null 2>&1
    return 0
}

cleanup() {
    teardown_scenario
    if [ "${KEEP_IMAGE:-0}" != 1 ]; then
        docker image rm -f "$IMG" >/dev/null 2>&1
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

net_create() {
    docker network create --driver bridge --subnet "$2" --label "$LABEL" "${@:3}" "$P-$1" >/dev/null
}

run_box() {
    local name=$1 net=$2 ip=$3
    shift 3
    docker run -d --name "$P-$name" --label "$LABEL" --hostname "$name" --network "$P-$net" --ip "$ip" "$@" \
        >/dev/null
}

router() {
    local name=$1 wan_ip=$2 lan=$3 lan_ip=$4 mode=$5
    docker create --name "$P-$name" --label "$LABEL" --hostname "$name" --cap-add NET_ADMIN --cap-add NET_RAW \
        --sysctl net.ipv4.ip_forward=1 --sysctl net.ipv6.conf.all.disable_ipv6=1 \
        --network "$P-wan" --ip "$wan_ip" "$IMG" sleep infinity >/dev/null || return 1
    docker network connect --ip "$lan_ip" "$P-$lan" "$P-$name" || return 1
    docker start "$P-$name" >/dev/null || return 1
    docker exec "$P-$name" nat-router "$mode" "$WAN_PREFIX"
}

client() {
    local name=$1 lan=$2 ip=$3 gw=$4
    run_box "$name" "$lan" "$ip" --cap-add NET_ADMIN --sysctl net.ipv6.conf.all.disable_ipv6=1 "$IMG" \
        sleep infinity || return 1
    docker exec "$P-$name" ip route replace default via "$gw" || return 1
    local route
    route=$(docker exec "$P-$name" ip route get "$LOBBY_IP")
    case "$route" in
    *"via $gw "*) ;;
    *) log "$name: lobby not routed via $gw: $route"; return 1 ;;
    esac
}

wan_if() {
    docker exec "$P-$1" sh -c "ip -o -4 addr show | awk -v p=$WAN_PREFIX 'index(\$4, p) == 1 { print \$2; exit }'"
}

setup() {
    local mode_a=$1 mode_b=$2 dir=$3
    net_create wan 198.18.0.0/24 --internal || return 1
    net_create lan-a 10.201.0.0/24 -o com.docker.network.bridge.enable_ip_masquerade=false || return 1
    net_create lan-b 10.202.0.0/24 -o com.docker.network.bridge.enable_ip_masquerade=false || return 1
    run_box lobby wan "$LOBBY_IP" "$IMG" opencmr2-lobby --verbose --relay-ports 28631-28694 || return 1
    router nat-a "$NAT_A_WAN" lan-a "$NAT_A_LAN" "$mode_a" >"$dir/nat-a.setup" 2>&1 || return 1
    router nat-b "$NAT_B_WAN" lan-b "$NAT_B_LAN" "$mode_b" >"$dir/nat-b.setup" 2>&1 || return 1
    client host-a lan-a "$HOST_IP" "$NAT_A_LAN" || return 1
    client guest-b lan-b "$GUEST_IP" "$NAT_B_LAN" || return 1
    local r
    for r in nat-a nat-b; do
        docker exec -d "$P-$r" tcpdump -i "$(wan_if $r)" -n -U -w /tmp/wan.pcap udp || return 1
    done
    sleep 1
}

collect() {
    local dir=$1 r
    docker logs "$P-lobby" >"$dir/lobby.log" 2>&1
    for r in nat-a nat-b; do
        docker exec "$P-$r" conntrack -L -p udp >"$dir/$r.conntrack" 2>&1
        { docker exec "$P-$r" iptables -v -n -L; docker exec "$P-$r" iptables -t nat -v -n -L; } \
            >"$dir/$r.iptables" 2>&1
        docker exec "$P-$r" pkill tcpdump >/dev/null 2>&1
    done
    sleep 1
    for r in nat-a nat-b; do
        docker exec "$P-$r" tcpdump -n -r /tmp/wan.pcap >"$dir/$r.wan.txt" 2>/dev/null
        docker cp "$P-$r:/tmp/wan.pcap" "$dir/$r.wan.pcap" >/dev/null 2>&1
    done
}

json_field() {
    printf '%s' "$1" | sed -n "s/.*\"$2\":\"\{0,1\}\([^,\"}]*\).*/\1/p"
}

scenario() {
    local name=$1 mode_a=$2 mode_b=$3 relay=$4 expect=$5
    if [ -n "$ONLY" ] && [ "$ONLY" != "$name" ]; then
        return 0
    fi
    local dir=$LOGS/$name
    mkdir -p "$dir"
    log "scenario $name: nat_a=$mode_a nat_b=$mode_b guest relay=$relay expect=$expect"
    teardown_scenario
    local actual="" relayed="-" rtt="-" notes=""
    if ! setup "$mode_a" "$mode_b" "$dir"; then
        actual="SETUP_FAILED"
    else
        docker exec "$P-host-a" code_client host --lobby "$LOBBY" --code-file /tmp/code --serve 90 \
            >"$dir/host.out" 2>"$dir/host.err" &
        local hpid=$! code="" i
        for i in $(seq 1 60); do
            code=$(sed -n 's/^CODE //p' "$dir/host.out" 2>/dev/null)
            [ -n "$code" ] && break
            kill -0 $hpid 2>/dev/null || break
            sleep 0.5
        done
        if [ -z "$code" ]; then
            actual="NO_CODE"
        else
            local grc=0
            docker exec "$P-guest-b" code_client join "$code" --lobby "$LOBBY" --relay "$relay" \
                >"$dir/guest.out" 2>"$dir/guest.err" || grc=$?
            local json status
            json=$(grep '^{' "$dir/guest.out" | tail -1)
            status=$(json_field "$json" status)
            relayed=$(json_field "$json" relayed)
            rtt=$(json_field "$json" rtt_ms)
            if [ $grc -eq 0 ]; then
                if [ "$relayed" = true ]; then actual=relay; else actual=direct; fi
                notes="g=$(json_field "$json" guaranteed_ok)/$(json_field "$json" guaranteed_sent)"
                notes="$notes u=$(json_field "$json" unguaranteed_ok)/$(json_field "$json" unguaranteed_sent)"
            else
                actual="fail:${status:-none}"
                notes="exit=$grc $(json_field "$json" error)"
            fi
        fi
        for i in $(seq 1 20); do
            kill -0 $hpid 2>/dev/null || break
            sleep 0.5
        done
        docker exec "$P-host-a" pkill code_client >/dev/null 2>&1
        wait $hpid 2>/dev/null
        collect "$dir"
        local lobby_log=$dir/lobby.log
        local reg_port
        reg_port=$(sed -n "s/.*registered by $NAT_A_WAN:\([0-9]*\).*/\1/p" "$lobby_log" | head -1)
        if [ -z "$reg_port" ]; then
            notes="$notes lobby:no-register-from-nat-a"
            actual="$actual!"
        fi
        if grep -q "registered by\|resolved by\|relay on port" "$lobby_log" &&
            grep -E "registered by|resolved by|relay on port" "$lobby_log" | grep -q "10\.20[12]\."; then
            notes="$notes lobby:saw-lan-address"
            actual="$actual!"
        fi
        if [ -n "$code" ] && ! grep -q "resolved by $NAT_B_WAN:[0-9]* for $NAT_A_WAN:" "$lobby_log"; then
            notes="$notes lobby:no-resolve-from-nat-b"
            actual="$actual!"
        fi
        local lobby_relay=no
        grep -q "relay on port" "$lobby_log" && lobby_relay=yes
        case "$actual" in
        relay) [ $lobby_relay = yes ] || { notes="$notes lobby:no-relay-line"; actual="$actual!"; } ;;
        direct | fail:*) [ $lobby_relay = no ] || notes="$notes lobby:relay-allocated" ;;
        esac
        local relay_pkts direct_in
        relay_pkts=$(awk -v l="$LOBBY_IP." 'index($5, l) == 1 && $5 != l "28630:"' "$dir/nat-a.wan.txt" | wc -l | tr -d ' ')
        direct_in=$(awk -v b="$NAT_B_WAN." -v a="$NAT_A_WAN." 'index($3, b) == 1 && index($5, a) == 1' \
            "$dir/nat-a.wan.txt" | wc -l | tr -d ' ')
        notes="$notes pcap:relay=$relay_pkts,b->a=$direct_in"
        case "$actual" in
        relay)
            [ "$relay_pkts" -ge 100 ] || { notes="$notes pcap:relay-unused"; actual="$actual!"; }
            if [ "$relay" = always ] && [ "$direct_in" -ne 0 ]; then
                notes="$notes pcap:direct-traffic"
                actual="$actual!"
            fi
            ;;
        direct)
            [ "$relay_pkts" -eq 0 ] || { notes="$notes pcap:relay-traffic"; actual="$actual!"; }
            [ "$direct_in" -ge 100 ] || { notes="$notes pcap:direct-unused"; actual="$actual!"; }
            ;;
        esac
        notes="$notes host-port=${reg_port:-?}"
        if [ "$mode_a" = cone ] && [ -n "$reg_port" ] && [ "$reg_port" != 28620 ]; then
            notes="$notes (cone-port-changed)"
        fi
        if [ "$mode_a" = symmetric ] && [ "$reg_port" = 28620 ]; then
            notes="$notes (random-fully-kept-port)"
        fi
    fi
    teardown_scenario
    local verdict=PASS
    [ "$actual" = "$expect" ] || { verdict=FAIL; FAILED=1; }
    RESULTS+=("$(printf '%-22s %-26s %-26s %-8s %-8s %-5s %s' "$name" "$expect" "$actual" "$relayed" "$rtt" \
        "$verdict" "$notes")")
    log "scenario $name: $verdict ($actual) $notes"
}

command -v docker >/dev/null || { log "docker not found"; exit 1; }
mkdir -p "$LOGS"
log "logs: $LOGS"
log "building $IMG"
if ! docker build -q -f "$ROOT/tools/nat_test/Dockerfile" -t "$IMG" "$ROOT" >"$LOGS/build.log" 2>&1; then
    cat "$LOGS/build.log" >&2
    exit 1
fi

scenario 1-cone-auto cone cone auto direct
scenario 2-symmetric-auto symmetric symmetric auto relay
scenario 3-symmetric-never symmetric symmetric never fail:HOST_UNREACHABLE
scenario 4-cone-always cone cone always relay

printf '\n%-22s %-26s %-26s %-8s %-8s %-5s %s\n' scenario expected actual relayed rtt_ms ok notes
if [ ${#RESULTS[@]} -eq 0 ]; then
    log "no scenario ran"
    FAILED=1
else
    for line in "${RESULTS[@]}"; do
        printf '%s\n' "$line"
    done
fi
printf '\nlogs: %s\nruntime: %ss\n' "$LOGS" "$(($(date +%s) - START))"
exit $FAILED
