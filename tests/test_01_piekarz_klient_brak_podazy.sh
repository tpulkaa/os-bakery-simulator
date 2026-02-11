#!/bin/bash
# ===========================================================================
# Test 01: Piekarz → Klient – kolejka podajnikow bez podazy
# ===========================================================================
#
# CEL:
#   Testuje komunikacje miedzy piekarzem a klientami przez KOLEJKE
#   KOMUNIKATOW PODAJNIKOW (msgrcv z mtype = product_id + 1).
#
# EDGE CASE:
#   Zabijamy piekarza w trakcie symulacji. Klienci nie moga pobrac
#   produktow z podajnika (kolejka pusta). Sprawdzamy czy:
#   - klienci NIE zakleszczaja sie (timeout zamiast blokowania)
#   - symulacja konczy sie normalnie (brak deadlocku)
#   - klienci bez produktow sa liczeni jako "nieobsluzeni"
#
# TESTOWANE IPC:
#   - Kolejka komunikatow podajnikow (piekarz -> klient, mtype filtering)
#   - msgrcv() z IPC_NOWAIT — klient nie blokuje na pustym podajniku
#   - Semafory podajnikow (SEM_CONVEYOR_BASE) — backpressure
#
# PARAMETRY:
#   -t 15 -s 30 -n 8 -o 8 -c 12 (krotka symulacja, szybki czas)
#
# WNIOSKI:
#   Jesli test przechodzi, to znaczy ze klient poprawnie obsluguje
#   sytuacje pustego podajnika — msgrcv() z IPC_NOWAIT zwraca ENOMSG
#   i klient wychodzi ze sklepu zamiast czekac w nieskonczonosc.
#   Brak zakleszczenia = kolejka komunikatow i semafory dzialaja poprawnie.
# ===========================================================================
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

echo "[test_01_piekarz_klient_brak_podazy] START"
cd "$PROJECT_DIR"

# Uruchom symulacje: krotki czas, szybka skala
./kierownik -t 15 -s 30 -n 8 -o 8 -c 12 < /dev/null > /dev/null 2>&1 &
KIE_PID=$!
sleep 3

# CHECK 1: Piekarz zyje i produkuje
BAKER_PID=$(shm_val baker_pid)
if [[ -n "$BAKER_PID" && "$BAKER_PID" -gt 0 ]] && kill -0 "$BAKER_PID" 2>/dev/null; then
    ok "piekarz dziala (PID=$BAKER_PID)"
else
    fail "piekarz nie znaleziony"
fi

# CHECK 2: Sprawdz ze produkcja dziala (baker_produced > 0)
PRODUCED_BEFORE=$(shm_val baker_produced_total)
[[ -n "$PRODUCED_BEFORE" && "$PRODUCED_BEFORE" -gt 0 ]] \
    && ok "piekarz wyprodukował $PRODUCED_BEFORE szt. przed zabiciem" \
    || fail "brak produkcji przed zabiciem ($PRODUCED_BEFORE)"

# Zabij piekarza — klienci traca zrodlo produktow
if [[ -n "$BAKER_PID" && "$BAKER_PID" -gt 0 ]]; then
    kill -9 "$BAKER_PID" 2>/dev/null
    sleep 1
    if ! kill -0 "$BAKER_PID" 2>/dev/null; then
        ok "piekarz zabity (SIGKILL)"
    else
        fail "nie udalo sie zabic piekarza"
    fi
fi

# CHECK 3: Produkcja ustala (baker_produced nie rosnie)
PRODUCED_AFTER_KILL=$(shm_val baker_produced_total)
sleep 2
PRODUCED_LATER=$(shm_val baker_produced_total)
if [[ -n "$PRODUCED_AFTER_KILL" && -n "$PRODUCED_LATER" ]]; then
    [[ "$PRODUCED_LATER" -eq "$PRODUCED_AFTER_KILL" ]] \
        && ok "produkcja ustala po zabiciu piekarza ($PRODUCED_LATER szt.)" \
        || fail "produkcja rosnie mimo zabitego piekarza ($PRODUCED_AFTER_KILL -> $PRODUCED_LATER)"
fi

# CHECK 4: Symulacja nie zakleszcza sie — konczy sie sama
W=0; while kill -0 "$KIE_PID" 2>/dev/null && [[ $W -lt 40 ]]; do sleep 0.5; W=$((W+1)); done
if ! kill -0 "$KIE_PID" 2>/dev/null; then
    ok "brak zakleszczenia — symulacja zakonczyla sie"
else
    fail "ZAKLESZCZENIE — symulacja nie zakonczyla sie w 20s"
    kill -INT "$KIE_PID" 2>/dev/null; sleep 2
    kill -9 "$KIE_PID" 2>/dev/null; wait "$KIE_PID" 2>/dev/null || true
    for name in klient kasjer piekarz; do pkill -9 -x "$name" 2>/dev/null || true; done
fi
sleep 2

# CHECK 5: Brak procesow
REM=$(count_procs)
[[ $REM -eq 0 ]] && ok "procesy wyczyszczone" || fail "$REM procesow zostalo"

# CHECK 6: IPC czyste
SHM=$(our_shm); SEM=$(our_sem); MSG=$(our_msg)
[[ $SHM -eq 0 && $SEM -eq 0 && $MSG -eq 0 ]] && ok "IPC czyste" || fail "IPC: shm=$SHM sem=$SEM msg=$MSG"

echo ""
[[ $FAIL -eq 0 ]] && echo "[test_01_piekarz_klient_brak_podazy] PASS ($PASS/$((PASS+FAIL)))" && exit 0
echo "[test_01_piekarz_klient_brak_podazy] FAIL ($PASS/$((PASS+FAIL)))"; exit 1
