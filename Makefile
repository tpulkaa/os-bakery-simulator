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
COMMON_SRCS = $(SRCDIR)/error_handler.c $(SRCDIR)/logger.c
COMMON_OBJS = $(COMMON_SRCS:.c=.o)

.PHONY: all clean help

all:
	@echo "TODO: pliki zrodlowe w trakcie tworzenia"

# --- Kompilacja plikow .c -> .o ---
$(SRCDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(SRCDIR)/*.o
	@echo "Wyczyszczono."

help:
	@echo ""
	@echo "  Dostepne cele:"
	@echo "    make         - kompiluje wszystkie programy"
	@echo "    make clean   - czysci pliki obiektowe"
	@echo "    make help    - wyswietla te informacje"
	@echo ""
