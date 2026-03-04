#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include "config.h"
#include "types.h"
#include "ipc.h"
#include "utils.h"

/*
 * Guard na "minę" konfiguracyjną:
 * generator losuje wiek dziecka z zakresu [WIEK_MIN..WIEK_WYMAGA_OPIEKI-1].
 * Jeśli ktoś włączy dzieci, a zostawi WIEK_WYMAGA_OPIEKI=0, to zakres robi się pusty.
 */
#if PROC_DZIECKO > 0
_Static_assert(WIEK_WYMAGA_OPIEKI > WIEK_MIN,
               "WIEK_WYMAGA_OPIEKI musi byc > WIEK_MIN gdy PROC_DZIECKO>0");
#endif

/*
 * KOLEJ KRZESEŁKOWA - GENERATOR KLIENTÓW
 * 
 * Odpowiedzialności:
 * 1. Generowanie losowych klientów
 * 2. Tworzenie procesów klientów (fork + exec)
 * 3. Kontrola tempa generowania
 */

static volatile sig_atomic_t g_koniec = 0;
static volatile sig_atomic_t g_child_event = 0;

static void handler_sigterm(int sig) {
    (void)sig;
    g_koniec = 1;
}

/* SIGCHLD - ustawiamy flagę, reaping w pętli */
static void handler_sigchld(int sig) {
    (void)sig;
    g_child_event = 1;
}

static void reap_children_and_maybe_panic(void) {
    if (!g_child_event) return;
    g_child_event = 0;

    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        /* jeśli klient zginął sygnałem w OPEN → PANIC */
        if (WIFSIGNALED(status) && g_shm && g_shm->faza_dnia == FAZA_OPEN) {
            g_shm->panic = 1;
            g_shm->panic_pid = pid;
            g_shm->panic_sig = WTERMSIG(status);
            if (g_shm->pid_main > 0) kill(g_shm->pid_main, SIGTERM);
            g_koniec = 1;
        }
    }
}


static void przekieruj_stdio_do_pliku(const char *sciezka) {
    if (sciezka == NULL || sciezka[0] == '\0') return;

    /* stdout wyciszamy, żeby przypadkowe printf nie mieszały logów */
    int fd_null = open("/dev/null", O_WRONLY);
    if (fd_null >= 0) {
        (void)dup2(fd_null, STDOUT_FILENO);
        if (fd_null > STDOUT_FILENO) close(fd_null);
    }

    /* stderr kierujemy do pliku logu (loguj() pisze na stderr) */
    int fd = open(sciezka, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd < 0) return;
    (void)dup2(fd, STDERR_FILENO);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (fd > STDERR_FILENO) close(fd);
}

static const char* nazwa_typu_klienta(int typ) {
    return (typ == TYP_ROWERZYSTA) ? "ROWER" : "PIESZY";
}

int main(int argc, char *argv[]) {
    int czas_symulacji = CZAS_SYMULACJI;
    int limit_utworzonych = MAX_WYG_KLIENTOW; /* 0 = bez limitu */
    int limit_aktywnych = MAX_KLIENTOW;       /* 0 = bez limitu */
    
    if (argc >= 2) {
        czas_symulacji = waliduj_liczbe(argv[1], 1, 3600);
        if (czas_symulacji < 0) czas_symulacji = CZAS_SYMULACJI;
    }

    /* Opcjonalne limity: [2]=łączna liczba utworzonych klientów, [3]=limit aktywnych jednocześnie */
    if (argc >= 3) {
        int v = waliduj_liczbe(argv[2], 0, 10000000);
        if (v >= 0) limit_utworzonych = v;
    }
    if (argc >= 4) {
        int v = waliduj_liczbe(argv[3], 0, 10000000);
        if (v >= 0) limit_aktywnych = v;
    }
    
    /* Ustaw aby zginąć gdy rodzic (main) umrze */
    ustaw_smierc_z_rodzicem();
    
    /* Inicjalizacja */
    inicjalizuj_losowanie();
    
    /* Obsługa sygnałów - sigaction BEZ SA_RESTART */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_sigterm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    
    /* SIGCHLD - ignorujemy (zbierzemy na końcu) */
    sa.sa_handler = handler_sigchld;
    sigaction(SIGCHLD, &sa, NULL);
    
    /* Dołącz do IPC */
    if (attach_ipc() != 0) {
        loguj("GENERATOR: Błąd dołączania do IPC");
        return EXIT_FAILURE;
    }
    
    loguj("GENERATOR: Start (czas=%d sek, limit_utworzonych=%d, limit_aktywnych=%d)",
          czas_symulacji, limit_utworzonych, limit_aktywnych);
    
    time_t czas_startu = g_shm->czas_startu;
    int id_klienta = 0;
    int wygenerowano = 0;
    int limit_zalogowany = 0;
    
    /* Główna pętla generowania - TYLKO gdy FAZA_OPEN */
    while (!g_koniec && g_shm->faza_dnia == FAZA_OPEN) {
        reap_children_and_maybe_panic();
        /* Sprawdź czas */
        if (czy_koniec_symulacji(czas_startu, czas_symulacji)) {
            break;
        }
        
        /* Sprawdź czy kolej aktywna */
        if (g_shm->awaria) {
            poll(NULL, 0, 100);
            continue;
        }
        
        /* LIMIT AKTYWNYCH KLIENTÓW - nie forkuj gdy za dużo (0 = brak limitu) */
        if (limit_aktywnych > 0 && g_shm->aktywni_klienci >= limit_aktywnych) {
            poll(NULL, 0, 100);  /* Czekaj 100ms */
            continue;
        }

        /* LIMIT ŁĄCZNY UTWORZONYCH - po osiągnięciu nie twórz nowych (0 = brak limitu) */
        if (limit_utworzonych > 0 && wygenerowano >= limit_utworzonych) {
            if (!limit_zalogowany) {
                loguj("GENERATOR: Osiągnięto limit_utworzonych=%d – wstrzymuję generowanie", limit_utworzonych);
                limit_zalogowany = 1;
            }
            reap_children_and_maybe_panic();
            poll(NULL, 0, 200);
            continue;
        }
        
        /* Losuj opóźnienie między klientami (0.5-2 sekundy) */
        int opoznienie = 0;
        poll(NULL, 0, opoznienie);
        
        if (g_koniec || g_shm->faza_dnia != FAZA_OPEN) break;
        
        /* Generuj parametry klienta */
        int next_id = id_klienta + 1;
        int wiek = losuj_zakres(WIEK_MIN, WIEK_MAX);
        int typ = losuj_procent(PROC_ROWERZYSTA) ? TYP_ROWERZYSTA : TYP_PIESZY;
        int vip = losuj_procent(PROC_VIP);
        int liczba_dzieci = 0;
        int wiek_dzieci[2] = {0, 0};
        
        /* Tylko dorośli mogą mieć dzieci */
        if (wiek >= WIEK_DOROSLY_MIN) {
            if (losuj_procent(PROC_DZIECKO)) {
                liczba_dzieci = 1;
                wiek_dzieci[0] = losuj_zakres(WIEK_MIN, WIEK_WYMAGA_OPIEKI - 1);
                
                if (losuj_procent(PROC_DRUGIE_DZIECKO)) {
                    liczba_dzieci = 2;
                    wiek_dzieci[1] = losuj_zakres(WIEK_MIN, WIEK_WYMAGA_OPIEKI - 1);
                }
            }
        }
        
        /* Fork procesu klienta */
        pid_t pid = fork();
        
        if (pid == -1) {
            /* BACKOFF przy błędzie fork - nie spamuj CPU */
            poll(NULL, 0, 1000);  /* Czekaj 1 sekundę */
            continue;
        }
        
        if (pid == 0) {
            /* Proces potomny - exec klienta */
            przekieruj_stdio_do_pliku("output/klienci.log");

            char arg_id[16], arg_wiek[8], arg_typ[4], arg_vip[4];
            char arg_dzieci[4], arg_wd1[8], arg_wd2[8];
            
            snprintf(arg_id, sizeof(arg_id), "%d", next_id);
            snprintf(arg_wiek, sizeof(arg_wiek), "%d", wiek);
            snprintf(arg_typ, sizeof(arg_typ), "%d", typ);
            snprintf(arg_vip, sizeof(arg_vip), "%d", vip);
            snprintf(arg_dzieci, sizeof(arg_dzieci), "%d", liczba_dzieci);
            snprintf(arg_wd1, sizeof(arg_wd1), "%d", wiek_dzieci[0]);
            snprintf(arg_wd2, sizeof(arg_wd2), "%d", wiek_dzieci[1]);
            
            char *argv_klient[] = {
                PATH_KLIENT,
                arg_id, arg_wiek, arg_typ, arg_vip,
                arg_dzieci, arg_wd1, arg_wd2,
                NULL
            };
            
            execv(PATH_KLIENT, argv_klient);
            dprintf(STDERR_FILENO, "execv klient: %s\n", strerror(errno));
            _exit(EXIT_FAILURE);
        }

        /* Proces rodzica: loguj parametry nowego klienta */
        if (pid > 0) {
            id_klienta = next_id;
            wygenerowano++;
            if (liczba_dzieci == 0) {
                loguj("GENERATOR: utworzono klienta id=%d pid=%d wiek=%d typ=%s vip=%d dzieci=0",
                      id_klienta, (int)pid, wiek, nazwa_typu_klienta(typ), vip);
            } else {
                loguj("GENERATOR: utworzono klienta id=%d pid=%d wiek=%d typ=%s vip=%d dzieci=%d (wiek:%d,%d)",
                      id_klienta, (int)pid, wiek, nazwa_typu_klienta(typ), vip,
                      liczba_dzieci, wiek_dzieci[0], wiek_dzieci[1]);
            }
        }
        
        /* Proces rodzica kontynuuje */
    }
    
    loguj("GENERATOR: Kończę generowanie (utworzono=%d, ostatnie_id=%d)", wygenerowano, id_klienta);
    
    /* WNOHANG: nie blokuj - klienci dostaną PDEATHSIG (SIGTERM) gdy generator wyjdzie */
    int status;
    pid_t child_pid;
    while ((child_pid = waitpid(-1, &status, WNOHANG)) > 0) {
        (void)child_pid;
    }
    loguj("GENERATOR: Kończę");
    detach_ipc();
    
    return EXIT_SUCCESS;
}
