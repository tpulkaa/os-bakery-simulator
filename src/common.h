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
#include <math.h>

/*
 *  STALE KONFIGURACYJNE
 */

#define MAX_PRODUCTS        20   /* Maksymalna liczba produktow */
#define MAX_CUSTOMERS_TOTAL 5000 /* Maks. laczna liczba klientow w symulacji */
#define MAX_ACTIVE_CUST     5000 /* Maks. procesow klientow jednoczesnie */
#define MAX_NAME_LEN        32   /* Maks. dlugosc nazwy produktu */

/* Sciezki plikow */
#define KEY_FILE            "ciastkarnia.key"
#define FIFO_CMD_PATH       "/tmp/ciastkarnia_cmd.fifo"
#define LOG_DIR             "logs"
#define REPORT_FILE         "logs/raport.txt"

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
/* Indeksy 2+P .. 2+P+2: guard semafory na kolejki komunikatow */
#define SEM_GUARD_CONV(P)   (2 + (P))     /* Guard na kolejke podajnikow */
#define SEM_GUARD_CHKOUT(P) (2 + (P) + 1) /* Guard na kolejke kas */
#define SEM_GUARD_RCPT(P)   (2 + (P) + 2) /* Guard na kolejke paragonow */
#define TOTAL_SEMS(P)       (2 + (P) + 3) /* Laczna liczba semaforow */

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

/* 
 *  STRUKTURY DANYCH
 */

/**
 * Definicja produktu ciastkarni.
 */
typedef struct {
    char name[MAX_NAME_LEN];   /* Nazwa produktu */
    double price;               /* Cena (PLN) */
    int conveyor_capacity;      /* Ki - pojemnosc podajnika */
} ProductDef;

/**
 * Glowna struktura pamieci dzielonej.
 * Przechowuje caly stan symulacji.
 */
typedef struct {
    /* --- Konfiguracja (ustawiana raz przez kierownika) --- */
    int num_products;           /* P - liczba produktow (>10) */
    int max_customers;          /* N - maks. klientow w sklepie */
    int time_scale_ms;          /* ms na minute symulacji */
    int open_hour, open_min;    /* Tp - godzina otwarcia ciastkarni */
    int close_hour, close_min;  /* Tk - godzina zamkniecia */

    /* --- Definicje produktow --- */
    ProductDef products[MAX_PRODUCTS];

    /* --- Stan sklepu (chronione przez SEM_SHM_MUTEX) --- */
    int customers_in_shop;           /* Ilu klientow jest w sklepie */
    int total_customers_entered;     /* Laczna liczba klientow */
    int register_open[2];            /* 1 = kasa jest obsadzona */
    int register_accepting[2];       /* 1 = kasa przyjmuje nowych klientow */
    int register_queue_len[2];       /* Dlugosc kolejki do kasy */

    /* --- Statystyki sprzedazy na kase --- */
    int register_sales[2][MAX_PRODUCTS];   /* Ilosc sprzedanych szt. */
    double register_revenue[2];             /* Przychod na kasie */

    /* --- Statystyki produkcji piekarza --- */
    int baker_produced[MAX_PRODUCTS];

    /* --- Kosz ewakuacyjny przy kasach --- */
    int basket_items[MAX_PRODUCTS];

    /* --- PID-y procesow --- */
    pid_t manager_pid;
    pid_t baker_pid;
    pid_t cashier_pids[2];

    /* --- Flagi stanu symulacji --- */
    int bakery_open;           /* 1 = piekarnia produkuje */
    int shop_open;             /* 1 = sklep przyjmuje klientow */
    int inventory_mode;        /* 1 = sygnal inwentaryzacji */
    int evacuation_mode;       /* 1 = sygnal ewakuacji */
    int simulation_running;    /* 1 = symulacja aktywna */

    /* --- Zegar symulacji --- */
    int sim_hour;
    int sim_min;

    /* --- Zarzadzanie procesami klientow --- */
    int active_customers;      /* Aktywne procesy klientow */

    /* --- Statystyki obslugi klientow --- */
    int customers_served;      /* Klienci obsluzeni (otrzymali paragon) */
    int customers_not_served;  /* Klienci nieobsluzeni (timeout/ewakuacja/pusty koszyk) */
} SharedData;

/* 
 *  STRUKTURY KOMUNIKATOW (kolejki komunikatow IPC)
 */

/**
 * Komunikat podajnika (piekarz -> klient).
 * Kazdy komunikat = jedno ciastko na podajniku.
 * mtype = product_id + 1 (mtype musi byc > 0)
 */
struct conveyor_msg {
    long mtype;
    int item_id;
};

/**
 * Komunikat checkout (klient -> kasjer).
 * Klient wysyla swoj koszyk do wybranej kasy.
 * mtype = register_id + 1 (1 lub 2)
 */
struct checkout_msg {
    long mtype;
    pid_t customer_pid;
    int items[MAX_PRODUCTS];
};

/**
 * Komunikat paragonu (kasjer -> klient).
 * mtype = customer_pid (aby klient mogl odebrac swoj paragon)
 */
struct receipt_msg {
    long mtype;
    double total;
    int items[MAX_PRODUCTS];
};

/*
 *  DOMYSLNA LISTA PRODUKTOW
 */

#define DEFAULT_NUM_PRODUCTS 1

static const ProductDef DEFAULT_PRODUCTS[DEFAULT_NUM_PRODUCTS] = {
    {"Bulka",            2.00, 100}
};

/* ================================================================
 *  union semun - wymagane na niektorych systemach
 * ================================================================ */

#ifdef __linux__
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};
#endif

#endif /* COMMON_H */
