#!/bin/bash
# ===========================================================================
# Test 05: SEM_UNDO – odpornosc semaforow na kill -9 klienta
# ===========================================================================
#
# CEL:
#   Testuje mechanizm SEM_UNDO w semaforach System V. Gdy klient jest
#   w sklepie (trzyma SEM_SHOP_ENTRY), zabicie go przez kill -9 powinno
#   automatycznie zwolnic semafor dzieki SEM_UNDO.
#
# EDGE CASE:
#   Maly sklep (N=3) — trzy sloty. Zabijamy klientow w sklepie.
#   Bez SEM_UNDO slot bylby stracony na zawsze i kolejni klienci
#   nie mogliby wejsc -> deadlock. Z SEM_UNDO kernel zwalnia semafor.
#
# TESTOWANE IPC:
#   - Semafory System V z flaga SEM_UNDO (semop)
#   - SEM_SHOP_ENTRY — semafor zliczajacy kontrolujacy wejscie
#   - SEM_SHM_MUTEX z SEM_UNDO — mutex na SHM
#   - Automatyczne cofniecie operacji semafora przez kernel po smierci procesu
#
# PARAMETRY:
#   -t 20 -s 30 -n 3 -o 8 -c 14 (maly sklep N=3, sredni czas)
#
# WNIOSKI:
#   Jesli po zabiciu klientow (kill -9) sem_shop_entry wraca do wyzszej
#   wartosci (sloty zwolnione) i nowi klienci nadal wchodza, to SEM_UNDO
#   dziala poprawnie. Brak zakleszczenia po kill -9 = semafory sa odporne
#   na nagla smierc procesow.
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

N=3  # Maly sklep — latwo zablokowac jesli SEM_UNDO nie dziala

echo "[test_05_sem_undo_kill] START"
cd "$PROJECT_DIR"

# Uruchom: maly sklep, srednia predkosc
./kierownik -t 20 -s 30 -n $N -o 8 -c 14 < /dev/null > /dev/null 2>&1 &
KIE_PID=$!
sleep 4

# CHECK 1: Sa klienci w sklepie
CIS=$(shm_val customers_in_shop)
SEM_BEFORE=$(shm_val sem_shop_entry)
echo "  INFO: przed zabiciem: customers_in_shop=$CIS, sem_shop_entry=$SEM_BEFORE"
[[ -n "$CIS" && "$CIS" -gt 0 ]] \
    && ok "klienci w sklepie ($CIS os.) — trzymaja semafor" \
    || fail "brak klientow w sklepie"

# Zabij 1-2 klientow SIGKILL (nieobslugiwalny — jedyny ratunek to SEM_UNDO)
KILLED=0
for KPID in $(pgrep -x klient 2>/dev/null | head -2); do
    kill -9 "$KPID" 2>/dev/null && KILLED=$((KILLED + 1))
done
echo "  INFO: zabito $KILLED klientow (kill -9)"
sleep 2

# CHECK 2: Semafor zostal zwolniony (SEM_UNDO zadziala)
SEM_AFTER=$(shm_val sem_shop_entry)
echo "  INFO: po zabiciu: sem_shop_entry zmienil sie z $SEM_BEFORE na $SEM_AFTER"
if [[ -n "$SEM_AFTER" && -n "$SEM_BEFORE" ]]; then
    # Po zabiciu klientow wolnych slotow powinno byc wiecej (lub tyle samo)
    [[ "$SEM_AFTER" -ge "$SEM_BEFORE" ]] \
        && ok "SEM_UNDO: sem_shop_entry wzroslo/utrzymane ($SEM_BEFORE -> $SEM_AFTER) — sloty zwolnione" \
        || fail "sem_shop_entry spadlo ($SEM_BEFORE -> $SEM_AFTER) — SEM_UNDO nie dziala?"
fi

# CHECK 3: Nowi klienci nadal wchodza po zabiciu starych
TOTAL_BEFORE=$(shm_val total_customers_entered)
sleep 3
TOTAL_AFTER=$(shm_val total_customers_entered)
echo "  INFO: total_entered: $TOTAL_BEFORE -> $TOTAL_AFTER"
if [[ -n "$TOTAL_BEFORE" && -n "$TOTAL_AFTER" ]]; then
    [[ "$TOTAL_AFTER" -gt "$TOTAL_BEFORE" ]] \
        && ok "nowi klienci wchodza po kill -9 (brak deadlocka semaforow)" \
        || fail "nowi klienci nie wchodza — mozliwy deadlock semafora"
fi

# CHECK 4: Symulacja nie zakleszcza sie
W=0; while kill -0 "$KIE_PID" 2>/dev/null && [[ $W -lt 40 ]]; do sleep 0.5; W=$((W+1)); done
if ! kill -0 "$KIE_PID" 2>/dev/null; then
    ok "symulacja zakonczyla sie (SEM_UNDO zapobiegl deadlockowi)"
else
    fail "ZAKLESZCZENIE — SEM_UNDO nie zwolnil semafora, sklep zablokowany"
    kill -INT "$KIE_PID" 2>/dev/null; sleep 2
    kill -9 "$KIE_PID" 2>/dev/null; wait "$KIE_PID" 2>/dev/null || true
    for name in klient kasjer piekarz; do pkill -9 -x "$name" 2>/dev/null || true; done
fi
sleep 2

# CHECK 5: Procesy i IPC czyste
REM=$(count_procs)
[[ $REM -eq 0 ]] && ok "procesy wyczyszczone" || fail "$REM procesow zostalo"
SHM=$(our_shm); SEM=$(our_sem); MSG=$(our_msg)
[[ $SHM -eq 0 && $SEM -eq 0 && $MSG -eq 0 ]] && ok "IPC czyste" || fail "IPC: shm=$SHM sem=$SEM msg=$MSG"

echo ""
[[ $FAIL -eq 0 ]] && echo "[test_05_sem_undo_kill] PASS ($PASS/$((PASS+FAIL)))" && exit 0
echo "[test_05_sem_undo_kill] FAIL ($PASS/$((PASS+FAIL)))"; exit 1
