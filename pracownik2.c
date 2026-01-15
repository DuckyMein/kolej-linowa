#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include "config.h"
#include "types.h"
#include "ipc.h"
#include "utils.h"

/*
 * KOLEJ KRZESEŁKOWA - PRACOWNIK 2 (Stacja górna)
 * 
 * Odpowiedzialności:
 * 1. Obsługa wysiadania ze krzesełek
 * 2. Kierowanie do wyjść (2 wyjścia)
 * 3. Obsługa awarii (STOP/START)
 * 4. Komunikacja z Pracownikiem1
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
        loguj("PRACOWNIK2: Błąd dołączania do IPC");
        return EXIT_FAILURE;
    }
    
    loguj("PRACOWNIK2: Rozpoczynam pracę na stacji górnej");
    
    /* Główna pętla */
    while (!g_koniec && !g_shm->koniec_dnia) {
        /* Sprawdź komunikaty */
        MsgPracownicy msg;
        int ret = msg_recv_nowait(g_mq_prac, &msg, sizeof(msg), 2); /* mtype=2 = do P2 */
        
        if (ret > 0) {
            switch (msg.typ_komunikatu) {
                case MSG_TYP_STOP:
                    loguj("PRACOWNIK2: Otrzymano STOP");
                    g_awaria = 1;
                    
                    /* Potwierdź gotowość */
                    MsgPracownicy odp;
                    odp.mtype = 1; /* do P1 */
                    odp.typ_komunikatu = MSG_TYP_GOTOWY;
                    odp.nadawca = getpid();
                    msg_send(g_mq_prac, &odp, sizeof(odp));
                    
                    /* Sygnalizuj gotowość przez semafor */
                    sem_signal_ipc(SEM_GOTOWY_P2);
                    break;
                    
                case MSG_TYP_START:
                    loguj("PRACOWNIK2: Otrzymano START - wznawianie");
                    g_awaria = 0;
                    break;
                    
                case MSG_TYP_GOTOWY:
                    loguj("PRACOWNIK2: P1 gotowy");
                    break;
            }
        }
        
        /* Podczas awarii - nie obsługuj klientów */
        if (g_awaria || g_shm->awaria) {
            usleep(100000); /* 100ms */
            continue;
        }
        
        /* Obsługa stacji górnej */
        /* W pełnej implementacji tu byłaby logika wysiadania */
        
        usleep(100000); /* 100ms */
    }
    
    loguj("PRACOWNIK2: Kończę pracę");
    detach_ipc();
    
    return EXIT_SUCCESS;
}
