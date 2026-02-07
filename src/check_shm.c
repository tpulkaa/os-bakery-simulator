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

    shmdt(shm);
    return 0;
}
