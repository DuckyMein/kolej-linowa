#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <poll.h>
#include "config.h"
#include "types.h"
#include "ipc.h"
#include "utils.h"

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

int main(int argc, char *argv[]) {
    int czas_symulacji = CZAS_SYMULACJI;
    
    if (argc >= 2) {
        czas_symulacji = waliduj_liczbe(argv[1], 1, 3600);
        if (czas_symulacji < 0) czas_symulacji = CZAS_SYMULACJI;
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
    
    loguj("GENERATOR: Rozpoczynam generowanie klientów (czas=%d sek)", czas_symulacji);
    
    time_t czas_startu = g_shm->czas_startu;
    int id_klienta = 0;
    
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
        
        /* LIMIT AKTYWNYCH KLIENTÓW - nie forkuj gdy za dużo */
        if (g_shm->aktywni_klienci >= MAX_KLIENTOW) {
            poll(NULL, 0, 100);  /* Czekaj 100ms */
            continue;
        }
        
        /* Losuj opóźnienie między klientami (0.5-2 sekundy) */
        int opoznienie = 0;
        poll(NULL, 0, opoznienie);
        
        if (g_koniec || g_shm->faza_dnia != FAZA_OPEN) break;
        
        /* Generuj parametry klienta */
        id_klienta++;
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
            char arg_id[16], arg_wiek[8], arg_typ[4], arg_vip[4];
            char arg_dzieci[4], arg_wd1[8], arg_wd2[8];
            
            snprintf(arg_id, sizeof(arg_id), "%d", id_klienta);
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
            perror("execv klient");
            _exit(EXIT_FAILURE);
        }
        
        /* Proces rodzica kontynuuje */
    }
    
    loguj("GENERATOR: Kończę generowanie (wygenerowano %d klientów)", id_klienta);
    
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
