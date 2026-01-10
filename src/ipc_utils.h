
#ifndef IPC_UTILS_H
#define IPC_UTILS_H

#include "common.h"

/* ===== Pamiec dzielona (Shared Memory) ===== */

int create_shared_memory(const char *keyfile);
SharedData *attach_shared_memory(const char *keyfile);
void detach_shared_memory(SharedData *shm);
void remove_shared_memory(const char *keyfile);

#endif /* IPC_UTILS_H */
