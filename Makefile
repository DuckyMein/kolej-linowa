# ============================================
# KOLEJ KRZESEŁKOWA - MAKEFILE
# ============================================

CC = gcc
CFLAGS = -Wall -Wextra -g -pedantic
LDFLAGS = 

# Pliki nagłówkowe (zależności)
HEADERS = config.h types.h ipc.h utils.h

# Programy do zbudowania
PROGRAMS = main kasjer bramka pracownik1 pracownik2 generator klient wyciag

# Moduły wspólne (kompilowane do .o)
COMMON_OBJ = ipc.o utils.o

# ============================================
# GŁÓWNE TARGETY
# ============================================

all: $(PROGRAMS)
	@echo "=== Kompilacja zakończona ==="
	@echo "Uruchom: ./main [N] [czas_symulacji]"
	@echo "  N - limit osób na terenie (domyślnie 100)"
	@echo "  czas - czas symulacji w sekundach (domyślnie 300)"

# ============================================
# PROGRAMY WYKONYWALNE
# ============================================

main: main.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

kasjer: kasjer.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

bramka: bramka.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

pracownik1: pracownik1.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

pracownik2: pracownik2.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

generator: generator.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

klient: klient.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

wyciag: wyciag.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ============================================
# PLIKI OBIEKTOWE
# ============================================

main.o: main.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

kasjer.o: kasjer.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

bramka.o: bramka.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

pracownik1.o: pracownik1.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

pracownik2.o: pracownik2.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

generator.o: generator.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

klient.o: klient.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

wyciag.o: wyciag.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

ipc.o: ipc.c ipc.h config.h types.h
	$(CC) $(CFLAGS) -c $< -o $@

utils.o: utils.c utils.h config.h types.h
	$(CC) $(CFLAGS) -c $< -o $@

# ============================================
# CZYSZCZENIE
# ============================================

clean:
	rm -f *.o $(PROGRAMS)
	@echo "Wyczyszczono pliki obiektowe i wykonywalne"

cleanall: clean
	rm -f output/*.txt
	@echo "Wyczyszczono również raporty"

# ============================================
# POMOCNICZE
# ============================================

# Usuń zasoby IPC (po awarii)
cleanipc:
	@echo "Usuwanie zasobów IPC użytkownika..."
	-ipcrm -a 2>/dev/null || true
	@echo "Gotowe"

# Pokaż zasoby IPC
showipc:
	@echo "=== SEMAFORY ==="
	@ipcs -s
	@echo "=== PAMIĘĆ WSPÓŁDZIELONA ==="
	@ipcs -m
	@echo "=== KOLEJKI KOMUNIKATÓW ==="
	@ipcs -q

# Uruchomienie testowe (krótkie)
test: all
	./main 10 30

# Uruchomienie z domyślnymi parametrami
run: all
	./main

# ============================================
# PHONY TARGETS
# ============================================

.PHONY: all clean cleanall cleanipc showipc test run
