#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
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
static int g_waga_peronu = 0;  /* ile slotów peronu zajmujemy */

static const char* nazwa_typu_klienta(int typ) {
    return (typ == TYP_ROWERZYSTA) ? "ROWER" : "PIESZY";
}

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

/*
 * Wersja "odporna" na zapchanie kolejek SysV:
 * - nie blokuje się w msgsnd() (używa IPC_NOWAIT)
 * - stosuje exponential backoff (zmniejsza 100% CPU przy tysiącach procesów)
 * - w CLOSING/DRAINING lub po koniec_dnia wraca, żeby klient mógł szybko wyjść
 */
static int wyslij_z_backoff(int mq_id, void *msg, size_t size) {
    int delay_ms = 1;
    const int max_delay_ms = 200;

    while (!g_koniec) {
        int r = msg_send_nowait(mq_id, msg, size);
        if (r == 0) return 0;
        if (r == -2) return -2; /* IPC usunięte */

        /* Jeżeli dzień się kończy, nie próbuj już "przepchać" wiadomości */
        if (g_shm != NULL) {
            if (g_shm->koniec_dnia || g_shm->faza_dnia != FAZA_OPEN) {
                return -1;
            }
        }

        /* Gdy main umarł (albo IPC zaraz zniknie), wyjdź */
        if (!czy_rodzic_zyje()) {
            return -1;
        }

        /* Jeśli to nie jest typowy "queue full", potraktuj jako błąd */
        if (errno != EAGAIN && errno != ENOSPC && errno != EINTR) {
            return -1;
        }

        poll(NULL, 0, delay_ms);
        if (delay_ms < max_delay_ms) delay_ms *= 2;
    }
    return -1;
}

/* Bezpieczne zakończenie - zwalnia semafory, aktualizuje liczniki i detach */
static void bezpieczne_zakonczenie(void) {
    static int juz_wywolane = 0;
    if (juz_wywolane) return;
    juz_wywolane = 1;
    
    if (g_shm == NULL) return;
    
    /* Zwolnij zasoby w zależności od stanu */
    switch (g_stan) {
        case STAN_NA_TERENIE:
            /* Na terenie - zwolnij SEM_TEREN */
            sem_signal_n(SEM_TEREN, g_klient.rozmiar_grupy);
            MUTEX_SHM_LOCK();
            g_shm->osoby_na_terenie -= g_klient.rozmiar_grupy;
            MUTEX_SHM_UNLOCK();
            break;
            
        case STAN_NA_PERONIE:
            /* Na peronie - zwolnij SEM_PERON i SEM_TEREN */
            if (g_waga_peronu > 0) {
                sem_signal_n_undo(SEM_PERON, g_waga_peronu);
                g_waga_peronu = 0;
            }
            MUTEX_SHM_LOCK();
            g_shm->osoby_na_peronie -= g_klient.rozmiar_grupy;
            MUTEX_SHM_UNLOCK();
            if (g_wpuszczony_na_teren) {
                sem_signal_n(SEM_TEREN, g_klient.rozmiar_grupy);
                MUTEX_SHM_LOCK();
                g_shm->osoby_na_terenie -= g_klient.rozmiar_grupy;
                MUTEX_SHM_UNLOCK();
            }
            break;
            
        case STAN_W_KRZESLE:
            /* W krzesełku: osoby_w_krzesle przenosi WYCIĄG przy ARRIVE.
             * Nie dekrementuj tutaj, bo łatwo o podwójny dekrement (sygnał/EINTR).
             */
            break;
            
        default:
            break;
    }
    
    /* ATOMOWY dekrement - BEZ mutexa (unika thundering herd) */
    __sync_fetch_and_sub(&g_shm->aktywni_klienci, 1);
    
    /* Detach IPC na samym końcu */
    detach_ipc();
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

    loguj("KLIENT %d: start pid=%d wiek=%d typ=%s vip=%d dzieci=%d (%d,%d) rozmiar_grupy=%d",
          g_klient.id, (int)g_klient.pid, g_klient.wiek, nazwa_typu_klienta(g_klient.typ),
          g_klient.vip, g_klient.liczba_dzieci, g_klient.wiek_dzieci[0], g_klient.wiek_dzieci[1],
          g_klient.rozmiar_grupy);
    
    /* WAŻNE: Zarejestruj cleanup PRZED inkrementacją licznika */
    atexit(bezpieczne_zakonczenie);
    
    /* ATOMOWY inkrement - BEZ mutexa (unika thundering herd) */
    __sync_fetch_and_add(&g_shm->aktywni_klienci, 1);
    
    /* ========================================
     * KROK 1: KASA
     * ======================================== */
    g_stan = STAN_KASA;
    
    /* CHECK #1: Przed kasą - czy stacja przyjmuje nowych? */
    if (g_shm->faza_dnia != FAZA_OPEN) {
        return EXIT_SUCCESS;  /* atexit() wywoła bezpieczne_zakonczenie() */
    }

    /* Nie wszyscy przychodzący muszą korzystać z kolei.
     * Symulacja: część osób odchodzi bez kupowania karnetu. */
    if (PROC_NIE_KORZYSTA > 0) {
        int r = rand() % 100;
        if (r < PROC_NIE_KORZYSTA) {
            loguj("KLIENT %d: odchodzi - dziś nie korzysta z kolei (los=%d < %d%%)",
                  g_klient.id, r, PROC_NIE_KORZYSTA);
            return EXIT_SUCCESS;
        }
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
    
    /* Nie blokuj się na zapchanej kolejce - backoff + szybkie wyjście w CLOSING */
    if (wyslij_z_backoff(g_mq_kasa, &msg_kasa, sizeof(msg_kasa)) < 0) {
        return EXIT_SUCCESS;  /* atexit() zrobi cleanup */
    }
    
    /* Czekaj na odpowiedź BLOKUJĄCO (kasjer ZAWSZE odpowiada) */
    MsgKasaOdp odp_kasa;
    int ret = msg_recv(g_mq_kasa_odp, &odp_kasa, sizeof(odp_kasa), g_klient.pid);
    
    if (ret < 0 || !odp_kasa.sukces) {
        return EXIT_SUCCESS;  /* atexit() zrobi cleanup */
    }
    
    g_klient.id_karnetu = odp_kasa.id_karnetu;

    {
        Karnet *k = pobierz_karnet(g_klient.id_karnetu);
        if (k != NULL) {
            loguj("KLIENT %d: kupił karnet id_karnetu=%d typ=%s czas_waznosci=%ds vip=%d",
                  g_klient.id, g_klient.id_karnetu, nazwa_karnetu(k->typ),
                  k->czas_waznosci_sek, k->vip);
        } else {
            loguj("KLIENT %d: kupił karnet id_karnetu=%d",
                  g_klient.id, g_klient.id_karnetu);
        }
    }
    
    /* ========================================
     * PĘTLA GŁÓWNA
     * ======================================== */
    int przejazdy = 0; /* liczba ZAKOŃCZONYCH przejazdów (ARRIVE) */
    
    while (!g_koniec) {
        
        /* ========================================
         * BRAMKA1 (wejście na teren)
         * ======================================== */
        g_stan = STAN_PRZED_BRAMKA1;
        
        /* CHECK #2: Przed bramką - czy karnet ważny? (karnet ucięty do końca dnia) */
        Karnet *karnet = pobierz_karnet(g_klient.id_karnetu);
        if (karnet == NULL || !czy_karnet_wazny(karnet, time(NULL))) {
            break;
        }
        
        MsgBramka1 msg_bramka;
        msg_bramka.pid_klienta = g_klient.pid;
        msg_bramka.id_karnetu = g_klient.id_karnetu;
        msg_bramka.rozmiar_grupy = g_klient.rozmiar_grupy;

        /* VIP wchodzi "bez kolejki" przez dedykowaną bramkę 1 (VIP-only).
         * Zwykli klienci losują bramkę 2..N. */
        int nr_bramki1 = g_klient.vip ? 1 : losuj_zakres(2, LICZBA_BRAMEK1);
        msg_bramka.mtype = nr_bramki1;      /* routing do konkretnej bramki */
        msg_bramka.vip = g_klient.vip;
        msg_bramka.numer_bramki = nr_bramki1;

        loguj("KLIENT %d: id_karnetu=%d -> BRAMKA1 nr=%d (vip=%d, grupa=%d)",
              g_klient.id, g_klient.id_karnetu, nr_bramki1, g_klient.vip, g_klient.rozmiar_grupy);
        
        /* Nie blokuj się na zapchanej kolejce - backoff + szybkie wyjście w CLOSING */
        if (wyslij_z_backoff(g_mq_bramka, &msg_bramka, sizeof(msg_bramka)) < 0) break;
        
        /* Czekaj na bramkę BLOKUJĄCO - osobna kolejka odpowiedzi */
        MsgBramkaOdp odp_bramka;
        ret = msg_recv(g_mq_bramka_odp, &odp_bramka, sizeof(odp_bramka), g_klient.pid);
        
        if (ret < 0 || !odp_bramka.sukces) {
            loguj("KLIENT %d: BRAMKA1 odmówiła (nr=%d) - kończę", g_klient.id, nr_bramki1);
            break;
        }
        
        g_stan = STAN_NA_TERENIE;
        g_wpuszczony_na_teren = 1;

        loguj("KLIENT %d: BRAMKA1 OK (nr=%d) - jestem na terenie", g_klient.id, nr_bramki1);
        
        /* ========================================
         * BRAMKA2 -> PERON -> WYCIĄG
         * ======================================== */
        
        int nr_bramki2 = losuj_zakres(1, LICZBA_BRAMEK2);
        dodaj_log(g_klient.id_karnetu, LOG_BRAMKA2, nr_bramki2);
        
        /* Waga peronu = liczba slotów krzesełka.
         * oblicz_miejsca_krzeselko() już uwzględnia: pieszy=1, rower=2, dziecko=+1.
         */
        g_waga_peronu = g_klient.rozmiar_grupy;
        
        /* Sprawdź czy w ogóle zmieścimy się na krzesełko (max 4 sloty) */
        if (g_waga_peronu > PERON_SLOTY) {
            /* Grupa za duża - nie wejdziemy (np. pieszy + 4 dzieci = 5 > 4) */
            sem_signal_n(SEM_TEREN, g_klient.rozmiar_grupy);
            MUTEX_SHM_LOCK();
            g_shm->osoby_na_terenie -= g_klient.rozmiar_grupy;
            MUTEX_SHM_UNLOCK();
            g_wpuszczony_na_teren = 0;
            g_waga_peronu = 0;
            break;
        }
        
        /* Czekaj na awarii jeśli aktywna */
        if (g_shm->awaria && !g_koniec) {
            char buf[32];
            snprintf(buf, sizeof(buf), "KLIENT %d (przed peronem)", g_klient.id);
            czekaj_na_wznowienie(buf);
        }

        loguj("KLIENT %d: czekam na peron (sloty=%d, bramka2=%d)",
              g_klient.id, g_waga_peronu, nr_bramki2);
        

        /* PROSI_P1_PERON: Pracownik1 kontroluje wejście na peron (bramki2).
         * Jeśli pracownik1 jest wstrzymany (SIGSTOP), nie odpowie i klienci nie przejdą dalej. */
        MsgPeron msg_peron;
        msg_peron.mtype = MSG_TYP_NORMALNY;
        msg_peron.pid_klienta = g_klient.pid;
        msg_peron.id_karnetu = g_klient.id_karnetu;
        msg_peron.miejsca = g_waga_peronu;
        msg_peron.numer_bramki2 = nr_bramki2;

        loguj("KLIENT %d: prosi PRACOWNIK1 o wejście na peron (bramka2=%d sloty=%d)",
              g_klient.id, nr_bramki2, g_waga_peronu);

        if (wyslij_z_backoff(g_mq_peron, &msg_peron, sizeof(msg_peron)) < 0) {
            break;
        }

        /* Czekaj na odpowiedź od pracownika1.
         * Używamy pętli NOWAIT + poll, żeby móc szybko wyjść w CLOSING/PANIC. */
        MsgPeronOdp odp_peron;
        int got_peron = 0;
        while (!g_koniec && !got_peron) {
            int r = msg_recv_nowait(g_mq_peron_odp, &odp_peron, sizeof(odp_peron), (long)g_klient.pid);
            if (r >= 0) {
                if (!odp_peron.sukces) {
                    loguj("KLIENT %d: PRACOWNIK1 odmówił wejścia na peron", g_klient.id);
                    goto koniec_petli;
                }
                got_peron = 1;
                break;
            }

            /* jeśli pracownik1 umarł, nie czekaj bez końca */
            pid_t pid_p1 = 0;
            int faza = FAZA_OPEN;
            int panic = 0;
            MUTEX_SHM_LOCK();
            pid_p1 = g_shm->pid_pracownik1;
            faza = g_shm->faza_dnia;
            panic = g_shm->panic;
            MUTEX_SHM_UNLOCK();

            if (panic || faza != FAZA_OPEN) {
                goto koniec_petli;
            }
            if (pid_p1 > 0 && kill(pid_p1, 0) < 0 && errno == ESRCH) {
                loguj("KLIENT %d: PRACOWNIK1 nie żyje - rezygnuję", g_klient.id);
                goto koniec_petli;
            }

            poll(NULL, 0, 20);
        }

        if (!got_peron) {
            goto koniec_petli;
        }

        loguj("KLIENT %d: PRACOWNIK1 pozwolił wejść na peron", g_klient.id);

        /* Czekaj na miejsce na peronie (semafor slotów) */
        if (sem_wait_n_undo(SEM_PERON, g_waga_peronu) != 0) {
            /* Przerwane - muszę się ewakuować */
            sem_signal_n(SEM_TEREN, g_klient.rozmiar_grupy);
            MUTEX_SHM_LOCK();
            g_shm->osoby_na_terenie -= g_klient.rozmiar_grupy;
            MUTEX_SHM_UNLOCK();
            g_wpuszczony_na_teren = 0;
            g_waga_peronu = 0;
            break;
        }
        
        g_stan = STAN_NA_PERONIE;
        
        /* Zwolnij teren (ale jeszcze trzymamy peron) */
        sem_signal_n(SEM_TEREN, g_klient.rozmiar_grupy);
        MUTEX_SHM_LOCK();
        g_shm->osoby_na_terenie -= g_klient.rozmiar_grupy;
        g_shm->osoby_na_peronie += g_klient.rozmiar_grupy;
        MUTEX_SHM_UNLOCK();
        g_wpuszczony_na_teren = 0;
        
        /* Wyślij request do wyciągu */
        MsgWyciagReq req;
        req.mtype = g_klient.vip ? MSG_TYP_VIP : MSG_TYP_NORMALNY;
        req.pid_klienta = g_klient.pid;
        req.typ_klienta = g_klient.typ;
        req.vip = g_klient.vip;
        req.rozmiar_grupy = g_klient.rozmiar_grupy;
        req.waga_slotow = g_waga_peronu;
        
        if (wyslij_z_backoff(g_mq_wyciag_req, &req, sizeof(req)) != 0) {
            /* Nie udało się wysłać - ewakuacja */
            sem_signal_n_undo(SEM_PERON, g_waga_peronu);
            MUTEX_SHM_LOCK();
            g_shm->osoby_na_peronie -= g_klient.rozmiar_grupy;
            MUTEX_SHM_UNLOCK();
            g_waga_peronu = 0;
            break;
        }
        
        /* Czekaj na BOARD od wyciągu */
        MsgWyciagOdp odp;
        int got_board = 0;
        while (!g_koniec && !got_board) {
            int r = msg_recv_nowait(g_mq_wyciag_odp, &odp, sizeof(odp), (long)g_klient.pid);
            if (r > 0) {
                if (odp.typ == WYCIAG_ODP_BOARD) {
                    got_board = 1;
                } else if (odp.typ == WYCIAG_ODP_KONIEC) {
                    /* Wyciąg kazał wyjść */
                    sem_signal_n_undo(SEM_PERON, g_waga_peronu);
                    MUTEX_SHM_LOCK();
                    g_shm->osoby_na_peronie -= g_klient.rozmiar_grupy;
                    MUTEX_SHM_UNLOCK();
                    g_waga_peronu = 0;
                    g_stan = STAN_KASA;  /* reset stanu */
                    goto koniec_petli;
                }
            } else {
                poll(NULL, 0, 10);
            }
            if (g_shm->koniec_dnia && g_shm->faza_dnia != FAZA_OPEN) {
                poll(NULL, 0, 50);  /* Cierpliwość - wyciąg nas obsłuży */
            }
        }
        
        if (!got_board) {
            /* Przerwane sygnałem */
            sem_signal_n_undo(SEM_PERON, g_waga_peronu);
            MUTEX_SHM_LOCK();
            g_shm->osoby_na_peronie -= g_klient.rozmiar_grupy;
            MUTEX_SHM_UNLOCK();
            g_waga_peronu = 0;
            break;
        }

        loguj("KLIENT %d: BOARD - wsiadam na krzesełko (sloty=%d)",
              g_klient.id, req.waga_slotow);
        
        /* BOARD - od tego momentu jesteśmy "w krzesełku".
         * Ustaw stan PRZED zwolnieniem peronu: jeśli dostaniemy sygnał w środku,
         * SEM_UNDO odda sloty peronu automatycznie przy wyjściu procesu.
         */
        g_stan = STAN_W_KRZESLE;
        sem_signal_n_undo(SEM_PERON, g_waga_peronu);
        MUTEX_SHM_LOCK();
        g_shm->osoby_na_peronie -= g_klient.rozmiar_grupy;
        g_shm->osoby_w_krzesle += g_klient.rozmiar_grupy;
        MUTEX_SHM_UNLOCK();
        g_waga_peronu = 0;
        
        /* Czekaj na ARRIVE od wyciągu */
        int got_arrive = 0;
        while (!g_koniec && !got_arrive) {
            int r = msg_recv_nowait(g_mq_wyciag_odp, &odp, sizeof(odp), (long)g_klient.pid);
            if (r > 0) {
                if (odp.typ == WYCIAG_ODP_ARRIVE) {
                    got_arrive = 1;
                } else if (odp.typ == WYCIAG_ODP_KONIEC) {
                    /* Wyciąg się zatrzymał - ewakuacja.
                     * Nie dotykamy osoby_w_krzesle: wyciąg sam domyka liczniki przy shutdown.
                     */
                    g_stan = STAN_KASA;
                    goto koniec_petli;
                }
            } else {
                poll(NULL, 0, 10);
            }
        }
        
        if (!got_arrive) {
            /* Przerwane sygnałem - nie ruszaj osoby_w_krzesle (może być już przeniesione). */
            break;
        }

        przejazdy++;
        loguj("KLIENT %d: ARRIVE - jestem na górze (przejazd=%d)", g_klient.id, przejazdy);
        
        /* ARRIVE - jesteśmy na górze (wyciąg już zaktualizował liczniki) */
        g_stan = STAN_NA_GORZE;
        
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
        loguj("KLIENT %d: zjazd trasą %s (czas=%ds)", g_klient.id, nazwa_trasy(trasa), czas_trasy);
        symuluj_czas_ms(czas_trasy * 1000);

        loguj("KLIENT %d: wróciłem na dół po trasie %s", g_klient.id, nazwa_trasy(trasa));
        
        MUTEX_SHM_LOCK();
        g_shm->osoby_na_gorze -= g_klient.rozmiar_grupy;
        g_shm->stats.uzycia_tras[trasa]++;
        MUTEX_SHM_UNLOCK();
        
        g_stan = STAN_PRZED_BRAMKA1;
        
        /* CHECK #3: Czy kontynuować? */
        karnet = pobierz_karnet(g_klient.id_karnetu);
        if (karnet == NULL || !czy_karnet_wazny(karnet, time(NULL))) {
            break;
        }
        
        if (karnet->typ == KARNET_JEDNORAZOWY) {
            break;
        }
        
        //symuluj_czas_ms(100);
    }
    
koniec_petli:
    loguj("KLIENT %d: koniec (przejazdy=%d)", g_klient.id, przejazdy);
    return EXIT_SUCCESS;  /* atexit() wywoła bezpieczne_zakonczenie() */
}
