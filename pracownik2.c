#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <poll.h>

#include "config.h"
#include "types.h"
#include "ipc.h"
#include "utils.h"

/*
 * KOLEJ KRZESEŁKOWA - PRACOWNIK 2 (Stacja górna)
 * 
 * Odpowiedzialności:
 * 1. Obsługa wysiadania ze krzesełek
 * 2. Kierowanie do wyjść (2 bramki górne - dummy)
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
    
    /* Ustaw aby zginąć gdy rodzic (main) umrze */
    ustaw_smierc_z_rodzicem();
    
    /* Obsługa sygnałów - sigaction BEZ SA_RESTART */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    
    sa.sa_handler = handler_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    
    sa.sa_handler = handler_sigusr1;
    sigaction(SIGUSR1, &sa, NULL);
    
    sa.sa_handler = handler_sigusr2;
    sigaction(SIGUSR2, &sa, NULL);
    
    /* Dołącz do IPC */
    if (attach_ipc() != 0) {
        loguj("PRACOWNIK2: Błąd dołączania do IPC");
        return EXIT_FAILURE;
    }
    
    loguj("PRACOWNIK2: Rozpoczynam pracę na stacji górnej (2 bramki dummy)");
    
    /* Główna pętla - blokujące msg_recv */
    while (!g_koniec) {
        /* Odbierz komunikat BLOKUJĄCO (mtype=2 = do P2) */
        MsgPracownicy msg;
        int ret = msg_recv(g_mq_prac, &msg, sizeof(msg), 2);
        
        if (ret < 0 || g_koniec) {
            /* Przerwane sygnałem lub koniec - wyjdź */
            break;
        }
        
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
        
        /* BRAMKI GÓRNE (dummy) - zawsze przepuszczają */
        /* Klienci po prostu przechodzą, logowanie w klient.c */
    }
    
    loguj("PRACOWNIK2: Kończę pracę");
    detach_ipc();
    
    return EXIT_SUCCESS;
}
