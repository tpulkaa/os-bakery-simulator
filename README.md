# OS Bakery Simulator

**Temat 15** -- Systemy Operacyjne

Symulacja ciastkarni z samoobslugowym sklepem firmowym.
Wieloprocesowy system oparty na IPC System V i laczach (pipe, FIFO).

## 1. Srodowisko

| Wymaganie | Wersja |
|-----------|--------|
| System    | Linux / macOS |
| Kompilator| GCC z `-std=c11 -Wall -Wextra -pedantic` |
| Biblioteki| pthread, System V IPC (domyslne) |

## 2. Budowanie i uruchamianie

```bash
make                # kompilacja
./kierownik         # domyslne parametry
./kierownik -n 10 -p 12 -s 100 -o 8 -c 16  # przyklad
make test           # testy integracyjne
make clean          # czyszczenie
```

### Parametry

| Flaga | Opis | Zakres | Domyslnie |
|-------|------|--------|-----------|
| `-n`  | Max klientow w sklepie | 3-300 | 10 |
| `-p`  | Liczba produktow | 11-20 | 12 |
| `-s`  | Skala czasu (ms/min) | 10-2000 | 100 |
| `-o`  | Godzina otwarcia | 6-12 | 8 |
| `-c`  | Godzina zamkniecia | 12-22 | 16 |
| `-t`  | Timeout rzeczywisty (s) | 0=brak | 0 |

### Sterowanie (FIFO)

```bash
echo 'inwentaryzacja' > /tmp/ciastkarnia_cmd.fifo
echo 'ewakuacja' > /tmp/ciastkarnia_cmd.fifo
```

## 3. Pokrycie wymagan

- **7 mechanizmow IPC**: pamiec dzielona, semafory (SEM_UNDO), kolejki komunikatow (3 szt. z guard semaphores), pipe, FIFO
- **Semafory z SEM_UNDO** -- kernel zwalnia zasoby po `kill -9`
- **Uprawnienia 0660** -- nie-world-readable
- **Wielowatkowosc**: piekarz (2 watki produkcyjne), kasjer (watek monitora)
- **Sygnaly**: SIGCHLD, SIGINT, SIGTERM, SIGUSR1, SIGUSR2

## 4. Struktura kodu

```
src/
  common.h           Stale, struktury, definicje IPC
  error_handler.h/c  Obsluga bledow (perror, walidacja)
  ipc_utils.h/c      Narzedzia IPC (shm, sem, msg, pipe, fifo)
  logger.h/c         Kolorowe logowanie z zegarem
  kierownik.c        Glowny proces (manager)
  piekarz.c          Piekarz (2 watki produkcyjne)
  kasjer.c           Kasjer (2 instancje, watek monitora)
  klient.c           Klient (zakupy, kasa, wyjscie)
  check_shm.c        Narzedzie diagnostyczne SHM
tests/
  run_tests.sh       Runner testow
  test_01-05_*.sh    Testy integracyjne
  test_kill.sh       Test odpornosci na kill
docs/
  opis_projektu.md   Pelny opis techniczny
```

## 5. Mechaniki

1. Kierownik tworzy IPC, forkuje piekarza + 2 kasjerow, prowadzi zegar
2. Piekarz produkuje ciastka (2 watki), uklada na podajnikach (semafory zliczajace)
3. Klienci wchodza (SEM_SHOP_ENTRY z SEM_UNDO), zbieraja ciastka (msgrcv), placa (msgsnd)
4. Kasjer skanuje produkty, wysyla paragon (msgrcv/msgsnd)
5. FIFO umozliwia inwentaryzacje i ewakuacje

## 6. Testy

### Automatyczne (`make test`)

| # | Opis |
|---|------|
| 01 | Start/cleanup IPC |
| 02 | Zero zombie |
| 03 | Stress: N nigdy przekroczony |
| 04 | Ewakuacja FIFO |
| 05 | SIGINT cleanup |

### Dodatkowy: `test_kill.sh`

Testuje `kill` i `kill -9` na procesach -- symulacja kontynuuje dzieki SEM_UNDO.

## 7. Funkcje wymagane przez projekt (gdzie szukac)

- **Pliki**: `creat()`, `open()`, `close()`, `read()`, `write()`, `unlink()` -- kierownik.c, ipc_utils.c
- **Procesy**: `fork()`, `execl()`, `exit()`, `waitpid()` -- kierownik.c
- **Watki**: `pthread_create()`, `pthread_join()`, `pthread_detach()`, `pthread_mutex_*`, `pthread_cond_*` -- piekarz.c, kasjer.c
- **Sygnaly**: `kill()`, `sigaction()` -- kierownik.c, piekarz.c, kasjer.c, klient.c
- **Semafory**: `ftok()`, `semget()`, `semctl()`, `semop()` -- ipc_utils.c
- **Lacza**: `mkfifo()`, `pipe()`, `dup2()`, `popen()` -- ipc_utils.c, kierownik.c
- **Pamiec dzielona**: `ftok()`, `shmget()`, `shmat()`, `shmdt()`, `shmctl()` -- ipc_utils.c
- **Kolejki komunikatow**: `ftok()`, `msgget()`, `msgsnd()`, `msgrcv()`, `msgctl()` -- ipc_utils.c
