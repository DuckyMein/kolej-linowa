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
 * KOLEJ KRZESEŁKOWA - PRACOWNIK 1 (Stacja dolna)
 *
 * Wymaganie z zadania (awaria):
 * - SIGUSR1: pracownik zatrzymuje kolej (ustawia SHM->awaria i SHM->kolej_aktywna=0)
 * - SIGUSR2: tylko pracownik, który zatrzymał, może wznowić.
 *   Przed wznowieniem komunikuje się z drugim pracownikiem i czeka na GOTOWY.
 */

#define MY_MTYPE    1
#define OTHER_MTYPE 2

static volatile sig_atomic_t g_koniec = 0;
static volatile sig_atomic_t g_stop_req = 0;
static volatile sig_atomic_t g_start_req = 0;

/* Czy to ten pracownik zainicjował STOP (i tylko on ma prawo zrobić START) */
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
 * Zgodnie z wymaganiem projektu: wznowienie (START) może nastąpić dopiero po
 * otrzymaniu GOTOWY od drugiego pracownika.
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
            /* Jeśli w trakcie czekania przyjdzie STOP/START, obsłuż je od razu */
            if (msg.typ_komunikatu == MSG_TYP_STOP) {
                /* Drugi pracownik prosi o STOP -> potwierdź gotowość */
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
                /* Drugi pracownik prosi o START -> potwierdź gotowość */
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


/* ============================================
 * OBSŁUGA PERONU (bramki2)
 * Klient wysyła MsgPeron, pracownik1 odsyła MsgPeronOdp.
 * Jeśli pracownik1 jest wstrzymany (SIGSTOP), klienci nie dostaną odpowiedzi
 * i nie przejdą dalej na peron.
 * ============================================ */
static int obsluz_peron(void) {
    int handled = 0;
    while (!g_koniec) {
        MsgPeron req;
        int r = msg_recv_nowait(g_mq_peron, &req, sizeof(req), 0);
        if (r < 0) break;

        /*
         * Peron (bramki2) w końcówce dnia:
         * - Po FAZA_CLOSING NIE wpuszczamy nowych osób przez BRAMKA1.
         * - Ale osoby, które JUŻ są na terenie dolnej stacji (przeszły BRAMKA1),
         *   mają dokończyć cykl: wejść na peron, wsiąść, dojechać i zjechać.
         * Dlatego w CLOSING/DRAINING nadal potwierdzamy peron, o ile nie ma PANIC/awarii.
         *
         * Uwaga: to jest rozszerzenie względem "po Tk bramki przestają działać" –
         * bramka1 nadal odmawia, ale peron nie blokuje osób już wpuszczonych na teren.
         */
        MUTEX_SHM_LOCK();
        int panic = g_shm->panic;
        int awaria = g_shm->awaria;
        MUTEX_SHM_UNLOCK();

        MsgPeronOdp odp;
        odp.mtype = req.pid_klienta;
        odp.sukces = (!panic && !awaria) ? 1 : 0;

        msg_send_nowait(g_mq_peron_odp, &odp, sizeof(odp));
        handled++;
    }
    return handled;
}

/* Obsługa wiadomości pracowników bez blokowania (żeby nie zagłodzić peronu) */
static int obsluz_prac_messages(void) {
    int handled = 0;
    while (!g_koniec) {
        MsgPracownicy msg;
        int ret = msg_recv_nowait(g_mq_prac, &msg, sizeof(msg), MY_MTYPE);
        if (ret < 0) break;

        switch (msg.typ_komunikatu) {
            case MSG_TYP_STOP: {
                loguj("PRACOWNIK1: Otrzymano STOP (od P2) - potwierdzam GOTOWY");
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
                loguj("PRACOWNIK1: Otrzymano START (od P2) - potwierdzam GOTOWY");
                MsgPracownicy odp;
                odp.mtype = OTHER_MTYPE;
                odp.typ_komunikatu = MSG_TYP_GOTOWY;
                odp.nadawca = getpid();
                msg_send_nowait(g_mq_prac, &odp, sizeof(odp));
                break;
            }
            case MSG_TYP_GOTOWY:
                /* GOTOWY dla inicjatora odbieramy w czekaj_na_gotowy() */
                break;
        }
        handled++;
    }
    return handled;
}

static void wykonaj_stop_inicjator(void) {
    /* Zgłoś awarię w SHM (tylko pierwszy inicjator ją "posiada") */
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
        /* Ktoś już zatrzymał kolej */
        return;
    }

    g_jest_inicjatorem = 1;
    loguj("PRACOWNIK1: STOP (inicjator) - kolej zatrzymana");

    /* Poproś drugiego pracownika o gotowość */
    MsgPracownicy msg;
    msg.mtype = OTHER_MTYPE;
    msg.typ_komunikatu = MSG_TYP_STOP;
    msg.nadawca = getpid();
    msg_send_nowait(g_mq_prac, &msg, sizeof(msg));

    /* Czekaj na GOTOWY (max 2s, żeby nie zablokować na wieczność) */
    if (czekaj_na_gotowy(2000, 0) == 0) {
        loguj("PRACOWNIK1: Drugi pracownik GOTOWY (STOP)");
    } else {
        loguj("PRACOWNIK1: Brak GOTOWY od P2 (STOP) - kontynuuję (timeout)");
    }
}

static void wykonaj_start_inicjator(void) {
    /* Tylko inicjator ma prawo wznawiać */
    MUTEX_SHM_LOCK();
    int moja_awaria = (g_shm->pid_awaria_inicjator == getpid());
    MUTEX_SHM_UNLOCK();

    if (!moja_awaria) {
        loguj("PRACOWNIK1: Ignoruję START - nie jestem inicjatorem");
        return;
    }

    /* Jeśli dzień się zamyka / panic - nie wznawiamy */
    MUTEX_SHM_LOCK();
    int faza = g_shm->faza_dnia;
    int panic = g_shm->panic;
    pid_t pid_p2 = g_shm->pid_pracownik2;
    MUTEX_SHM_UNLOCK();

    if (panic || faza != FAZA_OPEN) {
        loguj("PRACOWNIK1: START zignorowany - nie FAZA_OPEN / PANIC");
        return;
    }

    if (pid_p2 > 0 && kill(pid_p2, 0) < 0 && errno == ESRCH) {
        loguj("PRACOWNIK1: Nie wznawiam - pracownik2 nie żyje (brak GOTOWY)");
        return;
    }

    loguj("PRACOWNIK1: START (inicjator) - proszę P2 o gotowość");

    /* Poproś P2 o gotowość do wznowienia */
    MsgPracownicy msg;
    msg.mtype = OTHER_MTYPE;
    msg.typ_komunikatu = MSG_TYP_START;
    msg.nadawca = getpid();
    msg_send_nowait(g_mq_prac, &msg, sizeof(msg));

    /* Wymaganie: bez GOTOWY nie wznawiamy */
    int r = czekaj_na_gotowy(-1, 1);
    if (r == 0) {
        loguj("PRACOWNIK1: P2 GOTOWY (START)");
    } else if (r == -2) {
        loguj("PRACOWNIK1: START przerwany - koniec dnia lub PANIC (nie wznawiam)");
        return;
    } else {
        loguj("PRACOWNIK1: START przerwany - nie otrzymałem GOTOWY (nie wznawiam)");
        return;
    }

    /* Wznów kolej */
    MUTEX_SHM_LOCK();
    g_shm->awaria = 0;
    g_shm->kolej_aktywna = 1;
    g_shm->pid_awaria_inicjator = 0;
    MUTEX_SHM_UNLOCK();

    /* Odblokuj wszystkie procesy czekające na barierze awarii */
    odblokuj_czekajacych();

    g_jest_inicjatorem = 0;
    loguj("PRACOWNIK1: Kolej wznowiona");
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
        loguj("PRACOWNIK1: Błąd dołączania do IPC");
        return EXIT_FAILURE;
    }

    loguj("PRACOWNIK1: Rozpoczynam pracę");

    while (!g_koniec) {
        /* Jeśli przyszły sygnały, obsłuż je */
        if (g_stop_req) {
            g_stop_req = 0;
            wykonaj_stop_inicjator();
        }
        if (g_start_req) {
            g_start_req = 0;
            wykonaj_start_inicjator();
        }
        /* Obsługa PERONU (bramki2) + komunikacji pracowników bez blokowania */
        int handled = 0;
        handled += obsluz_peron();
        handled += obsluz_prac_messages();

        if (!handled) {
            /* Mały sleep żeby nie kręcić CPU */
            poll(NULL, 0, 20);
        }

    }

    loguj("PRACOWNIK1: Kończę pracę");
    detach_ipc();
    return EXIT_SUCCESS;
}
