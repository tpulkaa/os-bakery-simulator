#!/bin/bash
# Test obrony: czy zabijanie procesow powoduje deadlock/zombie/wyciek IPC

echo "=== KILL TEST: Starting simulation ==="
# -s 200 = 200ms per sim min → shop opens at Tp+30 = 6s real
# -n 200 = lots of customers so they overlap and pgrep finds them
# -c 12  = close at 12:00 → 4h sim = 48s real total
./kierownik -n 100 -p 12 -s 200 -o 8 -c 12 -t 5000 > /tmp/sim_kill.log 2>&1 &
SIM_PID=$!
echo "Simulation PID: $SIM_PID"

# Wait for shop to open (6s) + first customers to appear (~1s)
sleep 8

echo "--- Processes before kill ---"
ps -o pid,comm | grep -E "kierownik|piekarz|kasjer|klient" | grep -v grep

# TEST 1: Kill klient (SIGTERM)
KLIENT_PID=$(pgrep klient 2>/dev/null | head -1)
if [ -n "$KLIENT_PID" ]; then
    echo ""
    echo "=== TEST 1: kill klient PID $KLIENT_PID (SIGTERM) ==="
    kill "$KLIENT_PID"
    sleep 1
    if kill -0 "$SIM_PID" 2>/dev/null; then
        echo "PASS: Simulation still running after klient kill"
    else
        echo "FAIL: Simulation died after klient kill!"
    fi
else
    echo "SKIP: No klient found"
fi

# TEST 2: Kill kasjer (SIGTERM)
KASJER_PID=$(pgrep kasjer 2>/dev/null | head -1)
if [ -n "$KASJER_PID" ]; then
    echo ""
    echo "=== TEST 2: kill kasjer PID $KASJER_PID (SIGTERM) ==="
    kill "$KASJER_PID"
    sleep 1
    if kill -0 "$SIM_PID" 2>/dev/null; then
        echo "PASS: Simulation still running after kasjer kill"
    else
        echo "FAIL: Simulation died after kasjer kill!"
    fi
else
    echo "SKIP: No kasjer found"
fi

# TEST 3: Kill -9 klient (SIGKILL - tests SEM_UNDO)
KLIENT2_PID=$(pgrep klient 2>/dev/null | head -1)
if [ -n "$KLIENT2_PID" ]; then
    echo ""
    echo "=== TEST 3: kill -9 klient PID $KLIENT2_PID (SIGKILL - SEM_UNDO test) ==="
    kill -9 "$KLIENT2_PID"
    sleep 1
    if kill -0 "$SIM_PID" 2>/dev/null; then
        echo "PASS: Simulation still running after SIGKILL"
    else
        echo "FAIL: Simulation died after SIGKILL!"
    fi
else
    echo "SKIP: No klient found"
fi

echo ""
echo "--- Processes after kills ---"
ps -o pid,comm | grep -E "kierownik|piekarz|kasjer|klient" | grep -v grep || echo "(no simulation processes)"

# Wait for simulation to end
echo ""
echo "=== Waiting for simulation to finish ==="
wait "$SIM_PID" 2>/dev/null || true

echo ""
echo "=== FINAL RESULTS ==="

echo "Zombies:"
ZOMBIES=$(ps -o pid,stat,comm 2>/dev/null | grep Z | grep -v grep || true)
if [ -z "$ZOMBIES" ]; then
    echo "  PASS: No zombies!"
else
    echo "  FAIL: $ZOMBIES"
fi

echo "IPC resources:"
IPC=$(ipcs -a 2>/dev/null | grep "$(whoami)" || true)
if [ -z "$IPC" ]; then
    echo "  PASS: IPC clean!"
else
    echo "  FAIL: Leftover IPC:"
    echo "  $IPC"
fi

echo "Leftover processes:"
LEFTOVER=$(ps aux | grep -E "./kierownik|./piekarz|./kasjer|./klient" | grep -v grep || true)
if [ -z "$LEFTOVER" ]; then
    echo "  PASS: No leftover processes!"
else
    echo "  FAIL: $LEFTOVER"
fi

echo ""
echo "=== Log excerpts (kill detection) ==="
grep -E "UWAGA|zakonczyl nieoczekiwanie" /tmp/sim_kill.log || echo "(no kill detection messages)"
