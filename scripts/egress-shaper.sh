#!/bin/bash
# Tunable egress traffic shaper for the WebRTC uplink on the Raspberry Pi.
#
# Reads a target rate from RATE_FILE and applies an HTB + fq_codel shaper on the
# uplink interface. fq_codel under the shaped class keeps queueing latency low,
# so WebRTC's congestion control sees a clean bottleneck and adapts its bitrate
# instead of bufferbloating.
#
# Tune live (no restart): just write a tc-style rate to the file:
#     echo 3mbit > /run/pi-webrtc/egress_rate
#     echo 800kbit > /run/pi-webrtc/egress_rate
#     echo off    > /run/pi-webrtc/egress_rate     # remove shaping
# The daemon polls the file and re-applies only when the value changes.
#
# Env knobs:
#   RATE_FILE        path to the rate file        (default /run/pi-webrtc/egress_rate)
#   IFACE            interface to shape            (default: auto from default route)
#   DEFAULT_RATE     initial rate if file absent   (default off)
#   POLL             poll interval seconds         (default 2)
#   SHAPE_UDP_ONLY   1 = shape only UDP (media),   (default 0 = shape all egress)
#                    leaving TCP (ssh/mqtt) un-throttled
#   QDISC_LIMIT      bytes of buffer under the shaped class (default 64kb).
#                    A simple bfifo (drop-tail) is used, NOT fq_codel: the
#                    camera's REMB-based control is loss-driven, so it needs a
#                    moderate queue that builds then drops, giving a gradual loss
#                    signal. fq_codel keeps the queue tiny and drops early, which
#                    loss-based control can only sense as abrupt overshoot.
set -u

RATE_FILE="${RATE_FILE:-/run/pi-webrtc/egress_rate}"
IFACE="${IFACE:-$(ip route show default 2>/dev/null | awk '/default/ {print $5; exit}')}"
DEFAULT_RATE="${DEFAULT_RATE:-off}"
POLL="${POLL:-2}"
SHAPE_UDP_ONLY="${SHAPE_UDP_ONLY:-0}"
QDISC_LIMIT="${QDISC_LIMIT:-64kb}"

log() { echo "egress-shaper: $*"; }

if [ -z "$IFACE" ]; then
    log "no default-route interface found; set IFACE=<iface> and retry; exiting"
    exit 1
fi

mkdir -p "$(dirname "$RATE_FILE")"
[ -f "$RATE_FILE" ] || echo "$DEFAULT_RATE" > "$RATE_FILE"

apply_off() {
    tc qdisc del dev "$IFACE" root 2>/dev/null
    log "shaping OFF on $IFACE"
}

apply_rate() {
    local rate="$1"
    # Clean slate every change to avoid stale classes/filters from a prior mode.
    tc qdisc del dev "$IFACE" root 2>/dev/null

    if [ "$SHAPE_UDP_ONLY" = "1" ]; then
        # Class 1:10 = shaped (media/UDP); 1:99 = unshaped (ssh, mqtt, ...).
        tc qdisc add dev "$IFACE" root handle 1: htb default 99 || return 1
        tc class add dev "$IFACE" parent 1: classid 1:10 htb rate "$rate" ceil "$rate" || return 1
        tc class add dev "$IFACE" parent 1: classid 1:99 htb rate 1000mbit ceil 1000mbit || return 1
        tc qdisc add dev "$IFACE" parent 1:10 handle 10: bfifo limit "$QDISC_LIMIT" || return 1
        tc filter add dev "$IFACE" parent 1: protocol ip prio 1 \
            u32 match ip protocol 17 0xff flowid 1:10 || return 1
        tc filter add dev "$IFACE" parent 1: protocol ipv6 prio 2 \
            u32 match ip6 protocol 17 0xff flowid 1:10 || return 1
        log "shaping $rate on $IFACE (UDP only)"
    else
        # Shape all egress on the interface through one rate-limited class.
        tc qdisc add dev "$IFACE" root handle 1: htb default 10 || return 1
        tc class add dev "$IFACE" parent 1: classid 1:10 htb rate "$rate" ceil "$rate" || return 1
        tc qdisc add dev "$IFACE" parent 1:10 handle 10: bfifo limit "$QDISC_LIMIT" || return 1
        log "shaping $rate on $IFACE (all egress)"
    fi
}

last=""
trap 'apply_off; exit 0' INT TERM

log "watching $RATE_FILE, interface $IFACE, udp_only=$SHAPE_UDP_ONLY"
while :; do
    want="$(tr -d '[:space:]' < "$RATE_FILE" 2>/dev/null)"
    if [ "$want" != "$last" ]; then
        case "$want" in
            "" | 0 | off | none | OFF | None)
                apply_off
                ;;
            *)
                if ! apply_rate "$want"; then
                    log "failed to apply rate '$want' (bad value?) - shaping left OFF"
                    tc qdisc del dev "$IFACE" root 2>/dev/null
                fi
                ;;
        esac
        last="$want"
    fi
    sleep "$POLL"
done
