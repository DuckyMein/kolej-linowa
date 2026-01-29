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
static int g_pgid = -1;

static void on_signal(int sig) {
    g_sig = sig;
}

static void try_remove_sem(key_t base) {
    int semid = semget(base + IPC_KEY_SEM, 1, 0);
    if (semid != -1) semctl(semid, 0, IPC_RMID);
}

static void try_remove_shm(key_t base) {
    int shmid = shmget(base + IPC_KEY_SHM, 1, 0);
    if (shmid != -1) shmctl(shmid, IPC_RMID, NULL);
}

static void try_remove_mq(key_t base, int offset) {
    int mqid = msgget(base + offset, 0);
    if (mqid != -1) msgctl(mqid, IPC_RMID, NULL);
}

static void cleanup_ipc_by_keys(void) {
    key_t base = ftok(".", IPC_KEY_BASE);
    if (base == -1) return;

    try_remove_shm(base);
    try_remove_sem(base);
    try_remove_mq(base, IPC_KEY_MQ_KASA);
    try_remove_mq(base, IPC_KEY_MQ_KASA_ODP);
    try_remove_mq(base, IPC_KEY_MQ_BRAMKA);
    try_remove_mq(base, IPC_KEY_MQ_BRAMKA_ODP);
    try_remove_mq(base, IPC_KEY_MQ_PRAC);
    try_remove_mq(base, IPC_KEY_MQ_WYCIAG_REQ);
    try_remove_mq(base, IPC_KEY_MQ_WYCIAG_ODP);
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

    /* gdy main umrze (nawet SIGKILL) → dostaniemy SIGTERM */
    prctl(PR_SET_PDEATHSIG, SIGTERM);

    /* Jeśli main już nie żyje (race check) */
    if (getppid() == 1) g_sig = SIGTERM;

    /* Czekamy na sygnał */
    while (!g_sig) pause();

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
