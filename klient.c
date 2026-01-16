#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>

#include "config.h"
#include "types.h"
#include "ipc.h"
#include "utils.h"

/*
 * KOLEJ KRZESEŁKOWA - PROCES KLIENTA
 * 
 * Cykl życia:
 * 1. Podejście do kasy -> kupno karnetu
 * 2. Pętla (dopóki karnet ważny):
 *    a. Bramka1 -> wejście na teren
 *    b. Bramka2 -> wejście na peron
 *    c. Wsiadanie do krzesełka
 *    d. Wjazd na górę
 *    e. Wyjście ze stacji górnej
 *    f. Trasa powrotna (T1-T4)
 * 3. Zakończenie
 */

static volatile sig_atomic_t g_koniec = 0;
static Klient g_klient;

/* Stan klienta - gdzie się znajduje */
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

static void handler_sigterm(int sig) {
    (void)sig;
    g_koniec = 1;
}

/* Sprawdza czy można kontynuować */
static int czy_kontynuowac(void) {
    if (g_koniec) return 0;
    if (g_shm == NULL) return 0;
    if (g_shm->koniec_dnia && g_stan == STAN_PRZED_BRAMKA1) return 0;
    return 1;
}

/* Symulacja czasu (zamiast sleep, sprawdzamy sygnały) */
static void symuluj_czas(int sekundy) {
    for (int i = 0; i < sekundy * 10 && !g_koniec; i++) {
        usleep(100000); /* 100ms */
        if (g_shm == NULL || g_shm->koniec_dnia) break;
    }
}

/* Bezpieczne zakończenie - zwalnia zasoby w zależności od stanu */
static void bezpieczne_zakonczenie(void) {
    if (g_shm == NULL) return;
    
    /* Zwolnij zasoby w zależności od stanu */
    switch (g_stan) {
        case STAN_NA_TERENIE:
            /* Zwolnij miejsce na terenie */
            sem_signal_n(SEM_TEREN, g_klient.rozmiar_grupy);
            MUTEX_SHM_LOCK();
            g_shm->osoby_na_terenie -= g_klient.rozmiar_grupy;
            MUTEX_SHM_UNLOCK();
            break;
            
        case STAN_NA_PERONIE:
            /* Na peronie - zwolnij miejsce na terenie już zrobione przy wsiadaniu */
            break;
            
        case STAN_W_KRZESLE:
        case STAN_NA_GORZE:
            /* Na górze - zmniejsz licznik */
            MUTEX_SHM_LOCK();
            g_shm->osoby_na_gorze -= g_klient.rozmiar_grupy;
            MUTEX_SHM_UNLOCK();
            break;
            
        case STAN_NA_TRASIE:
            /* Na trasie - licznik już zaktualizowany */
            break;
            
        default:
            break;
    }
}

int main(int argc, char *argv[]) {
    /* Parsowanie argumentów:
     * argv[1] = id
     * argv[2] = wiek
     * argv[3] = typ (0=pieszy, 1=rowerzysta)
     * argv[4] = vip
     * argv[5] = liczba_dzieci
     * argv[6] = wiek_dziecko1
     * argv[7] = wiek_dziecko2
     */
    
    if (argc < 6) {
        fprintf(stderr, "KLIENT: Za mało argumentów\n");
        return EXIT_FAILURE;
    }
    
    /* Ustaw aby zginąć gdy rodzic (generator) umrze */
    ustaw_smierc_z_rodzicem();
    
    g_klient.id = atoi(argv[1]);
    g_klient.wiek = atoi(argv[2]);
    g_klient.typ = atoi(argv[3]);
    g_klient.vip = atoi(argv[4]);
    g_klient.liczba_dzieci = atoi(argv[5]);
    g_klient.pid = getpid();
    
    if (argc >= 7) g_klient.wiek_dzieci[0] = atoi(argv[6]);
    if (argc >= 8) g_klient.wiek_dzieci[1] = atoi(argv[7]);
    
    /* Oblicz rozmiar grupy */
    g_klient.rozmiar_grupy = oblicz_miejsca_krzeselko(g_klient.typ, g_klient.liczba_dzieci);
    
    /* Inicjalizacja */
    inicjalizuj_losowanie();
    
    /* Obsługa sygnałów */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_sigterm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    
    /* Dołącz do IPC */
    if (attach_ipc() != 0) {
        return EXIT_FAILURE;
    }
    
    loguj("KLIENT %d: Start (wiek=%d, %s, VIP=%d, dzieci=%d, grupa=%d)",
          g_klient.id, g_klient.wiek,
          g_klient.typ == TYP_ROWERZYSTA ? "rowerzysta" : "pieszy",
          g_klient.vip, g_klient.liczba_dzieci, g_klient.rozmiar_grupy);
    
    /* ========================================
     * KROK 1: KASA
     * ======================================== */
    g_stan = STAN_KASA;
    
    if (!czy_kontynuowac()) {
        loguj("KLIENT %d: Zamknięcie przed kasą", g_klient.id);
        detach_ipc();
        return EXIT_SUCCESS;
    }
    
    loguj("KLIENT %d: Podchodzę do kasy", g_klient.id);
    
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
        loguj("KLIENT %d: Błąd wysyłania do kasy", g_klient.id);
        detach_ipc();
        return EXIT_SUCCESS;
    }
    
    /* Czekaj na odpowiedź (z timeoutem) */
    MsgKasaOdp odp_kasa;
    int timeout = 100; /* 10 sekund */
    int otrzymano = 0;
    
    while (timeout > 0 && !g_koniec && !otrzymano) {
        int ret = msg_recv_nowait(g_mq_kasa_odp, &odp_kasa, sizeof(odp_kasa), g_klient.pid);
        if (ret > 0) {
            otrzymano = 1;
        } else {
            usleep(100000);
            timeout--;
        }
    }
    
    if (!otrzymano) {
        loguj("KLIENT %d: Timeout w kasie - kończę", g_klient.id);
        detach_ipc();
        return EXIT_SUCCESS;
    }
    
    if (!odp_kasa.sukces) {
        loguj("KLIENT %d: Odmowa w kasie - kończę", g_klient.id);
        detach_ipc();
        return EXIT_SUCCESS;
    }
    
    g_klient.id_karnetu = odp_kasa.id_karnetu;
    g_klient.id_karnety_dzieci[0] = odp_kasa.id_karnety_dzieci[0];
    g_klient.id_karnety_dzieci[1] = odp_kasa.id_karnety_dzieci[1];
    
    loguj("KLIENT %d: Kupiłem karnet %d (%s)", 
          g_klient.id, g_klient.id_karnetu, nazwa_karnetu(odp_kasa.typ_karnetu));
    
    /* ========================================
     * PĘTLA GŁÓWNA (dopóki karnet ważny)
     * ======================================== */
    int przejazdy = 0;
    
    while (czy_kontynuowac()) {
        przejazdy++;
        loguj("KLIENT %d: Przejazd #%d", g_klient.id, przejazdy);
        
        /* ========================================
         * KROK 2: BRAMKA1 (wejście na teren)
         * ======================================== */
        g_stan = STAN_PRZED_BRAMKA1;
        
        if (!czy_kontynuowac()) {
            loguj("KLIENT %d: Zamknięcie przed bramką", g_klient.id);
            break;
        }
        
        loguj("KLIENT %d: Podchodzę do Bramki1", g_klient.id);
        
        /* Sprawdź ważność karnetu przed podejściem */
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
        
        if (msg_send(g_mq_bramka, &msg_bramka, sizeof(msg_bramka)) < 0) {
            loguj("KLIENT %d: Błąd wysyłania do bramki", g_klient.id);
            break;
        }
        
        /* Czekaj na wpuszczenie (z timeoutem) */
        MsgBramkaOdp odp_bramka;
        timeout = 300; /* 30 sekund */
        otrzymano = 0;
        
        while (timeout > 0 && czy_kontynuowac() && !otrzymano) {
            int ret = msg_recv_nowait(g_mq_kasa_odp, &odp_bramka, sizeof(odp_bramka), g_klient.pid);
            if (ret > 0) {
                otrzymano = 1;
            } else {
                usleep(100000);
                timeout--;
            }
        }
        
        if (!otrzymano || !odp_bramka.sukces) {
            loguj("KLIENT %d: Nie wpuszczono na teren - kończę", g_klient.id);
            break;
        }
        
        g_stan = STAN_NA_TERENIE;
        loguj("KLIENT %d: Wszedłem na teren stacji", g_klient.id);
        
        /* ========================================
         * KROK 3: BRAMKA2 -> PERON
         * ======================================== */
        loguj("KLIENT %d: Przechodzę przez Bramkę2 na peron", g_klient.id);
        
        /* Symulacja przejścia przez teren */
        symuluj_czas(1);
        
        if (!czy_kontynuowac() && g_stan == STAN_NA_TERENIE) {
            /* Musimy dokończyć przejazd */
            loguj("KLIENT %d: Zamknięcie - dokańczam przejazd", g_klient.id);
        }
        
        /* Zaloguj przejście przez Bramkę2 */
        int nr_bramki2 = losuj_zakres(1, LICZBA_BRAMEK2);
        dodaj_log(g_klient.id_karnetu, LOG_BRAMKA2, nr_bramki2);
        
        /* ========================================
         * KROK 4: WSIADANIE DO KRZESEŁKA
         * ======================================== */
        g_stan = STAN_NA_PERONIE;
        
        /* Podczas awarii - czekaj na semaforze (nie busy-wait!) */
        if (g_shm->awaria && !g_koniec && !g_shm->koniec_dnia) {
            char buf[32];
            snprintf(buf, sizeof(buf), "KLIENT %d (peron)", g_klient.id);
            czekaj_na_wznowienie(buf);
        }
        
        if (g_koniec || g_shm->koniec_dnia) {
            loguj("KLIENT %d: Zamknięcie na peronie - muszę dokończyć", g_klient.id);
            /* Kontynuuj - musi zjechać */
        }
        
        loguj("KLIENT %d: Czekam na miejsce w krzesełku (%d miejsc)", 
              g_klient.id, g_klient.rozmiar_grupy);
        
        /* Czekaj na miejsca (semafor SEM_PERON) */
        sem_wait_n(SEM_PERON, g_klient.rozmiar_grupy);
        
        /* Zwolnij miejsce na terenie */
        sem_signal_n(SEM_TEREN, g_klient.rozmiar_grupy);
        
        /* Aktualizuj liczniki */
        MUTEX_SHM_LOCK();
        g_shm->osoby_na_terenie -= g_klient.rozmiar_grupy;
        g_shm->osoby_na_peronie += g_klient.rozmiar_grupy;
        MUTEX_SHM_UNLOCK();
        
        g_stan = STAN_W_KRZESLE;
        loguj("KLIENT %d: Wsiadam do krzesełka", g_klient.id);
        
        /* ========================================
         * KROK 5: WJAZD NA GÓRĘ
         * ======================================== */
        
        /* Podczas awarii - czekaj w krzesełku (kolej stoi!) */
        if (g_shm->awaria && !g_koniec) {
            char buf[32];
            snprintf(buf, sizeof(buf), "KLIENT %d (krzesło)", g_klient.id);
            czekaj_na_wznowienie(buf);
        }
        
        loguj("KLIENT %d: Jadę na górę...", g_klient.id);
        
        /* Aktualizuj liczniki */
        MUTEX_SHM_LOCK();
        g_shm->osoby_na_peronie -= g_klient.rozmiar_grupy;
        g_shm->osoby_na_gorze += g_klient.rozmiar_grupy;
        g_shm->stats.liczba_przejazdow++;
        MUTEX_SHM_UNLOCK();
        
        /* Symulacja wjazdu */
        symuluj_czas(CZAS_WJAZDU / 10 + 1); /* Skrócony czas dla testów */
        
        /* ========================================
         * KROK 6: WYJŚCIE ZE STACJI GÓRNEJ
         * ======================================== */
        g_stan = STAN_NA_GORZE;
        
        int nr_wyjscia = losuj_zakres(1, LICZBA_WYJSC_GORA);
        loguj("KLIENT %d: Wysiadam, wyjście %d", g_klient.id, nr_wyjscia);
        
        dodaj_log(g_klient.id_karnetu, LOG_WYJSCIE_GORA, nr_wyjscia);
        
        /* ========================================
         * KROK 7: TRASA POWROTNA
         * ======================================== */
        g_stan = STAN_NA_TRASIE;
        
        Trasa trasa;
        if (g_klient.typ == TYP_ROWERZYSTA) {
            trasa = losuj_trase_rower();
        } else {
            trasa = TRASA_T4;
        }
        
        int czas_trasy = pobierz_czas_trasy(trasa);
        loguj("KLIENT %d: Zjeżdżam trasą %s (%d sek)", 
              g_klient.id, nazwa_trasy(trasa), czas_trasy);
        
        /* Symulacja zjazdu (skrócony czas dla testów) */
        symuluj_czas(czas_trasy / 10 + 1);
        
        /* Aktualizuj statystyki */
        MUTEX_SHM_LOCK();
        g_shm->osoby_na_gorze -= g_klient.rozmiar_grupy;
        g_shm->stats.uzycia_tras[trasa]++;
        MUTEX_SHM_UNLOCK();
        
        g_stan = STAN_PRZED_BRAMKA1; /* Wrócił na dół */
        loguj("KLIENT %d: Zjechałem na dół", g_klient.id);
        
        /* ========================================
         * SPRAWDZENIE KARNETU
         * ======================================== */
        karnet = pobierz_karnet(g_klient.id_karnetu);
        if (karnet == NULL || !czy_karnet_wazny(karnet, time(NULL))) {
            loguj("KLIENT %d: Karnet wygasł po %d przejazdach - kończę", 
                  g_klient.id, przejazdy);
            break;
        }
        
        /* Karnet jednorazowy - tylko jeden przejazd */
        if (karnet->typ == KARNET_JEDNORAZOWY) {
            loguj("KLIENT %d: Karnet jednorazowy wykorzystany - kończę", g_klient.id);
            break;
        }
        
        /* Sprawdź czy koniec dnia */
        if (g_shm->koniec_dnia) {
            loguj("KLIENT %d: Koniec dnia - kończę po %d przejazdach", g_klient.id, przejazdy);
            break;
        }
        
        /* Krótka przerwa przed kolejnym przejazdem */
        symuluj_czas(1);
    }
    
    /* Bezpieczne zakończenie */
    bezpieczne_zakonczenie();
    
    loguj("KLIENT %d: Kończę dzień (przejazdy=%d)", g_klient.id, przejazdy);
    detach_ipc();
    
    return EXIT_SUCCESS;
}
