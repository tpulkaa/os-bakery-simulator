/**
 * logger.c - Implementacja modulu logowania
 * Projekt: Ciastkarnia (Temat 15) - Systemy Operacyjne
 *
 * Kolorowe, synchronizowane logowanie na terminal.
 * Kazdy typ procesu ma wlasny kolor dla czytelnosci.
 */

#include "logger.h"
#include <stdarg.h>

/* Zmienne globalne modulu - ustawiane raz przez logger_init() */
static SharedData *g_shm      = NULL;
static ProcessType g_proc_type = PROC_MANAGER;
static int         g_proc_id   = 0;

/*
 * logger_init - Inicjalizacja loggera.
 * Kazdy proces wywoluje ja raz po dolaczeniu do pamieci dzielonej.
 */
void logger_init(SharedData *shm, ProcessType type, int id)
{
    g_shm       = shm;
    g_proc_type = type;
    g_proc_id   = id;
}

/*
 * get_process_color - Zwraca kod koloru ANSI dla danego typu procesu.
 */
const char *get_process_color(ProcessType type)
{
    switch (type) {
        case PROC_MANAGER:  return C_WHITE;
        case PROC_BAKER:    return C_YELLOW;
        case PROC_CASHIER:  return C_GREEN;
        case PROC_CUSTOMER: return C_CYAN;
        default:            return C_RESET;
    }
}

/*
 * get_process_name - Generuje etykiete procesu (np. "KIEROWNIK", "KASJER-1").
 * Uzywa statycznego bufora - nie jest thread-safe, ale wystarczajace
 * bo kazdy proces ma wlasna kopie.
 */
const char *get_process_name(ProcessType type, int id)
{
    static char buf[32];

    switch (type) {
        case PROC_MANAGER:
            snprintf(buf, sizeof(buf), "KIEROWNIK");
            break;
        case PROC_BAKER:
            snprintf(buf, sizeof(buf), "PIEKARZ");
            break;
        case PROC_CASHIER:
            snprintf(buf, sizeof(buf), "KASJER-%d", id + 1);
            break;
        case PROC_CUSTOMER:
            snprintf(buf, sizeof(buf), "KLIENT-%d", id);
            break;
        default:
            snprintf(buf, sizeof(buf), "UNKNOWN");
            break;
    }

    return buf;
}

/*
 * log_msg - Loguje komunikat na terminal z kolorami i czasem symulacji.
 * Format: [HH:MM] [ETYKIETA] komunikat
 *
 * Uzywa flockfile/funlockfile dla ochrony wyjscia na konsole
 * w srodowisku wieloprocesowym.
 */
void log_msg(const char *fmt, ...)
{
    const char *color = get_process_color(g_proc_type);
    const char *name  = get_process_name(g_proc_type, g_proc_id);

    int hour = 0, min = 0;
    if (g_shm != NULL) {
        hour = g_shm->sim_hour;
        min  = g_shm->sim_min;
    }

    /* Budowanie pelnego komunikatu */
    char msg_buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    /* Atomowe wypisanie na stdout */
    flockfile(stdout);
    fprintf(stdout, "%s[%02d:%02d]%s %s[%-12s]%s %s\n",
            C_GRAY, hour, min, C_RESET,
            color, name, C_RESET,
            msg_buf);
    fflush(stdout);
    funlockfile(stdout);
}

/*
 * log_msg_color - Loguje komunikat z nadpisanym kolorem.
 * Przydatne do wyroznienia waznych zdarzen (np. bledy, sygnaly).
 */
void log_msg_color(const char *color, const char *fmt, ...)
{
    const char *name = get_process_name(g_proc_type, g_proc_id);

    int hour = 0, min = 0;
    if (g_shm != NULL) {
        hour = g_shm->sim_hour;
        min  = g_shm->sim_min;
    }

    char msg_buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    flockfile(stdout);
    fprintf(stdout, "%s[%02d:%02d]%s %s[%-12s] %s%s\n",
            C_GRAY, hour, min, C_RESET,
            color, name, msg_buf, C_RESET);
    fflush(stdout);
    funlockfile(stdout);
}
