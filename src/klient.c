/**
 * klient.c - Proces klienta (customer)
 * Projekt: Ciastkarnia (Temat 15) - Systemy Operacyjne
 *
 * Wchodzi do sklepu, zbiera ciastka z podajnikow,
 * placit przy kasie i wychodzi.
 */

#include "common.h"
#include "error_handler.h"
#include "ipc_utils.h"
#include "logger.h"

static SharedData *g_shm         = NULL;
static int         g_sem_id      = -1;
static int         g_mq_conveyor = -1;
static int         g_mq_checkout = -1;
static int         g_mq_receipt  = -1;

static int g_in_shop = 0;
static int g_cart[MAX_PRODUCTS];

static volatile sig_atomic_t g_terminate = 0;
static volatile sig_atomic_t g_evacuate  = 0;
static volatile sig_atomic_t g_inventory = 0;

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
}

/* Lista zakupow */
typedef struct {
    int product_id;
    int quantity;
} ShoppingItem;

static ShoppingItem g_shopping_list[MAX_PRODUCTS];
static int g_shopping_count = 0;

static void generate_shopping_list(int np)
{
    g_shopping_count = 2 + (rand() % 4); /* 2-5 produktow */
    if (g_shopping_count > np) g_shopping_count = np;

    int used[MAX_PRODUCTS] = {0};
    for (int i = 0; i < g_shopping_count; i++) {
        int prod;
        do { prod = rand() % np; } while (used[prod]);
        used[prod] = 1;
        g_shopping_list[i].product_id = prod;
        g_shopping_list[i].quantity = 1 + (rand() % 3); /* 1-3 szt */
    }
}

static void leave_shop(void)
{
    if (!g_in_shop) return;

    sem_wait_op(g_sem_id, SEM_SHM_MUTEX);
    if (g_shm->customers_in_shop > 0)
        g_shm->customers_in_shop--;
    sem_signal_op(g_sem_id, SEM_SHM_MUTEX);

    sem_signal_op(g_sem_id, SEM_SHOP_ENTRY);
    g_in_shop = 0;
    log_msg("Wyszedl ze sklepu.");
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Uzycie: %s <keyfile>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *keyfile = argv[1];
    srand(getpid() ^ time(NULL));
    setup_signals();

    g_shm = attach_shared_memory(keyfile);
    int np = g_shm->num_products;
    g_sem_id = get_semaphores(keyfile, 2 + np);
    g_mq_conveyor = get_message_queue(keyfile, PROJ_MQ_CONV);
    g_mq_checkout = get_message_queue(keyfile, PROJ_MQ_CHKOUT);
    g_mq_receipt  = get_message_queue(keyfile, PROJ_MQ_RCPT);

    logger_init(g_shm, PROC_CUSTOMER, getpid() % 10000);

    generate_shopping_list(np);
    memset(g_cart, 0, sizeof(g_cart));

    /* Proba wejscia do sklepu */
    if (!g_shm->shop_open || g_terminate) {
        log_msg("Sklep zamkniety - odchodze.");
        detach_shared_memory(g_shm);
        return 0;
    }

    /* Czekaj na wejscie (semafor zliczajacy) */
    int max_attempts = 100;
    int entered = 0;
    for (int i = 0; i < max_attempts && !g_terminate; i++) {
        if (sem_trywait_op(g_sem_id, SEM_SHOP_ENTRY) == 0) {
            entered = 1;
            break;
        }
        usleep(50000); /* 50ms */
    }

    if (!entered) {
        log_msg("Nie udalo sie wejsc - sklep pelny.");
        detach_shared_memory(g_shm);
        return 0;
    }

    g_in_shop = 1;
    sem_wait_op(g_sem_id, SEM_SHM_MUTEX);
    g_shm->customers_in_shop++;
    sem_signal_op(g_sem_id, SEM_SHM_MUTEX);

    log_msg("Wszedl do sklepu (produkty: %d pozycji)", g_shopping_count);

    /* TODO: do_shopping() - zbieranie ciastek z podajnikow */
    /* TODO: do_checkout() - platnosc przy kasie */

    leave_shop();
    detach_shared_memory(g_shm);
    return 0;
}
