/**
 * error_handler.h - Obsluga bledow i walidacja danych
 * Projekt: Ciastkarnia (Temat 15) - Systemy Operacyjne
 *
 * Wlasne funkcje do zglaszania i obslugi wyjatkow.
 * Opakowuje perror()/errno z dodatkowym kontekstem.
 */

#ifndef ERROR_HANDLER_H
#define ERROR_HANDLER_H

#include "common.h"

/**
 * Obsluguje blad krytyczny - wypisuje komunikat, wywoluje perror(), konczy proces.
 * @param msg Opis kontekstu bledu
 */
void handle_error(const char *msg);

/**
 * Obsluguje ostrzezenie - wypisuje komunikat i perror(), ale kontynuuje dzialanie.
 * @param msg Opis kontekstu ostrzezenia
 */
void handle_warning(const char *msg);

/**
 * Waliduje wartosc calkowita w zakresie [min, max].
 * @param value Wartosc do sprawdzenia
 * @param min   Minimalna dopuszczalna wartosc
 * @param max   Maksymalna dopuszczalna wartosc
 * @param name  Nazwa parametru (do komunikatu bledu)
 * @return 0 jesli poprawne, -1 jesli blad (wyswietla komunikat)
 */
int validate_int_range(int value, int min, int max, const char *name);

/**
 * Waliduje wartosc zmiennoprzecinkowa w zakresie [min, max].
 * @param value Wartosc do sprawdzenia
 * @param min   Minimalna dopuszczalna wartosc
 * @param max   Maksymalna dopuszczalna wartosc
 * @param name  Nazwa parametru (do komunikatu bledu)
 * @return 0 jesli poprawne, -1 jesli blad
 */
int validate_double_range(double value, double min, double max, const char *name);

/**
 * Sprawdza wynik funkcji systemowej. Jesli ret == -1, obsluguje blad.
 * @param ret    Wartosc zwrocona przez funkcje systemowa
 * @param msg    Opis kontekstu
 * @param fatal  1 = blad krytyczny (exit), 0 = ostrzezenie
 * @return ret (wartosc oryginalna)
 */
int check_sys_call(int ret, const char *msg, int fatal);

#endif /* ERROR_HANDLER_H */
