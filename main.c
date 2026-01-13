#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>

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

/* ============================================
 * MAIN
 * ============================================ */
int main(int argc, char *argv[]) {
    printf("==============================================\n");
    printf("   SYMULACJA KOLEI KRZESEŁKOWEJ\n");
    printf("==============================================\n\n");
    
    /* 1. Walidacja argumentów */
    if (waliduj_argumenty(argc, argv, &g_N, &g_czas_symulacji) != 0) {
        return EXIT_FAILURE;
    }
    
    loguj("Start symulacji: N=%d, czas=%d sekund", g_N, g_czas_symulacji);
    
    /* 2. Inicjalizacja losowania */
    inicjalizuj_losowanie();
    
    /* 3. Instalacja handlerów sygnałów */
    instaluj_handlery_sygnalow();
    
    /* 4. Inicjalizacja IPC */
    loguj("Inicjalizacja zasobów IPC...");
    if (init_ipc(g_N) != 0) {
        fprintf(stderr, "BŁĄD: Nie udało się zainicjalizować IPC!\n");
        return EXIT_FAILURE;
    }
    
    /* 5. Uruchomienie procesów stałych */
    loguj("Uruchamianie procesów stałych...");
    if (uruchom_procesy_stale() != 0) {
        fprintf(stderr, "BŁĄD: Nie udało się uruchomić procesów!\n");
        cleanup_ipc();
        return EXIT_FAILURE;
    }
    
    /* 6. Główna pętla symulacji */
    loguj("=== SYMULACJA ROZPOCZĘTA ===");
    petla_glowna();
    
    /* 7. Procedura końca dnia */
    loguj("=== KONIEC DNIA - ZAMYKANIE ===");
    procedura_konca_dnia();
    
    /* 8. Generowanie raportu */
    generuj_raport_koncowy();
    
    /* 9. Cleanup */
    loguj("Czyszczenie zasobów...");
    cleanup_ipc();
    
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
    
    /* Czekaj na gotowość pracowników */
    loguj("Oczekiwanie na gotowość pracowników...");
    
    /* Wysyłamy sygnał START */
    MsgPracownicy msg;
    msg.mtype = 1;
    msg.typ_komunikatu = MSG_TYP_START;
    msg.nadawca = getpid();
    msg_send_nowait(g_mq_prac, &msg, sizeof(msg));
    
    msg.mtype = 2;
    msg_send_nowait(g_mq_prac, &msg, sizeof(msg));
    
    g_awaria = 0;
    if (g_shm != NULL) {
        g_shm->awaria = 0;
        g_shm->kolej_aktywna = 1;
    }
    
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
        
        /* Krótkie czekanie (nie busy-wait) */
        usleep(100000);  /* 100ms */
    }
}

/* ============================================
 * PROCEDURA KOŃCA DNIA
 * ============================================ */

static void procedura_konca_dnia(void) {
    loguj("Rozpoczęcie procedury końca dnia...");
    
    /* Krok 1: Oznacz koniec dnia */
    g_shm->koniec_dnia = 1;
    loguj("Krok 1: Flaga koniec_dnia ustawiona");
    
    /* Krok 2: Zatrzymaj generator (nie przyjmuj nowych klientów) */
    if (g_shm->pid_generator > 0) {
        kill(g_shm->pid_generator, SIGTERM);
        loguj("Krok 2: Generator zatrzymany");
    }
    
    /* Krok 3: Poczekaj na opróżnienie terenu (max 30 sekund) */
    loguj("Krok 3: Oczekiwanie na opróżnienie terenu...");
    int timeout = 30;
    while (g_shm->osoby_na_terenie > 0 && timeout > 0) {
        loguj("  Pozostało osób na terenie: %d", g_shm->osoby_na_terenie);
        sleep(1);
        timeout--;
    }
    
    if (g_shm->osoby_na_terenie > 0) {
        loguj("UWAGA: Timeout - na terenie wciąż %d osób", g_shm->osoby_na_terenie);
    }
    
    /* Krok 4: Poczekaj 3 sekundy (zgodnie ze specyfikacją) */
    loguj("Krok 4: Oczekiwanie 3 sekundy...");
    sleep(3);
    
    /* Krok 5: Zatrzymaj kolej */
    g_shm->kolej_aktywna = 0;
    loguj("Krok 5: Kolej zatrzymana");
    
    /* Krok 6: Poczekaj na zejście wszystkich z góry (max 60 sekund) */
    loguj("Krok 6: Oczekiwanie na zejście osób z góry...");
    timeout = 60;
    while (g_shm->osoby_na_gorze > 0 && timeout > 0) {
        loguj("  Pozostało osób na górze: %d", g_shm->osoby_na_gorze);
        sleep(1);
        timeout--;
    }
    
    if (g_shm->osoby_na_gorze > 0) {
        loguj("UWAGA: Timeout - na górze wciąż %d osób", g_shm->osoby_na_gorze);
    }
    
    /* Krok 7: Zakończ wszystkie procesy potomne */
    loguj("Krok 7: Zamykanie procesów potomnych...");
    zakoncz_procesy_potomne();
    
    loguj("Procedura końca dnia zakończona");
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
    
    /* Czekaj na zakończenie wszystkich dzieci */
    loguj("Oczekiwanie na zakończenie procesów potomnych...");
    int status;
    pid_t pid;
    int timeout = 10;
    
    while (timeout > 0) {
        pid = waitpid(-1, &status, WNOHANG);
        if (pid == -1) {
            if (errno == ECHILD) {
                /* Brak więcej dzieci */
                break;
            }
        } else if (pid == 0) {
            /* Dzieci wciąż działają */
            sleep(1);
            timeout--;
        }
    }
    
    /* Jeśli timeout - wymuś zakończenie */
    if (timeout == 0) {
        loguj("Wymuszanie zakończenia (SIGKILL)...");
        if (g_shm->pid_kasjer > 0) kill(g_shm->pid_kasjer, SIGKILL);
        if (g_shm->pid_pracownik1 > 0) kill(g_shm->pid_pracownik1, SIGKILL);
        if (g_shm->pid_pracownik2 > 0) kill(g_shm->pid_pracownik2, SIGKILL);
        for (int i = 0; i < LICZBA_BRAMEK1; i++) {
            if (g_shm->pid_bramki1[i] > 0) kill(g_shm->pid_bramki1[i], SIGKILL);
        }
        if (g_shm->pid_generator > 0) kill(g_shm->pid_generator, SIGKILL);
        
        /* Zbierz wszystkie zombie */
        while (waitpid(-1, &status, WNOHANG) > 0);
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
