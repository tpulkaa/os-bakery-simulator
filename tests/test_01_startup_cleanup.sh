#!/bin/bash
# Test 01: Normalne uruchomienie i sprzatanie
# Sprawdza: procesy startuja, IPC istnieje, po -t konczy sie, czysti IPC+procesy
set -u
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PASS=0; FAIL=0
ok()   { echo "  OK: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

count_procs() {
    local c=0
    for name in kierownik piekarz kasjer klient; do
        c=$((c + $(pgrep -x "$name" 2>/dev/null | wc -l)))
    done
    echo "$c"
}

MYUSER=$(whoami)
our_shm() { ipcs -m 2>/dev/null | grep "^m.*$MYUSER" | wc -l | tr -d ' '; }
our_sem() { ipcs -s 2>/dev/null | grep "^s.*$MYUSER" | wc -l | tr -d ' '; }
our_msg() { ipcs -q 2>/dev/null | grep "^q.*$MYUSER" | wc -l | tr -d ' '; }

shm_val() { "$PROJECT_DIR/check_shm" 2>/dev/null | grep "^$1=" | cut -d= -f2; }

echo "[test_01_startup_cleanup] START"
cd "$PROJECT_DIR"

./kierownik -t 10 -s 50 -o 8 -c 12 < /dev/null > /dev/null 2>&1 &
KIE_PID=$!
sleep 3

CNT=$(count_procs)
[[ $CNT -ge 4 ]] && ok "$CNT procesow symulacji (>= 4)" || fail "tylko $CNT procesow"

kill -0 "$KIE_PID" 2>/dev/null && ok "kierownik zyje" || fail "kierownik nie zyje"

[[ $(our_shm) -ge 1 ]] && ok "SHM istnieje" || fail "brak SHM"
[[ $(our_sem) -ge 1 ]] && ok "SEM istnieje" || fail "brak SEM"
[[ $(our_msg) -ge 1 ]] && ok "MSG istnieje" || fail "brak MSG"

SIM_RUN=$(shm_val simulation_running)
[[ "$SIM_RUN" == "1" ]] && ok "simulation_running=1 w SHM" || fail "simulation_running=$SIM_RUN"

W=0; while kill -0 "$KIE_PID" 2>/dev/null && [[ $W -lt 40 ]]; do sleep 0.5; W=$((W+1)); done
if ! kill -0 "$KIE_PID" 2>/dev/null; then
    ok "kierownik zakonczyl sie"
else
    fail "timeout"
    kill -INT "$KIE_PID" 2>/dev/null; sleep 2
    kill -9 "$KIE_PID" 2>/dev/null; wait "$KIE_PID" 2>/dev/null || true
fi
sleep 2

REM=$(count_procs)
[[ $REM -eq 0 ]] && ok "brak procesow symulacji" || fail "$REM procesow zostalo"

[[ $(our_shm) -eq 0 ]] && ok "SHM czyste" || fail "SHM zostalo"
[[ $(our_sem) -eq 0 ]] && ok "SEM czyste" || fail "SEM zostalo"
[[ $(our_msg) -eq 0 ]] && ok "MSG czyste" || fail "MSG zostalo"

[[ -f "$PROJECT_DIR/logs/raport.txt" ]] && ok "raport wygenerowany" || fail "brak raportu"

echo ""; [[ $FAIL -eq 0 ]] && echo "[test_01_startup_cleanup] PASS ($PASS/$((PASS+FAIL)))" && exit 0
echo "[test_01_startup_cleanup] FAIL ($PASS/$((PASS+FAIL)))"; exit 1
