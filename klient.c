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
 * KOLEJ KRZESEŁKOWA - PROCES KLIENTA (v3.0 UPROSZCZONY)
 * 
 * ZASADY:
 * - NIE używa PDEATHSIG od generatora (żeby generator nie zabijał klientów)
 * - Sprawdza faza_dnia tylko w 3 miejscach:
 *   1. Przed kasą
 *   2. Przed bramką1 (nowy cykl)
 *   3. Po powrocie na dół (przed kolejnym przejazdem)
 * - Reszta opiera się na ważności karnetu (karnet ucięty do końca dnia)
 * - Rejestruje się w aktywni_klienci (do drenowania)
 */

static volatile sig_atomic_t g_koniec = 0;
static Klient g_klient;

typedef enum {
    STAN_KASA,
    STAN_PRZED_BRAMKA1,
    STAN_NA_TERENIE,
    STAN_NA_PERONIE,
    STAN_W_KRZESLE,
    STAN_NA_GORZE,
    STAN_NA_TRASIE
} StanKlienta;

static volatile StanKlienta g_stan = STAN_KASA;
static volatile sig_atomic_t g_wpuszczony_na_teren = 0;

static void handler_sigterm(int sig) {
    (void)sig;
    g_koniec = 1;
}

/* Symulacja czasu */
static void symuluj_czas_ms(int ms) {
    if (!g_koniec && ms > 0) {
        poll(NULL, 0, ms);
    }
}

/* Bezpieczne zakończenie - zwalnia zasoby i dekrementuje aktywni_klienci */
static void bezpieczne_zakonczenie(void) {
    static int juz_wywolane = 0;
    if (juz_wywolane) return;
    juz_wywolane = 1;
    
    if (g_shm == NULL) return;
    
    /* Zwolnij zasoby w zależności od stanu */
    if (g_wpuszczony_na_teren && g_stan != STAN_NA_PERONIE && 
        g_stan != STAN_W_KRZESLE && g_stan != STAN_NA_GORZE && g_stan != STAN_NA_TRASIE) {
        sem_signal_n(SEM_TEREN, g_klient.rozmiar_grupy);
        MUTEX_SHM_LOCK();
        g_shm->osoby_na_terenie -= g_klient.rozmiar_grupy;
        MUTEX_SHM_UNLOCK();
    } else {
        switch (g_stan) {
            case STAN_NA_TERENIE:
                sem_signal_n(SEM_TEREN, g_klient.rozmiar_grupy);
                MUTEX_SHM_LOCK();
                g_shm->osoby_na_terenie -= g_klient.rozmiar_grupy;
                MUTEX_SHM_UNLOCK();
                break;
                
            case STAN_NA_PERONIE:
                if (g_wpuszczony_na_teren) {
                    sem_signal_n(SEM_TEREN, g_klient.rozmiar_grupy);
                    MUTEX_SHM_LOCK();
                    g_shm->osoby_na_terenie -= g_klient.rozmiar_grupy;
                    MUTEX_SHM_UNLOCK();
                }
                break;
                
            case STAN_W_KRZESLE:
                MUTEX_SHM_LOCK();
                g_shm->osoby_na_peronie -= g_klient.rozmiar_grupy;
                MUTEX_SHM_UNLOCK();
                break;
                
            case STAN_NA_GORZE:
            case STAN_NA_TRASIE:
                MUTEX_SHM_LOCK();
                g_shm->osoby_na_gorze -= g_klient.rozmiar_grupy;
                MUTEX_SHM_UNLOCK();
                break;
                
            default:
                break;
        }
    }
    
    /* WAŻNE: Dekrementuj licznik aktywnych klientów */
    MUTEX_SHM_LOCK();
    g_shm->aktywni_klienci--;
    MUTEX_SHM_UNLOCK();
}

int main(int argc, char *argv[]) {
    if (argc < 6) {
        fprintf(stderr, "KLIENT: Za mało argumentów\n");
        return EXIT_FAILURE;
    }
    
    /* NIE używamy ustaw_smierc_z_rodzicem() - generator nie ma zabijać klientów! */
    
    g_klient.id = atoi(argv[1]);
    g_klient.wiek = atoi(argv[2]);
    g_klient.typ = atoi(argv[3]);
    g_klient.vip = atoi(argv[4]);
    g_klient.liczba_dzieci = atoi(argv[5]);
    g_klient.pid = getpid();
    
    if (argc >= 7) g_klient.wiek_dzieci[0] = atoi(argv[6]);
    if (argc >= 8) g_klient.wiek_dzieci[1] = atoi(argv[7]);
    
    g_klient.rozmiar_grupy = oblicz_miejsca_krzeselko(g_klient.typ, g_klient.liczba_dzieci);
    
    inicjalizuj_losowanie();
    
    /* Obsługa sygnałów - sigaction BEZ SA_RESTART */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_sigterm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    
    if (attach_ipc() != 0) {
        return EXIT_FAILURE;
    }
    
    /* WAŻNE: Zarejestruj się jako aktywny klient */
    MUTEX_SHM_LOCK();
    g_shm->aktywni_klienci++;
    MUTEX_SHM_UNLOCK();
    
    atexit(bezpieczne_zakonczenie);
    
    loguj("KLIENT %d: Start (wiek=%d, %s, VIP=%d, dzieci=%d)",
          g_klient.id, g_klient.wiek,
          g_klient.typ == TYP_ROWERZYSTA ? "rowerzysta" : "pieszy",
          g_klient.vip, g_klient.liczba_dzieci);
    
    /* ========================================
     * KROK 1: KASA
     * ======================================== */
    g_stan = STAN_KASA;
    
    /* CHECK #1: Przed kasą - czy stacja przyjmuje nowych? */
    if (g_shm->faza_dnia != FAZA_OPEN) {
        loguj("KLIENT %d: Stacja zamknięta - kończę", g_klient.id);
        detach_ipc();
        return EXIT_SUCCESS;
    }
    
    MsgKasa msg_kasa;
    msg_kasa.mtype = g_klient.vip ? MSG_TYP_VIP : MSG_TYP_NORMALNY;
    msg_kasa.pid_klienta = g_klient.pid;
    msg_kasa.id_klienta = g_klient.id;
    msg_kasa.wiek = g_klient.wiek;
    msg_kasa.typ = g_klient.typ;
    msg_kasa.vip = g_klient.vip;
    msg_kasa.liczba_dzieci = g_klient.liczba_dzieci;
    msg_kasa.wiek_dzieci[0] = g_klient.wiek_dzieci[0];
    msg_kasa.wiek_dzieci[1] = g_klient.wiek_dzieci[1];
    
    if (msg_send(g_mq_kasa, &msg_kasa, sizeof(msg_kasa)) < 0) {
        detach_ipc();
        return EXIT_SUCCESS;
    }
    
    /* Czekaj na odpowiedź BLOKUJĄCO (kasjer ZAWSZE odpowiada) */
    MsgKasaOdp odp_kasa;
    int ret = msg_recv(g_mq_kasa_odp, &odp_kasa, sizeof(odp_kasa), g_klient.pid);
    
    if (ret < 0 || !odp_kasa.sukces) {
        loguj("KLIENT %d: Odmowa/błąd w kasie - kończę", g_klient.id);
        detach_ipc();
        return EXIT_SUCCESS;
    }
    
    g_klient.id_karnetu = odp_kasa.id_karnetu;
    loguj("KLIENT %d: Kupiłem karnet %d (%s)", 
          g_klient.id, g_klient.id_karnetu, nazwa_karnetu(odp_kasa.typ_karnetu));
    
    /* ========================================
     * PĘTLA GŁÓWNA
     * ======================================== */
    int przejazdy = 0;
    
    while (!g_koniec) {
        przejazdy++;
        
        /* ========================================
         * BRAMKA1 (wejście na teren)
         * ======================================== */
        g_stan = STAN_PRZED_BRAMKA1;
        
        /* CHECK #2: Przed bramką - czy karnet ważny? (karnet ucięty do końca dnia) */
        Karnet *karnet = pobierz_karnet(g_klient.id_karnetu);
        if (karnet == NULL || !czy_karnet_wazny(karnet, time(NULL))) {
            loguj("KLIENT %d: Karnet nieważny - kończę", g_klient.id);
            break;
        }
        
        MsgBramka1 msg_bramka;
        msg_bramka.mtype = g_klient.vip ? MSG_TYP_VIP : MSG_TYP_NORMALNY;
        msg_bramka.pid_klienta = g_klient.pid;
        msg_bramka.id_karnetu = g_klient.id_karnetu;
        msg_bramka.rozmiar_grupy = g_klient.rozmiar_grupy;
        msg_bramka.numer_bramki = losuj_zakres(1, LICZBA_BRAMEK1);
        
        if (msg_send(g_mq_bramka, &msg_bramka, sizeof(msg_bramka)) < 0) break;
        
        /* Czekaj na bramkę BLOKUJĄCO (bramka ZAWSZE odpowiada) */
        MsgBramkaOdp odp_bramka;
        ret = msg_recv(g_mq_kasa_odp, &odp_bramka, sizeof(odp_bramka), g_klient.pid);
        
        if (ret < 0 || !odp_bramka.sukces) {
            loguj("KLIENT %d: Odmowa na bramce - kończę", g_klient.id);
            break;
        }
        
        g_stan = STAN_NA_TERENIE;
        g_wpuszczony_na_teren = 1;
        
        /* ========================================
         * BRAMKA2 -> PERON -> KRZESEŁKO
         * ======================================== */
        symuluj_czas_ms(100);
        
        int nr_bramki2 = losuj_zakres(1, LICZBA_BRAMEK2);
        dodaj_log(g_klient.id_karnetu, LOG_BRAMKA2, nr_bramki2);
        
        g_stan = STAN_NA_PERONIE;
        
        /* Czekaj na awarii jeśli aktywna */
        if (g_shm->awaria && !g_koniec) {
            char buf[32];
            snprintf(buf, sizeof(buf), "KLIENT %d (peron)", g_klient.id);
            czekaj_na_wznowienie(buf);
        }
        
        /* Czekaj na miejsce w krzesełku (semafor) */
        if (sem_wait_n(SEM_PERON, g_klient.rozmiar_grupy) != 0) {
            /* Przerwane - muszę się ewakuować */
            sem_signal_n(SEM_TEREN, g_klient.rozmiar_grupy);
            MUTEX_SHM_LOCK();
            g_shm->osoby_na_terenie -= g_klient.rozmiar_grupy;
            MUTEX_SHM_UNLOCK();
            g_wpuszczony_na_teren = 0;
            break;
        }
        
        /* HIGH PERF: Natychmiast zwolnij SEM_PERON */
        sem_signal_n(SEM_PERON, g_klient.rozmiar_grupy);
        
        /* Zwolnij teren */
        sem_signal_n(SEM_TEREN, g_klient.rozmiar_grupy);
        
        MUTEX_SHM_LOCK();
        g_shm->osoby_na_terenie -= g_klient.rozmiar_grupy;
        g_shm->osoby_na_peronie += g_klient.rozmiar_grupy;
        MUTEX_SHM_UNLOCK();
        
        g_wpuszczony_na_teren = 0;
        g_stan = STAN_W_KRZESLE;
        
        /* ========================================
         * WJAZD NA GÓRĘ
         * ======================================== */
        if (g_shm->awaria && !g_koniec) {
            char buf[32];
            snprintf(buf, sizeof(buf), "KLIENT %d (krzesło)", g_klient.id);
            czekaj_na_wznowienie(buf);
        }
        
        MUTEX_SHM_LOCK();
        g_shm->osoby_na_peronie -= g_klient.rozmiar_grupy;
        g_shm->osoby_na_gorze += g_klient.rozmiar_grupy;
        g_shm->stats.liczba_przejazdow++;
        MUTEX_SHM_UNLOCK();
        
        /* ========================================
         * WYJŚCIE ZE STACJI GÓRNEJ
         * ======================================== */
        g_stan = STAN_NA_GORZE;
        
        int nr_wyjscia = losuj_zakres(1, LICZBA_WYJSC_GORA);
        dodaj_log(g_klient.id_karnetu, LOG_WYJSCIE_GORA, nr_wyjscia);
        
        /* ========================================
         * TRASA POWROTNA
         * ======================================== */
        g_stan = STAN_NA_TRASIE;
        
        Trasa trasa;
        if (g_klient.typ == TYP_ROWERZYSTA) {
            trasa = losuj_trase_rower();
        } else {
            trasa = TRASA_T4;
        }
        
        int czas_trasy = pobierz_czas_trasy(trasa);
        symuluj_czas_ms(czas_trasy * 1000);
        
        MUTEX_SHM_LOCK();
        g_shm->osoby_na_gorze -= g_klient.rozmiar_grupy;
        g_shm->stats.uzycia_tras[trasa]++;
        MUTEX_SHM_UNLOCK();
        
        g_stan = STAN_PRZED_BRAMKA1;
        
        /* ========================================
         * CHECK #3: Czy kontynuować?
         * ======================================== */
        karnet = pobierz_karnet(g_klient.id_karnetu);
        if (karnet == NULL || !czy_karnet_wazny(karnet, time(NULL))) {
            loguj("KLIENT %d: Karnet wygasł po %d przejazdach", g_klient.id, przejazdy);
            break;
        }
        
        if (karnet->typ == KARNET_JEDNORAZOWY) {
            loguj("KLIENT %d: Karnet jednorazowy wykorzystany", g_klient.id);
            break;
        }
        
        symuluj_czas_ms(100);
    }
    
    bezpieczne_zakonczenie();
    loguj("KLIENT %d: Kończę (%d przejazdów)", g_klient.id, przejazdy);
    detach_ipc();
    
    return EXIT_SUCCESS;
}
