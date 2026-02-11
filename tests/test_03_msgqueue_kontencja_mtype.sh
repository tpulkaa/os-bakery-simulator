#!/bin/bash
# ===========================================================================
# Test 03: Piekarz → Klienci – rywalizacja o msgrcv na jednym mtype
# ===========================================================================
#
# CEL:
#   Testuje kolejke komunikatow podajnikow gdy wielu klientow rywalizuje
#   o ten sam produkt (mtype = 1). Piekarz produkuje tylko Bulki,
#   kazdy klient chce kupic 1-3 Bulki. Kazda bulka to 1 msgsnd(),
#   kazdy zakup to 1 msgrcv() — sprawdzamy czy wiadomosci nie sa
#   gubione ani duplikowane.
#
# EDGE CASE:
#   1 produkt, wielu klientow — wysoka kontencja na msgrcv() z tym
#   samym mtype=1. Wiele procesow jednoczesnie wywoluje msgrcv()
#   na tej samej kolejce. Sprawdzamy:
#   - baker_produced >= customers_served (nie mozna sprzedac wiecej
#     niz wyprodukowano — brak duplikacji wiadomosci)
#   - customers_served > 0 (wiadomosci dochodza mimo kontencji)
#   - brak zakleszczenia (msgrcv IPC_NOWAIT nie blokuje)
#
# TESTOWANE IPC:
#   - Kolejka komunikatow (msg queue) — msgsnd/msgrcv
#   - Filtrowanie mtype: wszyscy klienci rywalizuja o mtype=1
#   - IPC_NOWAIT w msgrcv — brak blokowania na pustej kolejce
#   - Guard semaphore SEM_GUARD_CONV — backpressure na kolejke
#
# PARAMETRY:
#   -t 12 -s 25 -n 10 -p 1 -o 8 -c 12 (1 produkt, duzo klientow)
#
# WNIOSKI:
#   Jesli baker_produced >= customers_served, to kazdy msgrcv() pobral
#   dokladnie 1 wiadomosc z kolejki (brak duplikacji). Jesli served > 0,
#   to msgsnd/msgrcv poprawnie przekazuja komunikaty mimo rywalizacji
#   wielu procesow. Brak zakleszczenia = IPC_NOWAIT + guard semaphore
#   dzialaja poprawnie pod obciazeniem.
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

echo "[test_03_msgqueue_kontencja_mtype] START"
cd "$PROJECT_DIR"

# 1 produkt (Bulka), duzo klientow, szybki czas
./kierownik -t 12 -s 25 -n 10 -p 1 -o 8 -c 12 < /dev/null > /dev/null 2>&1 &
KIE_PID=$!
sleep 3

# CHECK 1: Symulacja dziala
CNT=$(count_procs)
[[ $CNT -ge 4 ]] && ok "symulacja dziala ($CNT procesow)" || fail "za malo procesow ($CNT)"

# Monitoruj produkcje i sprzedaz
MAX_PRODUCED=0
MAX_SERVED=0
PRODUCED_GROWS=0
SERVED_GROWS=0
PREV_P=0; PREV_S=0

for _ in $(seq 1 12); do
    P=$(shm_val baker_produced_total)
    S=$(shm_val customers_served)
    [[ -n "$P" && "$P" -gt "$PREV_P" ]] && PRODUCED_GROWS=$((PRODUCED_GROWS + 1))
    [[ -n "$S" && "$S" -gt "$PREV_S" ]] && SERVED_GROWS=$((SERVED_GROWS + 1))
    [[ -n "$P" && "$P" -gt "$MAX_PRODUCED" ]] && MAX_PRODUCED=$P
    [[ -n "$S" && "$S" -gt "$MAX_SERVED" ]] && MAX_SERVED=$S
    PREV_P=${P:-0}; PREV_S=${S:-0}
    sleep 0.5
done

echo "  INFO: produced=$MAX_PRODUCED, served=$MAX_SERVED (w trakcie)"

# CHECK 2: Piekarz produkuje (msgsnd dziala)
[[ $PRODUCED_GROWS -gt 0 ]] \
    && ok "msgsnd: piekarz produkuje bulki ($MAX_PRODUCED szt., wzrost $PRODUCED_GROWS razy)" \
    || fail "piekarz nie produkuje — msgsnd nie dziala"

# CHECK 3: Klienci kupuja (msgrcv dziala mimo kontencji)
[[ $SERVED_GROWS -gt 0 ]] \
    && ok "msgrcv: klienci kupuja mimo rywalizacji o mtype=1 ($MAX_SERVED obsluzonych)" \
    || fail "klienci nie kupuja — msgrcv nie dziala pod kontencja"

# Czekaj na zakonczenie
W=0; while kill -0 "$KIE_PID" 2>/dev/null && [[ $W -lt 40 ]]; do sleep 0.5; W=$((W+1)); done
if ! kill -0 "$KIE_PID" 2>/dev/null; then
    ok "brak zakleszczenia — symulacja zakonczyla sie"
else
    fail "ZAKLESZCZENIE — msgrcv moglby blokowac"
    kill -INT "$KIE_PID" 2>/dev/null; sleep 2
    kill -9 "$KIE_PID" 2>/dev/null; wait "$KIE_PID" 2>/dev/null || true
    for name in klient kasjer piekarz; do pkill -9 -x "$name" 2>/dev/null || true; done
fi
sleep 2

# Koncowe statystyki z raportu
FINAL_PRODUCED=0; FINAL_SERVED=0; FINAL_TOTAL=0
if [[ -f "$PROJECT_DIR/logs/raport.txt" ]]; then
    FINAL_TOTAL=$(grep 'Laczna liczba klientow' "$PROJECT_DIR/logs/raport.txt" 2>/dev/null | grep -oE '[0-9]+' | tail -1)
    FINAL_SERVED=$(grep 'Obsluzonych (paragon)' "$PROJECT_DIR/logs/raport.txt" 2>/dev/null | grep -oE '[0-9]+' | tail -1)
    # Szukaj linii z produkcja Bulka
    FINAL_PRODUCED=$(grep -A1 'PRODUKCJA PIEKARZA' "$PROJECT_DIR/logs/raport.txt" 2>/dev/null | grep -oE '[0-9]+ szt' | grep -oE '[0-9]+' | head -1)
fi
[[ -z "$FINAL_PRODUCED" ]] && FINAL_PRODUCED=$MAX_PRODUCED
[[ -z "$FINAL_SERVED" ]] && FINAL_SERVED=$MAX_SERVED
[[ -z "$FINAL_TOTAL" ]] && FINAL_TOTAL=0

echo "  INFO: KONCOWE — produced=$FINAL_PRODUCED, served=$FINAL_SERVED, total=$FINAL_TOTAL"

# CHECK 4: Nie sprzedano wiecej niz wyprodukowano (brak duplikacji wiadomosci)
if [[ "$FINAL_PRODUCED" -gt 0 ]]; then
    [[ "$FINAL_SERVED" -le "$FINAL_PRODUCED" ]] \
        && ok "brak duplikacji: served=$FINAL_SERVED <= produced=$FINAL_PRODUCED" \
        || fail "DUPLIKACJA msgrcv: served=$FINAL_SERVED > produced=$FINAL_PRODUCED!"
fi

# CHECK 5: Ktos zostal obsluzony
[[ "$FINAL_SERVED" -gt 0 ]] \
    && ok "komunikacja piekarz->klient dziala: $FINAL_SERVED klientow obsluzonych" \
    || fail "nikt nie obsluzony — komunikacja piekarz->klient zerwana"

# CHECK 6: Procesy i IPC czyste
REM=$(count_procs)
[[ $REM -eq 0 ]] && ok "procesy wyczyszczone" || fail "$REM procesow zostalo"
SHM=$(our_shm); SEM=$(our_sem); MSG=$(our_msg)
[[ $SHM -eq 0 && $SEM -eq 0 && $MSG -eq 0 ]] && ok "IPC czyste" || fail "IPC: shm=$SHM sem=$SEM msg=$MSG"

echo ""
[[ $FAIL -eq 0 ]] && echo "[test_03_msgqueue_kontencja_mtype] PASS ($PASS/$((PASS+FAIL)))" && exit 0
echo "[test_03_msgqueue_kontencja_mtype] FAIL ($PASS/$((PASS+FAIL)))"; exit 1
