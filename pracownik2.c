#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <poll.h>
#include <errno.h>

#include "config.h"
#include "types.h"
#include "ipc.h"
#include "utils.h"

/*
 * KOLEJ KRZESEŁKOWA - PRACOWNIK 2 (Stacja górna)
 *
 * Wymaganie z zadania (awaria):
 * - SIGUSR1: pracownik zatrzymuje kolej
 * - SIGUSR2: tylko pracownik, który zatrzymał, może wznowić (po handshake)
 */

#define MY_MTYPE    2
#define OTHER_MTYPE 1

static volatile sig_atomic_t g_koniec = 0;
static volatile sig_atomic_t g_stop_req = 0;
static volatile sig_atomic_t g_start_req = 0;

static int g_jest_inicjatorem = 0;

static void handler_sigterm(int sig) {
    (void)sig;
    g_koniec = 1;
}

static void handler_sigusr1(int sig) {
    (void)sig;
    g_stop_req = 1;
}

static void handler_sigusr2(int sig) {
    (void)sig;
    g_start_req = 1;
}

/*
 * Czeka na komunikat GOTOWY od drugiego pracownika.
 *
 * Wymaganie: wznowienie (START) może nastąpić dopiero po otrzymaniu GOTOWY.
 *
 * timeout_ms >= 0: maksymalny czas oczekiwania
 * timeout_ms < 0: czekaj bez limitu, ale przerwij gdy:
 *   - proces ma się kończyć (SIGTERM/SIGINT)
 *   - system wychodzi z FAZA_OPEN (CLOSING/DRAINING) lub PANIC
 */
static int czekaj_na_gotowy(int timeout_ms, int wymagaj_open) {
    int waited = 0;
    while (!g_koniec) {
        if (wymagaj_open) {
            MUTEX_SHM_LOCK();
            int faza = g_shm->faza_dnia;
            int panic = g_shm->panic;
            MUTEX_SHM_UNLOCK();
            if (panic || faza != FAZA_OPEN) {
                return -2;
            }
        }

        MsgPracownicy msg;
        int r = msg_recv_nowait(g_mq_prac, &msg, sizeof(msg), MY_MTYPE);
        if (r >= 0) {
            if (msg.typ_komunikatu == MSG_TYP_GOTOWY) {
                return 0;
            }
            if (msg.typ_komunikatu == MSG_TYP_STOP) {
                MUTEX_SHM_LOCK();
                g_shm->awaria = 1;
                g_shm->kolej_aktywna = 0;
                MUTEX_SHM_UNLOCK();

                MsgPracownicy odp;
                odp.mtype = OTHER_MTYPE;
                odp.typ_komunikatu = MSG_TYP_GOTOWY;
                odp.nadawca = getpid();
                msg_send_nowait(g_mq_prac, &odp, sizeof(odp));
                continue;
            }
            if (msg.typ_komunikatu == MSG_TYP_START) {
                MsgPracownicy odp;
                odp.mtype = OTHER_MTYPE;
                odp.typ_komunikatu = MSG_TYP_GOTOWY;
                odp.nadawca = getpid();
                msg_send_nowait(g_mq_prac, &odp, sizeof(odp));
                continue;
            }
        }

        if (timeout_ms >= 0 && waited >= timeout_ms) {
            return -1;
        }
        poll(NULL, 0, 20);
        waited += 20;
    }
    return -1;
}

static void wykonaj_stop_inicjator(void) {
    int jestem_wlascicielem = 0;

    MUTEX_SHM_LOCK();
    if (!g_shm->awaria) {
        g_shm->awaria = 1;
        g_shm->kolej_aktywna = 0;
        g_shm->stats.liczba_zatrzyman++;
        g_shm->pid_awaria_inicjator = getpid();
        jestem_wlascicielem = 1;
    } else if (g_shm->pid_awaria_inicjator == getpid()) {
        jestem_wlascicielem = 1;
    }
    MUTEX_SHM_UNLOCK();

    if (!jestem_wlascicielem) {
        return;
    }

    g_jest_inicjatorem = 1;
    loguj("PRACOWNIK2: STOP (inicjator) - kolej zatrzymana");

    MsgPracownicy msg;
    msg.mtype = OTHER_MTYPE;
    msg.typ_komunikatu = MSG_TYP_STOP;
    msg.nadawca = getpid();
    msg_send_nowait(g_mq_prac, &msg, sizeof(msg));

    if (czekaj_na_gotowy(2000, 0) == 0) {
        loguj("PRACOWNIK2: Drugi pracownik GOTOWY (STOP)");
    } else {
        loguj("PRACOWNIK2: Brak GOTOWY od P1 (STOP) - kontynuuję (timeout)");
    }
}

static void wykonaj_start_inicjator(void) {
    MUTEX_SHM_LOCK();
    int moja_awaria = (g_shm->pid_awaria_inicjator == getpid());
    MUTEX_SHM_UNLOCK();

    if (!moja_awaria) {
        loguj("PRACOWNIK2: Ignoruję START - nie jestem inicjatorem");
        return;
    }

    /* Jeśli dzień się zamyka / panic - nie wznawiamy */
    MUTEX_SHM_LOCK();
    int faza = g_shm->faza_dnia;
    int panic = g_shm->panic;
    pid_t pid_p1 = g_shm->pid_pracownik1;
    MUTEX_SHM_UNLOCK();

    if (panic || faza != FAZA_OPEN) {
        loguj("PRACOWNIK2: START zignorowany - nie FAZA_OPEN / PANIC");
        return;
    }

    if (pid_p1 > 0 && kill(pid_p1, 0) < 0 && errno == ESRCH) {
        loguj("PRACOWNIK2: Nie wznawiam - pracownik1 nie żyje (brak GOTOWY)");
        return;
    }

    loguj("PRACOWNIK2: START (inicjator) - proszę P1 o gotowość");

    MsgPracownicy msg;
    msg.mtype = OTHER_MTYPE;
    msg.typ_komunikatu = MSG_TYP_START;
    msg.nadawca = getpid();
    msg_send_nowait(g_mq_prac, &msg, sizeof(msg));

    /* Wymaganie: bez GOTOWY nie wznawiamy */
    int r = czekaj_na_gotowy(-1, 1);
    if (r == 0) {
        loguj("PRACOWNIK2: P1 GOTOWY (START)");
    } else if (r == -2) {
        loguj("PRACOWNIK2: START przerwany - koniec dnia lub PANIC (nie wznawiam)");
        return;
    } else {
        loguj("PRACOWNIK2: START przerwany - nie otrzymałem GOTOWY (nie wznawiam)");
        return;
    }

    MUTEX_SHM_LOCK();
    g_shm->awaria = 0;
    g_shm->kolej_aktywna = 1;
    g_shm->pid_awaria_inicjator = 0;
    MUTEX_SHM_UNLOCK();

    odblokuj_czekajacych();

    g_jest_inicjatorem = 0;
    loguj("PRACOWNIK2: Kolej wznowiona");
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    ustaw_smierc_z_rodzicem();

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

    if (attach_ipc() != 0) {
        loguj("PRACOWNIK2: Błąd dołączania do IPC");
        return EXIT_FAILURE;
    }

    loguj("PRACOWNIK2: Rozpoczynam pracę");

    while (!g_koniec) {
        if (g_stop_req) {
            g_stop_req = 0;
            wykonaj_stop_inicjator();
        }
        if (g_start_req) {
            g_start_req = 0;
            wykonaj_start_inicjator();
        }

        MsgPracownicy msg;
        int ret = msg_recv(g_mq_prac, &msg, sizeof(msg), MY_MTYPE);
        if (ret < 0) {
            continue; /* EINTR -> obsłuż flagi */
        }
        if (g_koniec) {
            break;
        }

        switch (msg.typ_komunikatu) {
            case MSG_TYP_STOP: {
                loguj("PRACOWNIK2: Otrzymano STOP (od P1) - potwierdzam GOTOWY");
                MUTEX_SHM_LOCK();
                g_shm->awaria = 1;
                g_shm->kolej_aktywna = 0;
                MUTEX_SHM_UNLOCK();

                MsgPracownicy odp;
                odp.mtype = OTHER_MTYPE;
                odp.typ_komunikatu = MSG_TYP_GOTOWY;
                odp.nadawca = getpid();
                msg_send_nowait(g_mq_prac, &odp, sizeof(odp));
                break;
            }
            case MSG_TYP_START: {
                loguj("PRACOWNIK2: Otrzymano START (od P1) - potwierdzam GOTOWY");
                MsgPracownicy odp;
                odp.mtype = OTHER_MTYPE;
                odp.typ_komunikatu = MSG_TYP_GOTOWY;
                odp.nadawca = getpid();
                msg_send_nowait(g_mq_prac, &odp, sizeof(odp));
                break;
            }
            case MSG_TYP_GOTOWY:
                break;
        }
    }

    loguj("PRACOWNIK2: Kończę pracę");
    detach_ipc();
    return EXIT_SUCCESS;
}
