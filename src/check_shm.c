/**
 * @file check_shm.c
 * @brief Narzedzie diagnostyczne - odczytuje stan pamieci dzielonej symulacji.
 *
 * Podlacza sie do istniejacego segmentu SHM i wypisuje stan w formacie
 * latwo parsowalnym (KEY=VALUE), np.:
 *   customers_in_shop=3
 *   max_customers=4
 *   shop_open=1
 *   evacuation_mode=0
 *   simulation_running=1
 *   active_customers=5
 *   total_customers_entered=12
 *   sem_shop_entry=1
 *
 * Uzycie: ./check_shm <key_file>
 * Zwraca 0 jesli SHM istnieje, 1 jesli nie moze sie podlaczyc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <string.h>
#include "common.h"

int main(int argc, char *argv[])
{
    const char *key_file = (argc > 1) ? argv[1] : KEY_FILE;

    /* Polacz z SHM */
    key_t shm_key = ftok(key_file, PROJ_SHM);
    if (shm_key == -1) { fprintf(stderr, "ftok shm\n"); return 1; }

    int shm_id = shmget(shm_key, sizeof(SharedData), 0);
    if (shm_id == -1) { fprintf(stderr, "shmget: nie istnieje\n"); return 1; }

    SharedData *shm = (SharedData *)shmat(shm_id, NULL, SHM_RDONLY);
    if (shm == (void *)-1) { fprintf(stderr, "shmat\n"); return 1; }

    /* Polacz z semaforami */
    key_t sem_key = ftok(key_file, PROJ_SEM);
    int sem_id = -1;
    int sem_shop_val = -1;
    if (sem_key != -1) {
        sem_id = semget(sem_key, 0, 0);
        if (sem_id != -1) {
            sem_shop_val = semctl(sem_id, SEM_SHOP_ENTRY, GETVAL);
        }
    }

    /* Wypisz stan */
    printf("customers_in_shop=%d\n", shm->customers_in_shop);
    printf("max_customers=%d\n", shm->max_customers);
    printf("shop_open=%d\n", shm->shop_open);
    printf("evacuation_mode=%d\n", shm->evacuation_mode);
    printf("simulation_running=%d\n", shm->simulation_running);
    printf("active_customers=%d\n", shm->active_customers);
    printf("total_customers_entered=%d\n", shm->total_customers_entered);
    printf("sim_hour=%d\n", shm->sim_hour);
    printf("sim_min=%d\n", shm->sim_min);
    printf("sem_shop_entry=%d\n", sem_shop_val);
    printf("register_open_0=%d\n", shm->register_open[0]);
    printf("register_open_1=%d\n", shm->register_open[1]);
    printf("register_queue_0=%d\n", shm->register_queue_len[0]);
    printf("register_queue_1=%d\n", shm->register_queue_len[1]);
    printf("num_products=%d\n", shm->num_products);
    printf("customers_served=%d\n", shm->customers_served);
    printf("customers_not_served=%d\n", shm->customers_not_served);
    printf("baker_pid=%d\n", (int)shm->baker_pid);
    printf("bakery_open=%d\n", shm->bakery_open);

    /* Suma produkcji piekarza */
    int baker_total = 0;
    for (int i = 0; i < shm->num_products; i++) {
        printf("baker_produced_%d=%d\n", i, shm->baker_produced[i]);
        baker_total += shm->baker_produced[i];
    }
    printf("baker_produced_total=%d\n", baker_total);

    /* Suma sprzedazy obu kas */
    double revenue_total = shm->register_revenue[0] + shm->register_revenue[1];
    printf("register_revenue_total=%.2f\n", revenue_total);

    /* Kosz ewakuacyjny */
    int basket_total = 0;
    for (int i = 0; i < shm->num_products; i++)
        basket_total += shm->basket_items[i];
    printf("basket_total=%d\n", basket_total);

    shmdt(shm);
    return 0;
}
