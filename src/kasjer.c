/**
 * kasjer.c - Proces kasjera
 * Projekt: Ciastkarnia (Temat 15) - Systemy Operacyjne
 *
 * Kasjer obsluguje klientow przy stanowisku kasowym.
 * Odbiera komunikaty checkout od klientow (kolejka komunikatow),
 * przetwarza zakupy, wystawia paragon.
 *
 * Kazdy kasjer ma watek monitorujacy ktory sprawdza czy kasa
 * powinna byc otwarta/zamknieta na podstawie liczby klientow.
 *
 * Komunikacja:
 * - Checkout: kolejka komunikatow (msgrcv z mtype = register_id + 1)
 * - Paragony: kolejka komunikatow (msgsnd z mtype = customer_pid)
 * - Stan: pamiec dzielona
 * - Sygnaly: SIGUSR1 (inwentaryzacja), SIGUSR2 (ewakuacja), SIGTERM
 */

#include "common.h"
#include "error_handler.h"
#include "ipc_utils.h"
#include "logger.h"

/* ================================================================
 *  ZMIENNE GLOBALNE PROCESU
 * ================================================================ */

static SharedData *g_shm         = NULL;
static int         g_sem_id      = -1;
static int         g_mq_checkout = -1;
static int         g_mq_receipt  = -1;
static int         g_register_id = -1;  /* Numer kasy (0 lub 1) */

static volatile sig_atomic_t g_evacuation = 0;
static volatile sig_atomic_t g_inventory  = 0;
static volatile sig_atomic_t g_terminate  = 0;

/* Mutex i condvar dla watku monitorujacego */
static pthread_mutex_t g_cash_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cash_cond  = PTHREAD_COND_INITIALIZER;
static int g_should_be_active = 1;

/* ================================================================
 *  OBSLUGA SYGNALOW
 * ================================================================ */

static void sigusr1_handler(int sig)
{
    (void)sig;
    g_inventory = 1;
}

static void sigusr2_handler(int sig)
{
    (void)sig;
    g_evacuation = 1;
}

static void sigterm_handler(int sig)
{
    (void)sig;
    g_terminate = 1;
}

static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = sigusr1_handler;
    sa.sa_flags   = 0;
    sigaction(SIGUSR1, &sa, NULL);

    sa.sa_handler = sigusr2_handler;
    sigaction(SIGUSR2, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);
}

/* ================================================================
 *  WATEK MONITORUJACY STAN KASY
 * ================================================================ */

/**
 * Watek monitorujacy - sprawdza czy kasa powinna byc aktywna.
 * Uzywa pthread_cond_wait/signal do efektywnego oczekiwania.
 *
 * Demonstruje: pthread_cond_wait, pthread_cond_signal,
 *              pthread_mutex_lock/unlock
 */
static void *monitor_thread(void *arg)
{
    (void)arg;

    while (!g_terminate && !g_evacuation && g_shm->simulation_running) {
        pthread_mutex_lock(&g_cash_mutex);

        /* Sprawdz czy kasa powinna byc aktywna */
        int should_active = g_shm->register_open[g_register_id];

        if (should_active != g_should_be_active) {
            g_should_be_active = should_active;
            /* Obudz glowny watek kasowy */
            pthread_cond_signal(&g_cash_cond);
        }

        pthread_mutex_unlock(&g_cash_mutex);

        /* Sprawdzaj co 500ms */
        usleep(500000);
    }

    /* Przy zamknieciu - obudz glowny watek na wszelki wypadek */
    pthread_mutex_lock(&g_cash_mutex);
    pthread_cond_broadcast(&g_cash_cond);
    pthread_mutex_unlock(&g_cash_mutex);

    return NULL;
}

/* ================================================================
 *  OBSLUGA KLIENTA PRZY KASIE
 * ================================================================ */

/**
 * Przetwarza zakupy klienta.
 * Oblicza laczna kwote, aktualizuje statystyki i wysyla paragon.
 *
 * @param cmsg Komunikat checkout od klienta
 */
static void process_checkout(struct checkout_msg *cmsg)
{
    double total = 0.0;
    int total_items = 0;
    struct receipt_msg rmsg;
    rmsg.mtype = cmsg->customer_pid;
    memset(rmsg.items, 0, sizeof(rmsg.items));

    /* Skanowanie produktow - z symulowanym opoznieniem */
    for (int i = 0; i < g_shm->num_products; i++) {
        if (cmsg->items[i] > 0) {
            /* Symulacja skanowania: szybkie skanowanie */
            usleep(g_shm->time_scale_ms * 50); /* 0.05 min na szt */

            rmsg.items[i] = cmsg->items[i];
            double item_cost = cmsg->items[i] * g_shm->products[i].price;
            total += item_cost;
            total_items += cmsg->items[i];

            /* Aktualizuj statystyki kasy (chronione semaforem) */
            sem_wait_undo(g_sem_id, SEM_SHM_MUTEX);
            g_shm->register_sales[g_register_id][i] += cmsg->items[i];
            sem_signal_undo(g_sem_id, SEM_SHM_MUTEX);
        }
    }

    rmsg.total = total;

    /* Aktualizuj przychod kasy */
    sem_wait_undo(g_sem_id, SEM_SHM_MUTEX);
    g_shm->register_revenue[g_register_id] += total;
    sem_signal_undo(g_sem_id, SEM_SHM_MUTEX);

    /* Wyslij paragon klientowi */
    if (msgsnd_guarded(g_mq_receipt, &rmsg, sizeof(rmsg) - sizeof(long),
                       g_sem_id, SEM_GUARD_RCPT(g_shm->num_products)) == -1) {
        if (errno == EIDRM || errno == EINVAL || errno == EINTR) {
            log_msg("Paragon dla PID:%d nie wyslany (zamykanie symulacji)",
                    cmsg->customer_pid);
        } else {
            handle_warning("msgsnd (receipt)");
            log_msg("Blad wysylania paragonu do klienta PID:%d",
                    cmsg->customer_pid);
        }
    }

    log_msg("Obsluzono klienta PID:%d - %d produktow, %.2f PLN",
            cmsg->customer_pid, total_items, total);
}

/* ================================================================
 *  GLOWNA FUNKCJA KASJERA
 * ================================================================ */

int main(int argc, char *argv[])
{
    /* --- Parsowanie argumentow --- */
    if (argc < 3) {
        fprintf(stderr, "Uzycie: kasjer <keyfile> <register_id>\n");
        return EXIT_FAILURE;
    }

    const char *keyfile = argv[1];
    g_register_id = atoi(argv[2]);

    if (validate_int_range(g_register_id, 0, 1, "register_id") != 0)
        return EXIT_FAILURE;

    srand(time(NULL) ^ getpid());

    /* --- Dolaczenie do zasobow IPC --- */
    g_shm = attach_shared_memory(keyfile);

    int num_sems = TOTAL_SEMS(g_shm->num_products);
    g_sem_id = get_semaphores(keyfile, num_sems);

    g_mq_checkout = get_message_queue(keyfile, PROJ_MQ_CHKOUT);
    g_mq_receipt  = get_message_queue(keyfile, PROJ_MQ_RCPT);

    /* --- Logger --- */
    logger_init(g_shm, PROC_CASHIER, g_register_id);

    /* --- Sygnaly --- */
    setup_signals();

    log_msg("Kasjer gotowy! Kasa nr %d, PID: %d",
            g_register_id + 1, getpid());

    /* --- Uruchom watek monitorujacy --- */
    pthread_t monitor_tid;
    if (pthread_create(&monitor_tid, NULL, monitor_thread, NULL) != 0) {
        handle_error("pthread_create (cashier monitor)");
    }
    pthread_detach(monitor_tid);

    /* --- Glowna petla obslugi klientow --- */
    while (!g_terminate) {

        /* Sprawdz czy symulacja wciaz trwa */
        if (!g_shm->simulation_running) {
            /* Symulacja konczy sie - obsluz pozostalych w kolejce */
            sem_wait_undo(g_sem_id, SEM_SHM_MUTEX);
            int queue = g_shm->register_queue_len[g_register_id];
            sem_signal_undo(g_sem_id, SEM_SHM_MUTEX);
            if (queue == 0) break;
        }

        /* Sprawdz ewakuacje */
        if (g_evacuation) {
            log_msg_color(C_RED, "EWAKUACJA! Kasa %d konczy prace.",
                          g_register_id + 1);
            break;
        }

        /* Sprawdz sygnal inwentaryzacji */
        if (g_inventory) {
            log_msg_color(C_MAGENTA, "Sygnal inwentaryzacji - kontynuuje obsluge.");
            g_inventory = 0;
        }

        /* Sprawdz czy kasa jest aktywna */
        pthread_mutex_lock(&g_cash_mutex);
        int active = g_should_be_active;
        pthread_mutex_unlock(&g_cash_mutex);

        /* Kasa 0 jest zawsze aktywna; kasa 1 moze byc nieaktywna */
        if (!active && g_register_id == 1) {
            /* Sprawdz czy sa jeszcze klienci w kolejce do obslugi */
            sem_wait_undo(g_sem_id, SEM_SHM_MUTEX);
            int queue = g_shm->register_queue_len[g_register_id];
            sem_signal_undo(g_sem_id, SEM_SHM_MUTEX);

            if (queue == 0) {
                /* Brak klientow, kasa moze odpoczac */
                usleep(g_shm->time_scale_ms * 2000);
                continue;
            }
            /* Jesli sa klienci - obsluz ich przed zamknieciem */
        }

        /* Proba odbioru komunikatu checkout (nieblokujaca z krotkim opoznieniem) */
        struct checkout_msg cmsg;
        ssize_t ret = msgrcv_guarded(g_mq_checkout, &cmsg,
                             sizeof(cmsg) - sizeof(long),
                             g_register_id + 1, IPC_NOWAIT,
                             g_sem_id, SEM_GUARD_CHKOUT(g_shm->num_products));

        if (ret == -1) {
            if (errno == ENOMSG || errno == EINTR) {
                /* Brak komunikatu - czekaj chwile */
                usleep(g_shm->time_scale_ms * 500);
                continue;
            }
            if (errno == EIDRM) {
                /* Kolejka zostala usunieta - konczymy */
                break;
            }
            handle_warning("msgrcv (checkout)");
            usleep(100000);
            continue;
        }

        /* Mamy klienta do obslugi! */
        log_msg("Rozpoczynam obsluge klienta PID:%d", cmsg.customer_pid);
        process_checkout(&cmsg);

        /* Zmniejsz kolejke */
        sem_wait_undo(g_sem_id, SEM_SHM_MUTEX);
        if (g_shm->register_queue_len[g_register_id] > 0)
            g_shm->register_queue_len[g_register_id]--;
        sem_signal_undo(g_sem_id, SEM_SHM_MUTEX);
    }

    /* --- Podsumowanie sprzedazy --- */
    log_msg("=== PODSUMOWANIE KASY NR %d ===", g_register_id + 1);
    int total_sold = 0;
    for (int i = 0; i < g_shm->num_products; i++) {
        if (g_shm->register_sales[g_register_id][i] > 0) {
            log_msg("  %s: %d szt.",
                    g_shm->products[i].name,
                    g_shm->register_sales[g_register_id][i]);
            total_sold += g_shm->register_sales[g_register_id][i];
        }
    }
    log_msg("  RAZEM: %d szt., PRZYCHOD: %.2f PLN",
            total_sold, g_shm->register_revenue[g_register_id]);

    /* Zapisz podsumowanie na stderr (do pliku logu) */
    fprintf(stderr, "=== PODSUMOWANIE KASY %d ===\n", g_register_id + 1);
    fprintf(stderr, "Razem: %d szt., Przychod: %.2f PLN\n",
            total_sold, g_shm->register_revenue[g_register_id]);

    /* --- Sprzatanie --- */
    pthread_mutex_destroy(&g_cash_mutex);
    pthread_cond_destroy(&g_cash_cond);
    detach_shared_memory(g_shm);

    log_msg("Kasjer %d zakonczyl prace. PID: %d",
            g_register_id + 1, getpid());
    return EXIT_SUCCESS;
}
