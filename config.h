#ifndef CONFIG_H
#define CONFIG_H

/*
 * KOLEJ KRZESEŁKOWA - KONFIGURACJA
 * Plik zawiera wszystkie stałe i parametry systemu
 */


/* ============================================
 * STAŁE DLA FTOK / LOCK (odporność na crash)
 * ============================================ */
#define FTOK_FILE          "/tmp/kolej_krzeselkowa_ipc.ftok"
#define OWNER_LOCK_FILE    "/tmp/kolej_krzeselkowa_owner.lock"
/* ============================================
 * LIMITY SYSTEMOWE
 * ============================================ */
#define MAX_KLIENTOW        60000     // max procesów klientów jednocześnie
#define MAX_KARNETOW        999999 // max karnetów w pamięci
#define MAX_LOGOW           999999   // max wpisów w logu przejść

/* ============================================
 * INFRASTRUKTURA KOLEI
 * ============================================ */
#define N_LIMIT_TERENU      100     // max osób na terenie dolnej stacji
#define LICZBA_BRAMEK1      4       // bramki wejściowe (do terenu)
#define LICZBA_BRAMEK2      3       // bramki peronowe
#define LICZBA_RZEDOW       18      // rzędów krzesełek
#define KRZESLA_W_RZEDZIE   4       // miejsc w jednym rzędzie
#define LICZBA_WYJSC_GORA   2       // wyjścia ze stacji górnej

/* ============================================
 * CZASY (w sekundach) - HIGH PERFORMANCE
 * T1=1s, T2=2s, T3=3s, T4=4s
 * ============================================ */
#define CZAS_T1             1       // trasa rowerowa łatwa
#define CZAS_T2             2       // trasa rowerowa średnia
#define CZAS_T3             3       // trasa rowerowa trudna
#define CZAS_T4             4       // trasa piesza
#define CZAS_WJAZDU         0       // DEPRECATED - nieużywane

/* ============================================
 * WYCIĄG (MODEL RING 18 RZĘDÓW)
 * Czas przejazdu = 9 * INTERWAL_KRZESELKA_MS
 * ============================================ */
#define INTERWAL_KRZESELKA_MS 200   // co ile podjeżdża krzesełko (ms)
#define KURS_ROWEROWY_CO      3     // co który kurs gwarantuje rower
#define PERON_SLOTY           4     // max slotów na peronie (pieszy=1, rower=2)

/* ============================================
 * GODZINY PRACY
 * Dla symulacji: czas względny w sekundach od startu
 * ============================================ */
#define GODZINA_OTWARCIA    0       // start symulacji = otwarcie
#define CZAS_SYMULACJI      300     // domyślny czas symulacji (5 min testowo)

/* ============================================
 * PRAWDOPODOBIEŃSTWA (w procentach)
 * ============================================ */
#define PROC_VIP            1       // 1% klientów to VIP
#define PROC_ROWERZYSTA     50      // 50% klientów to rowerzyści
#define PROC_DZIECKO        20      // 20% dorosłych ma dziecko
#define PROC_DRUGIE_DZIECKO 30      // 30% z dzieci ma drugie dziecko

/* ============================================
 * CENY KARNETÓW (w groszach dla uniknięcia float)
 * ============================================ */
#define CENA_JEDNORAZOWY    500     // 5.00 zł
#define CENA_TK1            2000    // 20.00 zł (30 min)
#define CENA_TK2            3500    // 35.00 zł (60 min)
#define CENA_TK3            5000    // 50.00 zł (120 min)
#define CENA_DZIENNY        10000   // 100.00 zł

/* ============================================
 * CZAS WAŻNOŚCI KARNETÓW (w sekundach)
 * ============================================ */
#define WAZNOSC_JEDNORAZOWY 0       // 0 = jedno przejście
#define WAZNOSC_TK1         1800    // 30 minut
#define WAZNOSC_TK2         3600    // 60 minut
#define WAZNOSC_TK3         7200    // 120 minut
#define WAZNOSC_DZIENNY     86400   // 24h (praktycznie do końca dnia)

/* ============================================
 * ZNIŻKI
 * ============================================ */
#define WIEK_ZNIZKA_DZIECKO 10      // <10 lat = zniżka
#define WIEK_ZNIZKA_SENIOR  65      // >=65 lat = zniżka
#define WIEK_WYMAGA_OPIEKI  8       // <8 lat = wymaga opiekuna
#define ZNIZKA_PROCENT      25      // 25% zniżki

/* ============================================
 * WIEK KLIENTÓW
 * ============================================ */
#define WIEK_MIN            4       // minimalny wiek
#define WIEK_MAX            80      // maksymalny wiek
#define WIEK_DOROSLY_MIN    18      // minimalny wiek dorosłego

/* ============================================
 * INDEKSY SEMAFORÓW
 * Wszystkie semafory w jednym zestawie
 * ============================================ */
#define SEM_TEREN           0       // limit N osób na terenie (init: N)
#define SEM_MUTEX_SHM       1       // mutex pamięci współdzielonej (init: 1)
#define SEM_MUTEX_KASA      2       // mutex kasy (init: 1)
#define SEM_MUTEX_LOG       3       // mutex logów (init: 1)
#define SEM_PERON           4       // sloty peronu (init: PERON_SLOTY=4, pieszy=1, rower=2)
#define SEM_PRACOWNIK1      5       // sygnalizacja dla P1 (init: 0)
#define SEM_PRACOWNIK2      6       // sygnalizacja dla P2 (init: 0)
#define SEM_GOTOWY_P1       7       // P1 gotowy po awarii (init: 0)
#define SEM_GOTOWY_P2       8       // P2 gotowy po awarii (init: 0)
#define SEM_KONIEC          9       // sygnał zakończenia (init: 0)
#define SEM_BARIERA_AWARIA  10      // bariera podczas awarii (init: 0)
#define SEM_COUNT           11      // łączna liczba semaforów

/* ============================================
 * TYPY KOMUNIKATÓW (mtype w kolejkach)
 * ============================================ */
#define MSG_TYP_VIP         1       // klient VIP (priorytet - NIŻSZY = WYŻSZY w msgrcv)
#define MSG_TYP_NORMALNY    2       // zwykły klient
#define MSG_TYP_STOP        10      // sygnał STOP
#define MSG_TYP_GOTOWY      11      // potwierdzenie gotowości
#define MSG_TYP_START       12      // sygnał START

/* ============================================
 * KLUCZE IPC (offsety od bazowego klucza)
 * ============================================ */
#define IPC_KEY_BASE        'K'     // bazowy znak dla ftok
#define IPC_KEY_SEM         0       // semafory
#define IPC_KEY_SHM         1       // pamięć współdzielona
#define IPC_KEY_MQ_KASA     2       // kolejka do kasy
#define IPC_KEY_MQ_KASA_ODP 3       // kolejka odpowiedzi z kasy
#define IPC_KEY_MQ_BRAMKA   4       // kolejka do bramek
#define IPC_KEY_MQ_BRAMKA_ODP 5     // kolejka odpowiedzi z bramek (NOWA)
#define IPC_KEY_MQ_PRAC     6       // kolejka pracowników
#define IPC_KEY_MQ_WYCIAG_REQ 7     // kolejka peron->wyciąg
#define IPC_KEY_MQ_WYCIAG_ODP 8     // odpowiedzi wyciągu

/* ============================================
 * ŚCIEŻKI DO PLIKÓW WYKONYWALNYCH
 * ============================================ */
#define PATH_KASJER         "./kasjer"
#define PATH_BRAMKA         "./bramka"
#define PATH_PRACOWNIK1     "./pracownik1"
#define PATH_PRACOWNIK2     "./pracownik2"
#define PATH_GENERATOR      "./generator"
#define PATH_KLIENT         "./klient"
#define PATH_WYCIAG         "./wyciag"
#define PATH_SPRZATACZ      "./sprzatacz"

/* ============================================
 * PLIKI RAPORTÓW
 * ============================================ */
#define PLIK_RAPORT         "output/raport_dzienny.txt"
#define PLIK_LOG            "output/log_przejsc.txt"

/* ============================================
 * UPRAWNIENIA IPC (minimalne)
 * ============================================ */
#define IPC_PERMS           0600    // rw dla właściciela

#endif /* CONFIG_H */
