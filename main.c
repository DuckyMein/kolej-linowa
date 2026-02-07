#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>
#include <poll.h>

#include "config.h"
#include "types.h"
#include "ipc.h"
#include "utils.h"

/*
 * KOLEJ KRZESEŁKOWA - PROCES GŁÓWNY (MAIN)
 * 
 * Odpowiedzialności:
 * 1. Parsowanie argumentów
 * 2. Inicjalizacja IPC
 * 3. Uruchomienie procesów stałych (fork + exec)
 * 4. Obsługa sygnałów (SIGINT, SIGTERM, SIGUSR1, SIGUSR2)
 * 5. Monitorowanie czasu symulacji
 * 6. Procedura końca dnia
 * 7. Cleanup i generowanie raportu
 */

/* ============================================
 * ZMIENNE GLOBALNE
 * ============================================ */
static volatile sig_atomic_t g_zamykanie = 0;      /* flaga zamykania */
static volatile sig_atomic_t g_awaria = 0;         /* flaga awarii (STOP) */
static int g_N = N_LIMIT_TERENU;                   /* limit osób */
static int g_czas_symulacji = CZAS_SYMULACJI;      /* czas symulacji */
static int g_ipc_zainicjalizowane = 0;             /* czy IPC zostało utworzone */
static int g_cleanup_wykonany = 0;                 /* czy cleanup już był */

static volatile sig_atomic_t g_child_event = 0;    /* flaga: jest SIGCHLD do obsłużenia */
static pid_t g_pgid = -1;                          /* PGID grupy symulacji */
static pid_t g_pid_sprzatacz = -1;                 /* PID strażnika czyszczącego IPC */
static int g_owner_fd = -1;                        /* fd pliku blokady właściciela */
static int g_raport_wygenerowany = 0;              /* czy raport już zapisany */

/* ============================================
 * DEKLARACJE FUNKCJI
 * ============================================ */
static void instaluj_handlery_sygnalow(void);
static void handler_sigint(int sig);
static void handler_sigterm(int sig);
static void handler_sigusr1(int sig);
static void handler_sigusr2(int sig);
static void handler_sigchld(int sig);

static void start_sprzatacz(void);
static int pid_jest_procesem_stalym(pid_t pid);
static void reap_children_and_check(void);
static void panic_shutdown(const char *powod, pid_t pid, int kod, int przez_sygnal);

static int uruchom_procesy_stale(void);
static pid_t fork_exec(const char *program, char *const argv[], const char *log_path);
static void zakoncz_procesy_potomne(void);
static void procedura_konca_dnia(void);
static void generuj_raport_koncowy(void);
static void petla_glowna(void);
static void awaryjny_cleanup(void);
static void owner_lock_setup_and_maybe_cleanup(void);
static void owner_lock_mark_clean(void);
static void przygotuj_pliki_logow(void);

/* ============================================
 * CLEANUP PRZY WYJŚCIU (atexit)
 * ============================================ */
static void awaryjny_cleanup(void) {
    if (g_cleanup_wykonany) return;
    g_cleanup_wykonany = 1;
    
    /* Wypisz ostrzeżenie jeśli to awaryjne zamknięcie */
    if (g_ipc_zainicjalizowane && g_shm != NULL && !g_shm->koniec_dnia) {
        fprintf(stderr, "\n[AWARYJNE ZAMKNIĘCIE] Sprzątanie zasobów IPC...\n");
    }
    
        /* Sprzątacz jest od tego, by posprzątać po śmierci main (PDEATHSIG).
     * Nie zabijamy go tutaj, bo to może uniemożliwić cleanup po crashu. */

    /* Zabij wszystkie procesy potomne */
    if (g_shm != NULL) {
        if (g_shm->pid_kasjer > 0) kill(g_shm->pid_kasjer, SIGKILL);
        if (g_shm->pid_pracownik1 > 0) kill(g_shm->pid_pracownik1, SIGKILL);
        if (g_shm->pid_pracownik2 > 0) kill(g_shm->pid_pracownik2, SIGKILL);
        if (g_shm->pid_wyciag > 0) kill(g_shm->pid_wyciag, SIGKILL);
        for (int i = 0; i < LICZBA_BRAMEK1; i++) {
            if (g_shm->pid_bramki1[i] > 0) kill(g_shm->pid_bramki1[i], SIGKILL);
        }
        if (g_shm->pid_generator > 0) kill(g_shm->pid_generator, SIGKILL);
    }
    
    /* Zbierz zombie (WNOHANG - nie blokuj) */
    poll(NULL, 0, 200);
    while (waitpid(-1, NULL, WNOHANG) > 0);
    
    /* Wyczyść IPC */
    if (g_ipc_zainicjalizowane) {
        cleanup_ipc();
    }
}
/* ============================================
 * OWNER LOCK (twarda gwarancja po zabiciu sprzątacza)
 *
 * Mechanizm:
 * - LOCK_EX na OWNER_LOCK_FILE → nie uruchomisz 2 instancji równolegle
 * - W pliku trzymamy znacznik DIRTY=1/0
 * - Jeśli poprzednia instancja padła (DIRTY=1), na starcie sprzątamy IPC po kluczach
 * ============================================ */
static int read_dirty_flag(int fd) {
    char buf[128];
    lseek(fd, 0, SEEK_SET);
    ssize_t n = read(fd, buf, sizeof(buf)-1);
    if (n <= 0) return 0;
    buf[n] = '\0';
    return (strstr(buf, "DIRTY=1") != NULL);
}

static void owner_lock_write_state(int dirty) {
    if (g_owner_fd < 0) return;
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
                       "DIRTY=%d\nPID_MAIN=%d\nPGID=%d\nPID_SPRZATACZ=%d\n",
                       dirty, (int)getpid(), (int)g_pgid, (int)g_pid_sprzatacz);
    ftruncate(g_owner_fd, 0);
    lseek(g_owner_fd, 0, SEEK_SET);
    (void)write(g_owner_fd, buf, (size_t)len);
    fsync(g_owner_fd);
}

static void owner_lock_setup_and_maybe_cleanup(void) {
    /* FTOK_FILE musi istnieć, bo klucze IPC od tego zależą */
    int fd_ftok = open(FTOK_FILE, O_CREAT | O_RDWR, 0644);
    if (fd_ftok != -1) close(fd_ftok);

    g_owner_fd = open(OWNER_LOCK_FILE, O_CREAT | O_RDWR, 0644);
    if (g_owner_fd == -1) {
        blad_krytyczny("open OWNER_LOCK_FILE");
    }

    /* lock całego pliku */
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    if (fcntl(g_owner_fd, F_SETLK, &fl) == -1) {
        if (errno == EACCES || errno == EAGAIN) {
            fprintf(stderr, "BŁĄD: Druga instancja symulacji już działa (lock: %s).\n", OWNER_LOCK_FILE);
            exit(EXIT_FAILURE);
        }
        blad_krytyczny("fcntl lock OWNER_LOCK_FILE");
    }

    /* Jeśli poprzedni run padł, posprzątaj IPC zanim utworzysz nowe */
    if (read_dirty_flag(g_owner_fd)) {
        fprintf(stderr, "[START] Wykryto nieczyste zakończenie (DIRTY=1) - czyszczę IPC po kluczach...\n");
        cleanup_ipc_by_keys();
    }

    /* Zaznacz że jesteśmy w trakcie działania */
    owner_lock_write_state(1);
}

static void owner_lock_mark_clean(void) {
    owner_lock_write_state(0);
    if (g_owner_fd >= 0) {
        close(g_owner_fd);
        g_owner_fd = -1;
    }
}



/* ============================================
 * MAIN
 * ============================================ */
int main(int argc, char *argv[]) {
    printf("==============================================\n");
    printf("   SYMULACJA KOLEI KRZESEŁKOWEJ\n");
    printf("==============================================\n\n");
    
    /* 1. Zarejestruj cleanup przy wyjściu (nawet przy crash'u) */
    atexit(awaryjny_cleanup);
    
    /* 2. Walidacja argumentów */
    if (waliduj_argumenty(argc, argv, &g_N, &g_czas_symulacji) != 0) {
        return EXIT_FAILURE;
    }
    
    loguj("Start symulacji: N=%d, czas=%d sekund", g_N, g_czas_symulacji);
    
    /* 3. Inicjalizacja losowania */
    inicjalizuj_losowanie();
    
    /* 3b. Nowa grupa procesów: umożliwia killpg() całej symulacji */
    if (setpgid(0, 0) == -1 && errno != EPERM) {
        blad_ostrzezenie("setpgid");
    }
    g_pgid = getpgrp();
    
    /* 4. Instalacja handlerów sygnałów */
    instaluj_handlery_sygnalow();

    /* 4a. Owner lock + ewentualny cleanup po crashu */
    owner_lock_setup_and_maybe_cleanup();
    
    /* 5. Inicjalizacja IPC */
    loguj("Inicjalizacja zasobów IPC...");
    if (init_ipc(g_N) != 0) {
        fprintf(stderr, "BŁĄD: Nie udało się zainicjalizować IPC!\n");
        return EXIT_FAILURE;
    }
    g_ipc_zainicjalizowane = 1;
    
    /* 5a. Ustaw czas końca dnia (karnet ucięty do tego czasu) */
    g_shm->czas_konca_dnia = g_shm->czas_startu + g_czas_symulacji;
    g_shm->faza_dnia = FAZA_OPEN;
    g_shm->aktywni_klienci = 0;
    loguj("Czas końca dnia: %ld (za %d sekund)", 
          (long)g_shm->czas_konca_dnia, g_czas_symulacji);

    /* 5aa. Przygotuj pliki logów live (podział na terminale) */
    przygotuj_pliki_logow();
    
    /* 5b. Uruchom strażnika: posprząta IPC nawet po SIGKILL main */
    start_sprzatacz();

    /* 6. Uruchomienie procesów stałych */
    loguj("Uruchamianie procesów stałych...");
    if (uruchom_procesy_stale() != 0) {
        fprintf(stderr, "BŁĄD: Nie udało się uruchomić procesów!\n");
        cleanup_ipc();
        return EXIT_FAILURE;
    }
    
    /* 7. Główna pętla symulacji */
    loguj("=== SYMULACJA ROZPOCZĘTA ===");
    petla_glowna();
    
    /* 8. Procedura końca dnia */
    loguj("=== KONIEC DNIA - ZAMYKANIE ===");
    procedura_konca_dnia();
    
    /* 9. Generowanie raportu */
    generuj_raport_koncowy();
    
    /* 10. Cleanup (oznacz że wykonany normalnie) */
    loguj("Czyszczenie zasobów...");
    g_cleanup_wykonany = 1;
    cleanup_ipc();
    owner_lock_mark_clean();
    g_ipc_zainicjalizowane = 0;
    
    printf("\n==============================================\n");
    printf("   SYMULACJA ZAKOŃCZONA POMYŚLNIE\n");
    printf("==============================================\n");
    
    return EXIT_SUCCESS;
}

/* ============================================
 * OBSŁUGA SYGNAŁÓW
 * ============================================ */

static void instaluj_handlery_sygnalow(void) {
    struct sigaction sa;
    
    /* SIGINT (Ctrl+C) - zamknij symulację */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        blad_krytyczny("sigaction SIGINT");
    }
    
    /* SIGTERM - zamknij symulację */
    sa.sa_handler = handler_sigterm;
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        blad_krytyczny("sigaction SIGTERM");
    }
    
    /* SIGUSR1 - awaria (STOP) */
    sa.sa_handler = handler_sigusr1;
    if (sigaction(SIGUSR1, &sa, NULL) == -1) {
        blad_krytyczny("sigaction SIGUSR1");
    }
    
    /* SIGUSR2 - koniec awarii (START) */
    sa.sa_handler = handler_sigusr2;
    if (sigaction(SIGUSR2, &sa, NULL) == -1) {
        blad_krytyczny("sigaction SIGUSR2");
    }
    
    /* SIGCHLD - proces potomny zakończony */
    sa.sa_handler = handler_sigchld;
    sa.sa_flags = SA_NOCLDSTOP; /* ignoruj STOP/CONT */
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        blad_krytyczny("sigaction SIGCHLD");
    }
    
    loguj("Handlery sygnałów zainstalowane");
}

static void handler_sigint(int sig) {
    (void)sig;
    g_zamykanie = 1;
    /* Bezpiecznie zapisz do shm */
    if (g_shm != NULL) {
        g_shm->koniec_dnia = 1;
        g_shm->faza_dnia = FAZA_CLOSING;
    }
}

static void handler_sigterm(int sig) {
    (void)sig;
    g_zamykanie = 1;
    if (g_shm != NULL) {
        g_shm->koniec_dnia = 1;
        g_shm->faza_dnia = FAZA_CLOSING;
    }
}

static void handler_sigusr1(int sig) {
    (void)sig;
    loguj("SYGNAŁ SIGUSR1 - AWARIA (STOP) -> przekazuję do pracownika");

    if (g_shm == NULL) {
        return;
    }

    /* Jeśli awaria już aktywna, nie dubluj */
    if (g_shm->awaria) {
        return;
    }

    /* Wymaganie: zatrzymuje PRACOWNIK (nie main). Domyślnie P1, fallback P2. */
    pid_t target = (g_shm->pid_pracownik1 > 0) ? g_shm->pid_pracownik1 : g_shm->pid_pracownik2;
    if (target > 0) {
        kill(target, SIGUSR1);
    }
}

static void handler_sigusr2(int sig) {
    (void)sig;
    loguj("SYGNAŁ SIGUSR2 - WZNOWIENIE (START) -> przekazuję do inicjatora");

    if (g_shm == NULL) {
        return;
    }

    if (!g_shm->awaria) {
        return;
    }

    /* Wymaganie: wznawia pracownik, który zatrzymał */
    pid_t target = g_shm->pid_awaria_inicjator;
    if (target <= 0) {
        target = (g_shm->pid_pracownik1 > 0) ? g_shm->pid_pracownik1 : g_shm->pid_pracownik2;
    }

    if (target > 0) {
        kill(target, SIGUSR2);
    }
}

static void handler_sigchld(int sig) {
    (void)sig;
    int saved_errno = errno;
    g_child_event = 1; /* obsłużymy w pętli głównej (poza handlerem) */
    errno = saved_errno;
}


/* ============================================
 * PANIC / MONITOROWANIE DZIECI
 * ============================================ */

static void start_sprzatacz(void) {
    char arg_pgid[32];
    snprintf(arg_pgid, sizeof(arg_pgid), "%d", (int)g_pgid);
    char *argv_s[] = {PATH_SPRZATACZ, arg_pgid, NULL};

    g_pid_sprzatacz = fork_exec(PATH_SPRZATACZ, argv_s, "output/sprzatacz.log");
    if (g_pid_sprzatacz == -1) {
        loguj("UWAGA: nie udało się uruchomić sprzątacza IPC (%s)", PATH_SPRZATACZ);
    } else {
        loguj("Sprzątacz IPC uruchomiony (PID=%d, PGID=%d)", g_pid_sprzatacz, (int)g_pgid);
    }
}

static int pid_jest_procesem_stalym(pid_t pid) {
    if (g_shm == NULL || pid <= 0) return 0;

    if (pid == g_shm->pid_kasjer) return 1;
    if (pid == g_shm->pid_generator) return 1;
    if (pid == g_shm->pid_pracownik1) return 1;
    if (pid == g_shm->pid_pracownik2) return 1;
    if (pid == g_shm->pid_wyciag) return 1;
    if (pid == g_pid_sprzatacz) return 1;

    for (int i = 0; i < LICZBA_BRAMEK1; i++) {
        if (pid == g_shm->pid_bramki1[i]) return 1;
    }
    return 0;
}

static void panic_shutdown(const char *powod, pid_t pid, int kod, int przez_sygnal) {
    if (g_zamykanie) return;
    g_zamykanie = 1;

    if (g_shm != NULL) {
        g_shm->panic = 1;
        g_shm->panic_pid = pid;
        g_shm->panic_sig = przez_sygnal ? kod : 0;
        g_shm->faza_dnia = FAZA_CLOSING;
        g_shm->koniec_dnia = 1;
        g_shm->czas_konca_dnia = time(NULL);
        g_shm->kolej_aktywna = 0;
        g_shm->awaria = 0;
    }

    if (przez_sygnal) {
        loguj("=== PANIC SHUTDOWN === %s (PID=%d, SIG=%d)", powod, (int)pid, kod);
    } else {
        loguj("=== PANIC SHUTDOWN === %s (PID=%d, exit=%d)", powod, (int)pid, kod);
    }

    /* Odblokuj ewentualnych czekających na barierze awarii */
    odblokuj_czekajacych();

    /* Poproś całą grupę o zakończenie. SIGKILL wykona sprzątacz po śmierci main. */
    if (g_pgid > 1) {
        kill(-g_pgid, SIGTERM);
    }

    /* Daj chwilę na zejście procesów i wyczyść IPC */
    poll(NULL, 0, 200);

    if (!g_cleanup_wykonany && g_ipc_zainicjalizowane) {
        g_cleanup_wykonany = 1;
        cleanup_ipc();
        g_ipc_zainicjalizowane = 0;
    }

    _exit(EXIT_FAILURE);
}

static void reap_children_and_check(void) {
    if (!g_child_event) return;
    g_child_event = 0;

    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        /* Jeśli trwa normalne zamykanie, ignoruj zakończenia */
        if (g_shm != NULL && (g_shm->koniec_dnia || g_shm->faza_dnia != FAZA_OPEN || g_zamykanie)) {
            continue;
        }

        if (pid_jest_procesem_stalym(pid)) {
            if (WIFSIGNALED(status)) {
                panic_shutdown("Proces stały zakończony sygnałem", pid, WTERMSIG(status), 1);
            } else if (WIFEXITED(status)) {
                panic_shutdown("Proces stały zakończony", pid, WEXITSTATUS(status), 0);
            } else {
                panic_shutdown("Proces stały zakończony (inne)", pid, status, 0);
            }
        }
    }
}

/* ============================================
 * URUCHAMIANIE PROCESÓW
 * ============================================ */

static void przygotuj_pliki_logow(void) {
    /* Upewnij się że katalog output istnieje (dla logów live) */
    if (mkdir("output", 0755) == -1 && errno != EEXIST) {
        blad_ostrzezenie("mkdir output");
        return;
    }

    /* Wyzeruj pliki na start (czytelny demo-output) */
    const char *pliki[] = {
        "output/generator.log",
        "output/kasa.log",
        "output/bramki.log",
        "output/wyciag.log",
        "output/klienci.log",
        "output/pracownicy.log",
        "output/sprzatacz.log",
        NULL
    };

    for (int i = 0; pliki[i] != NULL; i++) {
        int fd = open(pliki[i], O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
    }
}

static pid_t fork_exec(const char *program, char *const argv[], const char *log_path) {
    pid_t pid = fork();
    
    if (pid == -1) {
        blad_ostrzezenie("fork");
        return -1;
    }
    
    if (pid == 0) {
        /* Proces potomny */

        if (log_path != NULL && log_path[0] != '\0') {
            int fd = open(log_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
            if (fd >= 0) {
                (void)dup2(fd, STDOUT_FILENO);
                (void)dup2(fd, STDERR_FILENO);
                if (fd > STDERR_FILENO) close(fd);
            }
        }

        execv(program, argv);
        /* Jeśli exec się nie udał */
        perror("execv");
        _exit(EXIT_FAILURE);
    }
    
    /* Proces rodzica */
    return pid;
}

static int uruchom_procesy_stale(void) {
    char arg_klucz[32];
    snprintf(arg_klucz, sizeof(arg_klucz), "%d", g_N);
    
    /* Kasjer */
    char *argv_kasjer[] = {PATH_KASJER, NULL};
    g_shm->pid_kasjer = fork_exec(PATH_KASJER, argv_kasjer, "output/kasa.log");
    if (g_shm->pid_kasjer == -1) {
        loguj("BŁĄD: Nie udało się uruchomić kasjera");
        /* Kontynuuj bez kasjera dla testów */
    } else {
        loguj("Kasjer uruchomiony (PID=%d)", g_shm->pid_kasjer);
    }
    
    /* Pracownik1 */
    char *argv_p1[] = {PATH_PRACOWNIK1, NULL};
    g_shm->pid_pracownik1 = fork_exec(PATH_PRACOWNIK1, argv_p1, "output/pracownicy.log");
    if (g_shm->pid_pracownik1 == -1) {
        loguj("BŁĄD: Nie udało się uruchomić pracownika1");
    } else {
        loguj("Pracownik1 uruchomiony (PID=%d)", g_shm->pid_pracownik1);
    }
    
    /* Pracownik2 */
    char *argv_p2[] = {PATH_PRACOWNIK2, NULL};
    g_shm->pid_pracownik2 = fork_exec(PATH_PRACOWNIK2, argv_p2, "output/pracownicy.log");
    if (g_shm->pid_pracownik2 == -1) {
        loguj("BŁĄD: Nie udało się uruchomić pracownika2");
    } else {
        loguj("Pracownik2 uruchomiony (PID=%d)", g_shm->pid_pracownik2);
    }
    
    /* Wyciąg */
    char *argv_wyciag[] = {PATH_WYCIAG, NULL};
    g_shm->pid_wyciag = fork_exec(PATH_WYCIAG, argv_wyciag, "output/wyciag.log");
    if (g_shm->pid_wyciag == -1) {
        loguj("BŁĄD: Nie udało się uruchomić wyciągu");
    } else {
        loguj("Wyciąg uruchomiony (PID=%d)", g_shm->pid_wyciag);
    }
    
    /* Bramki (4 sztuki) */
    for (int i = 0; i < LICZBA_BRAMEK1; i++) {
        char arg_numer[8];
        snprintf(arg_numer, sizeof(arg_numer), "%d", i + 1);
        char *argv_bramka[] = {PATH_BRAMKA, arg_numer, NULL};
        
        g_shm->pid_bramki1[i] = fork_exec(PATH_BRAMKA, argv_bramka, "output/bramki.log");
        if (g_shm->pid_bramki1[i] == -1) {
            loguj("BŁĄD: Nie udało się uruchomić bramki %d", i + 1);
        } else {
            loguj("Bramka %d uruchomiona (PID=%d)", i + 1, g_shm->pid_bramki1[i]);
        }
    }
    
    /* Generator klientów */
    char arg_czas[16];
    snprintf(arg_czas, sizeof(arg_czas), "%d", g_czas_symulacji);
    char *argv_gen[] = {PATH_GENERATOR, arg_czas, NULL};
    
    g_shm->pid_generator = fork_exec(PATH_GENERATOR, argv_gen, "output/generator.log");
    if (g_shm->pid_generator == -1) {
        loguj("BŁĄD: Nie udało się uruchomić generatora");
    } else {
        loguj("Generator uruchomiony (PID=%d)", g_shm->pid_generator);
    }
    
    loguj("Wszystkie procesy stałe uruchomione");
    return 0;
}

/* ============================================
 * GŁÓWNA PĘTLA
 * ============================================ */

static void petla_glowna(void) {
    time_t czas_startu = g_shm->czas_startu;
    int ostatni_raport = 0;
    
    while (!g_zamykanie) {
        /* Sprawdź czy czas symulacji minął — PRZED reapem, żeby uniknąć fałszywego panic */
        int czas_uplynal = (int)czas_symulacji(czas_startu);
        
        if (czas_uplynal >= g_czas_symulacji) {
            loguj("Czas symulacji (%d sek) upłynął", g_czas_symulacji);
            g_shm->koniec_dnia = 1;
            g_shm->faza_dnia = FAZA_CLOSING;
            break;
        }

        /* Monitoruj dzieci — po sprawdzeniu czasu */
        reap_children_and_check();
        if (g_shm != NULL && g_shm->panic && !g_shm->koniec_dnia && g_shm->faza_dnia == FAZA_OPEN) {
            panic_shutdown("PANIC (zgłoszone przez proces potomny)", g_shm->panic_pid, g_shm->panic_sig, 1);
        }
        
        /* Raport co 30 sekund */
        if (czas_uplynal - ostatni_raport >= 30) {
            ostatni_raport = czas_uplynal;
            loguj("Status: czas=%d/%d, teren=%d, góra=%d, klienci=%d, przychód=%.2f zł",
                  czas_uplynal, g_czas_symulacji,
                  g_shm->osoby_na_terenie,
                  g_shm->osoby_na_gorze,
                  g_shm->stats.laczna_liczba_klientow,
                  g_shm->stats.przychod_gr / 100.0);
        }
        
        /* Krótkie czekanie (poll zamiast busy-wait) */
        poll(NULL, 0, 100);  /* 100ms */
    }
}

/* ============================================
 * PROCEDURA KOŃCA DNIA
 * ============================================ */

static void procedura_konca_dnia(void) {
    loguj("=== PROCEDURA KOŃCA DNIA ===");
    
    /* ==========================================
     * FAZA 1: CLOSING (zamykanie)
     * - Nie wpuszczamy nowych
     * - Generator przestaje generować
     * - Kasjer odmawia
     * - Karnety "umierają" o czas_konca_dnia
     * ========================================== */
    loguj("FAZA 1: CLOSING - nie wpuszczamy nowych klientów");
    
    MUTEX_SHM_LOCK();
    g_shm->faza_dnia = FAZA_CLOSING;
    g_shm->koniec_dnia = 1;  /* LEGACY - dla kompatybilności */
    
    /* Ustaw czas_konca_dnia jeśli jeszcze nie ustawiony */
    if (g_shm->czas_konca_dnia == 0) {
        g_shm->czas_konca_dnia = time(NULL);
    }
    MUTEX_SHM_UNLOCK();
    
    loguj("  faza_dnia = CLOSING");
    loguj("  czas_konca_dnia = %ld", (long)g_shm->czas_konca_dnia);
    loguj("  aktywni_klienci = %d", g_shm->aktywni_klienci);

    /* Jeśli kończymy dzień podczas awarii, część procesów (klienci/bramki)
     * może wisieć na SEM_BARIERA_AWARIA. Na koniec dnia chcemy je wypuścić,
     * żeby mogły dokończyć cleanup i wyjść. */
    g_awaria = 0;
    MUTEX_SHM_LOCK();
    g_shm->awaria = 0;
    MUTEX_SHM_UNLOCK();
    odblokuj_czekajacych();
    
    /* NIE zabijaj generatora! Generator sam:
     * 1. Przestanie generować (bo faza_dnia != FAZA_OPEN)
     * 2. Poczeka na wszystkie swoje dzieci (klientów)
     * 3. Zakończy się sam */
    
    /* NIE czyścimy kolejki kasy manualnie - kasjer odmawia w CLOSING 
     * z msg_send (blokujące), więc odpowiedzi zawsze dotrą */
    
    /* ==========================================
     * FAZA 2: DRAINING (drenowanie)
     * Zgodnie z wymaganiami:
     * - po osiągnięciu Tk bramki przestają akceptować karnety
     * - wszystkie osoby, które weszły na peron, mają zostać przetransportowane na górę
     * - następnie po 3 sekundach kolej ma zostać wyłączona
     *
     * Realizacja:
     * - ustawiamy FAZA_DRAINING
     * - wyciąg sam dokończy przewóz (kolejka requestów + krzesełka) i po opróżnieniu
     *   odczeka 3s i zakończy się.
     * - tutaj czekamy na zakończenie procesu wyciągu.
     * ========================================== */
    loguj("FAZA 2: DRAINING - kończymy transport z peronu (czekam na wyciąg)");
    
    MUTEX_SHM_LOCK();
    g_shm->faza_dnia = FAZA_DRAINING;
    MUTEX_SHM_UNLOCK();
    
    /* Czekaj na zakończenie WYCIĄGU (z timeout).
     * Wyciąg kończy się dopiero gdy przewiezie wszystkich z peronu + odczeka 3s. */
    if (g_shm->pid_wyciag > 0) {
        loguj("  Czekam na wyciąg (PID %d)...", g_shm->pid_wyciag);
        int timeout_ms = 60000; /* 60s na drenowanie + 3s (duży zapas) */
        pid_t ret = 0;
        int status;
        while (timeout_ms > 0) {
            ret = waitpid(g_shm->pid_wyciag, &status, WNOHANG);
            if (ret > 0 || (ret == -1 && errno != EINTR)) break;
            poll(NULL, 0, 100);
            timeout_ms -= 100;
        }
        if (ret > 0) {
            loguj("  Wyciąg zakończył drenowanie i wyłączył się");
        } else {
            loguj("  Wyciąg nie zakończył się w czasie - wymuszam");
            kill(g_shm->pid_wyciag, SIGKILL);
            waitpid(g_shm->pid_wyciag, NULL, WNOHANG);
        }
        g_shm->pid_wyciag = 0;
    }
    
    /* ==========================================
     * FAZA 3: SHUTDOWN
     * - Zatrzymaj kolej
     * - Zakończ procesy stałe
     * ========================================== */
    loguj("FAZA 3: SHUTDOWN - zamykanie procesów");
    
    g_shm->kolej_aktywna = 0;
    loguj("  Kolej zatrzymana");
    
    zakoncz_procesy_potomne();
    
    loguj("=== PROCEDURA KOŃCA DNIA ZAKOŃCZONA ===");
}

static void zakoncz_procesy_potomne(void) {
    /* Wyślij SIGTERM do CAŁEJ grupy (łapie też ewentualnych orphan klientów) */
    if (g_pgid > 1) {
        kill(-g_pgid, SIGTERM);
    }

    /* Sprzątacz: zakończ go (bez cleanup, bo main żyje) aby nie blokował waitpid */
    if (g_pid_sprzatacz > 0) {
        kill(g_pid_sprzatacz, SIGTERM);
    }

    /* Wyślij SIGTERM do wszystkich procesów stałych */
    if (g_shm->pid_kasjer > 0) {
        kill(g_shm->pid_kasjer, SIGTERM);
    }
    if (g_shm->pid_pracownik1 > 0) {
        kill(g_shm->pid_pracownik1, SIGTERM);
    }
    if (g_shm->pid_pracownik2 > 0) {
        kill(g_shm->pid_pracownik2, SIGTERM);
    }
    if (g_shm->pid_wyciag > 0) {
        kill(g_shm->pid_wyciag, SIGTERM);
    }
    for (int i = 0; i < LICZBA_BRAMEK1; i++) {
        if (g_shm->pid_bramki1[i] > 0) {
            kill(g_shm->pid_bramki1[i], SIGTERM);
        }
    }
    if (g_shm->pid_generator > 0) {
        kill(g_shm->pid_generator, SIGTERM);
    }
    
    /* Czekaj na zakończenie dzieci z timeout */
    loguj("Oczekiwanie na zakończenie procesów potomnych...");
    
    int wait_ms = 8000;  /* 8 sekund max */
    while (wait_ms > 0) {
        pid_t pid = waitpid(-1, NULL, WNOHANG);
        if (pid == -1 && errno == ECHILD) break;  /* brak dzieci */
        if (pid <= 0) {
            poll(NULL, 0, 100);
            wait_ms -= 100;
        }
    }
    
    /* Jeśli ktoś jeszcze żyje - wymuś zamknięcie przez sprzątacza
 * (zapewnia cleanup IPC nawet jeśli main utknął). */
if (wait_ms <= 0) {
    loguj("Timeout: procesy potomne nie zakończyły się - wymuszam shutdown");
    /* raport best-effort zanim ubijemy wszystko */
    generuj_raport_koncowy();
    fflush(NULL);

    if (g_pid_sprzatacz > 0) {
        /* SIGUSR1 = FORCE: zabij grupę i wyczyść IPC nawet gdy main żyje */
        kill(g_pid_sprzatacz, SIGUSR1);
    } else {
        /* brak sprzątacza → best-effort: sprzątnij IPC po kluczach */
        cleanup_ipc_by_keys();
    }
    _exit(EXIT_FAILURE);
}

/* Grzecznie zakończ sprzątacza (bez czyszczenia IPC, bo main żyje) */
if (g_pid_sprzatacz > 0) {
    kill(g_pid_sprzatacz, SIGTERM);
    waitpid(g_pid_sprzatacz, NULL, 0);
    g_pid_sprzatacz = -1;
}

loguj("Wszystkie procesy potomne zakończone");
}


/* ============================================
 * RAPORT KOŃCOWY
 * ============================================ */

static void generuj_raport_koncowy(void) {
    if (g_raport_wygenerowany) return;
    g_raport_wygenerowany = 1;
    loguj("Generowanie raportu końcowego...");
    
    /* Otwórz plik raportu */
    FILE *f = fopen(PLIK_RAPORT, "w");
    if (f == NULL) {
        blad_ostrzezenie("fopen raport");
        /* Wypisz na stdout */
        f = stdout;
    }
    
    fprintf(f, "========================================\n");
    fprintf(f, "    RAPORT DZIENNY - KOLEJ KRZESEŁKOWA\n");
    fprintf(f, "========================================\n\n");
    
    /* Czas */
    char czas_buf[20];
    formatuj_czas(g_shm->czas_startu, czas_buf);
    fprintf(f, "Czas rozpoczęcia:    %s\n", czas_buf);
    formatuj_czas(time(NULL), czas_buf);
    fprintf(f, "Czas zakończenia:    %s\n", czas_buf);
    fprintf(f, "Czas trwania:        %ld sekund\n\n", 
            time(NULL) - g_shm->czas_startu);
    
    /* Statystyki klientów */
    fprintf(f, "--- KLIENCI ---\n");
    fprintf(f, "Łączna liczba klientów: %d\n", g_shm->stats.laczna_liczba_klientow);
    fprintf(f, "  - Piesi:              %d\n", g_shm->stats.liczba_pieszych);
    fprintf(f, "  - Rowerzyści:         %d\n", g_shm->stats.liczba_rowerzystow);
    fprintf(f, "  - VIP:                %d\n", g_shm->stats.liczba_vip);
    fprintf(f, "  - Grupy rodzinne:     %d\n", g_shm->stats.liczba_grup_rodzinnych);
    fprintf(f, "  - Dzieci odrzucone:   %d (bez opiekuna)\n\n", 
            g_shm->stats.liczba_dzieci_odrzuconych);
    
    /* Statystyki karnetów */
    fprintf(f, "--- KARNETY ---\n");
    fprintf(f, "Jednorazowe:     %d\n", g_shm->stats.sprzedane_karnety[0]);
    fprintf(f, "TK1 (30min):     %d\n", g_shm->stats.sprzedane_karnety[1]);
    fprintf(f, "TK2 (60min):     %d\n", g_shm->stats.sprzedane_karnety[2]);
    fprintf(f, "TK3 (120min):    %d\n", g_shm->stats.sprzedane_karnety[3]);
    fprintf(f, "Dzienne:         %d\n\n", g_shm->stats.sprzedane_karnety[4]);
    
    /* Przychód */
    char kwota_buf[20];
    formatuj_kwote(g_shm->stats.przychod_gr, kwota_buf);
    fprintf(f, "--- PRZYCHÓD ---\n");
    fprintf(f, "Łączny przychód: %s\n\n", kwota_buf);
    
    /* Trasy */
    fprintf(f, "--- TRASY ---\n");
    fprintf(f, "T1 (rower łatwa):    %d\n", g_shm->stats.uzycia_tras[0]);
    fprintf(f, "T2 (rower średnia):  %d\n", g_shm->stats.uzycia_tras[1]);
    fprintf(f, "T3 (rower trudna):   %d\n", g_shm->stats.uzycia_tras[2]);
    fprintf(f, "T4 (piesza):         %d\n\n", g_shm->stats.uzycia_tras[3]);
    
    /* Liczba przejazdów i awarii */
    fprintf(f, "--- OPERACJE ---\n");
    fprintf(f, "Liczba przejazdów:   %d\n", g_shm->stats.liczba_przejazdow);
    fprintf(f, "Liczba zatrzymań:    %d\n\n", g_shm->stats.liczba_zatrzyman);
    
    fprintf(f, "========================================\n");
    fprintf(f, "         KONIEC RAPORTU\n");
    fprintf(f, "========================================\n");
    
    if (f != stdout) {
        fclose(f);
        loguj("Raport zapisany do: %s", PLIK_RAPORT);
    }
    
    /* Zapisz logi przejść */
    FILE *flog = fopen(PLIK_LOG, "w");
    if (flog != NULL) {
        fprintf(flog, "ID_KARNETU;TYP_BRAMKI;NR_BRAMKI;CZAS\n");
        for (int i = 0; i < g_shm->liczba_logow; i++) {
            LogEntry *log = &g_shm->logi[i];
            const char *typ_str;
            switch (log->typ_bramki) {
                case LOG_BRAMKA1: typ_str = "BRAMKA1"; break;
                case LOG_BRAMKA2: typ_str = "BRAMKA2"; break;
                case LOG_WYJSCIE_GORA: typ_str = "WYJSCIE_GORA"; break;
                default: typ_str = "NIEZNANY"; break;
            }
            formatuj_czas(log->czas, czas_buf);
            fprintf(flog, "%d;%s;%d;%s\n",
                    log->id_karnetu, typ_str, log->numer_bramki, czas_buf);
        }
        fclose(flog);
        loguj("Log przejść zapisany do: %s (%d wpisów)", 
              PLIK_LOG, g_shm->liczba_logow);
    }
}
