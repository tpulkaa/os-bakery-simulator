
#ifndef IPC_UTILS_H
#define IPC_UTILS_H

#include "common.h"

/* ===== Pamiec dzielona (Shared Memory) ===== */

/**
 * Tworzy segment pamieci dzielonej.
 * Uzywa ftok() z keyfile i PROJ_SHM.
 * @param keyfile Plik klucza dla ftok()
 * @return ID segmentu pamieci dzielonej
 */
int create_shared_memory(const char *keyfile);

/**
 * Dolacza do istniejacego segmentu pamieci dzielonej.
 * @param keyfile Plik klucza dla ftok()
 * @return Wskaznik do struktury SharedData
 */
SharedData *attach_shared_memory(const char *keyfile);

/**
 * Odlacza pamiec dzielona od procesu.
 */
void detach_shared_memory(SharedData *shm);

/**
 * Usuwa segment pamieci dzielonej.
 */
void remove_shared_memory(const char *keyfile);

/* ===== Semafory ===== */

/**
 * Tworzy zbior semaforow.
 * @param keyfile Plik klucza dla ftok()
 * @param nsems   Liczba semaforow w zbiorze
 * @return ID zbioru semaforow
 */
int create_semaphores(const char *keyfile, int nsems);

/**
 * Pobiera ID istniejacego zbioru semaforow.
 * @param keyfile Plik klucza dla ftok()
 * @param nsems   Oczekiwana liczba semaforow
 * @return ID zbioru semaforow
 */
int get_semaphores(const char *keyfile, int nsems);

/**
 * Inicjalizuje wartosc semafora.
 * @param sem_id  ID zbioru semaforow
 * @param sem_num Numer semafora w zbiorze
 * @param value   Wartosc poczatkowa
 */
void init_semaphore(int sem_id, int sem_num, int value);

/**
 * Operacja P (wait/czekaj) na semaforze - dekrementacja.
 * Blokuje jesli wartosc = 0.
 */
void sem_wait_op(int sem_id, int sem_num);

/**
 * Operacja V (signal/sygnalizuj) na semaforze - inkrementacja.
 */
void sem_signal_op(int sem_id, int sem_num);

/**
 * Nieblokujaca proba operacji P na semaforze.
 * @return 0 jesli sukces, -1 jesli semafor = 0 (nie zablokowano)
 */
int sem_trywait_op(int sem_id, int sem_num);

/**
 * Pobiera aktualna wartosc semafora.
 */
int sem_getval(int sem_id, int sem_num);

/**
 * Operacja P z flaga SEM_UNDO.
 * Kernel automatycznie cofnie operacje jesli proces zginie.
 * Uzywane do mutexu (SEM_SHM_MUTEX) i zasobow per-proces (SEM_SHOP_ENTRY).
 */
void sem_wait_undo(int sem_id, int sem_num);

/**
 * Operacja V z flaga SEM_UNDO.
 */
void sem_signal_undo(int sem_id, int sem_num);

/**
 * Nieblokujaca proba P z flaga SEM_UNDO.
 * @return 0 jesli sukces, -1 jesli semafor = 0
 */
int sem_trywait_undo(int sem_id, int sem_num);

/**
 * Operacja P przerwalna przez sygnaly.
 * Wraca -1 przy EINTR zamiast powtarzac (caller sprawdza g_terminate).
 * @return 0 jesli sukces, -1 przy EINTR lub bledzie
 */
int sem_wait_interruptible(int sem_id, int sem_num);

/**
 * Usuwa zbior semaforow.
 */
void remove_semaphores(const char *keyfile);

/* ===== Kolejki komunikatow (Message Queues) ===== */

/**
 * Tworzy kolejke komunikatow.
 * @param keyfile Plik klucza dla ftok()
 * @param proj_id Identyfikator projektu dla ftok()
 * @return ID kolejki komunikatow
 */
int create_message_queue(const char *keyfile, int proj_id);

/**
 * Pobiera ID istniejącej kolejki komunikatow.
 */
int get_message_queue(const char *keyfile, int proj_id);

/**
 * Usuwa kolejke komunikatow.
 */
void remove_message_queue(const char *keyfile, int proj_id);

/**
 * Oblicza poczatkowa wartosc semafora-straznika kolejki.
 * @param mq_id   ID kolejki komunikatow
 * @param msgsz   Rozmiar jednego komunikatu (payload + sizeof(long))
 * @return Liczba slotow (minimum 1)
 */
int calc_queue_guard_init(int mq_id, size_t msgsz);

/**
 * Wysyla komunikat z guarded backpressure.
 * Dekrementuje semafor-straznika PRZED msgsnd (blokuje jesli kolejka pelna).
 * @param mq_id   ID kolejki
 * @param msg     Wskaznik na komunikat
 * @param msgsz   Rozmiar payloadu (bez mtype)
 * @param sem_id  ID zbioru semaforow
 * @param guard_idx Indeks semafora-straznika w zbiorze
 * @return 0 przy sukcesie, -1 przy bledzie
 */
int msgsnd_guarded(int mq_id, const void *msg, size_t msgsz,
                   int sem_id, int guard_idx);

/**
 * Odbiera komunikat i zwalnia slot w semaforze-strazniku.
 * Inkrementuje semafor-straznika PO msgrcv.
 * @return Liczba bajtow danych lub -1 przy bledzie
 */
ssize_t msgrcv_guarded(int mq_id, void *msg, size_t msgsz, long mtype,
                       int msgflg, int sem_id, int guard_idx);

/* ===== Lacza (Pipes & FIFOs) ===== */

/**
 * Tworzy lacze nienazwane (pipe).
 * @param pipefd Tablica dwoch deskryptorow [read, write]
 */
void create_pipe(int pipefd[2]);

/**
 * Tworzy lacze nazwane (FIFO).
 * @param path Sciezka do FIFO
 */
void create_fifo(const char *path);

/**
 * Usuwa lacze nazwane (FIFO).
 */
void remove_fifo(const char *path);

/* ===== Czyszczenie wszystkich zasobow IPC ===== */

/**
 * Usuwa wszystkie zasoby IPC utworzone przez symulacje.
 * Wywolywane przez kierownika przy zamykaniu.
 */
void cleanup_all_ipc(const char *keyfile, int num_products);

#endif /* IPC_UTILS_H */
