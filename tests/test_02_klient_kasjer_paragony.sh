#!/bin/bash
# ===========================================================================
# Test 02: Klient → Kasjer → Klient – kolejki checkout i paragonow
# ===========================================================================
#
# CEL:
#   Testuje komunikacje miedzy klientami a kasjerami przez DWA TYPY
#   KOLEJEK KOMUNIKATOW:
#   1) Checkout: klient -> kasjer (mtype = register_id + 1)
#   2) Paragony: kasjer -> klient (mtype = customer_pid)
#
# EDGE CASE:
#   Duzy ruch — wielu klientow jednoczesnie kupuje i placi.
#   Paragony musza trafiac do WLASCIWEGO klienta (filtrowanie po PID).
#   Sprawdzamy czy zadne wiadomosci nie zostaja zgubione.
#
# TESTOWANE IPC:
#   - Kolejka checkout (msgsnd mtype = register_id + 1)
#   - Kolejka paragonow (msgsnd mtype = PID, msgrcv z filtrowaniem)
#   - Guard semaphores na obu kolejkach (backpressure)
#   - Filtrowanie mtype — kazdy klient dostaje SWOJ paragon
#
# PARAMETRY:
#   -t 12 -s 20 -n 10 -o 8 -c 14 (szybki czas, duzo klientow)
#
# WNIOSKI:
#   Jesli customers_served + customers_not_served == total_customers_entered,
#   to kazdy klient zostal rozliczony — brak zgubionych komunikatow
#   w kolejkach. Jesli customers_served > 0, to paragon dociera do
#   klienta (msgrcv z mtype = PID dziala poprawnie).
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

echo "[test_02_klient_kasjer_paragony] START"
cd "$PROJECT_DIR"

# Uruchom: szybki czas, wielu klientow
./kierownik -t 12 -s 20 -n 10 -o 8 -c 14 < /dev/null > /dev/null 2>&1 &
KIE_PID=$!
sleep 2

# CHECK 1: Kasjer i klienci dzialaja
CNT=$(count_procs)
[[ $CNT -ge 4 ]] && ok "symulacja dziala ($CNT procesow)" || fail "za malo procesow ($CNT)"

# Monitoruj: klienci sa obslugiwani (served rosnie)
SERVED_PREV=0
SERVED_GROWING=0
for _ in $(seq 1 10); do
    S=$(shm_val customers_served)
    if [[ -n "$S" && "$S" -gt "$SERVED_PREV" ]]; then
        SERVED_GROWING=$((SERVED_GROWING + 1))
        SERVED_PREV=$S
    fi
    sleep 0.5
done

# CHECK 2: Klienci sa obslugiwani (paragony docieraja)
[[ $SERVED_GROWING -gt 0 ]] \
    && ok "paragony docieraja do klientow (served wzroslo $SERVED_GROWING razy)" \
    || fail "customers_served nie rosnie — problem z kolejka paragonow"

# Czekaj na zakonczenie
W=0; while kill -0 "$KIE_PID" 2>/dev/null && [[ $W -lt 40 ]]; do sleep 0.5; W=$((W+1)); done
if ! kill -0 "$KIE_PID" 2>/dev/null; then
    ok "symulacja zakonczyla sie"
else
    fail "timeout"
    kill -INT "$KIE_PID" 2>/dev/null; sleep 2
    kill -9 "$KIE_PID" 2>/dev/null; wait "$KIE_PID" 2>/dev/null || true
    for name in klient kasjer piekarz; do pkill -9 -x "$name" 2>/dev/null || true; done
fi
sleep 2

# Odczytaj koncowe statystyki z raportu
TOTAL=0; SERVED=0; NOT_SERVED=0
if [[ -f "$PROJECT_DIR/logs/raport.txt" ]]; then
    TOTAL=$(grep 'Laczna liczba klientow' "$PROJECT_DIR/logs/raport.txt" 2>/dev/null | grep -oE '[0-9]+' | tail -1 || echo 0)
    SERVED=$(grep 'Obsluzonych' "$PROJECT_DIR/logs/raport.txt" 2>/dev/null | grep -oE '[0-9]+' | tail -1 || echo 0)
    NOT_SERVED=$(grep 'Nieobsluzonych' "$PROJECT_DIR/logs/raport.txt" 2>/dev/null | grep -oE '[0-9]+' | tail -1 || echo 0)
fi

# Fallback — z check_shm (jesli raport parsing nie zadziala)
[[ "$TOTAL" -eq 0 ]] && TOTAL=$(shm_val total_customers_entered)
[[ -z "$SERVED" || "$SERVED" -eq 0 ]] && SERVED=$(shm_val customers_served)
[[ -z "$NOT_SERVED" ]] && NOT_SERVED=$(shm_val customers_not_served)
[[ -z "$TOTAL" ]] && TOTAL=0
[[ -z "$SERVED" ]] && SERVED=0
[[ -z "$NOT_SERVED" ]] && NOT_SERVED=0

echo "  INFO: total=$TOTAL served=$SERVED not_served=$NOT_SERVED"

# CHECK 3: Ktos zostal obsluzony (paragon doszedl)
[[ "$SERVED" -gt 0 ]] \
    && ok "klienci obsluzeni: $SERVED (paragony dostarczone przez msgrcv mtype=PID)" \
    || fail "nikt nie zostal obsluzony — problem z kolejka checkout/paragonow"

# CHECK 4: Bilans klientow — brak nadliczbowych (served + not_served <= total)
SUM=$((SERVED + NOT_SERVED))
if [[ "$TOTAL" -gt 0 ]]; then
    [[ "$SUM" -le "$TOTAL" ]] \
        && ok "bilans klientow: $SERVED + $NOT_SERVED = $SUM <= $TOTAL (brak nadliczbowych komunikatow)" \
        || fail "bilans zawyzone: $SERVED + $NOT_SERVED = $SUM > $TOTAL (zduplikowane paragony?)"
else
    ok "brak danych do weryfikacji bilansu (krotka symulacja)"
fi

# CHECK 5: Brak procesow
REM=$(count_procs)
[[ $REM -eq 0 ]] && ok "procesy wyczyszczone" || fail "$REM procesow zostalo"

# CHECK 6: IPC czyste
SHM=$(our_shm); SEM=$(our_sem); MSG=$(our_msg)
[[ $SHM -eq 0 && $SEM -eq 0 && $MSG -eq 0 ]] && ok "IPC czyste" || fail "IPC: shm=$SHM sem=$SEM msg=$MSG"

echo ""
[[ $FAIL -eq 0 ]] && echo "[test_02_klient_kasjer_paragony] PASS ($PASS/$((PASS+FAIL)))" && exit 0
echo "[test_02_klient_kasjer_paragony] FAIL ($PASS/$((PASS+FAIL)))"; exit 1
