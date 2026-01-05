/**
 * error_handler.c - Implementacja obslugi bledow i walidacji danych
 * Projekt: Ciastkarnia (Temat 15) - Systemy Operacyjne
 *
 * Wlasne funkcje obslugujace bledy systemowe (perror/errno)
 * oraz walidacje danych wprowadzanych przez uzytkownika.
 */

#include "error_handler.h"

/*
 * handle_error - Obsluga bledu krytycznego.
 * Wyswietla komunikat bledu z uzyciem perror() i errno,
 * nastepnie konczy proces z kodem EXIT_FAILURE.
 */
void handle_error(const char *msg)
{
    fprintf(stderr, "%s[BLAD KRYTYCZNY]%s ", C_RED, C_RESET);
    perror(msg);
    fprintf(stderr, "  errno = %d\n", errno);
    exit(EXIT_FAILURE);
}

/*
 * handle_warning - Obsluga ostrzezenia (niekrytycznego).
 * Wyswietla komunikat z uzyciem perror(), ale nie konczy procesu.
 */
void handle_warning(const char *msg)
{
    fprintf(stderr, "%s[OSTRZEZENIE]%s ", C_YELLOW, C_RESET);
    perror(msg);
}

/*
 * validate_int_range - Walidacja wartosci calkowitej.
 * Sprawdza czy value jest w zakresie [min, max].
 * Wyswietla informacyjny komunikat bledu jesli nie.
 */
int validate_int_range(int value, int min, int max, const char *name)
{
    if (value < min || value > max) {
        fprintf(stderr, "%s[WALIDACJA]%s Parametr '%s' = %d "
                "jest poza zakresem [%d, %d].\n",
                C_RED, C_RESET, name, value, min, max);
        return -1;
    }
    return 0;
}

/*
 * validate_double_range - Walidacja wartosci zmiennoprzecinkowej.
 * Sprawdza czy value jest w zakresie [min, max].
 */
int validate_double_range(double value, double min, double max, const char *name)
{
    if (value < min || value > max) {
        fprintf(stderr, "%s[WALIDACJA]%s Parametr '%s' = %.2f "
                "jest poza zakresem [%.2f, %.2f].\n",
                C_RED, C_RESET, name, value, min, max);
        return -1;
    }
    return 0;
}

/*
 * check_sys_call - Sprawdzanie wyniku funkcji systemowej.
 * Jesli ret == -1, obsluguje blad (krytyczny lub ostrzezenie).
 * Pozwala na zwarta obsluge bledow: check_sys_call(semop(...), "semop", 1);
 */
int check_sys_call(int ret, const char *msg, int fatal)
{
    if (ret == -1) {
        if (fatal) {
            handle_error(msg);
        } else {
            handle_warning(msg);
        }
    }
    return ret;
}
