#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include "config.h"
#include "types.h"
#include "ipc.h"
#include "utils.h"

/*
 * KOLEJ KRZESEŁKOWA - PROCES KASJERA
 * 
 * Odpowiedzialności:
 * 1. Odbieranie zgłoszeń od klientów (kolejka mq_kasa)
 * 2. Sprawdzanie czy dziecko <8 ma opiekuna
 * 3. Tworzenie karnetów ze zniżkami
 * 4. Wysyłanie odpowiedzi (kolejka mq_kasa_odp)
 */

static volatile sig_atomic_t g_koniec = 0;

static void handler_sigterm(int sig) {
    (void)sig;
    g_koniec = 1;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    /* Inicjalizacja */
    inicjalizuj_losowanie();
    
    /* Obsługa sygnałów */
    signal(SIGTERM, handler_sigterm);
    signal(SIGINT, handler_sigterm);
    
    /* Dołącz do IPC */
    if (attach_ipc() != 0) {
        loguj("KASJER: Błąd dołączania do IPC");
        return EXIT_FAILURE;
    }
    
    loguj("KASJER: Rozpoczynam pracę");
    
    /* Główna pętla */
    while (!g_koniec && !g_shm->koniec_dnia) {
        MsgKasa msg;
        MsgKasaOdp odp;
        
        /* Odbierz zgłoszenie (priorytet VIP: mtype=-2) */
        int ret = msg_recv_nowait(g_mq_kasa, &msg, sizeof(msg), -MSG_TYP_VIP);
        
        if (ret < 0) {
            /* Brak wiadomości - krótka pauza */
            usleep(50000); /* 50ms */
            continue;
        }
        
        loguj("KASJER: Obsługuję klienta %d (wiek=%d, dzieci=%d, VIP=%d)",
              msg.id_klienta, msg.wiek, msg.liczba_dzieci, msg.vip);
        
        /* Domyślna odpowiedź */
        odp.mtype = msg.pid_klienta;
        odp.sukces = 0;
        odp.id_karnetu = -1;
        odp.id_karnety_dzieci[0] = -1;
        odp.id_karnety_dzieci[1] = -1;
        
        /* Sprawdź czy dziecko <8 bez opiekuna */
        if (msg.wiek < WIEK_WYMAGA_OPIEKI && msg.liczba_dzieci == 0) {
            loguj("KASJER: Odmowa - dziecko %d lat bez opiekuna", msg.wiek);
            SHM_INC(stats.liczba_dzieci_odrzuconych);
            msg_send(g_mq_kasa_odp, &odp, sizeof(odp));
            continue;
        }
        
        /* Losuj typ karnetu */
        TypKarnetu typ = losuj_typ_karnetu();
        int cena = pobierz_cene_karnetu(typ);
        
        /* Zastosuj zniżkę */
        cena = oblicz_cene_ze_znizka(cena, msg.wiek);
        
        /* Utwórz karnet */
        int id = utworz_karnet(typ, cena, msg.vip);
        if (id < 0) {
            loguj("KASJER: Błąd tworzenia karnetu");
            msg_send(g_mq_kasa_odp, &odp, sizeof(odp));
            continue;
        }
        
        odp.sukces = 1;
        odp.id_karnetu = id;
        odp.typ_karnetu = typ;
        
        /* Karnety dla dzieci */
        for (int i = 0; i < msg.liczba_dzieci; i++) {
            int cena_dziecko = oblicz_cene_ze_znizka(pobierz_cene_karnetu(typ), 
                                                      msg.wiek_dzieci[i]);
            odp.id_karnety_dzieci[i] = utworz_karnet(typ, cena_dziecko, 0);
        }
        
        /* Aktualizuj statystyki */
        MUTEX_SHM_LOCK();
        g_shm->stats.laczna_liczba_klientow++;
        if (msg.typ == TYP_PIESZY) {
            g_shm->stats.liczba_pieszych++;
        } else {
            g_shm->stats.liczba_rowerzystow++;
        }
        if (msg.vip) {
            g_shm->stats.liczba_vip++;
        }
        if (msg.liczba_dzieci > 0) {
            g_shm->stats.liczba_grup_rodzinnych++;
        }
        MUTEX_SHM_UNLOCK();
        
        loguj("KASJER: Sprzedano karnet %d (%s) klientowi %d",
              id, nazwa_karnetu(typ), msg.id_klienta);
        
        /* Wyślij odpowiedź */
        msg_send(g_mq_kasa_odp, &odp, sizeof(odp));
    }
    
    loguj("KASJER: Kończę pracę");
    detach_ipc();
    
    return EXIT_SUCCESS;
}
