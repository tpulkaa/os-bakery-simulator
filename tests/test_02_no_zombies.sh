#!/bin/bash
# ===========================================================================
# Test 02: Brak zombie procesow i poprawne zbieranie potomnych
# Sprawdza: brak zombie podczas symulacji, brak po zakonczeniu
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

# Sprawdz zombie procesow naszej symulacji
count_zombies() {
    ps axo stat,comm 2>/dev/null | grep -E '^Z.*(kierownik|piekarz|kasjer|klient)' | wc -l | tr -d ' '
}

# Odczyt z check_shm
shm_val() { "$PROJECT_DIR/check_shm" 2>/dev/null | grep "^$1=" | cut -d= -f2; }

echo "[test_02_no_zombies] START"
cd "$PROJECT_DIR"

# Uruchom symulacje na 10s z szybka skala czasu (duzo klientow = duzo forkow)
./kierownik -t 10 -s 20 -n 8 -o 8 -c 12 < /dev/null > /dev/null 2>&1 &
KIE_PID=$!
sleep 2

# Monitoruj zombie co 0.5s przez 7s (14 probek)
MAX_ZOMBIES=0
ZOMBIE_DETECTED=0
MAX_ACTIVE=0
TOTAL_ENTERED_MAX=0
for _ in $(seq 1 14); do
    Z=$(count_zombies)
    [[ $Z -gt $MAX_ZOMBIES ]] && MAX_ZOMBIES=$Z
    [[ $Z -gt 0 ]] && ZOMBIE_DETECTED=$((ZOMBIE_DETECTED+1))
    # Sprawdz active_customers z SHM
    AC=$(shm_val active_customers)
    [[ -n "$AC" && $AC -gt $MAX_ACTIVE ]] && MAX_ACTIVE=$AC
    TE=$(shm_val total_customers_entered)
    [[ -n "$TE" && $TE -gt $TOTAL_ENTERED_MAX ]] && TOTAL_ENTERED_MAX=$TE
    sleep 0.5
done

# CHECK 1: Brak zombie podczas symulacji
[[ $ZOMBIE_DETECTED -eq 0 ]] && ok "brak zombie podczas symulacji" || fail "zombie wykryto $ZOMBIE_DETECTED razy (max=$MAX_ZOMBIES)"

# CHECK 2: Klienci sie generowali (weryfikacja z SHM)
[[ $TOTAL_ENTERED_MAX -gt 2 ]] && ok "klienci wchodzili do sklepu ($TOTAL_ENTERED_MAX total, max_active=$MAX_ACTIVE)" || fail "za malo klientow (total=$TOTAL_ENTERED_MAX)"

# Czekaj na zakonczenie
W=0; while kill -0 "$KIE_PID" 2>/dev/null && [[ $W -lt 30 ]]; do sleep 0.5; W=$((W+1)); done
! kill -0 "$KIE_PID" 2>/dev/null && ok "kierownik zakonczyl sie" || { fail "timeout"; kill -9 "$KIE_PID" 2>/dev/null; wait "$KIE_PID" 2>/dev/null || true; }
sleep 2

# CHECK 3: Brak zombie po zakonczeniu
Z=$(count_zombies)
[[ $Z -eq 0 ]] && ok "brak zombie po zakonczeniu" || fail "$Z zombie procesow"

# CHECK 4: Brak zywych procesow
REM=$(count_procs)
[[ $REM -eq 0 ]] && ok "procesy wyczyszczone" || fail "$REM procesow zostalo"

echo ""; [[ $FAIL -eq 0 ]] && echo "[test_02_no_zombies] PASS ($PASS/$((PASS+FAIL)))" && exit 0
echo "[test_02_no_zombies] FAIL ($PASS/$((PASS+FAIL)))"; exit 1
