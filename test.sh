#!/bin/bash
# Test skrypt dla symulacji kolei krzesełkowej
# Testuje funkcjonalność STOP/START (SIGUSR1/SIGUSR2)

echo "=== TEST KOLEI KRZESEŁKOWEJ ==="
echo ""

# Kompilacja
echo "1. Kompilacja..."
make clean > /dev/null 2>&1
make > /dev/null 2>&1
if [ $? -ne 0 ]; then
    echo "BŁĄD: Kompilacja nie powiodła się!"
    exit 1
fi
echo "   OK"

# Test 1: Podstawowe uruchomienie
echo ""
echo "2. Test podstawowy (15 sekund)..."
timeout 20 ./main 20 15 > /tmp/test1.log 2>&1
if [ $? -eq 0 ]; then
    echo "   OK"
    KLIENCI=$(grep "Łączna liczba klientów" output/raport_dzienny.txt | awk '{print $4}')
    echo "   Klienci obsłużeni: $KLIENCI"
else
    echo "   BŁĄD lub timeout"
fi

# Test 2: Test awarii STOP/START
echo ""
echo "3. Test awarii STOP/START..."
./main 20 30 > /tmp/test2.log 2>&1 &
MAIN_PID=$!
sleep 3

# Wyślij STOP
echo "   Wysyłam SIGUSR1 (STOP)..."
kill -USR1 $MAIN_PID 2>/dev/null
sleep 2

# Wyślij START
echo "   Wysyłam SIGUSR2 (START)..."
kill -USR2 $MAIN_PID 2>/dev/null
sleep 3

# Zakończ
echo "   Kończę symulację..."
kill -TERM $MAIN_PID 2>/dev/null
wait $MAIN_PID 2>/dev/null

if grep -q "AWARIA" /tmp/test2.log || grep -q "STOP" /tmp/test2.log; then
    echo "   OK - Awaria została obsłużona"
else
    echo "   UWAGA - Brak logów awarii"
fi

# Test 3: Test Ctrl+C (SIGINT)
echo ""
echo "4. Test przerwania (SIGINT)..."
./main 20 60 > /tmp/test3.log 2>&1 &
MAIN_PID=$!
sleep 3
kill -INT $MAIN_PID 2>/dev/null
wait $MAIN_PID 2>/dev/null

if grep -q "ZAKOŃCZONA" /tmp/test3.log; then
    echo "   OK - Graceful shutdown"
else
    echo "   UWAGA - Sprawdź log"
fi

# Test 4: Sprawdzenie czy IPC zostało wyczyszczone
echo ""
echo "5. Sprawdzanie zasobów IPC..."
SEM_COUNT=$(ipcs -s 2>/dev/null | grep -c "^0x")
SHM_COUNT=$(ipcs -m 2>/dev/null | grep -c "^0x")
MSG_COUNT=$(ipcs -q 2>/dev/null | grep -c "^0x")

if [ "$SEM_COUNT" -eq 0 ] && [ "$SHM_COUNT" -eq 0 ] && [ "$MSG_COUNT" -eq 0 ]; then
    echo "   OK - Brak wycieków IPC"
else
    echo "   UWAGA: Pozostały zasoby IPC (sem=$SEM_COUNT, shm=$SHM_COUNT, msg=$MSG_COUNT)"
    echo "   Czyszczenie..."
    ipcrm -a 2>/dev/null
fi

# Podsumowanie
echo ""
echo "=== KONIEC TESTÓW ==="
echo ""
echo "Logi zapisane w:"
echo "  /tmp/test1.log - test podstawowy"
echo "  /tmp/test2.log - test awarii"
echo "  /tmp/test3.log - test przerwania"
echo ""
echo "Raport: output/raport_dzienny.txt"
