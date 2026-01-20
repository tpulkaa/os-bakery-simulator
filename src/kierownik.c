/**
 * kierownik.c - Proces glowny (manager) symulacji ciastkarni
 * Projekt: Ciastkarnia (Temat 15) - Systemy Operacyjne
 *
 * Tworzy zasoby IPC, uruchamia procesy potomne,
 * prowadzi zegar symulacji i zarzadza cyklem zycia.
 */

#include "common.h"
#include "error_handler.h"
#include "ipc_utils.h"
#include "logger.h"

/* --- Zmienne globalne --- */
static SharedData *g_shm    = NULL;
static int g_sem_id         = -1;
static int g_baker_pipe[2]  = {-1, -1};

/* Tablica PIDow klientow (dynamicznie alokowana) */
static pid_t *g_customer_pids = NULL;
static int    g_num_customers = 0;
static int    g_customer_cap  = 0;

/* Flagi sygnalow */
static volatile sig_atomic_t g_sigchld_received = 0;
static volatile sig_atomic_t g_sigint_received  = 0;

static int g_cleanup_done = 0;

/* ===== Funkcje pomocnicze ===== */

static void create_key_file(void)
{
    int fd = creat(KEY_FILE, 0660);
    if (fd == -1)
        handle_error("creat (key file)");
    close(fd);
}

static void create_log_directory(void)
{
    struct stat st;
    if (stat(LOG_DIR, &st) == -1) {
        if (mkdir(LOG_DIR, 0755) == -1 && errno != EEXIST)
            handle_error("mkdir (logs)");
    }
}

/**
 * msleep_safe - EINTR-resistant sleep.
 * Uzywa nanosleep() z petla retry na EINTR.
 */
static void msleep_safe(int ms)
{
    struct timespec req, rem;
    req.tv_sec  = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;

    while (nanosleep(&req, &rem) == -1) {
        if (errno == EINTR) {
            req = rem;
            continue;
        }
        break;
    }
}

/* ===== atexit cleanup ===== */

static void atexit_cleanup(void)
{
    if (g_cleanup_done) return;
    g_cleanup_done = 1;

    if (g_shm) {
        int np = g_shm->num_products;
        detach_shared_memory(g_shm);
        g_shm = NULL;
        remove_message_queue(KEY_FILE, PROJ_MQ_CONV);
        remove_message_queue(KEY_FILE, PROJ_MQ_CHKOUT);
        remove_message_queue(KEY_FILE, PROJ_MQ_RCPT);
        remove_semaphores(KEY_FILE);
        remove_shared_memory(KEY_FILE);
        unlink(KEY_FILE);
        (void)np;
    }
    free(g_customer_pids);
}

/* ===== Signal handlers ===== */

static void sigchld_handler(int sig) { (void)sig; g_sigchld_received = 1; }
static void sigint_handler(int sig)  { (void)sig; g_sigint_received = 1; }

static void setup_signal_handlers(void)
{
    struct sigaction sa;

    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigint_handler;
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    /* Ignoruj SIGPIPE */
    signal(SIGPIPE, SIG_IGN);
}

/* ===== Zbieranie zombie ===== */

static void reap_children(void)
{
    pid_t pid;
    int status;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        /* Sprawdz czy to klient (nie piekarz/kasjer) */
        int is_customer = 1;
        if (g_shm) {
            if (pid == g_shm->baker_pid) is_customer = 0;
            if (pid == g_shm->cashier_pids[0]) is_customer = 0;
            if (pid == g_shm->cashier_pids[1]) is_customer = 0;
        }

        if (is_customer && g_shm) {
            sem_wait_op(g_sem_id, SEM_SHM_MUTEX);
            if (g_shm->active_customers > 0)
                g_shm->active_customers--;
            sem_signal_op(g_sem_id, SEM_SHM_MUTEX);
        }

        /* Usun z listy PIDow */
        for (int i = 0; i < g_num_customers; i++) {
            if (g_customer_pids[i] == pid) {
                g_customer_pids[i] = g_customer_pids[g_num_customers - 1];
                g_num_customers--;
                break;
            }
        }

        if (WIFEXITED(status)) {
            log_msg("Proces %d zakonczyl sie (kod %d)", pid, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            log_msg_color(C_RED, "Proces %d zabity sygnalem %d", pid, WTERMSIG(status));
        }
    }
    g_sigchld_received = 0;
}

/* ===== Uruchamianie procesow ===== */

static void start_baker(void)
{
    if (pipe(g_baker_pipe) == -1)
        handle_error("pipe (baker)");

    pid_t pid = fork();
    if (pid == -1) handle_error("fork (baker)");

    if (pid == 0) {
        /* Dziecko - piekarz */
        close(g_baker_pipe[0]); /* Zamknij odczyt */
        char pipe_fd_str[16];
        snprintf(pipe_fd_str, sizeof(pipe_fd_str), "%d", g_baker_pipe[1]);

        /* Przekieruj stderr do pliku logu */
        char logpath[64];
        snprintf(logpath, sizeof(logpath), "%s/piekarz.log", LOG_DIR);
        int logfd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logfd >= 0) { dup2(logfd, STDERR_FILENO); close(logfd); }

        execl("./piekarz", "piekarz", KEY_FILE, pipe_fd_str, (char *)NULL);
        handle_error("execl (piekarz)");
    }

    close(g_baker_pipe[1]); /* Zamknij zapis - tylko piekarz pisze */
    g_shm->baker_pid = pid;
    log_msg("Piekarz uruchomiony PID=%d", pid);
}

static void start_cashier(int register_id)
{
    pid_t pid = fork();
    if (pid == -1) handle_error("fork (cashier)");

    if (pid == 0) {
        char reg_str[4];
        snprintf(reg_str, sizeof(reg_str), "%d", register_id);

        char logpath[64];
        snprintf(logpath, sizeof(logpath), "%s/kasjer_%d.log", LOG_DIR, register_id);
        int logfd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logfd >= 0) { dup2(logfd, STDERR_FILENO); close(logfd); }

        execl("./kasjer", "kasjer", KEY_FILE, reg_str, (char *)NULL);
        handle_error("execl (kasjer)");
    }

    g_shm->cashier_pids[register_id] = pid;
    log_msg("Kasjer %d uruchomiony PID=%d", register_id, pid);
}

static void start_customer(void)
{
    pid_t pid = fork();
    if (pid == -1) {
        handle_warning("fork (customer)");
        return;
    }

    if (pid == 0) {
        execl("./klient", "klient", KEY_FILE, (char *)NULL);
        handle_error("execl (klient)");
    }

    /* Dodaj PID do tablicy (z dynamicznym powiekszaniem) */
    if (g_num_customers >= g_customer_cap) {
        g_customer_cap = (g_customer_cap == 0) ? 64 : g_customer_cap * 2;
        g_customer_pids = realloc(g_customer_pids, g_customer_cap * sizeof(pid_t));
        if (!g_customer_pids) handle_error("realloc (customer_pids)");
    }
    g_customer_pids[g_num_customers++] = pid;

    sem_wait_op(g_sem_id, SEM_SHM_MUTEX);
    g_shm->active_customers++;
    g_shm->total_customers_entered++;
    sem_signal_op(g_sem_id, SEM_SHM_MUTEX);
}

/* ===== Parse args / init ===== */

static void print_usage(const char *progname)
{
    fprintf(stderr,
        "\nUzycie: %s [OPCJE]\n\n"
        "Opcje:\n"
        "  -n NUM   Maks. klientow w sklepie jednoczesnie (3-%d, domyslnie 10)\n"
        "  -p NUM   Liczba produktow (11-%d, domyslnie %d)\n"
        "  -s NUM   Skala czasu: ms na minute symulacji (10-2000, domyslnie 100)\n"
        "  -o HOUR  Godzina otwarcia sklepu (6-12, domyslnie 8)\n"
        "  -c HOUR  Godzina zamkniecia sklepu (12-22, domyslnie 16)\n"
        "  -t SEC   Maks. czas rzeczywisty symulacji w sekundach (0=bez limitu)\n"
        "  -h       Wyswietl pomoc\n\n",
        progname, MAX_ACTIVE_CUST, MAX_PRODUCTS, DEFAULT_NUM_PRODUCTS);
}

static void parse_args(int argc, char *argv[], SharedData *shm)
{
    shm->max_customers = 10;
    shm->num_products  = DEFAULT_NUM_PRODUCTS;
    shm->time_scale_ms = 100;
    shm->open_hour     = 8;  shm->open_min  = 0;
    shm->close_hour    = 16; shm->close_min = 0;

    int opt;
    while ((opt = getopt(argc, argv, "n:p:s:o:c:t:h")) != -1) {
        switch (opt) {
            case 'n': shm->max_customers = atoi(optarg);
                      validate_int_range(shm->max_customers, 3, MAX_ACTIVE_CUST, "max_customers"); break;
            case 'p': shm->num_products = atoi(optarg);
                      validate_int_range(shm->num_products, 11, MAX_PRODUCTS, "num_products"); break;
            case 's': shm->time_scale_ms = atoi(optarg);
                      validate_int_range(shm->time_scale_ms, 10, 2000, "time_scale_ms"); break;
            case 'o': shm->open_hour = atoi(optarg);
                      validate_int_range(shm->open_hour, 6, 12, "open_hour"); break;
            case 'c': shm->close_hour = atoi(optarg);
                      validate_int_range(shm->close_hour, 12, 22, "close_hour"); break;
            case 't': break; /* TODO wall-clock timeout */
            case 'h': default: print_usage(argv[0]); exit(EXIT_SUCCESS);
        }
    }
    if (shm->close_hour <= shm->open_hour) {
        fprintf(stderr, "Blad: godzina zamkniecia <= otwarcia\n"); exit(EXIT_FAILURE);
    }
}

static void init_shared_data(SharedData *shm)
{
    int np = shm->num_products;
    for (int i = 0; i < np && i < DEFAULT_NUM_PRODUCTS; i++)
        shm->products[i] = DEFAULT_PRODUCTS[i];
    for (int i = DEFAULT_NUM_PRODUCTS; i < np; i++) {
        snprintf(shm->products[i].name, MAX_NAME_LEN, "Produkt-%d", i + 1);
        shm->products[i].price = 3.0 + (i % 10);
        shm->products[i].conveyor_capacity = 6 + (i % 5);
    }
    shm->manager_pid = getpid();
    shm->simulation_running = 1;
    shm->bakery_open = 0; shm->shop_open = 0;
    shm->register_open[0] = 1; shm->register_accepting[0] = 1;
    shm->register_open[1] = 0; shm->register_accepting[1] = 0;
    shm->sim_hour = shm->open_hour; shm->sim_min = 0;
}

static void init_semaphore_values(int sem_id, SharedData *shm)
{
    int np = shm->num_products;
    init_semaphore(sem_id, SEM_SHM_MUTEX, 1);
    init_semaphore(sem_id, SEM_SHOP_ENTRY, shm->max_customers);
    for (int i = 0; i < np; i++)
        init_semaphore(sem_id, SEM_CONVEYOR_BASE + i, shm->products[i].conveyor_capacity);
}

static void print_banner(SharedData *shm)
{
    printf("\n%s========================================%s\n", C_BOLD, C_RESET);
    printf("%s  CIASTKARNIA - Symulacja%s\n", C_BOLD, C_RESET);
    printf("%s========================================%s\n", C_BOLD, C_RESET);
    printf("  Produktow:    %d\n", shm->num_products);
    printf("  Max klientow: %d\n", shm->max_customers);
    printf("  Skala czasu:  %d ms/min\n", shm->time_scale_ms);
    printf("  Godziny:      %02d:%02d - %02d:%02d\n",
           shm->open_hour, shm->open_min, shm->close_hour, shm->close_min);
    printf("%s========================================%s\n\n", C_BOLD, C_RESET);
}

/* ===== MAIN ===== */

int main(int argc, char *argv[])
{
    create_key_file();
    create_log_directory();

    atexit(atexit_cleanup);
    setup_signal_handlers();

    int shm_id = create_shared_memory(KEY_FILE);
    g_shm = attach_shared_memory(KEY_FILE);

    parse_args(argc, argv, g_shm);
    init_shared_data(g_shm);

    int np = g_shm->num_products;
    int total_sems = 2 + np;
    g_sem_id = create_semaphores(KEY_FILE, total_sems);
    init_semaphore_values(g_sem_id, g_shm);

    create_message_queue(KEY_FILE, PROJ_MQ_CONV);
    create_message_queue(KEY_FILE, PROJ_MQ_CHKOUT);
    create_message_queue(KEY_FILE, PROJ_MQ_RCPT);

    logger_init(g_shm, PROC_MANAGER, 0);
    print_banner(g_shm);

    log_msg("Kierownik uruchomiony. PID=%d, SHM=%d", getpid(), shm_id);

    /* Uruchom procesy */
    start_baker();
    start_cashier(0);
    start_cashier(1);

    g_shm->bakery_open = 1;
    g_shm->shop_open = 0; /* Sklep otworzy sie po 30 min sim */

    /* === Prosta petla glowna === */
    while (g_shm->simulation_running && !g_sigint_received) {
        if (g_sigchld_received)
            reap_children();

        /* Aktualizuj zegar */
        g_shm->sim_min++;
        if (g_shm->sim_min >= 60) {
            g_shm->sim_min = 0;
            g_shm->sim_hour++;
        }

        /* Otworz sklep po 30 min */
        int total_min = g_shm->sim_hour * 60 + g_shm->sim_min;
        int open_at = g_shm->open_hour * 60 + g_shm->open_min + 30;
        if (!g_shm->shop_open && total_min >= open_at) {
            g_shm->shop_open = 1;
            log_msg_color(C_GREEN, "Sklep otwarty!");
        }

        /* Spawning klientow */
        if (g_shm->shop_open && g_shm->bakery_open) {
            int interval = 1 + (rand() % 10);
            if (g_shm->sim_min % interval == 0) {
                int batch = 2 + (rand() % 7);
                for (int i = 0; i < batch; i++) {
                    if (g_shm->active_customers < MAX_ACTIVE_CUST &&
                        g_shm->total_customers_entered < MAX_CUSTOMERS_TOTAL)
                        start_customer();
                }
            }
        }

        /* Sprawdz zamkniecie */
        if (g_shm->sim_hour >= g_shm->close_hour && g_shm->sim_min >= g_shm->close_min) {
            log_msg("Godzina zamkniecia - koncze.");
            break;
        }

        msleep_safe(g_shm->time_scale_ms);
    }

    /* Zakoncz procesy */
    g_shm->simulation_running = 0;
    g_shm->shop_open = 0;
    g_shm->bakery_open = 0;

    log_msg("Wysylam SIGTERM do procesow...");
    if (g_shm->baker_pid > 0) kill(g_shm->baker_pid, SIGTERM);
    if (g_shm->cashier_pids[0] > 0) kill(g_shm->cashier_pids[0], SIGTERM);
    if (g_shm->cashier_pids[1] > 0) kill(g_shm->cashier_pids[1], SIGTERM);
    for (int i = 0; i < g_num_customers; i++) {
        if (g_customer_pids[i] > 0) kill(g_customer_pids[i], SIGTERM);
    }

    /* Czekaj na procesy */
    sleep(2);
    reap_children();

    log_msg("Symulacja zakonczona.");
    g_cleanup_done = 0; /* Pozwol atexit sprzatac */
    return 0;
}
