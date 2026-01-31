#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/prctl.h>
#include <poll.h>

#include "config.h"
#include "ipc.h"

/*
 * KOLEJ KRZESEŁKOWA - SPRZĄTACZ IPC (GUARDIAN)
 *
 * Proces strażnik w osobnej grupie procesów.
 * Gdy main umrze (nawet SIGKILL), sprzątacz:
 * 1. Wysyła SIGTERM do grupy symulacji
 * 2. Po chwili SIGKILL
 * 3. Czyści wszystkie zasoby IPC (IPC_RMID)
 */

static volatile sig_atomic_t g_sig = 0;
static volatile sig_atomic_t g_force = 0;
static int g_pgid = -1;

static void on_signal(int sig) {
    if (sig == SIGUSR1) g_force = 1;
    g_sig = sig;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Użycie: %s <pgid>\n", argv[0]);
        return 2;
    }
    g_pgid = atoi(argv[1]);

    /* oddzielna grupa → killpg nie ubije sprzątacza */
    setpgid(0, 0);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);

    /* gdy main umrze (nawet SIGKILL) → dostaniemy SIGTERM */
    prctl(PR_SET_PDEATHSIG, SIGTERM);

    /* Jeśli main już nie żyje (race check) */
    if (getppid() == 1) g_sig = SIGTERM;

    /* Czekamy na sygnał */
    while (!g_sig) pause();

    /*
     * Normalny shutdown: main żyje i prosi sprzątacza o wyjście.
     * Wtedy NIE zabijamy grupy i NIE czyścimy IPC.
     * Sprzątanie uruchamiamy tylko gdy main umarł (ppid==1) lub gdy wymuszono (SIGUSR1).
     */
    if (!g_force && getppid() != 1) {
        return 0;
    }

    /* Zabij całą grupę symulacji */
    if (g_pgid > 1) {
        kill(-g_pgid, SIGTERM);
        poll(NULL, 0, 200);
        kill(-g_pgid, SIGKILL);
        poll(NULL, 0, 200);
    }

    /* Wyczyść IPC */
    cleanup_ipc_by_keys();
    return 0;
}
