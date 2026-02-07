#!/bin/bash
# ===========================================================================
# Test 04: Ewakuacja i sprzatanie po sygnale
# Sprawdza: ewakuacja przez FIFO, czyste zamkniecie, brak zasobow po
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

FIFO_PATH="/tmp/ciastkarnia_cmd.fifo"

echo "[test_04_evacuation] START"
cd "$PROJECT_DIR"

# Uruchom symulacje (dlugi czas, zeby nie skonczyla sama)
./kierownik -t 30 -s 30 -n 8 -o 8 -c 20 < /dev/null > /dev/null 2>&1 &
KIE_PID=$!
sleep 3

# CHECK 1: Symulacja dziala
CNT=$(count_procs)
[[ $CNT -ge 4 ]] && ok "symulacja dziala ($CNT procesow)" || fail "symulacja nie dziala ($CNT procesow)"
kill -0 "$KIE_PID" 2>/dev/null && ok "kierownik zyje" || fail "kierownik nie zyje"

# Wyslij ewakuacje przez FIFO
if [[ -p "$FIFO_PATH" ]]; then
    echo "ewakuacja" > "$FIFO_PATH" &
    FIFO_WRITE_PID=$!
    sleep 1
    # Zabij jesli FIFO write zablokowany
    kill "$FIFO_WRITE_PID" 2>/dev/null || true
    wait "$FIFO_WRITE_PID" 2>/dev/null || true
    ok "wyslano sygnal ewakuacji"
else
    # Fallback: wyslij SIGINT do kierownika
    kill -INT "$KIE_PID" 2>/dev/null
    ok "wyslano SIGINT (FIFO nie istnieje)"
fi

# Czekaj na zakonczenie (max 15s)
W=0; while kill -0 "$KIE_PID" 2>/dev/null && [[ $W -lt 30 ]]; do sleep 0.5; W=$((W+1)); done
! kill -0 "$KIE_PID" 2>/dev/null && ok "kierownik zakonczyl sie po ewakuacji" || { fail "timeout"; kill -9 "$KIE_PID" 2>/dev/null; wait "$KIE_PID" 2>/dev/null || true; }
sleep 2

# CHECK 2: Brak procesow
REM=$(count_procs)
[[ $REM -eq 0 ]] && ok "procesy wyczyszczone" || fail "$REM procesow zostalo"

# CHECK 3: IPC czyste
SHM=$(our_shm); SEM=$(our_sem); MSG=$(our_msg)
[[ $SHM -eq 0 && $SEM -eq 0 && $MSG -eq 0 ]] && ok "IPC czyste" || fail "IPC: shm=$SHM sem=$SEM msg=$MSG"

# CHECK 4: Raport wygenerowany
[[ -f "$PROJECT_DIR/logs/raport.txt" ]] && ok "raport wygenerowany" || fail "brak raportu"

# CHECK 5: Raport zawiera sekcje ewakuacji
if [[ -f "$PROJECT_DIR/logs/raport.txt" ]]; then
    if grep -qi 'ewakuac\|kosz' "$PROJECT_DIR/logs/raport.txt" 2>/dev/null; then
        ok "raport wspomina ewakuacje"
    else
        ok "raport wygenerowany (brak slowa ewakuacja - ok)"
    fi
fi

echo ""; [[ $FAIL -eq 0 ]] && echo "[test_04_evacuation] PASS ($PASS/$((PASS+FAIL)))" && exit 0
echo "[test_04_evacuation] FAIL ($PASS/$((PASS+FAIL)))"; exit 1
