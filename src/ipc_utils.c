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

// SEMAFORY

int create_semaphores(const char *keyfile, int nsems)
{
    key_t key = ftok(keyfile, PROJ_SEM);
    if (key == -1)
        handle_error("ftok (semaphores)");

    int sem_id = semget(key, nsems, IPC_CREAT | IPC_EXCL | IPC_PERMS);
    if (sem_id == -1) {
        if (errno == EEXIST) {
            sem_id = semget(key, nsems, IPC_PERMS);
            if (sem_id != -1) {
                semctl(sem_id, 0, IPC_RMID);
            }
            sem_id = semget(key, nsems, IPC_CREAT | IPC_EXCL | IPC_PERMS);
        }
        if (sem_id == -1)
            handle_error("semget (create)");
    }

    return sem_id;
}

int get_semaphores(const char *keyfile, int nsems)
{
    key_t key = ftok(keyfile, PROJ_SEM);
    if (key == -1)
        handle_error("ftok (sem get)");

    int sem_id = semget(key, nsems, IPC_PERMS);
    if (sem_id == -1)
        handle_error("semget (get)");

    return sem_id;
}

void init_semaphore(int sem_id, int sem_num, int value)
{
    union semun arg;
    arg.val = value;
    if (semctl(sem_id, sem_num, SETVAL, arg) == -1)
        handle_error("semctl SETVAL");
}

void sem_wait_op(int sem_id, int sem_num)
{
    struct sembuf sop;
    sop.sem_num = sem_num;
    sop.sem_op  = -1;
    sop.sem_flg = 0;

    while (semop(sem_id, &sop, 1) == -1) {
        if (errno == EINTR)
            continue;
        if (errno == EIDRM || errno == EINVAL)
            return;
        handle_error("semop (wait)");
    }
}

void sem_signal_op(int sem_id, int sem_num)
{
    struct sembuf sop;
    sop.sem_num = sem_num;
    sop.sem_op  = 1;
    sop.sem_flg = 0;

    if (semop(sem_id, &sop, 1) == -1) {
        if (errno != EINTR && errno != EIDRM && errno != EINVAL)
            handle_warning("semop (signal)");
    }
}

int sem_trywait_op(int sem_id, int sem_num)
{
    struct sembuf sop;
    sop.sem_num = sem_num;
    sop.sem_op  = -1;
    sop.sem_flg = IPC_NOWAIT;

    if (semop(sem_id, &sop, 1) == -1) {
        if (errno == EAGAIN)
            return -1;
        if (errno == EINTR)
            return -1;
        handle_warning("semop (trywait)");
        return -1;
    }
    return 0;
}

int sem_getval(int sem_id, int sem_num)
{
    int val = semctl(sem_id, sem_num, GETVAL);
    if (val == -1)
        handle_warning("semctl GETVAL");
    return val;
}

void remove_semaphores(const char *keyfile)
{
    key_t key = ftok(keyfile, PROJ_SEM);
    if (key == -1) return;

    int sem_id = semget(key, 0, IPC_PERMS);
    if (sem_id == -1) return;

    if (semctl(sem_id, 0, IPC_RMID) == -1)
        handle_warning("semctl IPC_RMID");
}

//KOLEJKI KOMUNIKATOW (Message Queues)

int create_message_queue(const char *keyfile, int proj_id)
{
    key_t key = ftok(keyfile, proj_id);
    if (key == -1)
        handle_error("ftok (message queue)");

    int mq_id = msgget(key, IPC_CREAT | IPC_EXCL | IPC_PERMS);
    if (mq_id == -1) {
        if (errno == EEXIST) {
            mq_id = msgget(key, IPC_PERMS);
            if (mq_id != -1) {
                msgctl(mq_id, IPC_RMID, NULL);
            }
            mq_id = msgget(key, IPC_CREAT | IPC_EXCL | IPC_PERMS);
        }
        if (mq_id == -1)
            handle_error("msgget (create)");
    }

    return mq_id;
}

int get_message_queue(const char *keyfile, int proj_id)
{
    key_t key = ftok(keyfile, proj_id);
    if (key == -1)
        handle_error("ftok (mq get)");

    int mq_id = msgget(key, IPC_PERMS);
    if (mq_id == -1)
        handle_error("msgget (get)");

    return mq_id;
}

void remove_message_queue(const char *keyfile, int proj_id)
{
    key_t key = ftok(keyfile, proj_id);
    if (key == -1) return;

    int mq_id = msgget(key, IPC_PERMS);
    if (mq_id == -1) return;

    if (msgctl(mq_id, IPC_RMID, NULL) == -1)
        handle_warning("msgctl IPC_RMID");
}
