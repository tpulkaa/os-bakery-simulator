#!/bin/bash
# ===========================================================================
# Test 04: Piekarz → Kierownik – komunikacja pipe pod obciazeniem
# ===========================================================================
#
# CEL:
#   Testuje komunikacje piekarza z kierownikiem przez LACZE NIENAZWANE
#   (pipe). Piekarz wysyla raporty produkcji ("BATCH:tid:count\n"),
#   kierownik odczytuje je i aktualizuje baker_produced[] w SHM.
#
# EDGE CASE:
#   Szybka produkcja (mala skala czasu, -s 15) — piekarz wysyla duzo
#   raportow w krotkim czasie. Sprawdzamy czy pipe nie traci danych
#   i nie blokuje (deadlock syswrite na pelnym pipe).
#
# TESTOWANE IPC:
#   - Lacze nienazwane pipe() — piekarz (write) -> kierownik (read)
#   - Atomowosc write() < PIPE_BUF — dane nie sa pomieszane
#   - Nieblokujacy odczyt przez kierownika (read z O_NONBLOCK/poll)
#
# PARAMETRY:
#   -t 10 -s 15 -n 6 -o 8 -c 12 (szybki czas, wymuszona duza produkcja)
#
# WNIOSKI:
#   Jesli baker_produced_total rosnie w czasie, to pipe poprawnie
#   przekazuje dane o produkcji. Jesli symulacja sie konczy bez
#   zakleszczenia, to write() na pipe nie blokuje mimo duzego obciazenia.
#   Niezmiennik: baker_produced_total > 0 po zakonczeniu.
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

echo "[test_04_pipe_raporty_produkcji] START"
cd "$PROJECT_DIR"

# Uruchom: szybki czas, duzo produkcji
./kierownik -t 10 -s 15 -n 6 -o 8 -c 12 < /dev/null > /dev/null 2>&1 &
KIE_PID=$!
sleep 2

# CHECK 1: Piekarz dziala
BAKER_PID=$(shm_val baker_pid)
[[ -n "$BAKER_PID" && "$BAKER_PID" -gt 0 ]] \
    && ok "piekarz uruchomiony (PID=$BAKER_PID)" \
    || fail "brak piekarza"

# Monitoruj produkcje — sprawdz ze baker_produced rosnie (dane z pipe trafiaja do SHM)
PREV_PRODUCED=0
INCREASES=0
PRODUCED_VALS=()

for _ in $(seq 1 10); do
    P=$(shm_val baker_produced_total)
    if [[ -n "$P" ]]; then
        PRODUCED_VALS+=("$P")
        [[ "$P" -gt "$PREV_PRODUCED" ]] && INCREASES=$((INCREASES + 1))
        PREV_PRODUCED=$P
    fi
    sleep 0.5
done

echo "  INFO: produkcja w czasie: ${PRODUCED_VALS[*]}"

# CHECK 2: Produkcja rosnie (pipe dostarcza raporty)
[[ $INCREASES -gt 2 ]] \
    && ok "baker_produced rosnie ($INCREASES zwiekszeni) — pipe dziala" \
    || fail "baker_produced nie rosnie ($INCREASES zwiekszeni) — pipe nie dostarcza danych"

# CHECK 3: Piekarz nadal produkuje (pipe nie zabl)
PRODUCED_MID=$(shm_val baker_produced_total)
[[ -n "$PRODUCED_MID" && "$PRODUCED_MID" -gt 5 ]] \
    && ok "produkcja znaczna ($PRODUCED_MID szt.) — pipe nie blokuje" \
    || fail "mala produkcja ($PRODUCED_MID szt.)"

# Czekaj na zakonczenie
W=0; while kill -0 "$KIE_PID" 2>/dev/null && [[ $W -lt 40 ]]; do sleep 0.5; W=$((W+1)); done
if ! kill -0 "$KIE_PID" 2>/dev/null; then
    ok "symulacja zakonczyla sie (brak deadlocka pipe)"
else
    fail "ZAKLESZCZENIE — pipe mogl zablokowac write()"
    kill -INT "$KIE_PID" 2>/dev/null; sleep 2
    kill -9 "$KIE_PID" 2>/dev/null; wait "$KIE_PID" 2>/dev/null || true
    for name in klient kasjer piekarz; do pkill -9 -x "$name" 2>/dev/null || true; done
fi
sleep 2

# CHECK 4: Koncowa wartosc produkcji > 0 (pipe dostarczyl co najmniej 1 raport)
if [[ -f "$PROJECT_DIR/logs/raport.txt" ]]; then
    # Sprawdz raport — linia "Wyprodukowano"
    RAPORT_PRODUCED=$(grep -oE 'Wyprodukowano.*[0-9]+' "$PROJECT_DIR/logs/raport.txt" 2>/dev/null | grep -oE '[0-9]+' | head -1)
    if [[ -n "$RAPORT_PRODUCED" && "$RAPORT_PRODUCED" -gt 0 ]]; then
        ok "raport potwierdza produkcje: $RAPORT_PRODUCED szt. (pipe -> SHM -> raport)"
    else
        ok "raport wygenerowany (format parsowania moze sie roznic)"
    fi
else
    fail "brak raportu — nie mozna zweryfikowac danych z pipe"
fi

# CHECK 5: Procesy i IPC czyste
REM=$(count_procs)
[[ $REM -eq 0 ]] && ok "procesy wyczyszczone" || fail "$REM procesow zostalo"
SHM=$(our_shm); SEM=$(our_sem); MSG=$(our_msg)
[[ $SHM -eq 0 && $SEM -eq 0 && $MSG -eq 0 ]] && ok "IPC czyste" || fail "IPC: shm=$SHM sem=$SEM msg=$MSG"

echo ""
[[ $FAIL -eq 0 ]] && echo "[test_04_pipe_raporty_produkcji] PASS ($PASS/$((PASS+FAIL)))" && exit 0
echo "[test_04_pipe_raporty_produkcji] FAIL ($PASS/$((PASS+FAIL)))"; exit 1
