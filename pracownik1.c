#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include "config.h"
#include "types.h"
#include "ipc.h"
#include "utils.h"

/*
 * KOLEJ KRZESEŁKOWA - PRACOWNIK 1 (Stacja dolna)
 * 
 * Odpowiedzialności:
 * 1. Obsługa Bramek2 (wpuszczanie na peron)
 * 2. Kontrola wsiadania do krzesełek
 * 3. Obsługa awarii (STOP/START)
 * 4. Komunikacja z Pracownikiem2
 */

static volatile sig_atomic_t g_koniec = 0;
static volatile sig_atomic_t g_awaria = 0;

static void handler_sigterm(int sig) {
    (void)sig;
    g_koniec = 1;
}

static void handler_sigusr1(int sig) {
    (void)sig;
    g_awaria = 1;
}

static void handler_sigusr2(int sig) {
    (void)sig;
    g_awaria = 0;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    /* Obsługa sygnałów */
    signal(SIGTERM, handler_sigterm);
    signal(SIGINT, handler_sigterm);
    signal(SIGUSR1, handler_sigusr1);
    signal(SIGUSR2, handler_sigusr2);
    
    /* Dołącz do IPC */
    if (attach_ipc() != 0) {
        loguj("PRACOWNIK1: Błąd dołączania do IPC");
        return EXIT_FAILURE;
    }
    
    loguj("PRACOWNIK1: Rozpoczynam pracę na stacji dolnej");
    
    /* Główna pętla */
    while (!g_koniec && !g_shm->koniec_dnia) {
        /* Sprawdź komunikaty od pracowników */
        MsgPracownicy msg;
        int ret = msg_recv_nowait(g_mq_prac, &msg, sizeof(msg), 1); /* mtype=1 = do P1 */
        
        if (ret > 0) {
            switch (msg.typ_komunikatu) {
                case MSG_TYP_STOP:
                    loguj("PRACOWNIK1: Otrzymano STOP");
                    g_awaria = 1;
                    
                    /* Potwierdź gotowość */
                    MsgPracownicy odp;
                    odp.mtype = 2; /* do P2 */
                    odp.typ_komunikatu = MSG_TYP_GOTOWY;
                    odp.nadawca = getpid();
                    msg_send(g_mq_prac, &odp, sizeof(odp));
                    
                    /* Sygnalizuj gotowość przez semafor */
                    sem_signal_ipc(SEM_GOTOWY_P1);
                    break;
                    
                case MSG_TYP_START:
                    loguj("PRACOWNIK1: Otrzymano START - wznawianie");
                    g_awaria = 0;
                    break;
                    
                case MSG_TYP_GOTOWY:
                    loguj("PRACOWNIK1: P2 gotowy");
                    break;
            }
        }
        
        /* Podczas awarii - nie obsługuj klientów */
        if (g_awaria || g_shm->awaria) {
            usleep(100000); /* 100ms */
            continue;
        }
        
        /* Obsługa peronu - kontrola rzędów krzesełek */
        /* W pełnej implementacji tu byłaby logika krzesełek */
        
        /* Symulacja cyklu krzesełka (uproszczona) */
        static int cykl = 0;
        cykl++;
        
        if (cykl % 50 == 0) { /* Co 5 sekund (50 * 100ms) */
            /* Odnów miejsca w rzędzie (nowy rząd nadjechał) */
            int aktualna_wartosc = sem_getval_ipc(SEM_PERON);
            if (aktualna_wartosc < KRZESLA_W_RZEDZIE) {
                sem_signal_n(SEM_PERON, KRZESLA_W_RZEDZIE - aktualna_wartosc);
            }
            
            /* Aktualizuj numer rzędu */
            MUTEX_SHM_LOCK();
            g_shm->aktualny_rzad = (g_shm->aktualny_rzad + 1) % LICZBA_RZEDOW;
            MUTEX_SHM_UNLOCK();
        }
        
        usleep(100000); /* 100ms */
    }
    
    loguj("PRACOWNIK1: Kończę pracę");
    detach_ipc();
    
    return EXIT_SUCCESS;
}
