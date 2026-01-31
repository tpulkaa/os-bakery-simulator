/**
 * piekarz.c - Proces piekarza
 * Projekt: Ciastkarnia (Temat 15) - Systemy Operacyjne
 *
 * Piekarz produkuje rozne produkty i uklada je na podajnikach.
 * Kazdy podajnik jest zaimplementowany jako kolejka komunikatow (FIFO).
 * Produkcja odbywa sie w watkach (pthread) - kazdy watek
 * odpowiada za grupe produktow.
 *
 * Komunikacja:
 * - Podajniki: kolejka komunikatow (msgsnd z mtype = product_id + 1)
 * - Stan: pamiec dzielona
 * - Pojemnosc podajnikow: semafory
 * - Raport produkcji: pipe do kierownika
 * - Sygnaly: SIGUSR1 (inwentaryzacja), SIGUSR2 (ewakuacja), SIGTERM
 */

#include "common.h"
#include "error_handler.h"
#include "ipc_utils.h"
#include "logger.h"

/* ================================================================
 *  ZMIENNE GLOBALNE PROCESU
 * ================================================================ */

static SharedData *g_shm    = NULL;
static int         g_sem_id = -1;
static int         g_mq_conveyor = -1;
static int         g_pipe_fd = -1;   /* Pipe do kierownika (write end) */
static int         g_item_counter = 0; /* Globalny licznik ciastek */

static volatile sig_atomic_t g_evacuation = 0;
static volatile sig_atomic_t g_inventory  = 0;
static volatile sig_atomic_t g_terminate  = 0;

/* Mutex do ochrony danych wspoldzielonych w watkach */
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

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
 *  STRUKTURA WATKOW PRODUKCJI
 * ================================================================ */

/**
 * Argumenty przekazywane do watkow produkcji.
 */
typedef struct {
    int thread_id;          /* Numer watku */
    int product_start;      /* Indeks pierwszego produktu */
    int product_end;        /* Indeks za ostatnim produktem (exclusive) */
} BakerThreadArgs;

/**
 * Funkcja watku produkcyjnego.
 * Kazdy watek jest odpowiedzialny za produkcje podzbioru produktow.
 * Produkuje losowa ilosc losowych produktow ze swojego zakresu.
 *
 * Demonstruje: pthread_create, pthread_mutex_lock/unlock, pthread_cond_wait
 *
 * @param arg Wskaznik do BakerThreadArgs
 */
static void *production_thread(void *arg)
{
    BakerThreadArgs *targs = (BakerThreadArgs *)arg;
    int tid = targs->thread_id;

    while (!g_terminate && !g_evacuation && g_shm->bakery_open
           && g_shm->simulation_running) {
        /* Losowy czas miedzy partiami: 5-15 minut symulacji */
        int delay = (5 + rand() % 11) * g_shm->time_scale_ms;
        /* Spimy w krotkich odcinkach aby szybko reagowac na sygnaly */
        for (int d = 0; d < delay && !g_terminate && !g_evacuation
             && g_shm->bakery_open && g_shm->simulation_running; d += 50) {
            usleep(50 * 1000);
        }

        if (g_terminate || g_evacuation || !g_shm->bakery_open
            || !g_shm->simulation_running)
            break;

        /* Losowa partia produktow z zakresu tego watku */
        int num_types = 1 + rand() % (targs->product_end - targs->product_start);
        int products_made = 0;

        for (int t = 0; t < num_types; t++) {
            int prod_id = targs->product_start +
                          rand() % (targs->product_end - targs->product_start);
            int quantity = 1 + rand() % 4; /* 1-4 sztuki */

            for (int q = 0; q < quantity; q++) {
                /* Sprawdz czy jest miejsce na podajniku (semafor) */
                if (sem_trywait_op(g_sem_id, SEM_CONVEYOR_BASE + prod_id) == 0) {
                    /* Jest miejsce - wyslij ciastko na podajnik */
                    struct conveyor_msg msg;
                    msg.mtype = prod_id + 1;

                    pthread_mutex_lock(&g_mutex);
                    msg.item_id = ++g_item_counter;
                    pthread_mutex_unlock(&g_mutex);

                    if (msgsnd_guarded(g_mq_conveyor, &msg, sizeof(msg) - sizeof(long),
                                      g_sem_id, SEM_GUARD_CONV(g_shm->num_products)) == -1) {
                        if (errno != EINTR)
                            handle_warning("msgsnd (conveyor)");
                        /* Zwroc miejsce na podajniku */
                        sem_signal_op(g_sem_id, SEM_CONVEYOR_BASE + prod_id);
                        continue;
                    }

                    /* Aktualizuj statystyki produkcji */
                    pthread_mutex_lock(&g_mutex);
                    sem_wait_undo(g_sem_id, SEM_SHM_MUTEX);
                    g_shm->baker_produced[prod_id]++;
                    sem_signal_undo(g_sem_id, SEM_SHM_MUTEX);
                    pthread_mutex_unlock(&g_mutex);

                    products_made++;
                }
                /* Jesli podajnik pelny - pomijamy (nie blokujemy) */
            }
        }

        if (products_made > 0) {
            log_msg("Watek %d wyprodukowa partie: %d szt. ciastek",
                    tid, products_made);

            /* Wyslij informacje o partii przez pipe do kierownika */
            if (g_pipe_fd >= 0) {
                char pipe_buf[128];
                int n = snprintf(pipe_buf, sizeof(pipe_buf),
                                 "BATCH:%d:%d\n", tid, products_made);
                write(g_pipe_fd, pipe_buf, n);
            }
        }
    }

    free(targs);
    return NULL;
}

/* ================================================================
 *  GLOWNA FUNKCJA PIEKARZA
 * ================================================================ */

int main(int argc, char *argv[])
{
    /* --- Parsowanie argumentow --- */
    if (argc < 3) {
        fprintf(stderr, "Uzycie: piekarz <keyfile> <pipe_write_fd>\n");
        return EXIT_FAILURE;
    }

    const char *keyfile = argv[1];
    g_pipe_fd = atoi(argv[2]);

    srand(time(NULL) ^ getpid());

    /* --- Dolaczenie do zasobow IPC --- */
    g_shm = attach_shared_memory(keyfile);

    int num_sems = TOTAL_SEMS(g_shm->num_products);
    g_sem_id = get_semaphores(keyfile, num_sems);

    g_mq_conveyor = get_message_queue(keyfile, PROJ_MQ_CONV);

    /* --- Logger --- */
    logger_init(g_shm, PROC_BAKER, 0);

    /* --- Sygnaly --- */
    setup_signals();

    log_msg("Piekarz gotowy! PID: %d, Produktow: %d",
            getpid(), g_shm->num_products);

    /* --- Uruchomienie watkow produkcyjnych ---
     * Dzielimy produkty na 2 grupy obsługiwane przez 2 watki.
     * Demonstracja: pthread_create, pthread_join
     */
    int num_threads = 2;
    pthread_t threads[2];
    int half = g_shm->num_products / 2;

    for (int i = 0; i < num_threads; i++) {
        BakerThreadArgs *args = malloc(sizeof(BakerThreadArgs));
        if (!args) handle_error("malloc (baker thread args)");

        args->thread_id    = i;
        args->product_start = (i == 0) ? 0 : half;
        args->product_end   = (i == 0) ? half : g_shm->num_products;

        if (pthread_create(&threads[i], NULL, production_thread, args) != 0) {
            handle_error("pthread_create (baker)");
        }
        log_msg("Watek produkcyjny %d uruchomiony (produkty %d-%d)",
                i, args->product_start, args->product_end - 1);
    }

    /* --- Glowna petla - czeka na sygnaly i monitoruje stan --- */
    while (!g_terminate && !g_evacuation && g_shm->bakery_open
           && g_shm->simulation_running) {
        usleep(g_shm->time_scale_ms * 1000);

        if (g_inventory) {
            log_msg_color(C_MAGENTA, "Sygnal inwentaryzacji odebrany - "
                          "kontynuuje produkcje do zamkniecia.");
            g_inventory = 0;
        }
    }

    if (g_evacuation) {
        log_msg_color(C_RED, "EWAKUACJA! Piekarz konczy prace natychmiast.");
    } else {
        log_msg("Piekarnia zamyka sie. Piekarz konczy prace.");
    }

    /* --- Czekaj na zakonczenie watkow --- */
    /* Obudz watki ewentualnie zablokowane na semaforze straznika kolejki */
    for (int i = 0; i < num_threads; i++)
        sem_signal_op(g_sem_id, SEM_GUARD_CONV(g_shm->num_products));
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    log_msg("Watki produkcyjne zakonczyly prace.");

    /* --- Wypisz podsumowanie produkcji (na stderr = plik logu) --- */
    fprintf(stderr, "=== PODSUMOWANIE PRODUKCJI PIEKARZA ===\n");
    int total = 0;
    for (int i = 0; i < g_shm->num_products; i++) {
        fprintf(stderr, "  %s: %d szt.\n",
                g_shm->products[i].name, g_shm->baker_produced[i]);
        total += g_shm->baker_produced[i];
    }
    fprintf(stderr, "  RAZEM: %d szt.\n", total);

    /* --- Wyslij podsumowanie przez pipe --- */
    if (g_pipe_fd >= 0) {
        char pipe_buf[64];
        int n = snprintf(pipe_buf, sizeof(pipe_buf), "DONE:%d\n", total);
        write(g_pipe_fd, pipe_buf, n);
        close(g_pipe_fd);
    }

    /* --- Sprzatanie --- */
    pthread_mutex_destroy(&g_mutex);
    detach_shared_memory(g_shm);

    log_msg("Piekarz zakonczyl prace. PID: %d", getpid());
    return EXIT_SUCCESS;
}
