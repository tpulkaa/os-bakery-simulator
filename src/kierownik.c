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
    /* Domyslne wartosci */
    shm->max_customers = 10;
    shm->num_products  = DEFAULT_NUM_PRODUCTS;
    shm->time_scale_ms = 100;
    shm->open_hour     = 8;
    shm->open_min      = 0;
    shm->close_hour    = 16;
    shm->close_min     = 0;

    int opt;
    while ((opt = getopt(argc, argv, "n:p:s:o:c:t:h")) != -1) {
        switch (opt) {
            case 'n':
                shm->max_customers = atoi(optarg);
                validate_int_range(shm->max_customers, 3, MAX_ACTIVE_CUST, "max_customers");
                break;
            case 'p':
                shm->num_products = atoi(optarg);
                validate_int_range(shm->num_products, 11, MAX_PRODUCTS, "num_products");
                break;
            case 's':
                shm->time_scale_ms = atoi(optarg);
                validate_int_range(shm->time_scale_ms, 10, 2000, "time_scale_ms");
                break;
            case 'o':
                shm->open_hour = atoi(optarg);
                validate_int_range(shm->open_hour, 6, 12, "open_hour");
                break;
            case 'c':
                shm->close_hour = atoi(optarg);
                validate_int_range(shm->close_hour, 12, 22, "close_hour");
                break;
            case 't':
                /* max wall-clock time - TODO */
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                exit(EXIT_SUCCESS);
        }
    }

    if (shm->close_hour <= shm->open_hour) {
        fprintf(stderr, "Blad: godzina zamkniecia (%d) musi byc wieksza od otwarcia (%d)\n",
                shm->close_hour, shm->open_hour);
        exit(EXIT_FAILURE);
    }
}

static void init_shared_data(SharedData *shm)
{
    /* Skopiuj definicje produktow */
    int np = shm->num_products;
    for (int i = 0; i < np && i < DEFAULT_NUM_PRODUCTS; i++) {
        shm->products[i] = DEFAULT_PRODUCTS[i];
    }
    /* Jesli wiecej niz domyslne */
    for (int i = DEFAULT_NUM_PRODUCTS; i < np; i++) {
        snprintf(shm->products[i].name, MAX_NAME_LEN, "Produkt-%d", i + 1);
        shm->products[i].price = 3.0 + (i % 10);
        shm->products[i].conveyor_capacity = 6 + (i % 5);
    }

    shm->manager_pid = getpid();
    shm->simulation_running = 1;
    shm->bakery_open = 0;
    shm->shop_open = 0;
    shm->register_open[0] = 1;      /* Kasa 0 zawsze otwarta */
    shm->register_accepting[0] = 1;
    shm->register_open[1] = 0;
    shm->register_accepting[1] = 0;
    shm->sim_hour = shm->open_hour;
    shm->sim_min  = 0;
}

static void init_semaphore_values(int sem_id, SharedData *shm)
{
    int np = shm->num_products;

    init_semaphore(sem_id, SEM_SHM_MUTEX, 1);
    init_semaphore(sem_id, SEM_SHOP_ENTRY, shm->max_customers);

    for (int i = 0; i < np; i++) {
        init_semaphore(sem_id, SEM_CONVEYOR_BASE + i, shm->products[i].conveyor_capacity);
    }
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

    /* Utworz pamiec dzielona */
    int shm_id = create_shared_memory(KEY_FILE);
    g_shm = attach_shared_memory(KEY_FILE);

    parse_args(argc, argv, g_shm);
    init_shared_data(g_shm);

    int np = g_shm->num_products;

    /* Utworz semafory */
    int total_sems = 2 + np; /* mutex + shop_entry + conveyors */
    g_sem_id = create_semaphores(KEY_FILE, total_sems);
    init_semaphore_values(g_sem_id, g_shm);

    /* Utworz kolejki komunikatow */
    int mq_conv   = create_message_queue(KEY_FILE, PROJ_MQ_CONV);
    int mq_chkout = create_message_queue(KEY_FILE, PROJ_MQ_CHKOUT);
    int mq_rcpt   = create_message_queue(KEY_FILE, PROJ_MQ_RCPT);

    logger_init(g_shm, PROC_MANAGER, 0);
    print_banner(g_shm);

    log_msg("Kierownik uruchomiony. PID=%d", getpid());
    log_msg("IPC utworzone: shm=%d, sem=%d, mq=%d/%d/%d",
            shm_id, g_sem_id, mq_conv, mq_chkout, mq_rcpt);

    /* TODO: fork() procesow potomnych */
    /* TODO: petla glowna symulacji */

    log_msg("Symulacja zakonczona (skeleton).");

    /* Czyszczenie */
    detach_shared_memory(g_shm);
    remove_message_queue(KEY_FILE, PROJ_MQ_CONV);
    remove_message_queue(KEY_FILE, PROJ_MQ_CHKOUT);
    remove_message_queue(KEY_FILE, PROJ_MQ_RCPT);
    remove_semaphores(KEY_FILE);
    remove_shared_memory(KEY_FILE);
    unlink(KEY_FILE);

    return 0;
}
