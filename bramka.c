#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include "config.h"
#include "types.h"
#include "ipc.h"
#include "utils.h"

/*
 * KOLEJ KRZESEŁKOWA - PROCES BRAMKI (Bramka1)
 * 
 * Odpowiedzialności:
 * 1. Odbieranie zgłoszeń od klientów (kolejka mq_bramka)
 * 2. Sprawdzanie ważności karnetu
 * 3. Kontrola limitu N osób na terenie (semafor)
 * 4. Aktywacja karnetu przy pierwszym przejściu
 * 5. Logowanie przejść
 */

static volatile sig_atomic_t g_koniec = 0;
static int g_numer_bramki = 1;

static void handler_sigterm(int sig) {
    (void)sig;
    g_koniec = 1;
}

int main(int argc, char *argv[]) {
    /* Pobierz numer bramki z argumentów */
    if (argc >= 2) {
        g_numer_bramki = waliduj_liczbe(argv[1], 1, LICZBA_BRAMEK1);
        if (g_numer_bramki < 0) g_numer_bramki = 1;
    }
    
    /* Inicjalizacja */
    inicjalizuj_losowanie();
    
    /* Obsługa sygnałów */
    signal(SIGTERM, handler_sigterm);
    signal(SIGINT, handler_sigterm);
    
    /* Dołącz do IPC */
    if (attach_ipc() != 0) {
        loguj("BRAMKA%d: Błąd dołączania do IPC", g_numer_bramki);
        return EXIT_FAILURE;
    }
    
    loguj("BRAMKA%d: Rozpoczynam pracę", g_numer_bramki);
    
    /* Główna pętla */
    while (!g_koniec && !g_shm->koniec_dnia) {
        MsgBramka1 msg;
        MsgBramkaOdp odp;
        
        /* Odbierz zgłoszenie (VIP ma priorytet) */
        int ret = msg_recv_nowait(g_mq_bramka, &msg, sizeof(msg), -MSG_TYP_VIP);
        
        if (ret < 0) {
            usleep(50000); /* 50ms */
            continue;
        }
        
        /* Sprawdź czy kolej aktywna */
        if (g_shm->awaria) {
            /* Podczas awarii nie wpuszczamy */
            odp.mtype = msg.pid_klienta;
            odp.sukces = 0;
            msg_send(g_mq_kasa_odp, &odp, sizeof(odp)); /* używamy tej samej kolejki */
            continue;
        }
        
        loguj("BRAMKA%d: Klient z karnetem %d (grupa=%d)", 
              g_numer_bramki, msg.id_karnetu, msg.rozmiar_grupy);
        
        /* Pobierz karnet i sprawdź ważność */
        Karnet *karnet = pobierz_karnet(msg.id_karnetu);
        
        odp.mtype = msg.pid_klienta;
        
        if (karnet == NULL || !czy_karnet_wazny(karnet, time(NULL))) {
            loguj("BRAMKA%d: Karnet %d nieważny - odmowa", g_numer_bramki, msg.id_karnetu);
            odp.sukces = 0;
            msg_send(g_mq_kasa_odp, &odp, sizeof(odp));
            continue;
        }
        
        /* Czekaj na miejsce na terenie (dla całej grupy) */
        loguj("BRAMKA%d: Oczekiwanie na %d miejsc na terenie...", 
              g_numer_bramki, msg.rozmiar_grupy);
        
        sem_wait_n(SEM_TEREN, msg.rozmiar_grupy);
        
        /* Aktywuj karnet (jeśli pierwsze użycie) */
        aktywuj_karnet(msg.id_karnetu);
        
        /* Dla karnetu jednorazowego - oznacz jako użyty */
        if (karnet->typ == KARNET_JEDNORAZOWY) {
            uzyj_karnet_jednorazowy(msg.id_karnetu);
        }
        
        /* Aktualizuj licznik osób na terenie */
        MUTEX_SHM_LOCK();
        g_shm->osoby_na_terenie += msg.rozmiar_grupy;
        MUTEX_SHM_UNLOCK();
        
        /* Zaloguj przejście */
        dodaj_log(msg.id_karnetu, LOG_BRAMKA1, g_numer_bramki);
        
        loguj("BRAMKA%d: Wpuszczono klienta z karnetem %d", 
              g_numer_bramki, msg.id_karnetu);
        
        /* Wyślij potwierdzenie */
        odp.sukces = 1;
        msg_send(g_mq_kasa_odp, &odp, sizeof(odp));
    }
    
    loguj("BRAMKA%d: Kończę pracę", g_numer_bramki);
    detach_ipc();
    
    return EXIT_SUCCESS;
}
