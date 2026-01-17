# ============================================
# Makefile - Ciastkarnia (Temat 15)
# Systemy Operacyjne - Projekt
# ============================================

CC      = gcc
CFLAGS  = -Wall -Wextra -pedantic -std=c11 -D_GNU_SOURCE
LDFLAGS = -lpthread

# Katalog zrodlowy
SRCDIR = src

# Pliki obiektowe wspoldzielone (linkowane do kazdego programu)
COMMON_SRCS = $(SRCDIR)/error_handler.c $(SRCDIR)/ipc_utils.c $(SRCDIR)/logger.c
COMMON_OBJS = $(COMMON_SRCS:.c=.o)

# Programy docelowe (w katalogu glownym projektu)
TARGETS = kierownik piekarz kasjer klient check_shm

# ============================================
#  Reguly budowania
# ============================================

.PHONY: all clean run help test

all: $(TARGETS)
	@echo ""
	@echo "  Kompilacja zakonczona pomyslnie!"
	@echo "  Uruchom symulacje: ./kierownik"
	@echo "  Lub z parametrami: ./kierownik -n 10 -p 12 -s 100 -o 8 -c 16"
	@echo ""

# --- Kierownik (manager) ---
kierownik: $(SRCDIR)/kierownik.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# --- Piekarz (baker) ---
piekarz: $(SRCDIR)/piekarz.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# --- Kasjer (cashier) ---
kasjer: $(SRCDIR)/kasjer.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# --- Klient (customer) ---
klient: $(SRCDIR)/klient.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# --- Check SHM (narzedzie diagnostyczne dla testow) ---
check_shm: $(SRCDIR)/check_shm.o
	$(CC) $(CFLAGS) -o $@ $^

# --- Kompilacja plikow .c -> .o ---
$(SRCDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/common.h $(SRCDIR)/error_handler.h $(SRCDIR)/ipc_utils.h $(SRCDIR)/logger.h
	$(CC) $(CFLAGS) -c -o $@ $<

# ============================================
#  Czyszczenie
# ============================================

clean:
	rm -f $(SRCDIR)/*.o $(TARGETS)
	rm -f ciastkarnia.key
	rm -f /tmp/ciastkarnia_cmd.fifo
	rm -rf logs/
	@echo "Wyczyszczono."

# ============================================
#  Uruchamianie
# ============================================

run: all
	@mkdir -p logs
	./kierownik

# Uruchomienie z krotszym czasem (szybka symulacja)
run-fast: all
	@mkdir -p logs
	./kierownik -s 50 -o 8 -c 12

# ============================================
#  Pomoc
# ============================================

help:
	@echo ""
	@echo "  Dostepne cele:"
	@echo "    make         - kompiluje wszystkie programy"
	@echo "    make clean   - czysci pliki obiektowe i binaria"
	@echo "    make run     - kompiluje i uruchamia symulacje"
	@echo "    make run-fast - szybka symulacja (4 godziny, 50ms/min)"
	@echo "    make test    - uruchamia testy integracyjne"
	@echo "    make help    - wyswietla te informacje"
	@echo ""
	@echo "  Sterowanie podczas symulacji:"
	@echo "    echo 'inwentaryzacja' > /tmp/ciastkarnia_cmd.fifo"
	@echo "    echo 'ewakuacja' > /tmp/ciastkarnia_cmd.fifo"
	@echo ""

# ============================================
#  Testy integracyjne
# ============================================

test: all
	@bash tests/run_tests.sh
