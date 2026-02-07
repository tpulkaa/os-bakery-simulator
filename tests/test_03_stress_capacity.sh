#!/bin/bash
# ===========================================================================
# Test 03: Stress test + weryfikacja pojemnosci sklepu
# Sprawdza:
#   - customers_in_shop NIGDY nie przekracza max_customers (N)
#   - sem_shop_entry NIGDY nie jest ujemny
#   - pod obciazeniem procesy sie generuja
#   - brak zakleszczenia (program konczy sie sam)
#   - IPC i procesy posprzatane po zakonczeniu
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

# Odczyt wartosci z check_shm
shm_val() { "$PROJECT_DIR/check_shm" 2>/dev/null | grep "^$1=" | cut -d= -f2; }

N=4  # Maly sklep zeby wymusic kolejke

echo "[test_03_stress_capacity] START"
cd "$PROJECT_DIR"

# Uruchom: szybki czas, N=4, 12s timeout
./kierownik -t 12 -s 15 -n $N -o 8 -c 16 < /dev/null > /dev/null 2>&1 &
KIE_PID=$!
sleep 3

# === MONITOROWANIE ~8s ===
MAX_PROCS=0
MAX_IN_SHOP=0
CAPACITY_VIOLATED=0
SEM_NEGATIVE=0
SAMPLES=0
MAX_TOTAL_ENTERED=0

for _ in $(seq 1 16); do
    C=$(count_procs)
    [[ $C -gt $MAX_PROCS ]] && MAX_PROCS=$C

    # Odczytaj stan z SHM
    CIS=$(shm_val customers_in_shop)
    SEM=$(shm_val sem_shop_entry)
    TE=$(shm_val total_customers_entered)

    if [[ -n "$CIS" ]]; then
        [[ $CIS -gt $MAX_IN_SHOP ]] && MAX_IN_SHOP=$CIS
        [[ $CIS -gt $N ]] && CAPACITY_VIOLATED=$((CAPACITY_VIOLATED + 1))
    fi
    if [[ -n "$SEM" ]]; then
        [[ $SEM -lt 0 ]] && SEM_NEGATIVE=$((SEM_NEGATIVE + 1))
    fi
    if [[ -n "$TE" && $TE -gt $MAX_TOTAL_ENTERED ]]; then
        MAX_TOTAL_ENTERED=$TE
    fi

    SAMPLES=$((SAMPLES + 1))
    sleep 0.5
done

TOTAL_ENTERED_END=$(shm_val total_customers_entered)
[[ -z "$TOTAL_ENTERED_END" || "$TOTAL_ENTERED_END" -lt "$MAX_TOTAL_ENTERED" ]] && TOTAL_ENTERED_END=$MAX_TOTAL_ENTERED

# CHECK 1: Pojemnosc sklepu NIGDY nie przekroczona
[[ $CAPACITY_VIOLATED -eq 0 ]] && ok "pojemnosc N=$N nigdy nie przekroczona (max_in_shop=$MAX_IN_SHOP)" || fail "PRZEKROCZONO POJEMNOSC $CAPACITY_VIOLATED/$SAMPLES razy (max_in_shop=$MAX_IN_SHOP, limit=$N)"

# CHECK 2: Semafor nigdy ujemny
[[ $SEM_NEGATIVE -eq 0 ]] && ok "sem_shop_entry >= 0 zawsze" || fail "sem_shop_entry < 0 wykryto $SEM_NEGATIVE razy"

# CHECK 3: Klienci sie generowali
ENTERED=$TOTAL_ENTERED_END
[[ $ENTERED -gt 2 ]] && ok "klienci wchodzili do sklepu ($ENTERED total)" || fail "za malo klientow ($ENTERED)"

# CHECK 4: Procesy sie pojawily
[[ $MAX_PROCS -gt 4 ]] && ok "procesy pod obciazeniem (max=$MAX_PROCS)" || fail "brak klientow (max=$MAX_PROCS)"

# CHECK 5: Brak zakleszczenia
W=0; while kill -0 "$KIE_PID" 2>/dev/null && [[ $W -lt 40 ]]; do sleep 0.5; W=$((W+1)); done
if ! kill -0 "$KIE_PID" 2>/dev/null; then
    ok "brak zakleszczenia - program zakonczyl sie"
else
    fail "ZAKLESZCZENIE - program nie zakonczyl sie w 20s"
    kill -INT "$KIE_PID" 2>/dev/null; sleep 2
    kill -9 "$KIE_PID" 2>/dev/null; wait "$KIE_PID" 2>/dev/null || true
    for name in klient kasjer piekarz; do pkill -9 -x "$name" 2>/dev/null || true; done
fi
sleep 2

# CHECK 6: Procesy wyczyszczone
REM=$(count_procs)
[[ $REM -eq 0 ]] && ok "procesy wyczyszczone" || fail "$REM procesow zostalo"

# CHECK 7: IPC czyste
SHM=$(our_shm); SEM=$(our_sem); MSG=$(our_msg)
[[ $SHM -eq 0 && $SEM -eq 0 && $MSG -eq 0 ]] && ok "IPC czyste" || fail "IPC: shm=$SHM sem=$SEM msg=$MSG"

echo ""; [[ $FAIL -eq 0 ]] && echo "[test_03_stress_capacity] PASS ($PASS/$((PASS+FAIL)))" && exit 0
echo "[test_03_stress_capacity] FAIL ($PASS/$((PASS+FAIL)))"; exit 1
