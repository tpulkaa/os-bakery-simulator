/**
 * klient.c - Proces klienta ciastkarni
 * Projekt: Ciastkarnia (Temat 15) - Systemy Operacyjne
 *
 * Klient przychodzi do sklepu z losowa lista zakupow,
 * pobiera produkty z podajnikow (kolejka komunikatow w trybie FIFO),
 * nastepnie udaje sie do kasy i otrzymuje paragon.
 *
 * Komunikacja:
 * - Podajniki: kolejka komunikatow (msgrcv z mtype = product_id + 1)
 * - Checkout: kolejka komunikatow (msgsnd z mtype = register_id + 1)
 * - Paragon: kolejka komunikatow (msgrcv z mtype = getpid())
 * - Stan: pamiec dzielona
 * - Wejscie do sklepu: semafor zliczajacy (SEM_SHOP_ENTRY)
 * - Sygnaly: SIGUSR2 (ewakuacja), SIGTERM
 */

#include "common.h"
#include "error_handler.h"
#include "ipc_utils.h"
#include "logger.h"

/* ================================================================
 *  ZMIENNE GLOBALNE PROCESU
 * ================================================================ */

static SharedData *g_shm          = NULL;
static int         g_sem_id       = -1;
static int         g_mq_conveyor  = -1;
static int         g_mq_checkout  = -1;
static int         g_mq_receipt   = -1;
static int         g_in_shop      = 0;   /* 1 jesli klient jest w sklepie */
static int         g_cart[MAX_PRODUCTS]; /* Koszyk - ile szt. kazdego produktu */

static volatile sig_atomic_t g_evacuation = 0;
static volatile sig_atomic_t g_terminate  = 0;

/* ================================================================
 *  OBSLUGA SYGNALOW
 * ================================================================ */

static void sigusr1_handler(int sig)
{
    (void)sig;
    /* Inwentaryzacja - klient kontynuuje zakupy normalnie */
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
 *  OPUSZCZANIE SKLEPU (wspoldzielone przez rozne sciezki wyjscia)
 * ================================================================ */

/**
 * Procedura opuszczania sklepu.
 * Dekrementuje licznik klientow i zwalnia semafor wejscia.
 */
static void leave_shop(void)
{
    if (!g_in_shop) return;

    sem_wait_undo(g_sem_id, SEM_SHM_MUTEX);
    if (g_shm->customers_in_shop > 0)
        g_shm->customers_in_shop--;
    sem_signal_undo(g_sem_id, SEM_SHM_MUTEX);

    /* Zwolnij miejsce w sklepie (semafor zliczajacy) */
    sem_signal_undo(g_sem_id, SEM_SHOP_ENTRY);

    g_in_shop = 0;
    log_msg("Opuscil sklep.");
}

/**
 * Procedura ewakuacji.
 * Klient odklada produkty do kosza przy kasach i wychodzi.
 */
static void handle_evacuation(void)
{
    log_msg_color(C_RED, "EWAKUACJA! Odkladam produkty do kosza i wychodzę!");

    /* Odloz produkty z koszyka do kosza ewakuacyjnego */
    sem_wait_undo(g_sem_id, SEM_SHM_MUTEX);
    for (int i = 0; i < g_shm->num_products; i++) {
        if (g_cart[i] > 0) {
            g_shm->basket_items[i] += g_cart[i];
            g_cart[i] = 0;
        }
    }
    sem_signal_undo(g_sem_id, SEM_SHM_MUTEX);

    leave_shop();
}

/* ================================================================
 *  GENEROWANIE LISTY ZAKUPOW
 * ================================================================ */

/**
 * Generuje losowa liste zakupow.
 * Klient wybiera min. 2 rozne produkty.
 * @param shopping_list  Tablica [MAX_PRODUCTS] - ilosc kazdego produktu
 */
static void generate_shopping_list(int *shopping_list)
{
    int np = g_shm->num_products;
    memset(shopping_list, 0, sizeof(int) * MAX_PRODUCTS);

    /* Wybierz min. 2, max. 5 roznych produktow */
    int num_types = 2 + rand() % 4;
    if (num_types > np) num_types = np;

    /* Losuj rozne produkty */
    int chosen[MAX_PRODUCTS];
    memset(chosen, 0, sizeof(chosen));
    int count = 0;

    while (count < num_types) {
        int prod = rand() % np;
        if (!chosen[prod]) {
            chosen[prod] = 1;
            shopping_list[prod] = 1 + rand() % 3;  /* 1-3 sztuki */
            count++;
        }
    }
}

/* ================================================================
 *  ZAKUPY - POBIERANIE PRODUKTOW Z PODAJNIKOW
 * ================================================================ */

/**
 * Klient pobiera produkty z podajnikow (kolejek komunikatow).
 * Produkty sa pobierane w kolejnosci FIFO z kazdego podajnika.
 * Jesli produkt niedostepny (podajnik pusty), klient go nie kupuje.
 *
 * @param shopping_list  Lista zakupow (ile chce)
 */
static void do_shopping(int *shopping_list)
{
    memset(g_cart, 0, sizeof(g_cart));

    for (int i = 0; i < g_shm->num_products; i++) {
        if (g_evacuation || g_terminate) return;

        if (shopping_list[i] <= 0) continue;

        int wanted = shopping_list[i];
        int got = 0;

        for (int q = 0; q < wanted; q++) {
            if (g_evacuation || g_terminate) return;

            /* Proba pobrania produktu z podajnika (nieblokujaca) */
            struct conveyor_msg cmsg;
            ssize_t ret = msgrcv_guarded(g_mq_conveyor, &cmsg,
                                 sizeof(cmsg) - sizeof(long),
                                 i + 1, IPC_NOWAIT,
                                 g_sem_id, SEM_GUARD_CONV(g_shm->num_products));

            if (ret == -1) {
                if (errno == ENOMSG) {
                    /* Podajnik pusty - nie kupujemy */
                    break;
                }
                if (errno == EINTR) continue;
                if (errno == EIDRM) return; /* Kolejka usunieta */
                break;
            }

            /* Pobralismy produkt - zwolnij miejsce na podajniku */
            sem_signal_op(g_sem_id, SEM_CONVEYOR_BASE + i);
            got++;

            /* Symulacja czasu pobierania: 0.5 min symulacji */
            usleep(g_shm->time_scale_ms * 500);
        }

        g_cart[i] = got;

        if (got > 0) {
            log_msg("Pobrano %d/%d szt. '%s' z podajnika",
                    got, wanted, g_shm->products[i].name);
        } else if (wanted > 0) {
            log_msg("Produkt '%s' niedostepny (podajnik pusty)",
                    g_shm->products[i].name);
        }
    }
}

/* ================================================================
 *  KASA - CHECKOUT
 * ================================================================ */

/**
 * Klient udaje sie do kasy z najkrotsza kolejka.
 * Wysyla komunikat checkout i czeka na paragon.
 *
 * @return 0 jesli obsluzony, -1 jesli przerwany
 */
static int do_checkout(void)
{
    /* Sprawdz czy mamy cokolwiek w koszyku */
    int total_items = 0;
    for (int i = 0; i < g_shm->num_products; i++)
        total_items += g_cart[i];

    if (total_items == 0) {
        log_msg("Koszyk pusty - opuszczam sklep bez zakupow.");
        return 0;
    }

    /* Wybierz kase z najkrotsza kolejka */
    sem_wait_undo(g_sem_id, SEM_SHM_MUTEX);

    int chosen_register = 0;
    if (g_shm->register_accepting[1] && g_shm->register_open[1]) {
        if (g_shm->register_queue_len[1] < g_shm->register_queue_len[0]) {
            chosen_register = 1;
        }
    }
    g_shm->register_queue_len[chosen_register]++;

    sem_signal_undo(g_sem_id, SEM_SHM_MUTEX);

    log_msg("Ustawiam sie w kolejce do kasy nr %d (dlugosc: %d)",
            chosen_register + 1,
            g_shm->register_queue_len[chosen_register]);

    /* Wyslij komunikat checkout */
    struct checkout_msg cmsg;
    cmsg.mtype = chosen_register + 1;
    cmsg.customer_pid = getpid();
    memcpy(cmsg.items, g_cart, sizeof(g_cart));

    if (msgsnd_guarded(g_mq_checkout, &cmsg, sizeof(cmsg) - sizeof(long),
                       g_sem_id, SEM_GUARD_CHKOUT(g_shm->num_products)) == -1) {
        if (errno == EINTR || errno == EIDRM || errno == EINVAL) return -1;
        handle_warning("msgsnd (checkout)");
        return -1;
    }

    /* Czekaj na paragon - z timeoutem (sprawdzaj ewakuacje) */
    struct receipt_msg rmsg;
    int wait_cycles = 0;
    int max_wait = 300; /* maks. 300 prob */

    while (!g_evacuation && !g_terminate && wait_cycles < max_wait) {
        ssize_t ret = msgrcv_guarded(g_mq_receipt, &rmsg,
                             sizeof(rmsg) - sizeof(long),
                             (long)getpid(), IPC_NOWAIT,
                             g_sem_id, SEM_GUARD_RCPT(g_shm->num_products));

        if (ret >= 0) {
            /* Otrzymano paragon! */
            log_msg_color(C_GREEN,
                "Paragon: %d produktow, RAZEM: %.2f PLN", total_items, rmsg.total);
            return 0;
        }

        if (errno == ENOMSG) {
            usleep(g_shm->time_scale_ms * 300);
            wait_cycles++;
            continue;
        }
        if (errno == EINTR) continue;
        if (errno == EIDRM) return -1;

        handle_warning("msgrcv (receipt)");
        return -1;
    }

    if (g_evacuation) return -1;

    log_msg("Timeout czekania na paragon - opuszczam sklep.");
    return -1;
}

/* ================================================================
 *  GLOWNA FUNKCJA KLIENTA
 * ================================================================ */

int main(int argc, char *argv[])
{
    /* --- Parsowanie argumentow --- */
    if (argc < 2) {
        fprintf(stderr, "Uzycie: klient <keyfile>\n");
        return EXIT_FAILURE;
    }

    const char *keyfile = argv[1];

    srand(time(NULL) ^ getpid());

    /* --- Dolaczenie do zasobow IPC --- */
    g_shm = attach_shared_memory(keyfile);

    int num_sems = TOTAL_SEMS(g_shm->num_products);
    g_sem_id = get_semaphores(keyfile, num_sems);

    g_mq_conveyor = get_message_queue(keyfile, PROJ_MQ_CONV);
    g_mq_checkout = get_message_queue(keyfile, PROJ_MQ_CHKOUT);
    g_mq_receipt  = get_message_queue(keyfile, PROJ_MQ_RCPT);

    /* --- Logger --- */
    logger_init(g_shm, PROC_CUSTOMER, getpid());

    /* --- Sygnaly --- */
    setup_signals();

    /* --- Generuj liste zakupow --- */
    int shopping_list[MAX_PRODUCTS];
    generate_shopping_list(shopping_list);

    /* Wyswietl liste zakupow */
    log_msg("Przyszedl do sklepu. Lista zakupow:");
    for (int i = 0; i < g_shm->num_products; i++) {
        if (shopping_list[i] > 0) {
            log_msg("  - %s: %d szt.", g_shm->products[i].name, shopping_list[i]);
        }
    }

    /* --- Wejscie do sklepu (semafor zliczajacy) --- */
    if (!g_shm->shop_open || g_shm->evacuation_mode) {
        log_msg("Sklep zamkniety - odchodzi.");
        detach_shared_memory(g_shm);
        return EXIT_SUCCESS;
    }

    log_msg("Czeka na wejscie do sklepu...");

    /* Proba wejscia z timeoutem - nie czekaj w nieskonczonosc */
    int entry_attempts = 0;
    while (entry_attempts < 100) {
        if (g_evacuation || g_terminate || !g_shm->shop_open) {
            log_msg("Sklep zamkniety/ewakuacja - odchodzi.");
            detach_shared_memory(g_shm);
            return EXIT_SUCCESS;
        }

        if (sem_trywait_undo(g_sem_id, SEM_SHOP_ENTRY) == 0) {
            break; /* Udalo sie wejsc */
        }

        /* Sklep pelny - czekaj */
        usleep(g_shm->time_scale_ms * 1000);
        entry_attempts++;
    }

    if (entry_attempts >= 100) {
        log_msg("Czekanie zbyt dlugie - odchodzi.");
        detach_shared_memory(g_shm);
        return EXIT_SUCCESS;
    }

    /* Klient wszedl do sklepu */
    g_in_shop = 1;
    sem_wait_undo(g_sem_id, SEM_SHM_MUTEX);
    g_shm->customers_in_shop++;
    sem_signal_undo(g_sem_id, SEM_SHM_MUTEX);

    log_msg("Wszedl do sklepu (klientow w srodku: %d/%d)",
            g_shm->customers_in_shop, g_shm->max_customers);

    /* --- Sprawdz ewakuacje --- */
    if (g_evacuation) {
        handle_evacuation();
        detach_shared_memory(g_shm);
        return EXIT_SUCCESS;
    }

    /* --- Zakupy --- */
    do_shopping(shopping_list);

    /* --- Sprawdz ewakuacje po zakupach --- */
    if (g_evacuation) {
        handle_evacuation();
        detach_shared_memory(g_shm);
        return EXIT_SUCCESS;
    }

    /* --- Kasa --- */
    int checkout_result = do_checkout();

    /* --- Sprawdz ewakuacje po kasie --- */
    if (g_evacuation && checkout_result != 0) {
        handle_evacuation();
        detach_shared_memory(g_shm);
        return EXIT_SUCCESS;
    }

    /* --- Opuszczenie sklepu --- */
    leave_shop();

    /* --- Sprzatanie --- */
    detach_shared_memory(g_shm);

    return EXIT_SUCCESS;
}
