/**
 * common.h - Wspolne definicje, stale i struktury danych
 * Projekt: Ciastkarnia (Temat 15) - Systemy Operacyjne
 *
 * Plik naglowkowy wspoldzielony przez wszystkie procesy symulacji.
 */

#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>

/*
 *  STALE KONFIGURACYJNE
 */

#define MAX_PRODUCTS        20   /* Maksymalna liczba produktow */
#define MAX_CUSTOMERS_TOTAL 5000 /* Maks. laczna liczba klientow w symulacji */
#define MAX_ACTIVE_CUST     300  /* Maks. procesow klientow jednoczesnie */
#define MAX_NAME_LEN        32   /* Maks. dlugosc nazwy produktu */

/* Sciezki plikow */
#define KEY_FILE            "ciastkarnia.key"
#define LOG_DIR             "logs"

/* ftok() identyfikatory projektow */
#define PROJ_SHM       'S'   /* Pamiec dzielona */
#define PROJ_SEM       'E'   /* Semafory */
#define PROJ_MQ_CONV   'C'   /* Kolejka komunikatow - podajniki */
#define PROJ_MQ_CHKOUT 'K'   /* Kolejka komunikatow - kasy (checkout) */
#define PROJ_MQ_RCPT   'R'   /* Kolejka komunikatow - paragony */

/* Indeksy semaforow w zbiorze */
#define SEM_SHM_MUTEX     0   /* Mutex na pamiec dzielona */
#define SEM_SHOP_ENTRY    1   /* Semafor zliczajacy - wejscie do sklepu (init N) */
#define SEM_CONVEYOR_BASE 2   /* Indeksy 2..2+P-1: wolne miejsca na podajnikach */

/*
 *  KOLORY TERMINALA
 */

#define C_RESET   "\033[0m"
#define C_RED     "\033[1;31m"
#define C_GREEN   "\033[1;32m"
#define C_YELLOW  "\033[1;33m"
#define C_BLUE    "\033[1;34m"
#define C_MAGENTA "\033[1;35m"
#define C_CYAN    "\033[1;36m"
#define C_WHITE   "\033[1;37m"
#define C_GRAY    "\033[0;90m"
#define C_BOLD    "\033[1m"

/*
 *  TYPY PROCESOW
 */

typedef enum {
    PROC_MANAGER  = 0,
    PROC_BAKER    = 1,
    PROC_CASHIER  = 2,
    PROC_CUSTOMER = 3
} ProcessType;

#endif /* COMMON_H */
