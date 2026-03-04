#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <sys/msg.h>

#include "ipc.h"
#include "utils.h"

/*
 * monitor.c
 * Prosty "dashboard" na żywo do prezentacji komunikacji i stanu IPC.
 *
 * Użycie:
 *   ./monitor                 # odświeża ekran co 500ms
 *   ./monitor --once          # pojedynczy snapshot
 *   ./monitor --interval-ms 200
 *   ./monitor --no-color
 */

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig) {
    (void)sig;
    g_stop = 1;
}

static int is_alive(pid_t pid) {
    if (pid <= 1) return 0;
    if (kill(pid, 0) == 0) return 1;
    return errno != ESRCH;
}

static const char *faza_name(FazaDnia f) {
    switch (f) {
        case FAZA_OPEN: return "OPEN";
        case FAZA_CLOSING: return "CLOSING";
        case FAZA_DRAINING: return "DRAINING";
        default: return "?";
    }
}

typedef struct {
    int color;
} Ui;

static const char *c(Ui *ui, const char *code) {
    return ui->color ? code : "";
}

static void print_hr(void) {
    printf("------------------------------------------------------------\n");
}

static long mq_qnum(int mqid) {
    struct msqid_ds ds;
    if (mqid < 0) return -1;
    if (msgctl(mqid, IPC_STAT, &ds) == -1) return -1;
    return (long)ds.msg_qnum;
}

static long mq_qbytes(int mqid) {
    struct msqid_ds ds;
    if (mqid < 0) return -1;
    if (msgctl(mqid, IPC_STAT, &ds) == -1) return -1;
    return (long)ds.msg_qbytes;
}

/* Zwraca 0 gdy OK, !=0 gdy monitor powinien wyjść (np. IPC usunięte / main nie żyje). */
static int print_snapshot(Ui *ui) {
    if (!g_shm) {
        printf("Brak SHM (g_shm=NULL)\n");
        return 1;
    }

    /*
     * UWAGA: SharedMemory jest bardzo duże (dziesiątki MB: karnety + logi).
     * Nie wolno robić: `SharedMemory s = *g_shm;` bo to kopiuje cały segment
     * do stosu i kończy się SIGSEGV (stack overflow).
     *
     * Zamiast tego kopiujemy TYLKO potrzebne pola do małego snapshota.
     */
    typedef struct {
        FazaDnia faza_dnia;
        int awaria;
        int panic;

        time_t czas_startu;
        time_t czas_konca_dnia;
        int aktywni_klienci;

        int osoby_na_terenie;
        int osoby_na_gorze;
        int osoby_na_peronie;
        int osoby_w_krzesle;

        Statystyki stats;

        pid_t pid_main;
        pid_t pid_generator;
        pid_t pid_kasjer;
        pid_t pid_wyciag;
        pid_t pid_pracownik1;
        pid_t pid_pracownik2;
        pid_t pid_bramki1[LICZBA_BRAMEK1];
    } MonitorSnapshot;

    MonitorSnapshot s;
    memset(&s, 0, sizeof(s));

    /* Odczyt bezpieczny (z mutexem). Jeśli mutex padł (IPC usunięte), pokaż info zamiast crash. */
    if (mutex_lock(SEM_MUTEX_SHM) != 0) {
        printf("Brak dostepu do SHM (mutex SEM_MUTEX_SHM) - czy IPC zostalo usuniete?\n");
        return 1;
    }
    s.faza_dnia = g_shm->faza_dnia;
    s.awaria = g_shm->awaria;
    s.panic = g_shm->panic;

    s.czas_startu = g_shm->czas_startu;
    s.czas_konca_dnia = g_shm->czas_konca_dnia;
    s.aktywni_klienci = g_shm->aktywni_klienci;

    s.osoby_na_terenie = g_shm->osoby_na_terenie;
    s.osoby_na_gorze = g_shm->osoby_na_gorze;
    s.osoby_na_peronie = g_shm->osoby_na_peronie;
    s.osoby_w_krzesle = g_shm->osoby_w_krzesle;

    s.stats = g_shm->stats;

    s.pid_main = g_shm->pid_main;
    s.pid_generator = g_shm->pid_generator;
    s.pid_kasjer = g_shm->pid_kasjer;
    s.pid_wyciag = g_shm->pid_wyciag;
    s.pid_pracownik1 = g_shm->pid_pracownik1;
    s.pid_pracownik2 = g_shm->pid_pracownik2;
    memcpy(s.pid_bramki1, g_shm->pid_bramki1, sizeof(s.pid_bramki1));
    mutex_unlock(SEM_MUTEX_SHM);

    /* Jeśli main nie żyje, nie ma sensu trzymać SHM (a po IPC_RMID blokuje to zwolnienie segmentu). */
    if (!is_alive(s.pid_main)) {
        printf("[monitor] main=%d nie zyje -> koncze monitor\n", (int)s.pid_main);
        return 1;
    }

    time_t now = time(NULL);
    long left = (s.czas_konca_dnia > 0) ? (long)(s.czas_konca_dnia - now) : -1;

    const char *phase_color = "";
    if (ui->color) {
        if (s.faza_dnia == FAZA_OPEN) phase_color = "\x1b[32m";          /* green */
        else if (s.faza_dnia == FAZA_CLOSING) phase_color = "\x1b[33m";   /* yellow */
        else if (s.faza_dnia == FAZA_DRAINING) phase_color = "\x1b[35m";  /* magenta */
    }

    printf("%sKOLEJ KRZESE\xC5\x81KOWA - MONITOR%s\n", c(ui, "\x1b[1m"), c(ui, "\x1b[0m"));
    print_hr();

    printf("Faza dnia: %s%s%s   awaria=%d   panic=%d\n",
           phase_color, faza_name(s.faza_dnia), c(ui, "\x1b[0m"),
           s.awaria, s.panic);

    if (left >= 0) {
        printf("Czas do konca dnia: %ld s   (start=%ld, koniec=%ld)\n",
               left, (long)s.czas_startu, (long)s.czas_konca_dnia);
    } else {
        printf("Czas do konca dnia: n/a\n");
    }

    print_hr();
    printf("Liczniki: teren=%d  peron=%d  w_krzesle=%d  gora=%d  aktywni_klienci=%d\n",
           s.osoby_na_terenie, s.osoby_na_peronie, s.osoby_w_krzesle, s.osoby_na_gorze, s.aktywni_klienci);
    printf("Statystyki: wygenerowani=%d  przejazdy=%d  przychod=%.2f zl\n",
           s.stats.laczna_liczba_klientow, s.stats.liczba_przejazdow, s.stats.przychod_gr / 100.0);

    print_hr();
    printf("Semafory: TEREN=%d  PERON=%d  BARIERA_AWARIA=%d\n",
           sem_getval_ipc(SEM_TEREN), sem_getval_ipc(SEM_PERON), sem_getval_ipc(SEM_BARIERA_AWARIA));

    print_hr();
    printf("MQ: kasa=%ld/%ld  kasa_odp=%ld/%ld  bramka=%ld/%ld  bramka_odp=%ld/%ld\n",
           mq_qnum(g_mq_kasa), mq_qbytes(g_mq_kasa),
           mq_qnum(g_mq_kasa_odp), mq_qbytes(g_mq_kasa_odp),
           mq_qnum(g_mq_bramka), mq_qbytes(g_mq_bramka),
           mq_qnum(g_mq_bramka_odp), mq_qbytes(g_mq_bramka_odp));
    printf("MQ: peron=%ld/%ld  peron_odp=%ld/%ld  wyciag_req=%ld/%ld  wyciag_odp=%ld/%ld  prac=%ld/%ld\n",
           mq_qnum(g_mq_peron), mq_qbytes(g_mq_peron),
           mq_qnum(g_mq_peron_odp), mq_qbytes(g_mq_peron_odp),
           mq_qnum(g_mq_wyciag_req), mq_qbytes(g_mq_wyciag_req),
           mq_qnum(g_mq_wyciag_odp), mq_qbytes(g_mq_wyciag_odp),
           mq_qnum(g_mq_prac), mq_qbytes(g_mq_prac));

    print_hr();
    printf("PIDy: main=%d%s  gen=%d%s  kasjer=%d%s  wyciag=%d%s\n",
           (int)s.pid_main, is_alive(s.pid_main) ? "" : "(dead)",
           (int)s.pid_generator, is_alive(s.pid_generator) ? "" : "(dead)",
           (int)s.pid_kasjer, is_alive(s.pid_kasjer) ? "" : "(dead)",
           (int)s.pid_wyciag, is_alive(s.pid_wyciag) ? "" : "(dead)");
    printf("PIDy: P1=%d%s  P2=%d%s  bramki:",
           (int)s.pid_pracownik1, is_alive(s.pid_pracownik1) ? "" : "(dead)",
           (int)s.pid_pracownik2, is_alive(s.pid_pracownik2) ? "" : "(dead)");
    for (int i = 0; i < LICZBA_BRAMEK1; i++) {
        printf(" %d%s", (int)s.pid_bramki1[i], is_alive(s.pid_bramki1[i]) ? "" : "(dead)");
    }
    printf("\n");

    return 0;
}

static void usage(const char *argv0) {
    printf("Uzycie: %s [--once] [--interval-ms N] [--no-color]\n", argv0);
}

int main(int argc, char **argv) {
    int once = 0;
    int interval_ms = 500;
    Ui ui = {.color = 1};

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--once") == 0) {
            once = 1;
        } else if (strcmp(argv[i], "--no-color") == 0) {
            ui.color = 0;
        } else if (strcmp(argv[i], "--interval-ms") == 0 && i + 1 < argc) {
            interval_ms = atoi(argv[++i]);
            if (interval_ms < 50) interval_ms = 50;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (attach_ipc() != 0) {
        fprintf(stderr, "[monitor] Nie moge podlaczyc IPC (czy ./main dziala?)\n");
        return 2;
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    if (once) {
        (void)print_snapshot(&ui);
        detach_ipc();
        return 0;
    }

    while (!g_stop) {
        /* czyść ekran */
        if (ui.color) {
            printf("\x1b[2J\x1b[H");
        } else {
            printf("\n\n");
        }
        if (print_snapshot(&ui) != 0) {
            break;
        }
        fflush(stdout);
        usleep((useconds_t)interval_ms * 1000u);
    }

    detach_ipc();
    return 0;
}
