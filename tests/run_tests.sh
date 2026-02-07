#!/bin/bash
# ===========================================================================
# Runner: uruchom wszystkie testy sekwencyjnie
# Uzycie: bash tests/run_tests.sh
# ===========================================================================
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_DIR"

GREEN='\033[0;32m'; RED='\033[0;31m'; CYAN='\033[0;36m'; NC='\033[0m'

# Buduj projekt
echo -e "${CYAN}===== Budowanie projektu =====${NC}"
make clean && make
if [[ $? -ne 0 ]]; then
    echo -e "${RED}Blad kompilacji! Przerywam testy.${NC}"
    exit 1
fi
echo ""

# Upewnij sie, ze nie ma starych procesow / IPC
cleanup_between() {
    for name in kierownik piekarz kasjer klient; do
        pkill -9 -x "$name" 2>/dev/null || true
    done
    sleep 1
    # Usun IPC (ostrożnie — tylko nasze)
    MYUSER=$(whoami)
    for id in $(ipcs -m 2>/dev/null | grep "^m.*$MYUSER" | awk '{print $2}'); do
        ipcrm -m "$id" 2>/dev/null || true
    done
    for id in $(ipcs -s 2>/dev/null | grep "^s.*$MYUSER" | awk '{print $2}'); do
        ipcrm -s "$id" 2>/dev/null || true
    done
    for id in $(ipcs -q 2>/dev/null | grep "^q.*$MYUSER" | awk '{print $2}'); do
        ipcrm -q "$id" 2>/dev/null || true
    done
    rm -f /tmp/ciastkarnia_cmd.fifo
    rm -rf "$PROJECT_DIR/logs" 2>/dev/null || true
}

TESTS=(
    "test_01_startup_cleanup.sh"
    "test_02_no_zombies.sh"
    "test_03_stress_capacity.sh"
    "test_04_evacuation.sh"
    "test_05_sigint_cleanup.sh"
)

TOTAL=0; PASSED=0; FAILED=0
RESULTS=()

echo -e "${CYAN}===== Uruchamianie testow =====${NC}"
echo ""

for t in "${TESTS[@]}"; do
    TOTAL=$((TOTAL + 1))
    TPATH="$SCRIPT_DIR/$t"
    if [[ ! -f "$TPATH" ]]; then
        echo -e "${RED}✗ $t — plik nie istnieje${NC}"
        FAILED=$((FAILED + 1))
        RESULTS+=("FAIL $t")
        continue
    fi

    cleanup_between
    echo -e "${CYAN}--- [$TOTAL] $t ---${NC}"
    bash "$TPATH"
    RC=$?
    echo ""

    if [[ $RC -eq 0 ]]; then
        PASSED=$((PASSED + 1))
        RESULTS+=("PASS $t")
    else
        FAILED=$((FAILED + 1))
        RESULTS+=("FAIL $t")
    fi
done

# Sprzatanie koncowe
cleanup_between

echo -e "${CYAN}===== Podsumowanie =====${NC}"
for r in "${RESULTS[@]}"; do
    STATUS="${r%% *}"
    NAME="${r#* }"
    if [[ "$STATUS" == "PASS" ]]; then
        echo -e "  ${GREEN}✓ $NAME${NC}"
    else
        echo -e "  ${RED}✗ $NAME${NC}"
    fi
done
echo ""
echo -e "Wynik: ${PASSED}/${TOTAL} testow przeszlo"

if [[ $FAILED -eq 0 ]]; then
    echo -e "${GREEN}Wszystkie testy przeszly!${NC}"
    exit 0
else
    echo -e "${RED}${FAILED} test(ow) nie przeszlo!${NC}"
    exit 1
fi
