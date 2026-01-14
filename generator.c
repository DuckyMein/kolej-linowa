#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>

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

static void handler_sigterm(int sig) {
    (void)sig;
    g_koniec = 1;
}

static void handler_sigchld(int sig) {
    (void)sig;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0);
    errno = saved_errno;
}

int main(int argc, char *argv[]) {
    int czas_symulacji = CZAS_SYMULACJI;
    
    if (argc >= 2) {
        czas_symulacji = waliduj_liczbe(argv[1], 1, 3600);
        if (czas_symulacji < 0) czas_symulacji = CZAS_SYMULACJI;
    }
    
    /* Inicjalizacja */
    inicjalizuj_losowanie();
    
    /* Obsługa sygnałów */
    signal(SIGTERM, handler_sigterm);
    signal(SIGINT, handler_sigterm);
    signal(SIGCHLD, handler_sigchld);
    
    /* Dołącz do IPC */
    if (attach_ipc() != 0) {
        loguj("GENERATOR: Błąd dołączania do IPC");
        return EXIT_FAILURE;
    }
    
    loguj("GENERATOR: Rozpoczynam generowanie klientów (czas=%d sek)", czas_symulacji);
    
    time_t czas_startu = g_shm->czas_startu;
    int id_klienta = 0;
    
    /* Główna pętla generowania */
    while (!g_koniec && !g_shm->koniec_dnia) {
        /* Sprawdź czas */
        if (czy_koniec_symulacji(czas_startu, czas_symulacji)) {
            loguj("GENERATOR: Czas symulacji upłynął");
            break;
        }
        
        /* Sprawdź czy kolej aktywna */
        if (g_shm->awaria) {
            usleep(100000); /* 100ms - czekaj na koniec awarii */
            continue;
        }
        
        /* Losuj czy generować klienta (średnio co 1-3 sekundy) */
        int opoznienie = losuj_zakres(500, 2000); /* 0.5-2 sekundy */
        usleep(opoznienie * 1000);
        
        if (g_koniec || g_shm->koniec_dnia) break;
        
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
        
        loguj("GENERATOR: Tworzę klienta %d (wiek=%d, typ=%s, VIP=%d, dzieci=%d)",
              id_klienta, wiek, 
              typ == TYP_ROWERZYSTA ? "rowerzysta" : "pieszy",
              vip, liczba_dzieci);
        
        /* Fork procesu klienta */
        pid_t pid = fork();
        
        if (pid == -1) {
            blad_ostrzezenie("fork klienta");
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
    
    loguj("GENERATOR: Kończę pracę (wygenerowano %d klientów)", id_klienta);
    
    /* Czekaj na zakończenie wszystkich dzieci */
    while (waitpid(-1, NULL, WNOHANG) > 0);
    
    detach_ipc();
    
    return EXIT_SUCCESS;
}
