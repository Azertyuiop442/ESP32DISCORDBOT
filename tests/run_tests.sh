#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CC="${CC:-clang}"
if ! command -v "$CC" >/dev/null 2>&1 && command -v gcc >/dev/null 2>&1; then
    CC=gcc
fi

SAN_FLAGS=()
if echo 'int main(void){return 0;}' | "$CC" -fsanitize=address,undefined -x c - -o "$OUT/san_check" >/dev/null 2>&1; then
    SAN_FLAGS=(-fsanitize=address,undefined)
fi

CFLAGS=(-std=c11 -Wall -Wextra -Werror -O1)

echo "== unit tests (sanitizers on) =="
$CC "${CFLAGS[@]}" "${SAN_FLAGS[@]}" \
    -I"$ROOT/components/discord_etf/include" \
    "$ROOT/components/discord_etf/src/discord_etf.c" \
    "$ROOT/tests/etf/test_etf.c" \
    -o "$OUT/test_etf"
"$OUT/test_etf"

echo
echo "== state bounds =="
$CC "${CFLAGS[@]}" "${SAN_FLAGS[@]}" \
    -I"$ROOT/components/discord_etf/include" \
    -I"$ROOT/components/discord_memory/include" \
    "$ROOT/components/discord_memory/src/discord_state.c" \
    "$ROOT/tests/memory/test_state.c" \
    -o "$OUT/test_state"
"$OUT/test_state"

echo
echo "== captcha feature =="
$CC "${CFLAGS[@]}" "${SAN_FLAGS[@]}" \
    -I"$ROOT/components/discord_etf/include" \
    -I"$ROOT/components/discord_memory/include" \
    -I"$ROOT/features/captcha" \
    "$ROOT/components/discord_memory/src/discord_state.c" \
    "$ROOT/features/captcha/discord_captcha.c" \
    "$ROOT/tests/memory/test_captcha.c" \
    -o "$OUT/test_captcha"
"$OUT/test_captcha"

echo
echo "== xp & role rewards feature =="
$CC "${CFLAGS[@]}" "${SAN_FLAGS[@]}" \
    -I"$ROOT/components/discord_core/include" \
    -I"$ROOT/components/discord_etf/include" \
    -I"$ROOT/components/discord_memory/include" \
    -I"$ROOT/components/discord_scheduler/include" \
    -I"$ROOT/features/xp" \
    "$ROOT/components/discord_memory/src/discord_state.c" \
    "$ROOT/components/discord_core/src/discord_feature.c" \
    "$ROOT/features/xp/discord_xp_curve.c" \
    "$ROOT/features/xp/discord_xp_cooldown.c" \
    "$ROOT/features/xp/discord_xp_roles.c" \
    "$ROOT/features/xp/discord_xp_guard.c" \
    "$ROOT/features/xp/discord_xp_server.c" \
    "$ROOT/features/xp/discord_xp.c" \
    "$ROOT/tests/features/test_xp.c" \
    -o "$OUT/test_xp"
"$OUT/test_xp"

echo
echo "== record store =="
$CC "${CFLAGS[@]}" "${SAN_FLAGS[@]}" \
    -I"$ROOT/components/discord_etf/include" \
    -I"$ROOT/components/discord_memory/include" \
    "$ROOT/components/discord_memory/src/discord_store.c" \
    "$ROOT/tests/memory/test_store.c" \
    -o "$OUT/test_store"
"$OUT/test_store"

echo
echo "== rate limiter =="
$CC "${CFLAGS[@]}" "${SAN_FLAGS[@]}" \
    -I"$ROOT/components/discord_rest/include" \
    "$ROOT/components/discord_rest/src/discord_ratelimit.c" \
    "$ROOT/tests/rest/test_ratelimit.c" \
    -o "$OUT/test_ratelimit"
"$OUT/test_ratelimit"

echo
echo "== gateway state machine =="
$CC "${CFLAGS[@]}" "${SAN_FLAGS[@]}" \
    -I"$ROOT/components/discord_gateway/include" \
    "$ROOT/components/discord_gateway/src/discord_gateway.c" \
    "$ROOT/tests/gateway/test_gateway.c" \
    -o "$OUT/test_gateway"
"$OUT/test_gateway"

echo
echo "== fuzz: ETF mutation =="
$CC "${CFLAGS[@]}" -g "${SAN_FLAGS[@]}" \
    -I"$ROOT/components/discord_etf/include" \
    "$ROOT/components/discord_etf/src/discord_etf.c" \
    "$ROOT/tests/fuzz/fuzz_etf_mutate.c" \
    -o "$OUT/fuzz_etf"
"$OUT/fuzz_etf"

echo
echo "== soak: Monte Carlo 7-day horizons =="
$CC "${CFLAGS[@]}" -g "${SAN_FLAGS[@]}" \
    -I"$ROOT/components/discord_gateway/include" \
    -I"$ROOT/components/discord_rest/include" \
    -I"$ROOT/components/discord_etf/include" \
    -I"$ROOT/components/discord_memory/include" \
    "$ROOT/components/discord_gateway/src/discord_gateway.c" \
    "$ROOT/components/discord_rest/src/discord_ratelimit.c" \
    "$ROOT/components/discord_memory/src/discord_state.c" \
    "$ROOT/tests/fuzz/soak.c" \
    -o "$OUT/soak"
"$OUT/soak"

echo
echo "== json writer =="
$CC "${CFLAGS[@]}" "${SAN_FLAGS[@]}" \
    -I"$ROOT/components/discord_json/include" \
    "$ROOT/components/discord_json/src/discord_json.c" \
    "$ROOT/tests/rest/test_json.c" \
    -o "$OUT/test_json"
"$OUT/test_json"

echo
echo "== v2 message body =="
$CC "${CFLAGS[@]}" "${SAN_FLAGS[@]}" \
    -I"$ROOT/components/discord_json/include" \
    -I"$ROOT/components/discord_transport/include" \
    "$ROOT/components/discord_json/src/discord_json.c" \
    "$ROOT/components/discord_transport/src/discord_v2.c" \
    "$ROOT/tests/rest/test_v2.c" \
    -o "$OUT/test_v2"
"$OUT/test_v2"

echo
echo "== feature registry =="
$CC "${CFLAGS[@]}" "${SAN_FLAGS[@]}" \
    -I"$ROOT/components/discord_core/include" \
    -I"$ROOT/components/discord_etf/include" \
    "$ROOT/components/discord_core/src/discord_feature.c" \
    "$ROOT/components/discord_core/src/discord_interaction_parse.c" \
    "$ROOT/tests/memory/test_feature.c" \
    -o "$OUT/test_feature"
"$OUT/test_feature"

echo
echo "== watchdog =="
$CC "${CFLAGS[@]}" "${SAN_FLAGS[@]}" \
    -I"$ROOT/components/discord_scheduler/include" \
    "$ROOT/components/discord_scheduler/src/discord_watchdog.c" \
    "$ROOT/tests/gateway/test_watchdog.c" \
    -o "$OUT/test_watchdog"
"$OUT/test_watchdog"

echo
echo "== footprint report =="
$CC "${CFLAGS[@]}" \
    -I"$ROOT/components/discord_core/include" \
    -I"$ROOT/components/discord_etf/include" \
    -I"$ROOT/components/discord_memory/include" \
    -I"$ROOT/components/discord_gateway/include" \
    -I"$ROOT/components/discord_rest/include" \
    -I"$ROOT/components/discord_scheduler/include" \
    -I"$ROOT/components/discord_json/include" \
    -I"$ROOT/components/discord_transport/include" \
    "$ROOT/tools/footprint.c" \
    "$ROOT/components/discord_core/src/discord_feature.c" \
    "$ROOT/components/discord_memory/src/discord_state.c" \
    "$ROOT/components/discord_memory/src/discord_store.c" \
    "$ROOT/components/discord_gateway/src/discord_gateway.c" \
    "$ROOT/components/discord_rest/src/discord_ratelimit.c" \
    "$ROOT/components/discord_scheduler/src/discord_watchdog.c" \
    "$ROOT/components/discord_scheduler/src/discord_jobq.c" \
    "$ROOT/components/discord_transport/src/discord_ackq.c" \
    -o "$OUT/footprint"
"$OUT/footprint"

echo
echo "== job queue =="
$CC "${CFLAGS[@]}" "${SAN_FLAGS[@]}" \
    -I"$ROOT/components/discord_scheduler/include" \
    -I"$ROOT/components/discord_etf/include" \
    "$ROOT/components/discord_scheduler/src/discord_jobq.c" \
    "$ROOT/tests/gateway/test_jobq.c" \
    -o "$OUT/test_jobq"
"$OUT/test_jobq"

echo
echo "== configuration =="
$CC "${CFLAGS[@]}" "${SAN_FLAGS[@]}" \
    -I"$ROOT/components/bot_config/include" \
    -I"$ROOT/components/discord_scheduler/include" \
    -I"$ROOT/components/discord_etf/include" \
    -I"$ROOT/components/discord_memory/include" \
    -I"$ROOT/components/discord_rest/include" \
    "$ROOT/components/bot_config/src/bot_config.c" \
    "$ROOT/components/discord_memory/src/discord_state.c" \
    "$ROOT/components/discord_rest/src/discord_ratelimit.c" \
    "$ROOT/components/discord_scheduler/src/discord_jobq.c" \
    "$ROOT/tests/test_config.c" \
    -o "$OUT/test_config"
"$OUT/test_config"

echo
echo "== security =="
$CC "${CFLAGS[@]}" "${SAN_FLAGS[@]}" \
    -I"$ROOT/components/discord_security/include" \
    "$ROOT/components/discord_security/src/discord_security.c" \
    "$ROOT/tests/test_security.c" \
    -o "$OUT/test_security"
"$OUT/test_security"

echo
echo "== authorization (abuse cases) =="
$CC "${CFLAGS[@]}" "${SAN_FLAGS[@]}" \
    -I"$ROOT/components/discord_core/include" \
    -I"$ROOT/components/discord_etf/include" \
    "$ROOT/components/discord_core/src/discord_perms.c" \
    "$ROOT/tests/test_perms.c" \
    -o "$OUT/test_perms"
"$OUT/test_perms"

echo
echo "== interaction parse =="
$CC "${CFLAGS[@]}" "${SAN_FLAGS[@]}" \
    -I"$ROOT/components/discord_core/include" \
    -I"$ROOT/components/discord_etf/include" \
    "$ROOT/components/discord_core/src/discord_interaction_parse.c" \
    "$ROOT/tests/test_interaction_parse.c" \
    -o "$OUT/test_interaction_parse"
"$OUT/test_interaction_parse"

echo
echo "== ack queue =="
$CC "${CFLAGS[@]}" "${SAN_FLAGS[@]}" \
    -I"$ROOT/components/discord_transport/include" \
    -I"$ROOT/components/discord_core/include" \
    -I"$ROOT/components/discord_etf/include" \
    "$ROOT/components/discord_transport/src/discord_ackq.c" \
    "$ROOT/tests/test_ackq.c" \
    -o "$OUT/test_ackq"
"$OUT/test_ackq"

echo
echo "== interaction dispatch queue =="
$CC "${CFLAGS[@]}" "${SAN_FLAGS[@]}" \
    -I"$ROOT/components/discord_scheduler/include" \
    -I"$ROOT/components/discord_core/include" \
    -I"$ROOT/components/discord_etf/include" \
    "$ROOT/components/discord_scheduler/src/discord_intq.c" \
    "$ROOT/tests/dispatch/test_intq.c" \
    -o "$OUT/test_intq"
"$OUT/test_intq"

echo
echo "== IDENTIFY budget =="
$CC "${CFLAGS[@]}" "${SAN_FLAGS[@]}" \
    -I"$ROOT/components/discord_transport/include" \
    -I"$ROOT/components/discord_gateway/include" \
    "$ROOT/components/discord_transport/src/discord_identify.c" \
    "$ROOT/tests/test_identify.c" \
    -o "$OUT/test_identify"
"$OUT/test_identify"
echo
echo "== all tests =="
