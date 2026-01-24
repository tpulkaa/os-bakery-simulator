/**
 * kasjer.c - Proces kasjera (cashier)
 * Projekt: Ciastkarnia (Temat 15) - Systemy Operacyjne
 *
 * Obsluguje klientow przy kasie - skanuje produkty, oblicza naleznosc,
 * wysyla paragony.
 */

#include "common.h"
#include "error_handler.h"
#include "ipc_utils.h"
#include "logger.h"

static SharedData *g_shm        = NULL;
static int         g_sem_id     = -1;
static int         g_mq_checkout = -1;
static int         g_mq_receipt  = -1;
static int         g_register_id = -1;

static volatile sig_atomic_t g_terminate = 0;
static volatile sig_atomic_t g_inventory = 0;
static volatile sig_atomic_t g_evacuate  = 0;

static void sigusr1_handler(int sig) { (void)sig; g_inventory = 1; }
static void sigusr2_handler(int sig) { (void)sig; g_evacuate  = 1; }
static void sigterm_handler(int sig) { (void)sig; g_terminate = 1; }

static void setup_signals(void)
{
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = sigusr1_handler;
    sigaction(SIGUSR1, &sa, NULL);
    sa.sa_handler = sigusr2_handler;
    sigaction(SIGUSR2, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Uzycie: %s <keyfile> <register_id>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *keyfile = argv[1];
    g_register_id = atoi(argv[2]);
    if (g_register_id < 0 || g_register_id > 1) {
        fprintf(stderr, "Nieprawidlowy numer kasy: %d\n", g_register_id);
        return EXIT_FAILURE;
    }

    srand(getpid() ^ time(NULL));
    setup_signals();

    g_shm = attach_shared_memory(keyfile);
    int np = g_shm->num_products;
    g_sem_id = get_semaphores(keyfile, 2 + np);
    g_mq_checkout = get_message_queue(keyfile, PROJ_MQ_CHKOUT);
    g_mq_receipt  = get_message_queue(keyfile, PROJ_MQ_RCPT);

    logger_init(g_shm, PROC_CASHIER, g_register_id);
    log_msg("Kasjer %d gotowy! PID=%d", g_register_id, getpid());

    /* TODO: watek monitora (pthread_create, detached) */
    /* TODO: petla obslugujaca msgrcv -> paragon */

    while (!g_terminate && g_shm->simulation_running) {
        if (g_evacuate) {
            log_msg_color(C_RED, "!!! EWAKUACJA !!!");
            break;
        }
        if (g_inventory) {
            log_msg_color(C_MAGENTA, "=== INWENTARYZACJA ===");
            g_inventory = 0;
        }
        usleep(g_shm->time_scale_ms * 1000);
    }

    log_msg("Kasjer %d konczy prace.", g_register_id);
    detach_shared_memory(g_shm);
    return 0;
}
