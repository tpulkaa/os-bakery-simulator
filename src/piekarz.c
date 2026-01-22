/**
 * piekarz.c - Proces piekarza (baker)
 * Projekt: Ciastkarnia (Temat 15) - Systemy Operacyjne
 *
 * Produkuje ciastka i umieszcza je na podajnikach.
 * Raportuje produkcje do kierownika przez pipe.
 */

#include "common.h"
#include "error_handler.h"
#include "ipc_utils.h"
#include "logger.h"

/* Zmienne globalne */
static SharedData *g_shm         = NULL;
static int         g_sem_id      = -1;
static int         g_mq_conveyor = -1;
static int         g_pipe_fd     = -1;

static volatile sig_atomic_t g_terminate  = 0;
static volatile sig_atomic_t g_inventory  = 0;
static volatile sig_atomic_t g_evacuate   = 0;

/* Signal handlers */
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
        fprintf(stderr, "Uzycie: %s <keyfile> <pipe_fd>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *keyfile = argv[1];
    g_pipe_fd = atoi(argv[2]);

    srand(getpid() ^ time(NULL));
    setup_signals();

    /* Dolacz do IPC */
    g_shm = attach_shared_memory(keyfile);
    int np = g_shm->num_products;
    g_sem_id = get_semaphores(keyfile, 2 + np);
    g_mq_conveyor = get_message_queue(keyfile, PROJ_MQ_CONV);

    logger_init(g_shm, PROC_BAKER, 0);
    log_msg("Piekarz gotowy! PID=%d, produktow=%d", getpid(), np);

    /* TODO: uruchom watki produkcyjne (pthread_create) */
    /* Na razie prosta petla */
    while (!g_terminate && g_shm->simulation_running) {
        if (g_inventory) {
            log_msg_color(C_MAGENTA, "=== INWENTARYZACJA ===");
            g_inventory = 0;
        }
        if (g_evacuate) {
            log_msg_color(C_RED, "!!! EWAKUACJA - zamykam !!!");
            break;
        }

        /* Probuj produkowac losowy produkt */
        int prod_id = rand() % np;
        if (sem_trywait_op(g_sem_id, SEM_CONVEYOR_BASE + prod_id) == 0) {
            /* Jest miejsce na podajniku - wyslij ciastko */
            struct conveyor_msg cmsg;
            cmsg.mtype = prod_id + 1;
            cmsg.item_id = prod_id;
            if (msgsnd(g_mq_conveyor, &cmsg, sizeof(cmsg) - sizeof(long), 0) == -1) {
                if (errno != EIDRM && errno != EINVAL && errno != EINTR)
                    handle_warning("msgsnd (conveyor)");
                sem_signal_op(g_sem_id, SEM_CONVEYOR_BASE + prod_id);
            } else {
                sem_wait_op(g_sem_id, SEM_SHM_MUTEX);
                g_shm->baker_produced[prod_id]++;
                sem_signal_op(g_sem_id, SEM_SHM_MUTEX);
            }
        }

        usleep(g_shm->time_scale_ms * 500);
    }

    log_msg("Piekarz konczy prace.");
    if (g_pipe_fd >= 0) close(g_pipe_fd);
    detach_shared_memory(g_shm);
    return 0;
}
