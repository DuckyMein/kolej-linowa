#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <poll.h>
#include <time.h>

#include "config.h"
#include "types.h"
#include "ipc.h"
#include "utils.h"

/*
 * KOLEJ KRZESEŁKOWA - PROCES WYCIĄGU (MODEL RING LICZBA_RZEDOW)
 * 
 * Model fizyczny:
 * - LICZBA_RZEDOW rzędów krzesełek w obiegu (ring buffer)
 * - Pozycja 0 = stacja dolna (załadunek pasażerów)
 * - Pozycja (LICZBA_RZEDOW/2) = stacja górna (wyładunek pasażerów)
 * - Pozycje 1..(POZYCJA_GORNA-1) = jazda w górę (z pasażerami)
 * - Pozycje (POZYCJA_GORNA+1)..(LICZBA_RZEDOW-1) = jazda w dół (puste krzesełka wracają)
 * - Każdy rząd ma 4 miejsca (KRZESLA_W_RZEDZIE slotów)
 * 
 * Co INTERWAL_KRZESELKA_MS:
 * 1. Rząd na pozycji (LICZBA_RZEDOW/2) wysadza pasażerów (ARRIVE)
 * 2. Rząd na pozycji 0 przyjmuje nowych z peronu (BOARD)
 * 3. Ring przesuwa się o 1 pozycję
 * 
 * Czas przejazdu = (LICZBA_RZEDOW/2) ticków
 */

#define POZYCJA_DOLNA       0   /* załadunek */
#define POZYCJA_GORNA       (LICZBA_RZEDOW/2)   /* wyładunek */
#define MAX_PASAZEROW_RZAD  4   /* max grup w jednym rzędzie */

static volatile sig_atomic_t g_stop = 0;

static void handler_sigterm(int sig) {
    (void)sig;
    g_stop = 1;
}

/* Struktura pasażera w krzesełku */
typedef struct {
    pid_t pid;
    int rozmiar_grupy;  /* ile osób (do statystyk) */
} Pasazer;

/* Struktura rzędu krzesełek */
typedef struct {
    Pasazer pasazerowie[MAX_PASAZEROW_RZAD];
    int liczba_pasazerow;   /* ile grup w rzędzie */
    int zajete_sloty;       /* suma wag slotów */
} Rzad;

/* Ring LICZBA_RZEDOW rzędów */
static Rzad g_ring[LICZBA_RZEDOW];
static int g_head = 0;  /* indeks rzędu na pozycji 0 (dolna stacja) */

/* Pobiera rząd na danej pozycji logicznej (0=dolna, POZYCJA_GORNA=górna) */
static Rzad* rzad_na_pozycji(int pozycja) {
    int idx = (g_head + pozycja) % LICZBA_RZEDOW;
    return &g_ring[idx];
}

/* Przesuwa ring o 1 (symulacja ruchu liny) */
static void przesun_ring(void) {
    g_head = (g_head + 1) % LICZBA_RZEDOW;
}

/* Wysyła odpowiedź do klienta z backoff */
static int wyslij_odp(pid_t pid, TypWyciagOdp typ) {
    MsgWyciagOdp odp;
    odp.mtype = (long)pid;
    odp.typ = typ;
    
    int backoff = 1;
    for (int i = 0; i < 100; i++) {
        int r = msg_send_nowait(g_mq_wyciag_odp, &odp, sizeof(odp));
        if (r == 0) return 0;
        if (r == -2) return -2;  /* IPC usunięte */
        if (errno == EAGAIN) {
            if (g_stop) return -1;
            poll(NULL, 0, backoff);
            if (backoff < 50) backoff *= 2;
            continue;
        }
        return -1;
    }
    return -1;
}

/* Wysadza pasażerów z rzędu na górnej stacji */
static void wysadz_pasazerow(Rzad *rzad) {
    for (int i = 0; i < rzad->liczba_pasazerow; i++) {
        Pasazer *p = &rzad->pasazerowie[i];
        if (p->pid > 0) {
            wyslij_odp(p->pid, WYCIAG_ODP_ARRIVE);
            
            /* Aktualizuj liczniki - przenieś z krzesła na górę */
            MUTEX_SHM_LOCK();
            g_shm->osoby_w_krzesle -= p->rozmiar_grupy;
            g_shm->osoby_na_gorze += p->rozmiar_grupy;
            g_shm->stats.liczba_przejazdow++;
            MUTEX_SHM_UNLOCK();
        }
    }
    /* Wyczyść rząd */
    rzad->liczba_pasazerow = 0;
    rzad->zajete_sloty = 0;
    memset(rzad->pasazerowie, 0, sizeof(rzad->pasazerowie));
}

/* Kolejka oczekujących na peronie */
static MsgWyciagReq g_kolejka[32];
static int g_kolejka_n = 0;

/* Zbiera requesty z kolejki MQ (non-blocking) */
static void zbierz_requesty(void) {
    for (;;) {
        if (g_kolejka_n >= (int)(sizeof(g_kolejka) / sizeof(g_kolejka[0]))) {
            break;  /* bufor pełny */
        }
        MsgWyciagReq req;
        int r = msg_recv_nowait(g_mq_wyciag_req, &req, sizeof(req), 0);
        if (r > 0) {
            g_kolejka[g_kolejka_n++] = req;
            continue;
        }
        if (r == -2) g_stop = 1;  /* IPC usunięte */
        break;
    }
}

/* Załaduj pasażerów do rzędu na dolnej stacji */
static void zaladuj_pasazerow(Rzad *rzad) {
    int slots = KRZESLA_W_RZEDZIE;  /* 4 sloty na rząd */
    
    /* Pakowanie: VIP najpierw, potem reszta */
    for (int pass = 0; pass < 2 && slots > 0; pass++) {
        for (int i = 0; i < g_kolejka_n && slots > 0; ) {
            int is_vip = g_kolejka[i].vip || (g_kolejka[i].mtype == MSG_TYP_VIP);
            
            /* pass 0 = tylko VIP, pass 1 = tylko nie-VIP */
            if ((pass == 0 && !is_vip) || (pass == 1 && is_vip)) {
                i++;
                continue;
            }
            
            int w = g_kolejka[i].waga_slotow;
            if (w <= 0) w = 1;
            
            if (w <= slots && rzad->liczba_pasazerow < MAX_PASAZEROW_RZAD) {
                /* Mieści się - wsiadaj */
                Pasazer *p = &rzad->pasazerowie[rzad->liczba_pasazerow++];
                p->pid = g_kolejka[i].pid_klienta;
                p->rozmiar_grupy = g_kolejka[i].rozmiar_grupy;
                rzad->zajete_sloty += w;
                slots -= w;
                
                /* Wyślij BOARD */
                wyslij_odp(g_kolejka[i].pid_klienta, WYCIAG_ODP_BOARD);
                
                /* Usuń z kolejki (swap z ostatnim) */
                g_kolejka[i] = g_kolejka[g_kolejka_n - 1];
                g_kolejka_n--;
                continue;
            }
            i++;
        }
    }
}

/* Wyślij KONIEC do wszystkich w kolejce */
static void ewakuuj_kolejke(void) {
    for (int i = 0; i < g_kolejka_n; i++) {
        wyslij_odp(g_kolejka[i].pid_klienta, WYCIAG_ODP_KONIEC);
    }
    g_kolejka_n = 0;
}

/* Sprawdź czy wszystkie rzędy są puste */
static int wszystkie_rzedy_puste(void) {
    for (int i = 0; i < LICZBA_RZEDOW; i++) {
        if (g_ring[i].liczba_pasazerow > 0) return 0;
    }
    return 1;
}

int main(void) {
    /* Handlery sygnałów */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    
    /* Dołącz do IPC */
    if (attach_ipc() == -1) {
        fprintf(stderr, "WYCIAG: attach_ipc failed\n");
        return EXIT_FAILURE;
    }
    
    /* Inicjalizuj ring */
    memset(g_ring, 0, sizeof(g_ring));
    g_head = 0;
    
    int czas_przejazdu_ms = POZYCJA_GORNA * INTERWAL_KRZESELKA_MS;
    loguj("WYCIAG: Start (INTERWAL=%dms, PRZEJAZD=%dms, RZEDOW=%d, SLOTY/RZAD=%d)",
          INTERWAL_KRZESELKA_MS, czas_przejazdu_ms, LICZBA_RZEDOW, KRZESLA_W_RZEDZIE);
    
    while (!g_stop) {
        /* Sprawdź awarię */
        if (g_shm && g_shm->awaria) {
            loguj("WYCIAG: Awaria - zatrzymuję");
            czekaj_na_wznowienie("WYCIAG");
            loguj("WYCIAG: Wznowiono");
        }
        
        /* Zbierz nowe requesty z kolejki */
        zbierz_requesty();
        
        /* === TICK: symulacja ruchu wyciągu === */
        
        /* 1. Wysadź pasażerów na górnej stacji */
        Rzad *rzad_gora = rzad_na_pozycji(POZYCJA_GORNA);
        if (rzad_gora->liczba_pasazerow > 0) {
            wysadz_pasazerow(rzad_gora);
        }
        
        /* 2. Załaduj pasażerów na dolnej stacji (pozycja 0) */
        Rzad *rzad_dol = rzad_na_pozycji(POZYCJA_DOLNA);
        if (g_kolejka_n > 0 && rzad_dol->liczba_pasazerow == 0) {
            zaladuj_pasazerow(rzad_dol);
        }
        
        /* 3. Przesuń ring (symulacja ruchu liny) */
        przesun_ring();
        
        /* Sprawdź koniec dnia */
        if (g_shm && g_shm->koniec_dnia) {
            /* Koniec dnia: NIE ewakuujemy osób z peronu.
             * Zgodnie z wymaganiami: osoby, które weszły na peron, mają zostać dowiezione na górę.
             * Gdy już nikogo nie ma w kolejce i w krzesełkach, czekamy jeszcze 3 sekundy
             * i dopiero wyłączamy kolej.
             */
            if (g_kolejka_n == 0 && wszystkie_rzedy_puste()) {
                loguj("WYCIAG: Drenowanie zakończone - wyłączam za 3s");
                poll(NULL, 0, 3000);
                break;
            }
        }
        
        /* Czekaj do następnego ticku */
        poll(NULL, 0, INTERWAL_KRZESELKA_MS);
    }
    
    /* Koniec - ewakuuj wszystkich */
    ewakuuj_kolejke();
    
    /* Wysadź wszystkich pozostałych w krzesełkach (wszystkie pozycje dla pewności) */
    for (int i = 0; i < LICZBA_RZEDOW; i++) {
        if (g_ring[i].liczba_pasazerow > 0) {
            wysadz_pasazerow(&g_ring[i]);
        }
    }
    
    loguj("WYCIAG: Kończę pracę");
    detach_ipc();
    return EXIT_SUCCESS;
}
