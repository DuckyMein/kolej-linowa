#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>

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
    
    /* Ustaw aby zginąć gdy rodzic (main) umrze */
    ustaw_smierc_z_rodzicem();
    
    /* Inicjalizacja */
    inicjalizuj_losowanie();
    
    /* Obsługa sygnałów - sigaction BEZ SA_RESTART */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_sigterm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    
    /* Dołącz do IPC */
    if (attach_ipc() != 0) {
        loguj("BRAMKA%d: Błąd dołączania do IPC", g_numer_bramki);
        return EXIT_FAILURE;
    }
    
    loguj("BRAMKA%d: Rozpoczynam pracę", g_numer_bramki);
    
    /* Główna pętla - blokujące msg_recv */
    while (!g_koniec) {
        MsgBramka1 msg;
        MsgBramkaOdp odp;
        
        /* Odbierz zgłoszenie BLOKUJĄCO (mtype=0 = pierwsza dostępna wiadomość) */
        int ret = msg_recv(g_mq_bramka, &msg, sizeof(msg), 0);
        
        if (ret < 0 || g_koniec) {
            /* Przerwane sygnałem lub koniec - wyjdź */
            break;
        }
        
        /* Podczas awarii - czekaj na wznowienie */
        if (g_shm->awaria && !g_koniec) {
            char buf[32];
            snprintf(buf, sizeof(buf), "BRAMKA%d", g_numer_bramki);
            czekaj_na_wznowienie(buf);
        }
        
        /* CHECK #1: Czy karnet ważny? (karnet jest ucięty do końca dnia) */
        Karnet *karnet = pobierz_karnet(msg.id_karnetu);
        odp.mtype = msg.pid_klienta;
        
        if (karnet == NULL || !czy_karnet_wazny(karnet, time(NULL))) {
            odp.sukces = 0;
            msg_send(g_mq_bramka_odp, &odp, sizeof(odp));
            continue;
        }
        
        /* Czekaj na miejsce na terenie (dla całej grupy) */
        if (sem_wait_n(SEM_TEREN, msg.rozmiar_grupy) != 0) {
            /* Semafor przerwany - odmów */
            odp.sukces = 0;
            msg_send(g_mq_bramka_odp, &odp, sizeof(odp));
            continue;
        }
        
        /* CHECK #2: Po pobraniu semafora sprawdź karnet PONOWNIE!
         * Mógł wygasnąć w trakcie czekania (czas upłynął lub koniec dnia). */
        if (!czy_karnet_wazny(karnet, time(NULL))) {
            sem_signal_n(SEM_TEREN, msg.rozmiar_grupy); /* Zwróć semafor */
            odp.sukces = 0;
            msg_send(g_mq_bramka_odp, &odp, sizeof(odp));
            continue;
        }
        
        /* Sprawdź czy klient jeszcze żyje - może się zakończyć podczas czekania */
        if (kill(msg.pid_klienta, 0) == -1 && errno == ESRCH) {
            /* Klient nie żyje - zwróć semafor i pomiń */
            sem_signal_n(SEM_TEREN, msg.rozmiar_grupy);
            continue;
        }
        
        /* Aktywuj karnet (jeśli pierwsze użycie) - UCINANIE do końca dnia */
        aktywuj_karnet(msg.id_karnetu);
        
        /* Dla karnetu jednorazowego - oznacz jako użyty */
        if (karnet->typ == KARNET_JEDNORAZOWY) {
            uzyj_karnet_jednorazowy(msg.id_karnetu);
        }
        
        /* Aktualizuj licznik osób na terenie */
        MUTEX_SHM_LOCK();
        g_shm->osoby_na_terenie += msg.rozmiar_grupy;
        MUTEX_SHM_UNLOCK();
        
        /* Zaloguj przejście do SHM (nie do stderr) */
        dodaj_log(msg.id_karnetu, LOG_BRAMKA1, g_numer_bramki);
        
        /* Wyślij potwierdzenie */
        odp.sukces = 1;
        msg_send(g_mq_bramka_odp, &odp, sizeof(odp));
    }
    
    loguj("BRAMKA%d: Kończę pracę", g_numer_bramki);
    
    /* Odpowiedz wszystkim czekającym klientom odmową */
    MsgBramka1 msg;
    while (msg_recv_nowait(g_mq_bramka, &msg, sizeof(msg), 0) > 0) {
        MsgBramkaOdp odp;
        odp.mtype = msg.pid_klienta;
        odp.sukces = 0;
        msg_send_nowait(g_mq_bramka_odp, &odp, sizeof(odp));
    }
    
    detach_ipc();
    
    return EXIT_SUCCESS;
}
