#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/prctl.h>

#include "config.h"
#include "ipc.h"

/*
 * SPRZĄTACZ IPC (GUARDIAN) - wersja odporna na race po SIGKILL main.
 *
 * Cel:
 * - po śmierci main (nawet SIGKILL) sprzątacz ma:
 *   1) ubić procesy symulacji (wyciąg/klienci itd.)
 *   2) wykonać IPC_RMID na wszystkich zasobach SysV
 *
 * Problemy, które ta wersja rozwiązuje:
 * - getppid()==1 jest zawodne (race) -> używamy kill(parent_pid,0) + weryfikacja /proc/<pid>/exe
 * - część procesów może nie być w PGID -> dodatkowy kill po nazwie binarki (/proc/<pid>/exe)
 */

static volatile sig_atomic_t g_sig = 0;
static volatile sig_atomic_t g_force = 0;

static int   g_pgid = -1;
static pid_t g_parent_pid = -1;

static void on_signal(int sig) {
    if (sig == SIGUSR1) g_force = 1;
    g_sig = sig;
}

/* basename bez <libgen.h> */
static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? (s + 1) : p;
}

static int readlink_exe(pid_t pid, char *buf, size_t buflen) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/exe", (int)pid);
    ssize_t n = readlink(path, buf, buflen - 1);
    if (n < 0) return -1;
    buf[n] = '\0';
    return 0;
}

static int pid_is_our_program(pid_t pid, const char *name) {
    char exe[512];
    /* W niektórych środowiskach (/proc hidepid, uprawnienia, WSL2 edge-case)
     * readlink("/proc/<pid>/exe") może się nie udać nawet dla własnego procesu.
     * To NIE powinno powodować fałszywego "crash" i ubijania całej symulacji.
     *
     * Zwracamy:
     *  1  - to nasz program
     *  0  - to na pewno nie nasz program
     * -1  - nie udało się ustalić (traktuj jako "nie wiem")
     */
    if (readlink_exe(pid, exe, sizeof(exe)) != 0) return -1;
    const char *bn = base_name(exe);
    return (strcmp(bn, name) == 0) ? 1 : 0;
}

/* Czy "rodzic" nadal żyje i jest tym samym main-em? */
static int parent_alive_and_is_main(void) {
    if (g_parent_pid <= 1) return 0;

    /* Szybka ścieżka: getppid() zmieniony = rodzic umarł (reparented do init/subreaper) */
    if (getppid() != g_parent_pid) return 0;

    /* kill(pid,0) == -1 ESRCH => nie żyje */
    if (kill(g_parent_pid, 0) == -1) {
        if (errno == ESRCH) return 0;
    }

    /* Jeśli PID został zre-użyty, /proc/<pid>/exe nie będzie "main".
     * Ale gdy nie mamy dostępu do /proc/<pid>/exe:
     *
     * KLUCZOWA ZMIANA: jeśli readlink zwraca -1, to MOŻE być zombie
     * (po SIGKILL: /proc/<pid>/exe staje się niedostępny).
     * Jeśli dostaliśmy sygnał (PDEATHSIG/SIGTERM) i nie możemy
     * zweryfikować exe → bezpieczniej zakładać że rodzic NIE żyje.
     * Fałszywy cleanup jest lepszy niż brak cleanup po crashu.
     */
    int is_main = pid_is_our_program(g_parent_pid, "main");
    if (is_main == 0) return 0;   /* na pewno nie "main" */
    if (is_main == -1) {
        /* Nie udało się odczytać /proc/<pid>/exe.
         * Jeśli mamy sygnał (g_sig != 0) → prawdopodobnie crash/zombie → sprzątamy.
         * Jeśli nie mamy sygnału (race-check na starcie) → zakładamy że żyje. */
        if (g_sig != 0) return 0;  /* dostaliśmy sygnał + readlink fail → crash */
        return 1;                  /* brak sygnału + readlink fail → ostrożnie */
    }

    return 1;
}

/* Kill po nazwie binarki (dla przypadków gdy proces nie jest w PGID) */
static void kill_by_exe_name(const char *name, int sig) {
    DIR *d = opendir("/proc");
    if (!d) return;

    uid_t myuid = getuid();

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        char *end = NULL;
        long lpid = strtol(de->d_name, &end, 10);
        if (!end || *end != '\0') continue;
        if (lpid <= 1) continue;
        pid_t pid = (pid_t)lpid;

        if (pid == getpid()) continue;

        /* sprawdź UID właściciela procesu */
        char procdir[64];
        snprintf(procdir, sizeof(procdir), "/proc/%d", (int)pid);
        struct stat st;
        if (stat(procdir, &st) != 0) continue;
        if (st.st_uid != myuid) continue;

        if (!pid_is_our_program(pid, name)) continue;

        kill(pid, sig);
    }
    closedir(d);
}

static void hard_kill_remaining(void) {
    /* Kolejność: najpierw wyciąg, potem klienci, reszta */
    kill_by_exe_name("wyciag", SIGKILL);
    kill_by_exe_name("klient", SIGKILL);
    /* monitor bywa uruchamiany z tmux (poza PGID symulacji) -> musi być dobijany po nazwie */
    kill_by_exe_name("monitor", SIGKILL);
    kill_by_exe_name("generator", SIGKILL);
    kill_by_exe_name("kasjer", SIGKILL);
    kill_by_exe_name("bramka", SIGKILL);
    kill_by_exe_name("pracownik1", SIGKILL);
    kill_by_exe_name("pracownik2", SIGKILL);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Użycie: %s <pgid>\n", argv[0]);
        return 2;
    }
    g_pgid = atoi(argv[1]);
    g_parent_pid = getppid();

    /* Odseparuj: osobna sesja + grupa */
    (void)setsid();
    (void)setpgid(0, 0);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);

    /* Gdy main umrze (nawet SIGKILL) -> PDEATHSIG */
    prctl(PR_SET_PDEATHSIG, SIGTERM);

    /* race-check: jeśli rodzic już nie żyje */
    if (!parent_alive_and_is_main()) {
        g_sig = SIGTERM;
    }

    while (!g_sig) pause();

    /*
     * Po obudzeniu: krótka pauza żeby zombie rodzica mógł zostać zebrany.
     * Bez tego kill(parent_pid, 0) widzi zombie i myśli że żyje.
     */
    poll(NULL, 0, 200);

    /*
     * Normalny shutdown: main żyje i wysłał SIGTERM -> wychodzimy bez sprzątania.
     * Crash main: rodzic nie żyje / nie jest main -> sprzątamy.
     * FORCE: SIGUSR1 -> zawsze sprzątamy.
     */
    if (!g_force && parent_alive_and_is_main()) {
        return 0;
    }

    /* 1) Zabij grupę symulacji */
    if (g_pgid > 1) {
        kill(-g_pgid, SIGTERM);
        poll(NULL, 0, 300);
        kill(-g_pgid, SIGKILL);
        poll(NULL, 0, 300);
        kill(-g_pgid, SIGKILL);
    }

    /* 2) Dodatkowe dobijanie procesów, które nie są w PGID */
    for (int i = 0; i < 5; i++) {
        hard_kill_remaining();
        poll(NULL, 0, 100);
    }

    /* 3) Wyczyść IPC */
    cleanup_ipc_by_keys();

    return 0;
}
