/**
 * Implementacja funkcji do obslugi: pamieci dzielonej (shmget, shmat, shmdt, shmctl),
 * semaforow (semget, semctl, semop), kolejek komunikatow (msgget, msgsnd, msgrcv, msgctl),
 * laczy nazwanych i nienazwanych (pipe, mkfifo).
 */

#include "ipc_utils.h"
#include "error_handler.h"

/* Minimalne uprawnienia dostepu dla zasobow IPC */
#define IPC_PERMS 0660

// PAMIEC DZIELONA (Shared Memory)

/*
 * create_shared_memory - Tworzy nowy segment pamieci dzielonej.
 * Uzywa ftok() do wygenerowania klucza na podstawie pliku i identyfikatora.
 * Ustawia minimalne prawa dostepu (0660).
 */
int create_shared_memory(const char *keyfile)
{
    key_t key = ftok(keyfile, PROJ_SHM);
    if (key == -1)
        handle_error("ftok (shared memory)");

    int shm_id = shmget(key, sizeof(SharedData), IPC_CREAT | IPC_EXCL | IPC_PERMS);
    if (shm_id == -1) {
        /* Jesli segment juz istnieje, sprobuj go usunac i utworzyc ponownie */
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

    /* Wyzeruj zawartosc segmentu */
    SharedData *shm = (SharedData *)shmat(shm_id, NULL, 0);
    if (shm == (SharedData *)-1)
        handle_error("shmat (init)");

    memset(shm, 0, sizeof(SharedData));
    shmdt(shm);

    return shm_id;
}

/*
 * attach_shared_memory - Dolacza do istniejacego segmentu pamieci dzielonej.
 * Proces potomny wywoluje to po exec(), aby uzyskac dostep do danych.
 */
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

/*
 * detach_shared_memory - Odlacza pamiec dzielona od biezacego procesu.
 */
void detach_shared_memory(SharedData *shm)
{
    if (shm != NULL && shm != (SharedData *)-1) {
        if (shmdt(shm) == -1)
            handle_warning("shmdt");
    }
}

/*
 * remove_shared_memory - Usuwa segment pamieci dzielonej.
 * Wywolywane tylko przez kierownika przy zakonczeniu symulacji.
 */
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

/*
 * create_semaphores - Tworzy zbior semaforow.
 */
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

/*
 * get_semaphores - Pobiera ID istniejacego zbioru semaforow.
 * Uzywane przez procesy potomne po exec().
 */
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

/*
 * init_semaphore - Ustawia wartosc poczatkowa semafora.
 */
void init_semaphore(int sem_id, int sem_num, int value)
{
    union semun arg;
    arg.val = value;
    if (semctl(sem_id, sem_num, SETVAL, arg) == -1)
        handle_error("semctl SETVAL");
}

/*
 * sem_wait_op - Operacja P (dekrementacja) na semaforze.
 * Blokuje proces jesli wartosc semafora wynosi 0.
 * Obsluguje przerwanie przez sygnal (EINTR) - powtarza operacje.
 */
void sem_wait_op(int sem_id, int sem_num)
{
    struct sembuf sop;
    sop.sem_num = sem_num;
    sop.sem_op  = -1;
    sop.sem_flg = 0;

    while (semop(sem_id, &sop, 1) == -1) {
        if (errno == EINTR)
            continue;  /* Przerwanie przez sygnal - powtorz */
        if (errno == EIDRM || errno == EINVAL)
            return;    /* Semafor usuniety podczas shutdown - cicho ignoruj */
        handle_error("semop (wait)");
    }
}

/*
 * sem_signal_op - Operacja V (inkrementacja) na semaforze.
 */
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

/*
 * sem_trywait_op - Nieblokujaca proba operacji P.
 * Zwraca 0 jesli udalo sie zdekrementowac, -1 jesli semafor = 0.
 * Uzywa flagi IPC_NOWAIT.
 */
int sem_trywait_op(int sem_id, int sem_num)
{
    struct sembuf sop;
    sop.sem_num = sem_num;
    sop.sem_op  = -1;
    sop.sem_flg = IPC_NOWAIT;

    if (semop(sem_id, &sop, 1) == -1) {
        if (errno == EAGAIN)
            return -1;  /* Semafor = 0, nie zablokowano */
        if (errno == EINTR)
            return -1;
        handle_warning("semop (trywait)");
        return -1;
    }
    return 0;
}

/*
 * sem_wait_undo - Operacja P z SEM_UNDO.
 * Kernel cofnie operacje jesli proces zginie trzymajac semafor.
 * Uzywane WYLACZNIE do SEM_SHM_MUTEX i SEM_SHOP_ENTRY.
 */
void sem_wait_undo(int sem_id, int sem_num)
{
    struct sembuf sop;
    sop.sem_num = sem_num;
    sop.sem_op  = -1;
    sop.sem_flg = SEM_UNDO;

    while (semop(sem_id, &sop, 1) == -1) {
        if (errno == EINTR)
            continue;
        if (errno == EIDRM || errno == EINVAL)
            return;
        handle_error("semop (wait_undo)");
    }
}

/*
 * sem_signal_undo - Operacja V z SEM_UNDO.
 */
void sem_signal_undo(int sem_id, int sem_num)
{
    struct sembuf sop; //dodac faktycnzy mutex
    sop.sem_num = sem_num;
    sop.sem_op  = 1;
    sop.sem_flg = SEM_UNDO;

    if (semop(sem_id, &sop, 1) == -1) {
        if (errno != EINTR && errno != EIDRM && errno != EINVAL)
            handle_warning("semop (signal_undo)");
    }
}

/*
 * sem_trywait_undo - Nieblokujaca proba P z SEM_UNDO.
 */
int sem_trywait_undo(int sem_id, int sem_num)
{
    struct sembuf sop;
    sop.sem_num = sem_num;
    sop.sem_op  = -1;
    sop.sem_flg = IPC_NOWAIT | SEM_UNDO;

    if (semop(sem_id, &sop, 1) == -1) {
        if (errno == EAGAIN)
            return -1;
        if (errno == EINTR)
            return -1;
        handle_warning("semop (trywait_undo)");
        return -1;
    }
    return 0;
}

/*
 * sem_wait_interruptible - Operacja P przerwalna przez sygnaly.
 * Wraca -1 przy EINTR (zamiast powtarzac jak sem_wait_op).
 * Uzywane w msgsnd_guarded zeby SIGTERM mogl przerwac blokujace czekanie.
 */
int sem_wait_interruptible(int sem_id, int sem_num)
{
    struct sembuf sop;
    sop.sem_num = sem_num;
    sop.sem_op  = -1;
    sop.sem_flg = 0;

    if (semop(sem_id, &sop, 1) == -1) {
        if (errno == EINTR)
            return -1;  /* Przerwane sygnalem - caller sprawdzi g_terminate */
        if (errno == EIDRM || errno == EINVAL)
            return -1;
        handle_error("semop (wait_interruptible)");
    }
    return 0;
}

/*
 * sem_getval - Pobiera aktualna wartosc semafora.
 * Uzywa semctl() z poleceniem GETVAL.
 */
int sem_getval(int sem_id, int sem_num)
{
    int val = semctl(sem_id, sem_num, GETVAL);
    if (val == -1)
        handle_warning("semctl GETVAL");
    return val;
}

/*
 * remove_semaphores - Usuwa zbior semaforow.
 */
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

/*
 * create_message_queue - Tworzy kolejke komunikatow.
 * Uzywa ftok() z podanym proj_id do wygenerowania unikalnego klucza.
 */
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

/*
 * get_message_queue - Pobiera ID istniejacej kolejki komunikatow.
 */
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

/*
 * remove_message_queue - Usuwa kolejke komunikatow.
 */
void remove_message_queue(const char *keyfile, int proj_id)
{
    key_t key = ftok(keyfile, proj_id);
    if (key == -1) return;

    int mq_id = msgget(key, IPC_PERMS);
    if (mq_id == -1) return;

    if (msgctl(mq_id, IPC_RMID, NULL) == -1)
        handle_warning("msgctl IPC_RMID");
}

/*
 * calc_queue_guard_init - Oblicza poczatkowa wartosc semafora-straznika.
 * Na podstawie msg_qbytes (max bajtow w kolejce) i rozmiaru komunikatu
 * oblicza ile komunikatow zmiesci sie w kolejce.
 * Zwraca (slots - 1), minimum 1.
 */
int calc_queue_guard_init(int mq_id, size_t msgsz)
{
    struct msqid_ds info;
    if (msgctl(mq_id, IPC_STAT, &info) == -1) {
        handle_warning("msgctl IPC_STAT (guard init)");
        return 8; /* bezpieczna wartosc domyslna */
    }

    size_t qbytes = info.msg_qbytes;
    if (qbytes == 0 || msgsz == 0)
        return 8;

    int slots = (int)(qbytes / msgsz);
    return (slots > 1) ? (slots - 1) : 1;
}

/*
 * msgsnd_guarded - Wysyla komunikat z backpressure przez semafor-straznika.
 * Czeka az w kolejce bedzie miejsce (sem_wait na guard),
 * potem robi msgsnd. Jesli kolejka usunieta - wraca cicho.
 */
int msgsnd_guarded(int mq_id, const void *msg, size_t msgsz,
                   int sem_id, int guard_idx)
{
    /* Czekaj na wolny slot (przerywalne przez sygnaly) */
    if (sem_wait_interruptible(sem_id, guard_idx) == -1)
        return -1; /* Przerwane sygnalem lub semafor usuniety */

    if (msgsnd(mq_id, msg, msgsz, 0) == -1) {
        if (errno == EIDRM || errno == EINVAL)
            return -1; /* Kolejka usunieta - shutdown */
        if (errno == EINTR)
            return -1;
        handle_warning("msgsnd (guarded)");
        /* Oddaj slot z powrotem przy bledzie */
        sem_signal_op(sem_id, guard_idx);
        return -1;
    }

    return 0;
}

/*
 * msgrcv_guarded - Odbiera komunikat i zwalnia slot w semaforze.
 * Po udanym msgrcv robi sem_signal, otwierajac miejsce dla nadawcow.
 */
ssize_t msgrcv_guarded(int mq_id, void *msg, size_t msgsz, long mtype,
                       int msgflg, int sem_id, int guard_idx)
{
    ssize_t ret = msgrcv(mq_id, msg, msgsz, mtype, msgflg);
    if (ret == -1) {
        if (errno == ENOMSG || errno == EAGAIN)
            return -1; /* IPC_NOWAIT i brak komunikatu */
        if (errno == EIDRM || errno == EINVAL)
            return -1; /* Kolejka usunieta */
        if (errno == EINTR)
            return -1;
        handle_warning("msgrcv (guarded)");
        return -1;
    }

    /* Zwolnij slot - sygnalizuj ze miejsce sie zrobilo */
    sem_signal_op(sem_id, guard_idx);

    return ret;
}


//LACZA (Pipes & FIFOs)


/*
 * create_pipe - Tworzy lacze nienazwane.
 * pipefd[0] = deskryptor do czytania
 * pipefd[1] = deskryptor do pisania
 */
void create_pipe(int pipefd[2])
{
    if (pipe(pipefd) == -1)
        handle_error("pipe");
}

/*
 * create_fifo - Tworzy lacze nazwane (FIFO) z minimalnymi prawami dostepu.
 * Jesli FIFO juz istnieje, nie jest bledem.
 */
void create_fifo(const char *path)
{
    /* Usun stare FIFO jesli istnieje */
    unlink(path);

    if (mkfifo(path, 0660) == -1) {
        if (errno != EEXIST)
            handle_error("mkfifo");
    }
}

/*
 * remove_fifo - Usuwa lacze nazwane (FIFO).
 */
void remove_fifo(const char *path)
{
    if (unlink(path) == -1) {
        if (errno != ENOENT)
            handle_warning("unlink (FIFO)");
    }
}

//CZYSZCZENIE WSZYSTKICH ZASOBOW IPC

/*
 * cleanup_all_ipc - Usuwa wszystkie zasoby IPC stworzone przez symulacje.
 * Wywolywane przez kierownika podczas zamykania (normalnego lub awaryjnego).
 * Zapewnia, ze po zakonczeniu nie pozostaja zadne zasoby.
 */
void cleanup_all_ipc(const char *keyfile, int num_products)
{
    (void)num_products;

    /* Usun kolejki komunikatow */
    remove_message_queue(keyfile, PROJ_MQ_CONV);
    remove_message_queue(keyfile, PROJ_MQ_CHKOUT);
    remove_message_queue(keyfile, PROJ_MQ_RCPT);

    /* Usun semafory */
    remove_semaphores(keyfile);

    /* Usun pamiec dzielona */
    remove_shared_memory(keyfile);

    /* Usun FIFO polecen */
    remove_fifo(FIFO_CMD_PATH);

    /* Usun plik klucza */
    if (unlink(keyfile) == -1) {
        if (errno != ENOENT)
            handle_warning("unlink (keyfile)");
    }
}
