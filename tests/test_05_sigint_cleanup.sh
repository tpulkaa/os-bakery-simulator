#!/bin/bash
# Test 05: SIGINT (Ctrl+C) - czyste zamkniecie i raport
# Sprawdza:
#   - SIGINT powoduje czyste zamkniecie (nie crash)
#   - Raport wygenerowany po SIGINT
#   - Brak zombie po SIGINT
#   - IPC posprzatane
#   - Procesy posprzatane
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
count_zombies() {
    ps axo stat,comm 2>/dev/null | grep -E '^Z.*(kierownik|piekarz|kasjer|klient)' | wc -l | tr -d ' '
}
MYUSER=$(whoami)
our_shm() { ipcs -m 2>/dev/null | grep "^m.*$MYUSER" | wc -l | tr -d ' '; }
our_sem() { ipcs -s 2>/dev/null | grep "^s.*$MYUSER" | wc -l | tr -d ' '; }
our_msg() { ipcs -q 2>/dev/null | grep "^q.*$MYUSER" | wc -l | tr -d ' '; }

shm_val() { "$PROJECT_DIR/check_shm" 2>/dev/null | grep "^$1=" | cut -d= -f2; }

echo "[test_05_sigint_cleanup] START"
cd "$PROJECT_DIR"

# Uruchom symulacje (dlugi timeout, zamkniemy recznie)
./kierownik -t 30 -s 25 -n 8 -o 8 -c 20 < /dev/null > /dev/null 2>&1 &
KIE_PID=$!
sleep 3

# CHECK 1: Symulacja dziala
CNT=$(count_procs)
[[ $CNT -ge 4 ]] && ok "symulacja dziala ($CNT procesow)" || fail "za malo procesow ($CNT)"

# CHECK 2: SHM potwierdza symulacje
SIM_RUN=$(shm_val simulation_running)
[[ "$SIM_RUN" == "1" ]] && ok "simulation_running=1 w SHM" || fail "simulation_running=$SIM_RUN"

# Wyslij SIGINT (Ctrl+C)
kill -INT "$KIE_PID" 2>/dev/null
ok "wyslano SIGINT"

# Czekaj na zakonczenie (max 15s)
W=0; while kill -0 "$KIE_PID" 2>/dev/null && [[ $W -lt 30 ]]; do sleep 0.5; W=$((W+1)); done
ELAPSED=$((W / 2))

if ! kill -0 "$KIE_PID" 2>/dev/null; then
    ok "kierownik zakonczyl sie po SIGINT (${ELAPSED}s)"
else
    fail "kierownik nie odpowiedzial na SIGINT w 15s"
    kill -9 "$KIE_PID" 2>/dev/null; wait "$KIE_PID" 2>/dev/null || true
fi
sleep 2

# CHECK 3: Raport wygenerowany
[[ -f "$PROJECT_DIR/logs/raport.txt" ]] && ok "raport wygenerowany po SIGINT" || fail "brak raportu po SIGINT"

# CHECK 4: W raporcie sa dane
if [[ -f "$PROJECT_DIR/logs/raport.txt" ]]; then
    LINES=$(wc -l < "$PROJECT_DIR/logs/raport.txt" | tr -d ' ')
    [[ $LINES -gt 5 ]] && ok "raport niepusty ($LINES linii)" || fail "raport prawie pusty ($LINES linii)"
fi

# CHECK 5: Brak zombie
Z=$(count_zombies)
[[ $Z -eq 0 ]] && ok "brak zombie po SIGINT" || fail "$Z zombie procesow"

# CHECK 6: Brak procesow
REM=$(count_procs)
[[ $REM -eq 0 ]] && ok "procesy wyczyszczone" || fail "$REM procesow zostalo"

# CHECK 7: IPC czyste
SHM=$(our_shm); SEM=$(our_sem); MSG=$(our_msg)
[[ $SHM -eq 0 && $SEM -eq 0 && $MSG -eq 0 ]] && ok "IPC czyste" || fail "IPC: shm=$SHM sem=$SEM msg=$MSG"

echo ""; [[ $FAIL -eq 0 ]] && echo "[test_05_sigint_cleanup] PASS ($PASS/$((PASS+FAIL)))" && exit 0
echo "[test_05_sigint_cleanup] FAIL ($PASS/$((PASS+FAIL)))"; exit 1
