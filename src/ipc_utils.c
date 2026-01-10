/**
 * Implementacja narzedzi IPC - pamiec dzielona.
 */

#include "ipc_utils.h"
#include "error_handler.h"

#define IPC_PERMS 0660

// PAMIEC DZIELONA (Shared Memory)

int create_shared_memory(const char *keyfile)
{
    key_t key = ftok(keyfile, PROJ_SHM);
    if (key == -1)
        handle_error("ftok (shared memory)");

    int shm_id = shmget(key, sizeof(SharedData), IPC_CREAT | IPC_EXCL | IPC_PERMS);
    if (shm_id == -1) {
        if (errno == EEXIST) {
            shm_id = shmget(key, sizeof(SharedData), IPC_PERMS);
            if (shm_id != -1) {
                shmctl(shm_id, IPC_RMID, NULL);
            }
            shm_id = shmget(key, sizeof(SharedData), IPC_CREAT | IPC_EXCL | IPC_PERMS);
        }
        if (shm_id == -1)
            handle_error("shmget (create)");
    }

    SharedData *shm = (SharedData *)shmat(shm_id, NULL, 0);
    if (shm == (SharedData *)-1)
        handle_error("shmat (init)");

    memset(shm, 0, sizeof(SharedData));
    shmdt(shm);

    return shm_id;
}

SharedData *attach_shared_memory(const char *keyfile)
{
    key_t key = ftok(keyfile, PROJ_SHM);
    if (key == -1)
        handle_error("ftok (shm attach)");

    int shm_id = shmget(key, sizeof(SharedData), IPC_PERMS);
    if (shm_id == -1)
        handle_error("shmget (attach)");

    SharedData *shm = (SharedData *)shmat(shm_id, NULL, 0);
    if (shm == (SharedData *)-1)
        handle_error("shmat (attach)");

    return shm;
}

void detach_shared_memory(SharedData *shm)
{
    if (shm != NULL && shm != (SharedData *)-1) {
        if (shmdt(shm) == -1)
            handle_warning("shmdt");
    }
}

void remove_shared_memory(const char *keyfile)
{
    key_t key = ftok(keyfile, PROJ_SHM);
    if (key == -1) return;

    int shm_id = shmget(key, sizeof(SharedData), IPC_PERMS);
    if (shm_id == -1) return;

    if (shmctl(shm_id, IPC_RMID, NULL) == -1)
        handle_warning("shmctl IPC_RMID");
}
