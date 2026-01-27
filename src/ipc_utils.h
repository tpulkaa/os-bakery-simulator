
#ifndef IPC_UTILS_H
#define IPC_UTILS_H

#include "common.h"

/* ===== Pamiec dzielona (Shared Memory) ===== */

int create_shared_memory(const char *keyfile);
SharedData *attach_shared_memory(const char *keyfile);
void detach_shared_memory(SharedData *shm);
void remove_shared_memory(const char *keyfile);

/* ===== Semafory ===== */

int create_semaphores(const char *keyfile, int nsems);
int get_semaphores(const char *keyfile, int nsems);
void init_semaphore(int sem_id, int sem_num, int value);
void sem_wait_op(int sem_id, int sem_num);
void sem_signal_op(int sem_id, int sem_num);
int sem_trywait_op(int sem_id, int sem_num);
int sem_getval(int sem_id, int sem_num);
void remove_semaphores(const char *keyfile);

/* ===== Kolejki komunikatow (Message Queues) ===== */

int create_message_queue(const char *keyfile, int proj_id);
int get_message_queue(const char *keyfile, int proj_id);
void remove_message_queue(const char *keyfile, int proj_id);

/* ===== Lacza (Pipes & FIFOs) ===== */

void create_pipe(int pipefd[2]);
void create_fifo(const char *path);
void remove_fifo(const char *path);

/* ===== Czyszczenie wszystkich zasobow IPC ===== */

void cleanup_all_ipc(const char *keyfile, int num_products);

#endif /* IPC_UTILS_H */
