#!/bin/sh
set -eu
mode=$1
wan_prefix=$2
wan=$(ip -o -4 addr show | awk -v p="$wan_prefix" 'index($4, p) == 1 { print $2; exit }')
lan=$(ip -o -4 addr show | awk -v p="$wan_prefix" '$2 != "lo" && index($4, p) != 1 { print $2; exit }')
[ -n "$wan" ] && [ -n "$lan" ] || { echo "nat-router: interfaces not found" >&2; exit 1; }
case "$mode" in
cone) masq="" ;;
symmetric) masq="--random-fully" ;;
*) echo "nat-router: mode must be cone or symmetric" >&2; exit 2 ;;
esac
while ip route del default 2>/dev/null; do :; done
iptables -t nat -A POSTROUTING -o "$wan" -j MASQUERADE $masq
iptables -P FORWARD DROP
iptables -A FORWARD -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
iptables -A FORWARD -i "$lan" -o "$wan" -j ACCEPT
iptables -P INPUT DROP
iptables -A INPUT -i lo -j ACCEPT
iptables -A INPUT -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
iptables -A INPUT -i "$lan" -j ACCEPT
[ "$(cat /proc/sys/net/ipv4/ip_forward)" = 1 ] || { echo "nat-router: ip_forward is off" >&2; exit 1; }
echo "wan=$wan lan=$lan mode=$mode"
