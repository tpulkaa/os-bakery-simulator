/**
 * kierownik.c - Proces kierownika ciastkarni (Manager)
 * Projekt: Ciastkarnia (Temat 15) - Systemy Operacyjne
 *
 * Glowny proces symulacji. Odpowiada za:
 * - Tworzenie i inicjalizacje zasobow IPC
 * - Uruchamianie procesow piekarza, kasjerow i klientow (fork + exec)
 * - Zarzadzanie zegarem symulacji
 * - Monitorowanie stanu sklepu (otwieranie/zamykanie kas)
 * - Obsluge sygnalow (inwentaryzacja, ewakuacja)
 * - Generowanie raportu koncowego
 * - Czyszczenie zasobow po zakonczeniu
 *
 * Uzycie: ./kierownik [-n max_klientow] [-p produkty] [-s skala_czasu_ms]
 *                      [-o godzina_otwarcia] [-c godzina_zamkniecia]
 */

#include "common.h"
#include "error_handler.h"
#include "ipc_utils.h"
#include "logger.h"

/* ================================================================
 *  ZMIENNE GLOBALNE PROCESU
 * ================================================================ */

static SharedData *g_shm         = NULL;   /* Wskaznik do pamieci dzielonej */
static int         g_sem_id      = -1;     /* ID zbioru semaforow */
static int         g_baker_pipe[2] = {-1, -1}; /* Pipe: piekarz -> kierownik */
static pid_t      *g_customer_pids = NULL;  /* Dynamiczna tablica PIDow klientow */
static int         g_num_customers = 0;    /* Liczba slotow w tablicy */
static int         g_customer_cap  = 0;    /* Pojemnosc tablicy */
static volatile sig_atomic_t g_sigchld_received = 0;
static volatile sig_atomic_t g_sigint_received  = 0;
static volatile sig_atomic_t g_sigcont_received = 0;
static int         g_max_time     = 0;     /* Maks. czas symulacji w sekundach (0 = bez limitu) */
static int         g_cleanup_done = 0;     /* Flaga zapobiegajaca podwojnemu czyszczeniu */

/**
 * EINTR-resistant sleep (milisekundy).
 * Retries nanosleep on signal interruption so simulation timing stays accurate.
 */
static void msleep_safe(int ms)
{
    struct timespec req = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    struct timespec rem;
    while (nanosleep(&req, &rem) == -1 && errno == EINTR) {
        req = rem;
    }
}

/* Safety net: atexit handler czysci IPC nawet przy niespodziewanym crash'u */
static void atexit_cleanup(void)
{
    if (g_cleanup_done) return;
    g_cleanup_done = 1;
    if (g_shm != NULL) {
        detach_shared_memory(g_shm);
        g_shm = NULL;
    }
    if (access(KEY_FILE, F_OK) == 0) {
        cleanup_all_ipc(KEY_FILE, MAX_PRODUCTS);
    }
    free(g_customer_pids);
    g_customer_pids = NULL;
}

/* ================================================================
 *  OBSLUGA SYGNALOW
 * ================================================================ */

/**
 * Handler SIGCHLD - natychmiastowe zbieranie zombie.
 * Wywoluje waitpid() bezposrednio w handlerze (async-signal-safe)
 * aby zapobiec akumulacji zombie podczas SIGTSTP (Ctrl+Z).
 */
static void sigchld_handler(int sig)
{
    (void)sig;
    int saved_errno = errno;
    /* Natychmiast zbieraj zombie - zapobiega ich akumulacji podczas SIGTSTP */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    g_sigchld_received = 1;
    errno = saved_errno;
}

/**
 * Handler SIGINT/SIGTERM - czyste zamkniecie symulacji.
 */
static void sigint_handler(int sig)
{
    (void)sig;
    g_sigint_received = 1;
}

/**
 * Handler SIGCONT - po wznowieniu procesu (po Ctrl+Z + fg).
 * Natychmiast zbiera zombie ktore mogly sie nagromadzic podczas zatrzymania.
 */
static void sigcont_handler(int sig)
{
    (void)sig;
    int saved_errno = errno;
    /* Po wznowieniu z SIGTSTP - zbierz wszystkie zombie */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    g_sigcont_received = 1;
    g_sigchld_received = 1;
    errno = saved_errno;
}

/**
 * Konfiguracja handlerow sygnalow za pomoca sigaction().
 */
static void setup_signal_handlers(void)
{
    struct sigaction sa;

    /* SIGCHLD - zbieranie zombie */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGCHLD, &sa, NULL) == -1)
        handle_error("sigaction (SIGCHLD)");

    /* SIGINT/SIGTERM - zamkniecie */
    sa.sa_handler = sigint_handler;
    sa.sa_flags   = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1)
        handle_error("sigaction (SIGINT)");
    if (sigaction(SIGTERM, &sa, NULL) == -1)
        handle_error("sigaction (SIGTERM)");

    /* SIGCONT - czyszczenie zombie po wznowieniu z Ctrl+Z */
    sa.sa_handler = sigcont_handler;
    sa.sa_flags   = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGCONT, &sa, NULL) == -1)
        handle_error("sigaction (SIGCONT)");
}

/* ================================================================
 *  ZBIERANIE PROCESOW POTOMNYCH
 * ================================================================ */

/**
 * Zbiera zakonczane procesy potomne i aktualizuje tablice PID.
 *
 * Uzywa kill(pid, 0) do skanowania tablicy PID zamiast polegac
 * wylacznie na waitpid() - handler SIGCHLD juz zbiera zombie inline,
 * wiec waitpid w petli glownej moze nie znalezc potomkow.
 * kill(pid, 0) + ESRCH niezawodnie wykrywa martwe procesy.
 *
 * SIGCHLD jest blokowany podczas aktualizacji tablic aby uniknac
 * interferencji z handlerem sygnalu.
 */
static void reap_children(void)
{
    /* Zablokuj SIGCHLD podczas aktualizacji tablic PID */
    sigset_t block_chld, oldset;
    sigemptyset(&block_chld);
    sigaddset(&block_chld, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block_chld, &oldset);

    /* Zbierz ewentualne zombie (safety net - handler tez zbiera) */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;

    if (g_shm == NULL) {
        sigprocmask(SIG_SETMASK, &oldset, NULL);
        return;
    }

    /* Sprawdz piekarza - czy proces jeszcze zyje */
    if (g_shm->baker_pid > 0) {
        if (kill(g_shm->baker_pid, 0) == -1 && errno == ESRCH) {
            if (g_shm->simulation_running)
                log_msg_color(C_RED, "UWAGA: Piekarz (PID:%d) zakonczyl prace nieoczekiwanie!",
                              g_shm->baker_pid);
            g_shm->baker_pid = 0;
        }
    }

    /* Sprawdz kasjerow */
    for (int c = 0; c < 2; c++) {
        if (g_shm->cashier_pids[c] > 0) {
            if (kill(g_shm->cashier_pids[c], 0) == -1 && errno == ESRCH) {
                if (g_shm->simulation_running)
                    log_msg_color(C_RED, "UWAGA: Kasjer %d (PID:%d) zakonczyl prace nieoczekiwanie!",
                                  c + 1, g_shm->cashier_pids[c]);
                g_shm->cashier_pids[c] = 0;
                g_shm->register_open[c] = 0;
                g_shm->register_accepting[c] = 0;
            }
        }
    }

    /* Sprawdz klientow - skanuj tablice PID i usun martwe procesy */
    int reaped_count = 0;
    for (int i = 0; i < g_num_customers; i++) {
        if (g_customer_pids[i] > 0) {
            if (kill(g_customer_pids[i], 0) == -1 && errno == ESRCH) {
                g_customer_pids[i] = 0;
                reaped_count++;
            }
        }
    }

    /* Zaktualizuj licznik aktywnych klientow hurtowo */
    if (reaped_count > 0) {
        sem_wait_undo(g_sem_id, SEM_SHM_MUTEX);
        g_shm->active_customers -= reaped_count;
        if (g_shm->active_customers < 0)
            g_shm->active_customers = 0;
        sem_signal_undo(g_sem_id, SEM_SHM_MUTEX);
    }

    /* Przywroc maske sygnalow */
    sigprocmask(SIG_SETMASK, &oldset, NULL);
}

/* ================================================================
 *  PARSOWANIE ARGUMENTOW I WALIDACJA
 * ================================================================ */

/**
 * Wyswietla informacje o uzyciu programu.
 */
static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Uzycie: %s [opcje]\n"
        "Opcje:\n"
        "  -n N     Maks. klientow w sklepie (domyslnie: 10)\n"
        "  -p P     Liczba produktow (domyslnie: 12, min: 11)\n"
        "  -s MS    Skala czasu: ms na minute symulacji (domyslnie: 100)\n"
        "  -o HH    Godzina otwarcia ciastkarni (domyslnie: 8)\n"
        "  -c HH    Godzina zamkniecia (domyslnie: 16)\n"
        "  -t SEC   Maks. czas symulacji w sekundach (0 = bez limitu)\n"
        "  -h       Wyswietl pomoc\n",
        prog);
}

/**
 * Parsowanie i walidacja argumentow wiersza polecen.
 * Zwraca 0 przy sukcesie, -1 przy bledzie.
 */
static int parse_args(int argc, char *argv[], SharedData *shm)
{
    /* Wartosci domyslne */
    shm->max_customers  = 1500;
    shm->num_products   = DEFAULT_NUM_PRODUCTS;
    shm->time_scale_ms  = 100;
    shm->open_hour      = 8;
    shm->open_min       = 0;
    shm->close_hour     = 23;
    shm->close_min      = 0;

    int opt;
    while ((opt = getopt(argc, argv, "n:p:s:o:c:t:h")) != -1) {
        switch (opt) {
            case 'n':
                shm->max_customers = atoi(optarg);
                break;
            case 'p':
                shm->num_products = atoi(optarg);
                break;
            case 's':
                shm->time_scale_ms = atoi(optarg);
                break;
            case 'o':
                shm->open_hour = atoi(optarg);
                break;
            case 'c':
                shm->close_hour = atoi(optarg);
                break;
            case 't':
                g_max_time = atoi(optarg);
                break;
            case 'h':
                print_usage(argv[0]);
                exit(EXIT_SUCCESS);
            default:
                print_usage(argv[0]);
                return -1;
        }
    }

    /* Walidacja parametrow */
    if (validate_int_range(shm->max_customers, 2, MAX_ACTIVE_CUST,
            "max_klientow (-n)") != 0) return -1;
    if (validate_int_range(shm->num_products, 1, MAX_PRODUCTS,
            "produkty (-p)") != 0) return -1;
    if (validate_int_range(shm->time_scale_ms, 10, 5000,
            "skala_czasu (-s)") != 0) return -1;
    if (validate_int_range(shm->open_hour, 0, 23,
            "godzina_otwarcia (-o)") != 0) return -1;
    if (validate_int_range(shm->close_hour, 1, 24,
            "godzina_zamkniecia (-c)") != 0) return -1;

    if (shm->close_hour <= shm->open_hour) {
        fprintf(stderr, "%s[WALIDACJA]%s Godzina zamkniecia (%d) musi byc "
                "pozniejsza niz otwarcia (%d).\n",
                C_RED, C_RESET, shm->close_hour, shm->open_hour);
        return -1;
    }

    if (g_max_time < 0) {
        fprintf(stderr, "%s[WALIDACJA]%s Czas symulacji (-t) musi byc >= 0.\n",
                C_RED, C_RESET);
        return -1;
    }

    return 0;
}

/* ================================================================
 *  INICJALIZACJA ZASOBOW
 * ================================================================ */

/**
 * Tworzy plik klucza dla ftok().
 * Uzywa creat() - wymagane do demonstracji operacji na plikach.
 */
static void create_key_file(void)
{
    int fd = creat(KEY_FILE, 0644);
    if (fd == -1)
        handle_error("creat (key file)");
    close(fd);
}

/**
 * Tworzy katalog na logi jesli nie istnieje.
 */
static void create_log_directory(void)
{
    struct stat st;
    if (stat(LOG_DIR, &st) == -1) {
        if (mkdir(LOG_DIR, 0755) == -1)
            handle_error("mkdir (logs)");
    }
}

/**
 * Inicjalizuje pamiec dzielona domyslnymi produktami.
 */
static void init_shared_data(SharedData *shm)
{
    /* Skopiuj definicje produktow */
    int p = shm->num_products;
    if (p > DEFAULT_NUM_PRODUCTS) p = DEFAULT_NUM_PRODUCTS;

    for (int i = 0; i < p; i++) {
        shm->products[i] = DEFAULT_PRODUCTS[i];
    }
    /* Jesli P > 12, dodaj warianty istniejacych produktow */
    for (int i = p; i < shm->num_products; i++) {
        snprintf(shm->products[i].name, MAX_NAME_LEN,
                 "%s Extra", DEFAULT_PRODUCTS[i % DEFAULT_NUM_PRODUCTS].name);
        shm->products[i].price = DEFAULT_PRODUCTS[i % DEFAULT_NUM_PRODUCTS].price * 1.2;
        shm->products[i].conveyor_capacity =
            DEFAULT_PRODUCTS[i % DEFAULT_NUM_PRODUCTS].conveyor_capacity;
    }

    /* Stan poczatkowy */
    shm->manager_pid       = getpid();
    shm->simulation_running = 1;
    shm->bakery_open        = 0;
    shm->shop_open           = 0;
    shm->inventory_mode      = 0;
    shm->evacuation_mode     = 0;
    shm->customers_in_shop   = 0;
    shm->active_customers    = 0;
    shm->total_customers_entered = 0;
    shm->customers_served        = 0;
    shm->customers_not_served    = 0;

    /* Kasy: obie otwarte od poczatku */
    shm->register_open[0]      = 1;
    shm->register_open[1]      = 1;
    shm->register_accepting[0] = 1;
    shm->register_accepting[1] = 1;

    /* Zegar symulacji = godzina otwarcia piekarni */
    shm->sim_hour = shm->open_hour;
    shm->sim_min  = 0;
}

/**
 * Inicjalizuje semafory poczatkowymi wartosciami.
 *
 * @param sem_id ID zbioru semaforow
 * @param shm    Wskaznik do pamieci dzielonej (konfiguracja)
 */
static void init_semaphore_values(int sem_id, SharedData *shm)
{
    /* SEM_SHM_MUTEX: binarny mutex (poczatkowo odblokowany) */
    init_semaphore(sem_id, SEM_SHM_MUTEX, 1);

    /* SEM_SHOP_ENTRY: semafor zliczajacy (poczatkowo N wolnych miejsc) */
    init_semaphore(sem_id, SEM_SHOP_ENTRY, shm->max_customers);

    /* Semafory podajnikow: wolne miejsca = pojemnosc Ki */
    for (int i = 0; i < shm->num_products; i++) {
        init_semaphore(sem_id, SEM_CONVEYOR_BASE + i,
                       shm->products[i].conveyor_capacity);
    }
}

/* ================================================================
 *  URUCHAMIANIE PROCESOW POTOMNYCH (fork + exec)
 * ================================================================ */

/**
 * Uruchamia proces piekarza.
 * Tworzy lacze nienazwane (pipe) do komunikacji piekarz -> kierownik.
 * Przekierowuje stderr piekarza do pliku logu (dup2).
 */
static pid_t start_baker(void)
{
    create_pipe(g_baker_pipe);

    pid_t pid = fork();
    if (pid == -1)
        handle_error("fork (baker)");

    if (pid == 0) {
        /* Proces potomny - piekarz */
        close(g_baker_pipe[0]); /* Zamknij koniec do czytania */

        /* Przekierowanie stderr do pliku logu (demonstracja dup2) */
        int log_fd = open("logs/piekarz.log",
                          O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd != -1) {
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }

        /* Przygotuj argumenty dla exec */
        char pipe_fd_str[16];
        snprintf(pipe_fd_str, sizeof(pipe_fd_str), "%d", g_baker_pipe[1]);

        execl("./piekarz", "piekarz", KEY_FILE, pipe_fd_str, (char *)NULL);
        /* Jesli exec sie nie powiodl */
        perror("execl (piekarz)");
        _exit(EXIT_FAILURE);
    }

    /* Proces macierzysty */
    close(g_baker_pipe[1]); /* Zamknij koniec do pisania */
    return pid;
}

/**
 * Uruchamia proces kasjera.
 * @param register_id Numer kasy (0 lub 1)
 */
static pid_t start_cashier(int register_id)
{
    pid_t pid = fork();
    if (pid == -1)
        handle_error("fork (cashier)");

    if (pid == 0) {
        /* Przekierowanie stderr do pliku logu */
        char log_path[64];
        snprintf(log_path, sizeof(log_path), "logs/kasjer_%d.log", register_id);
        int log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd != -1) {
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }

        char reg_str[8];
        snprintf(reg_str, sizeof(reg_str), "%d", register_id);

        execl("./kasjer", "kasjer", KEY_FILE, reg_str, (char *)NULL);
        perror("execl (kasjer)");
        _exit(EXIT_FAILURE);
    }

    return pid;
}

/**
 * Uruchamia proces klienta.
 * Klient jest tworzony w trakcie symulacji gdy sklep jest otwarty.
 */
static pid_t start_customer(void)
{
    pid_t pid = fork();
    if (pid == -1) {
        handle_warning("fork (customer)");
        return -1;
    }

    if (pid == 0) {
        execl("./klient", "klient", KEY_FILE, (char *)NULL);
        perror("execl (klient)");
        _exit(EXIT_FAILURE);
    }

    /* Zarejestruj PID klienta — szukaj wolnego slotu lub rozszerz tablice */
    int registered = 0;
    for (int i = 0; i < g_num_customers; i++) {
        if (g_customer_pids[i] <= 0) {
            g_customer_pids[i] = pid;
            registered = 1;
            break;
        }
    }
    if (!registered) {
        if (g_num_customers >= g_customer_cap) {
            int new_cap = g_customer_cap == 0 ? 64 : g_customer_cap * 2;
            pid_t *tmp = realloc(g_customer_pids, new_cap * sizeof(pid_t));
            if (tmp == NULL) {
                handle_warning("realloc (customer_pids)");
            } else {
                g_customer_pids = tmp;
                g_customer_cap = new_cap;
            }
        }
        if (g_num_customers < g_customer_cap) {
            g_customer_pids[g_num_customers++] = pid;
        }
    }

    sem_wait_undo(g_sem_id, SEM_SHM_MUTEX);
    g_shm->active_customers++;
    g_shm->total_customers_entered++;
    sem_signal_undo(g_sem_id, SEM_SHM_MUTEX);

    return pid;
}

/* ================================================================
 *  LOGIKA ZARZADZANIA KASAMI
 * ================================================================ */

/**
 * Aktualizuje stan kas na podstawie liczby klientow.
 *
 * Zasady:
 * - Zawsze min. 1 kasa otwarta (kasa 0)
 * - Jesli klientow >= N/2, obie kasy otwarte
 * - Jesli klientow < N/2, kasa 1 konczy obsluge kolejki i zamyka sie
 */
static void update_register_state(void)
{
    sem_wait_undo(g_sem_id, SEM_SHM_MUTEX);

    int nc = g_shm->customers_in_shop;
    int threshold = g_shm->max_customers / 4;

    if (threshold < 1) threshold = 1;

    if (nc >= threshold) {
        /* Potrzebne obie kasy */
        if (!g_shm->register_accepting[1]) {
            g_shm->register_accepting[1] = 1;
            g_shm->register_open[1]      = 1;
            sem_signal_undo(g_sem_id, SEM_SHM_MUTEX);
            log_msg("Otwieram kase nr 2 (klientow: %d >= %d)", nc, threshold);
            return;
        }
    } else {
        /* Wystarczy jedna kasa */
        if (g_shm->register_accepting[1]) {
            g_shm->register_accepting[1] = 0;
            /* Kasa 1 dokoncza obsluge kolejki */
            sem_signal_undo(g_sem_id, SEM_SHM_MUTEX);
            log_msg("Zamykam kase nr 2 (klientow: %d < %d) - dokonczy kolejke",
                    nc, threshold);
            return;
        }
        /* Sprawdz czy kasa 1 moze sie juz zamknac */
        if (g_shm->register_open[1] && g_shm->register_queue_len[1] == 0) {
            g_shm->register_open[1] = 0;
        }
    }

    sem_signal_undo(g_sem_id, SEM_SHM_MUTEX);
}

/* ================================================================
 *  OBSLUGA FIFO POLECEN (lacze nazwane)
 * ================================================================ */

/**
 * Sprawdza FIFO polecen w trybie nieblokujacym.
 * Komendy: "inventory" / "inwentaryzacja" -> SIGUSR1
 *          "evacuate" / "ewakuacja"       -> SIGUSR2
 *
 * @param fifo_fd Deskryptor FIFO (otwarty nieblokujaco)
 */
static void check_fifo_commands(int fifo_fd)
{
    if (fifo_fd < 0) return;

    char buf[64];
    ssize_t n = read(fifo_fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;

    buf[n] = '\0';
    /* Usun biale znaki z konca */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
        buf[--n] = '\0';

    if (strcmp(buf, "inventory") == 0 || strcmp(buf, "inwentaryzacja") == 0) {
        log_msg_color(C_RED, ">>> SYGNAL INWENTARYZACJI <<<");
        g_shm->inventory_mode = 1;

        /* Wyslij SIGUSR1 do wszystkich procesow potomnych */
        if (g_shm->baker_pid > 0)
            kill(g_shm->baker_pid, SIGUSR1);
        for (int i = 0; i < 2; i++) {
            if (g_shm->cashier_pids[i] > 0)
                kill(g_shm->cashier_pids[i], SIGUSR1);
        }
        for (int i = 0; i < g_num_customers; i++) {
            if (g_customer_pids[i] > 0)
                kill(g_customer_pids[i], SIGUSR1);
        }
    }
    else if (strcmp(buf, "evacuate") == 0 || strcmp(buf, "ewakuacja") == 0) {
        log_msg_color(C_RED, ">>> SYGNAL EWAKUACJI <<<");
        g_shm->evacuation_mode = 1;

        /* Wyslij SIGUSR2 do wszystkich procesow potomnych */
        if (g_shm->baker_pid > 0)
            kill(g_shm->baker_pid, SIGUSR2);
        for (int i = 0; i < 2; i++) {
            if (g_shm->cashier_pids[i] > 0)
                kill(g_shm->cashier_pids[i], SIGUSR2);
        }
        for (int i = 0; i < g_num_customers; i++) {
            if (g_customer_pids[i] > 0)
                kill(g_customer_pids[i], SIGUSR2);
        }
    }
    else {
        log_msg("Nieznane polecenie FIFO: '%s'", buf);
    }
}

/* ================================================================
 *  ODCZYT Z PIPE PIEKARZA
 * ================================================================ */

/**
 * Czyta komunikaty produkcji z pipe piekarza (nieblokujaco).
 * Piekarz wysyla przez pipe informacje o kazdej partii.
 */
static void read_baker_pipe(void)
{
    if (g_baker_pipe[0] < 0) return;

    char buf[256];
    /* Ustaw pipe jako nieblokujacy */
    int flags = fcntl(g_baker_pipe[0], F_GETFL, 0);
    fcntl(g_baker_pipe[0], F_SETFL, flags | O_NONBLOCK);

    ssize_t n;
    while ((n = read(g_baker_pipe[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        /* Komunikaty piekarza o produkcji - logujemy */
    }
}

/* ================================================================
 *  GENEROWANIE RAPORTU KONCOWEGO
 * ================================================================ */

/**
 * Generuje raport z symulacji ciastkarni.
 * Uzywa popen() do pobrania aktualnej daty (demonstracja popen).
 * Zapisuje raport do pliku tekstowego (demonstracja file I/O).
 */
static void generate_report(void)
{
    log_msg_color(C_BOLD, "=== GENEROWANIE RAPORTU KONCOWEGO ===");

    /* Pobierz aktualna date za pomoca popen() */
    char timestamp[64] = "brak daty";
    FILE *date_fp = popen("date '+%Y-%m-%d %H:%M:%S'", "r");
    if (date_fp != NULL) {
        if (fgets(timestamp, sizeof(timestamp), date_fp) != NULL) {
            /* Usun newline */
            timestamp[strcspn(timestamp, "\n")] = '\0';
        }
        pclose(date_fp);
    }

    /* Otworz plik raportu (demonstracja open/write/close) */
    int report_fd = open(REPORT_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (report_fd == -1) {
        handle_warning("open (report file)");
        return;
    }

    /* Buduj raport w buforze */
    char buf[4096];
    int offset = 0;

    offset += snprintf(buf + offset, sizeof(buf) - offset,
        "============================================\n"
        "  RAPORT CIASTKARNI - SYMULACJA\n"
        "  Data: %s\n"
        "============================================\n\n", timestamp);

    offset += snprintf(buf + offset, sizeof(buf) - offset,
        "--- KONFIGURACJA ---\n"
        "Produktow: %d\n"
        "Maks. klientow w sklepie: %d\n"
        "Godziny: %02d:%02d - %02d:%02d\n"
        "Skala czasu: %d ms/min\n\n",
        g_shm->num_products, g_shm->max_customers,
        g_shm->open_hour, g_shm->open_min,
        g_shm->close_hour, g_shm->close_min,
        g_shm->time_scale_ms);

    offset += snprintf(buf + offset, sizeof(buf) - offset,
        "--- STATYSTYKI OGOLNE ---\n"
        "Laczna liczba klientow: %d\n"
        "Obsluzonych (paragon):  %d\n"
        "Nieobsluzonych:         %d\n"
        "Tryb inwentaryzacji: %s\n"
        "Ewakuacja: %s\n\n",
        g_shm->total_customers_entered,
        g_shm->customers_served,
        g_shm->customers_not_served,
        g_shm->inventory_mode ? "TAK" : "NIE",
        g_shm->evacuation_mode ? "TAK" : "NIE");

    /* Produkcja piekarza */
    offset += snprintf(buf + offset, sizeof(buf) - offset,
        "--- PRODUKCJA PIEKARZA ---\n");
    int total_produced = 0;
    for (int i = 0; i < g_shm->num_products; i++) {
        offset += snprintf(buf + offset, sizeof(buf) - offset,
            "  %-20s: %d szt.\n",
            g_shm->products[i].name, g_shm->baker_produced[i]);
        total_produced += g_shm->baker_produced[i];
    }
    offset += snprintf(buf + offset, sizeof(buf) - offset,
        "  RAZEM: %d szt.\n\n", total_produced);

    /* Sprzedaz na kasach */
    for (int r = 0; r < 2; r++) {
        offset += snprintf(buf + offset, sizeof(buf) - offset,
            "--- KASA NR %d - PODSUMOWANIE ---\n", r + 1);
        int total_sold = 0;
        for (int i = 0; i < g_shm->num_products; i++) {
            if (g_shm->register_sales[r][i] > 0) {
                offset += snprintf(buf + offset, sizeof(buf) - offset,
                    "  %-20s: %d szt. (%.2f PLN)\n",
                    g_shm->products[i].name,
                    g_shm->register_sales[r][i],
                    g_shm->register_sales[r][i] * g_shm->products[i].price);
                total_sold += g_shm->register_sales[r][i];
            }
        }
        offset += snprintf(buf + offset, sizeof(buf) - offset,
            "  RAZEM: %d szt., PRZYCHOD: %.2f PLN\n\n",
            total_sold, g_shm->register_revenue[r]);
    }

    /* Stan podajnikow (ile zostalo na podajnikach) */
    offset += snprintf(buf + offset, sizeof(buf) - offset,
        "--- STAN PODAJNIKOW (KIEROWNIK) ---\n");
    int total_remaining = 0;
    for (int i = 0; i < g_shm->num_products; i++) {
        int capacity = g_shm->products[i].conveyor_capacity;
        int free_space = sem_getval(g_sem_id, SEM_CONVEYOR_BASE + i);
        int on_conveyor = capacity - free_space;
        if (on_conveyor < 0) on_conveyor = 0;
        offset += snprintf(buf + offset, sizeof(buf) - offset,
            "  %-20s: %d szt. (pojemnosc: %d)\n",
            g_shm->products[i].name, on_conveyor, capacity);
        total_remaining += on_conveyor;
    }
    offset += snprintf(buf + offset, sizeof(buf) - offset,
        "  RAZEM na podajnikach: %d szt.\n\n", total_remaining);

    /* Kosz ewakuacyjny */
    if (g_shm->evacuation_mode) {
        offset += snprintf(buf + offset, sizeof(buf) - offset,
            "--- KOSZ EWAKUACYJNY ---\n");
        int total_basket = 0;
        for (int i = 0; i < g_shm->num_products; i++) {
            if (g_shm->basket_items[i] > 0) {
                offset += snprintf(buf + offset, sizeof(buf) - offset,
                    "  %-20s: %d szt.\n",
                    g_shm->products[i].name, g_shm->basket_items[i]);
                total_basket += g_shm->basket_items[i];
            }
        }
        offset += snprintf(buf + offset, sizeof(buf) - offset,
            "  RAZEM w koszu: %d szt.\n\n", total_basket);
    }

    offset += snprintf(buf + offset, sizeof(buf) - offset,
        "============================================\n"
        "  KONIEC RAPORTU\n"
        "============================================\n");

    /* Zapisz raport do pliku */
    write(report_fd, buf, offset);
    close(report_fd);

    /* Wyswietl raport takze na konsoli */
    printf("\n%s%s%s\n", C_BOLD, buf, C_RESET);

    log_msg("Raport zapisany do: %s", REPORT_FILE);
}

/* ================================================================
 *  WYSWIETLANIE BANERA STARTOWEGO
 * ================================================================ */

static void print_banner(SharedData *shm)
{
    printf("\n%s", C_BOLD);
    printf("  ╔══════════════════════════════════════════════╗\n");
    printf("  ║        CIASTKARNIA - SYMULACJA               ║\n");
    printf("  ║        Systemy Operacyjne - Projekt           ║\n");
    printf("  ╚══════════════════════════════════════════════╝\n");
    printf("%s\n", C_RESET);
    printf("  Produktow:   %d\n", shm->num_products);
    printf("  Maks. klientow:  %d\n", shm->max_customers);
    printf("  Godziny:     %02d:%02d - %02d:%02d\n",
           shm->open_hour, shm->open_min,
           shm->close_hour, shm->close_min);
    printf("  Skala czasu: %d ms/min symulacji\n", shm->time_scale_ms);
    if (g_max_time > 0)
        printf("  Limit czasu: %d sekund\n", g_max_time);
    printf("  FIFO polecen: %s\n", FIFO_CMD_PATH);
    printf("    Wyslij: echo 'inwentaryzacja' > %s\n", FIFO_CMD_PATH);
    printf("            echo 'ewakuacja' > %s\n\n", FIFO_CMD_PATH);
}

/* ================================================================
 *  ZAMYKANIE SYMULACJI
 * ================================================================ */

/**
 * Procedura zamkniecia - wysyla sygnaly do procesow, czeka na zakonczenie.
 */
static void shutdown_simulation(void)
{
    log_msg_color(C_RED, "=== ZAMYKANIE SYMULACJI ===");

    g_shm->simulation_running = 0;
    g_shm->shop_open          = 0;
    g_shm->bakery_open        = 0;

    /* Czekaj az klienci opuszcza sklep (z limitem czasu) */
    int wait_cycles = 0;
    while (g_shm->customers_in_shop > 0 && wait_cycles < 100) {
        msleep_safe(g_shm->time_scale_ms);
        wait_cycles++;
        reap_children();
    }

    if (g_shm->customers_in_shop > 0) {
        log_msg("Wymuszam opuszczenie sklepu przez %d klientow",
                g_shm->customers_in_shop);
    }

    /* Wyslij SIGTERM do piekarza i kasjerow */
    if (g_shm->baker_pid > 0) {
        kill(g_shm->baker_pid, SIGTERM);
    }
    for (int i = 0; i < 2; i++) {
        if (g_shm->cashier_pids[i] > 0) {
            kill(g_shm->cashier_pids[i], SIGTERM);
        }
    }

    /* Wyslij SIGTERM do pozostalych klientow */
    for (int i = 0; i < g_num_customers; i++) {
        if (g_customer_pids[i] > 0) {
            kill(g_customer_pids[i], SIGTERM);
        }
    }

    /* Czekaj na zakonczenie procesow potomnych z limitem czasu */
    int timeout = 50;
    while (timeout > 0) {
        reap_children();

        /* Sprawdz czy ktokolwiek jeszcze zyje */
        int any_alive = 0;
        if (g_shm->baker_pid > 0) any_alive = 1;
        for (int i = 0; i < 2 && !any_alive; i++)
            if (g_shm->cashier_pids[i] > 0) any_alive = 1;
        for (int i = 0; i < g_num_customers && !any_alive; i++)
            if (g_customer_pids[i] > 0) any_alive = 1;

        if (!any_alive) break;

        msleep_safe(100);
        timeout--;
    }

    /* Jesli procesy wciaz zyja - SIGKILL jako ostatecznosc */
    if (timeout <= 0) {
        log_msg("Wymuszam zakonczenie procesow (SIGKILL)...");
        if (g_shm->baker_pid > 0)
            kill(g_shm->baker_pid, SIGKILL);
        for (int i = 0; i < 2; i++) {
            if (g_shm->cashier_pids[i] > 0)
                kill(g_shm->cashier_pids[i], SIGKILL);
        }
        for (int i = 0; i < g_num_customers; i++) {
            if (g_customer_pids[i] > 0)
                kill(g_customer_pids[i], SIGKILL);
        }
        /* Ostatnie czyszczenie po SIGKILL */
        msleep_safe(200);
        reap_children();
    }

    /* Finalne zbieranie zombie */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;

    log_msg("Wszystkie procesy zakonczane.");
}

/* ================================================================
 *  GLOWNA PETLA SYMULACJI
 * ================================================================ */

int main(int argc, char *argv[])
{
    srand(time(NULL) ^ getpid());

    /* --- 1. Usun zastarale zasoby IPC z poprzednich uruchomien --- */
    if (access(KEY_FILE, F_OK) == 0) {
        cleanup_all_ipc(KEY_FILE, MAX_PRODUCTS);
    }

    /* --- 2. Tworzenie pliku klucza i katalogu logow --- */
    create_key_file();
    create_log_directory();

    /* --- 2. Tworzenie pamieci dzielonej --- */
    create_shared_memory(KEY_FILE);
    g_shm = attach_shared_memory(KEY_FILE);

    /* --- 3. Parsowanie argumentow (zapisuje do shm) --- */
    if (parse_args(argc, argv, g_shm) != 0) {
        detach_shared_memory(g_shm);
        cleanup_all_ipc(KEY_FILE, DEFAULT_NUM_PRODUCTS);
        return EXIT_FAILURE;
    }

    /* --- 4. Inicjalizacja danych w pamieci dzielonej --- */
    init_shared_data(g_shm);

    /* --- 5. Tworzenie semaforow --- */
    int P = g_shm->num_products;
    int num_sems = TOTAL_SEMS(P);
    g_sem_id = create_semaphores(KEY_FILE, num_sems);
    init_semaphore_values(g_sem_id, g_shm);

    /* --- 6. Tworzenie kolejek komunikatow --- */
    int mq_conv   = create_message_queue(KEY_FILE, PROJ_MQ_CONV);
    int mq_chkout = create_message_queue(KEY_FILE, PROJ_MQ_CHKOUT);
    int mq_rcpt   = create_message_queue(KEY_FILE, PROJ_MQ_RCPT);

    /* --- 6a. Inicjalizacja semaforow-straznikow kolejek --- */
    init_semaphore(g_sem_id, SEM_GUARD_CONV(P),
                   calc_queue_guard_init(mq_conv,   sizeof(struct conveyor_msg)));
    init_semaphore(g_sem_id, SEM_GUARD_CHKOUT(P),
                   calc_queue_guard_init(mq_chkout, sizeof(struct checkout_msg)));
    init_semaphore(g_sem_id, SEM_GUARD_RCPT(P),
                   calc_queue_guard_init(mq_rcpt,   sizeof(struct receipt_msg)));

    /* --- 7. Tworzenie FIFO polecen (lacze nazwane) --- */
    create_fifo(FIFO_CMD_PATH);

    /* --- 8. Wyczysc plik logow z poprzedniego uruchomienia --- */
    {
        FILE *f = fopen(FULL_LOG_FILE, "w");
        if (f) fclose(f);
    }

    /* --- 9. Logger --- */
    logger_init(g_shm, PROC_MANAGER, 0);

    /* --- 9. Baner startowy --- */
    print_banner(g_shm);

    /* --- 10. Signal handlers + atexit safety net --- */
    setup_signal_handlers();
    atexit(atexit_cleanup);

    /* --- 11. Uruchomienie procesow --- */
    /* WAZNE: bakery_open MUSI byc ustawione PRZED start_baker(),
     * inaczej watki produkcyjne piekarza widza bakery_open==0
     * i natychmiast koncza prace (race condition). */
    g_shm->bakery_open = 1;

    log_msg("Uruchamiam piekarza...");
    g_shm->baker_pid = start_baker();

    log_msg("Uruchamiam kasjerow...");
    g_shm->cashier_pids[0] = start_cashier(0);
    g_shm->cashier_pids[1] = start_cashier(1);
    log_msg("Ciastkarnia otwarta! Godzina: %02d:%02d",
            g_shm->sim_hour, g_shm->sim_min);

    /* --- 13. Otworz FIFO do czytania (nieblokujaco) --- */
    int fifo_fd = open(FIFO_CMD_PATH, O_RDONLY | O_NONBLOCK);
    if (fifo_fd == -1)
        handle_warning("open (FIFO)");

    /* Ustaw pipe piekarza jako nieblokujacy */
    if (g_baker_pipe[0] >= 0) {
        int flags = fcntl(g_baker_pipe[0], F_GETFL, 0);
        fcntl(g_baker_pipe[0], F_SETFL, flags | O_NONBLOCK);
    }

    /* ============================================================
     * GLOWNA PETLA SYMULACJI
     * Kazda iteracja = 1 minuta czasu symulacji
     * ============================================================ */
    int customer_spawn_timer = 0;   /* Odliczanie do nastepnego klienta */
    int next_spawn_interval  = 3 + rand() % 8;  /* 3-10 min symulacji */

    /* Zegar scienny (wall-clock) do obslugi -t timeout */
    struct timespec wall_start;
    clock_gettime(CLOCK_MONOTONIC, &wall_start);

    while (g_shm->simulation_running && !g_sigint_received) {

        /* --- Obsluga SIGCONT (wznowienie po Ctrl+Z) --- */
        if (g_sigcont_received) {
            g_sigcont_received = 0;
            log_msg("Wznowiono po zatrzymaniu (SIGCONT) - czyszczenie zombie...");
            reap_children();
        }

        /* --- Obsluga SIGCHLD --- */
        if (g_sigchld_received) {
            g_sigchld_received = 0;
            reap_children();
        }

        /* --- Obsluga ewakuacji --- */
        if (g_shm->evacuation_mode) {
            log_msg_color(C_RED, "EWAKUACJA W TOKU - zamykanie...");
            shutdown_simulation();
            generate_report();
            break;
        }

        /* --- Postep zegara symulacji --- */
        g_shm->sim_min++;
        if (g_shm->sim_min >= 60) {
            g_shm->sim_min = 0;
            g_shm->sim_hour++;
        }

        /* --- Otwarcie sklepu (Tp + 30 min) --- */
        int shop_open_hour = g_shm->open_hour;
        int shop_open_min  = g_shm->open_min + 30;
        if (shop_open_min >= 60) {
            shop_open_hour++;
            shop_open_min -= 60;
        }
        if (!g_shm->shop_open && !g_shm->evacuation_mode &&
            (g_shm->sim_hour > shop_open_hour ||
             (g_shm->sim_hour == shop_open_hour && g_shm->sim_min >= shop_open_min))) {
            g_shm->shop_open = 1;
            log_msg_color(C_GREEN, "Sklep otwarty! Godzina: %02d:%02d",
                          g_shm->sim_hour, g_shm->sim_min);
        }

        /* --- Zamkniecie o godzinie Tk --- */
        if (g_shm->sim_hour >= g_shm->close_hour &&
            g_shm->sim_min >= g_shm->close_min) {
            /* Jesli klienci wciaz aktywni - czekaj na nich */
            if (g_shm->total_customers_entered > 0
                && g_shm->active_customers > 0) {
                static int close_delay_logged = 0;
                if (!close_delay_logged) {
                    close_delay_logged = 1;
                    log_msg_color(C_YELLOW,
                        "Godzina zamkniecia %02d:%02d - czekam na %d aktywnych klientow.",
                        g_shm->close_hour, g_shm->close_min,
                        g_shm->active_customers);
                }
            } else {
                log_msg_color(C_RED, "Godzina zamkniecia: %02d:%02d",
                              g_shm->sim_hour, g_shm->sim_min);
                shutdown_simulation();
                generate_report();
                break;
            }
        }

        /* --- Wall-clock timeout (-t) --- */
        if (g_max_time > 0) {
            struct timespec wall_now;
            clock_gettime(CLOCK_MONOTONIC, &wall_now);
            int elapsed = (int)(wall_now.tv_sec - wall_start.tv_sec);
            if (elapsed >= g_max_time) {
                /* Jesli klienci wciaz aktywni - nie zamykaj */
                if (g_shm->total_customers_entered > 0
                    && g_shm->active_customers > 0) {
                    static int timeout_delay_logged = 0;
                    if (!timeout_delay_logged) {
                        timeout_delay_logged = 1;
                        log_msg_color(C_YELLOW,
                            "Timeout %d s - czekam na %d aktywnych klientow.",
                            g_max_time, g_shm->active_customers);
                    }
                } else {
                    log_msg_color(C_RED, "Timeout %d s - zamykanie symulacji.",
                                  g_max_time);
                    shutdown_simulation();
                    generate_report();
                    break;
                }
            }
        }

        /* --- Generowanie klientow (wszystkich naraz przy otwarciu) --- */
        if (g_shm->shop_open && !g_shm->evacuation_mode
            && g_shm->total_customers_entered < MAX_CUSTOMERS_TOTAL) {
            int to_spawn = MAX_CUSTOMERS_TOTAL - g_shm->total_customers_entered;
            log_msg("Spawnowanie %d klientow do kolejki...", to_spawn);
            int spawned = 0;
            for (int b = 0; b < to_spawn; b++) {
                if (g_shm->active_customers >= MAX_ACTIVE_CUST)
                    break;
                start_customer();
                spawned++;
            }
            log_msg("Utworzono %d procesow klientow (lacznie: %d). "
                    "Czekaja w kolejce na wejscie do sklepu.",
                    spawned, g_shm->total_customers_entered);
        }

        /* --- Auto-zamkniecie po 5000 klientow --- */
        if (g_shm->total_customers_entered >= MAX_CUSTOMERS_TOTAL) {
            if (g_shm->active_customers == 0) {
                log_msg_color(C_GREEN, "Obsluzono %d klientow - zamykanie symulacji.",
                              g_shm->total_customers_entered);
                shutdown_simulation();
                generate_report();
                break;
            }
            /* Jesli zostalo kilku klientow i czekamy zbyt dlugo - wymus zamkniecie */
            static int idle_ticks = 0;
            static int last_active = -1;
            if (last_active < 0) last_active = g_shm->active_customers;
            if (g_shm->active_customers < last_active) {
                /* Jest postep - klienci wchodza/wychodza, resetuj timer */
                idle_ticks = 0;
                last_active = g_shm->active_customers;
            }
            idle_ticks++;
            if (idle_ticks > 600) { /* 600 min symulacji BEZ postepow */
                log_msg_color(C_YELLOW,
                    "Timeout: %d aktywnych klientow nie konczy zakupow - wymuszam zamkniecie.",
                    g_shm->active_customers);
                shutdown_simulation();
                generate_report();
                break;
            }
        }

        /* --- Zarzadzanie kasami --- */
        update_register_state();

        /* --- Odczyt polecen z FIFO --- */
        check_fifo_commands(fifo_fd);

        /* --- Odczyt z pipe piekarza --- */
        read_baker_pipe();

        /* --- Czekaj 1 minute symulacji --- */
        msleep_safe(g_shm->time_scale_ms);
    }

    /* --- Obsluga SIGINT --- */
    if (g_sigint_received) {
        log_msg("Otrzymano SIGINT - zamykanie...");
        shutdown_simulation();
        generate_report();
    }

    /* --- Czyszczenie zasobow --- */
    log_msg("Czyszczenie zasobow IPC...");
    log_msg_color(C_GREEN, "Symulacja zakonczona pomyslnie.");

    if (fifo_fd >= 0) close(fifo_fd);
    if (g_baker_pipe[0] >= 0) close(g_baker_pipe[0]);
    detach_shared_memory(g_shm);
    g_shm = NULL;
    logger_init(NULL, PROC_MANAGER, 0);
    cleanup_all_ipc(KEY_FILE, DEFAULT_NUM_PRODUCTS);
    g_cleanup_done = 1;  /* Zapobiegaj podwojnemu czyszczeniu przez atexit */

    return EXIT_SUCCESS;
}
