#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
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

/* ============================================
 * DEKLARACJE FUNKCJI
 * ============================================ */
static void instaluj_handlery_sygnalow(void);
static void handler_sigint(int sig);
static void handler_sigterm(int sig);
static void handler_sigusr1(int sig);
static void handler_sigusr2(int sig);
static void handler_sigchld(int sig);

static int uruchom_procesy_stale(void);
static pid_t fork_exec(const char *program, char *const argv[]);
static void zakoncz_procesy_potomne(void);
static void procedura_konca_dnia(void);
static void generuj_raport_koncowy(void);
static void petla_glowna(void);
static void awaryjny_cleanup(void);

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
    
    /* Zabij wszystkie procesy potomne */
    if (g_shm != NULL) {
        if (g_shm->pid_kasjer > 0) kill(g_shm->pid_kasjer, SIGKILL);
        if (g_shm->pid_pracownik1 > 0) kill(g_shm->pid_pracownik1, SIGKILL);
        if (g_shm->pid_pracownik2 > 0) kill(g_shm->pid_pracownik2, SIGKILL);
        for (int i = 0; i < LICZBA_BRAMEK1; i++) {
            if (g_shm->pid_bramki1[i] > 0) kill(g_shm->pid_bramki1[i], SIGKILL);
        }
        if (g_shm->pid_generator > 0) kill(g_shm->pid_generator, SIGKILL);
    }
    
    /* Czekaj aż WSZYSTKIE procesy potomne się zamkną */
    while (waitpid(-1, NULL, 0) > 0);
    
    /* Wyczyść IPC */
    if (g_ipc_zainicjalizowane) {
        cleanup_ipc();
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
    
    /* 4. Instalacja handlerów sygnałów */
    instaluj_handlery_sygnalow();
    
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
    }
}

static void handler_sigterm(int sig) {
    (void)sig;
    g_zamykanie = 1;
    if (g_shm != NULL) {
        g_shm->koniec_dnia = 1;
    }
}

static void handler_sigusr1(int sig) {
    (void)sig;
    g_awaria = 1;
    loguj("!!! SYGNAŁ SIGUSR1 - AWARIA (STOP) !!!");
    if (g_shm != NULL) {
        g_shm->awaria = 1;
        g_shm->kolej_aktywna = 0;
        g_shm->stats.liczba_zatrzyman++;
    }
    
    /* Rozgłoś STOP do pracowników */
    MsgPracownicy msg;
    msg.mtype = 1;  /* do Pracownika1 */
    msg.typ_komunikatu = MSG_TYP_STOP;
    msg.nadawca = getpid();
    msg_send_nowait(g_mq_prac, &msg, sizeof(msg));
    
    msg.mtype = 2;  /* do Pracownika2 */
    msg_send_nowait(g_mq_prac, &msg, sizeof(msg));
}

static void handler_sigusr2(int sig) {
    (void)sig;
    loguj("SYGNAŁ SIGUSR2 - WZNOWIENIE (START)");
    
    if (!g_awaria) {
        loguj("Ignoruję SIGUSR2 - nie było awarii");
        return;
    }
    
    /* Najpierw ustaw flagi */
    g_awaria = 0;
    if (g_shm != NULL) {
        g_shm->awaria = 0;
        g_shm->kolej_aktywna = 1;
    }
    
    /* Odblokuj wszystkich czekających na semaforze */
    odblokuj_czekajacych();
    
    /* Wysyłamy sygnał START do pracowników */
    MsgPracownicy msg;
    msg.mtype = 1;
    msg.typ_komunikatu = MSG_TYP_START;
    msg.nadawca = getpid();
    msg_send_nowait(g_mq_prac, &msg, sizeof(msg));
    
    msg.mtype = 2;
    msg_send_nowait(g_mq_prac, &msg, sizeof(msg));
    
    loguj("Kolej wznowiona");
}

static void handler_sigchld(int sig) {
    (void)sig;
    int saved_errno = errno;
    
    /* Zbierz wszystkie zakończone dzieci (unikaj zombie) */
    pid_t pid;
    int status;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        /* Opcjonalnie: loguj zakończenie procesu */
        /* Nie logujemy tutaj bo to handler sygnału */
    }
    
    errno = saved_errno;
}

/* ============================================
 * URUCHAMIANIE PROCESÓW
 * ============================================ */

static pid_t fork_exec(const char *program, char *const argv[]) {
    pid_t pid = fork();
    
    if (pid == -1) {
        blad_ostrzezenie("fork");
        return -1;
    }
    
    if (pid == 0) {
        /* Proces potomny */
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
    g_shm->pid_kasjer = fork_exec(PATH_KASJER, argv_kasjer);
    if (g_shm->pid_kasjer == -1) {
        loguj("BŁĄD: Nie udało się uruchomić kasjera");
        /* Kontynuuj bez kasjera dla testów */
    } else {
        loguj("Kasjer uruchomiony (PID=%d)", g_shm->pid_kasjer);
    }
    
    /* Pracownik1 */
    char *argv_p1[] = {PATH_PRACOWNIK1, NULL};
    g_shm->pid_pracownik1 = fork_exec(PATH_PRACOWNIK1, argv_p1);
    if (g_shm->pid_pracownik1 == -1) {
        loguj("BŁĄD: Nie udało się uruchomić pracownika1");
    } else {
        loguj("Pracownik1 uruchomiony (PID=%d)", g_shm->pid_pracownik1);
    }
    
    /* Pracownik2 */
    char *argv_p2[] = {PATH_PRACOWNIK2, NULL};
    g_shm->pid_pracownik2 = fork_exec(PATH_PRACOWNIK2, argv_p2);
    if (g_shm->pid_pracownik2 == -1) {
        loguj("BŁĄD: Nie udało się uruchomić pracownika2");
    } else {
        loguj("Pracownik2 uruchomiony (PID=%d)", g_shm->pid_pracownik2);
    }
    
    /* Bramki (4 sztuki) */
    for (int i = 0; i < LICZBA_BRAMEK1; i++) {
        char arg_numer[8];
        snprintf(arg_numer, sizeof(arg_numer), "%d", i + 1);
        char *argv_bramka[] = {PATH_BRAMKA, arg_numer, NULL};
        
        g_shm->pid_bramki1[i] = fork_exec(PATH_BRAMKA, argv_bramka);
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
    
    g_shm->pid_generator = fork_exec(PATH_GENERATOR, argv_gen);
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
        /* Sprawdź czy czas symulacji minął */
        int czas_uplynal = (int)czas_symulacji(czas_startu);
        
        if (czas_uplynal >= g_czas_symulacji) {
            loguj("Czas symulacji (%d sek) upłynął", g_czas_symulacji);
            break;
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
    
    /* NIE zabijaj generatora! Generator sam:
     * 1. Przestanie generować (bo faza_dnia != FAZA_OPEN)
     * 2. Poczeka na wszystkie swoje dzieci (klientów)
     * 3. Zakończy się sam */
    
    /* Wyczyść kolejkę kasy - odpowiedz odmową czekającym */
    loguj("  Czyszczenie kolejki kasy...");
    MsgKasa msg_kasa;
    while (msg_recv_nowait(g_mq_kasa, &msg_kasa, sizeof(msg_kasa), 0) > 0) {
        MsgKasaOdp odp = {0};
        odp.mtype = msg_kasa.pid_klienta;
        odp.sukces = 0;
        msg_send_nowait(g_mq_kasa_odp, &odp, sizeof(odp));
    }
    
    /* ==========================================
     * FAZA 2: DRAINING (drenowanie)
     * - Generator przestał generować i czeka na swoje dzieci
     * - Czekamy aż generator się zakończy (= wszyscy klienci skończyli)
     * ========================================== */
    loguj("FAZA 2: DRAINING - czekamy na zakończenie klientów");
    
    MUTEX_SHM_LOCK();
    g_shm->faza_dnia = FAZA_DRAINING;
    MUTEX_SHM_UNLOCK();
    
    /* Czekaj na zakończenie GENERATORA
     * Generator czeka BLOKUJĄCO na wszystkie swoje dzieci (klienci),
     * więc gdy generator się zakończy, wszyscy klienci już wyszli */
    if (g_shm->pid_generator > 0) {
        loguj("  Czekam na generator (PID %d) i jego klientów...", g_shm->pid_generator);
        int status;
        pid_t ret;
        while ((ret = waitpid(g_shm->pid_generator, &status, 0)) == -1) {
            if (errno == EINTR) continue; /* Przerwane sygnałem - kontynuuj czekanie */
            break; /* Inny błąd */
        }
        if (ret > 0) {
            loguj("  Generator zakończył pracę");
        }
        g_shm->pid_generator = 0;
    }
    
    loguj("Wszyscy klienci opuścili stację");
    
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
    for (int i = 0; i < LICZBA_BRAMEK1; i++) {
        if (g_shm->pid_bramki1[i] > 0) {
            kill(g_shm->pid_bramki1[i], SIGTERM);
        }
    }
    if (g_shm->pid_generator > 0) {
        kill(g_shm->pid_generator, SIGTERM);
    }
    
    /* Czekaj na zakończenie WSZYSTKICH dzieci (blokujące, bez timeout) */
    loguj("Oczekiwanie na zakończenie procesów potomnych...");
    
    while (1) {
        pid_t pid = waitpid(-1, NULL, 0);  /* BLOKUJĄCE */
        if (pid == -1) {
            if (errno == ECHILD) {
                /* Brak więcej dzieci - WSZYSTKIE zamknięte */
                break;
            }
            /* Inny błąd - przerwane sygnałem, kontynuuj */
            if (errno == EINTR) continue;
            break;
        }
    }
    
    loguj("Wszystkie procesy potomne zakończone");
}

/* ============================================
 * RAPORT KOŃCOWY
 * ============================================ */

static void generuj_raport_koncowy(void) {
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
