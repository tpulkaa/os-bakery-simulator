/**
 * logger.h - Naglowek modulu logowania
 * Projekt: Ciastkarnia (Temat 15) - Systemy Operacyjne
 *
 * Kolorowe logowanie z synchronizowanym czasem symulacji.
 */

#ifndef LOGGER_H
#define LOGGER_H

#include "common.h"

/**
 * Inicjalizuje logger - ustawia wskaznik do pamieci dzielonej.
 * @param shm   Wskaznik do pamieci dzielonej (zegar symulacji)
 * @param type  Typ procesu (dla kolorow)
 * @param id    Identyfikator procesu (np. numer kasy, PID klienta)
 */
void logger_init(SharedData *shm, ProcessType type, int id);

/**
 * Loguje komunikat z kolorami i znacznikiem czasu symulacji.
 * Format: [HH:MM] [NAZWA_PROCESU] komunikat
 * @param fmt Format printf
 * @param ... Argumenty
 */
void log_msg(const char *fmt, ...);

/**
 * Loguje komunikat z konkretnym kolorem (nadpisuje domyslny).
 */
void log_msg_color(const char *color, const char *fmt, ...);

/**
 * Zwraca nazwe procesu jako string.
 */
const char *get_process_name(ProcessType type, int id);

/**
 * Zwraca kolor procesu.
 */
const char *get_process_color(ProcessType type);

#endif /* LOGGER_H */
