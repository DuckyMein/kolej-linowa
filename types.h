#ifndef TYPES_H
#define TYPES_H

#include <sys/types.h>
#include <time.h>
#include "config.h"

/*
 * KOLEJ KRZESEŁKOWA - STRUKTURY DANYCH
 * Wszystkie typedef i struktury używane w projekcie
 */

/* ============================================
 * TYPY WYLICZENIOWE (ENUM)
 * ============================================ */

/* Typ karnetu */
typedef enum {
    KARNET_JEDNORAZOWY = 1,
    KARNET_TK1,             // 30 min
    KARNET_TK2,             // 60 min
    KARNET_TK3,             // 120 min
    KARNET_DZIENNY
} TypKarnetu;

/* Typ klienta (środek transportu) */
typedef enum {
    TYP_PIESZY = 0,
    TYP_ROWERZYSTA = 1
} TypKlienta;

/* Typ wpisu w logu */
typedef enum {
    LOG_BRAMKA1 = 1,        // przejście przez bramkę wejściową
    LOG_BRAMKA2,            // przejście przez bramkę peronową
    LOG_WYJSCIE_GORA        // wyjście ze stacji górnej
} TypLogu;

/* Faza dnia (do 2-fazowego zamykania) */
typedef enum {
    FAZA_OPEN = 0,          // normalny dzień pracy
    FAZA_CLOSING = 1,       // zamykamy - nie wpuszczamy nowych
    FAZA_DRAINING = 2       // drenujemy - czekamy aż wszyscy wyjdą
} FazaDnia;

/* Trasy powrotne */
typedef enum {
    TRASA_T1 = 0,           // rowerowa łatwa (20 min)
    TRASA_T2,               // rowerowa średnia (35 min)
    TRASA_T3,               // rowerowa trudna (50 min)
    TRASA_T4                // piesza (60 min)
} Trasa;

/* ============================================
 * STRUKTURA KARNETU
 * ============================================ */
typedef struct {
    int id;                     // unikalny ID karnetu
    TypKarnetu typ;             // typ karnetu
    int czas_waznosci_sek;      // czas ważności w sekundach (0 = jednorazowy)
    time_t czas_aktywacji;      // kiedy pierwszy raz użyty (0 = nieaktywny)
    int cena_gr;                // cena w groszach
    int uzyty;                  // dla jednorazowego: 0/1
    int vip;                    // czy VIP: 0/1
    int aktywny;                // czy karnet jest w użyciu: 0/1
} Karnet;

/* ============================================
 * STRUKTURA KLIENTA
 * ============================================ */
typedef struct {
    pid_t pid;                  // PID procesu klienta
    int id;                     // unikalny ID klienta
    int wiek;                   // wiek (4-80)
    TypKlienta typ;             // pieszy / rowerzysta
    int vip;                    // czy VIP: 0/1
    int id_karnetu;             // ID przypisanego karnetu
    int liczba_dzieci;          // 0, 1 lub 2 (dzieci <8 lat pod opieką)
    int wiek_dzieci[2];         // wiek dzieci (jeśli są)
    int id_karnety_dzieci[2];   // karnety dzieci
    int rozmiar_grupy;          // 1 + liczba_dzieci (ile miejsc zajmuje)
} Klient;

/* ============================================
 * STRUKTURA WPISU W LOGU
 * ============================================ */
typedef struct {
    int id_karnetu;             // ID karnetu
    TypLogu typ_bramki;         // która bramka
    int numer_bramki;           // numer konkretnej bramki (1-4, 1-3, 1-2)
    time_t czas;                // czas przejścia
} LogEntry;

/* ============================================
 * STRUKTURA STATYSTYK
 * ============================================ */
typedef struct {
    int laczna_liczba_klientow;     // wszyscy wygenerowani
    int liczba_pieszych;            // turyści piesi
    int liczba_rowerzystow;         // rowerzyści
    int liczba_vip;                 // klienci VIP
    int liczba_dzieci_odrzuconych;  // <8 lat bez opiekuna
    int liczba_grup_rodzinnych;     // grupy z dziećmi
    int sprzedane_karnety[5];       // indeks = TypKarnetu - 1
    int przychod_gr;                // łączny przychód w groszach
    int uzycia_tras[4];             // T1, T2, T3, T4
    int liczba_zatrzyman;           // sygnały STOP (awarie)
    int liczba_przejazdow;          // łączna liczba przejazdów
} Statystyki;

/* ============================================
 * PAMIĘĆ WSPÓŁDZIELONA - GŁÓWNA STRUKTURA
 * ============================================ */
typedef struct {
    /* Stan systemu */
    int kolej_aktywna;              // 0=stop, 1=działa
    int awaria;                     // 0=brak, 1=STOP aktywny
    int koniec_dnia;                // DEPRECATED - używaj faza_dnia
    time_t czas_startu;             // czas uruchomienia symulacji
    int czekajacych_na_wznowienie;  // ile procesów czeka na SEM_BARIERA_AWARIA
    
    /* NOWE: 2-fazowe zamykanie */
    FazaDnia faza_dnia;             // OPEN / CLOSING / DRAINING
    time_t czas_konca_dnia;         // kiedy kończy się dzień (absolutny timestamp)
    int aktywni_klienci;            // ile procesów klienta żyje (do drenowania)

    /* PANIC: awaryjne zamykanie po śmierci procesu */
    int panic;                      // 0=OK, 1=panic shutdown
    pid_t panic_pid;                // PID, który wywołał panic (jeśli znany)
    int panic_sig;                  // sygnał (jeśli znany)

    /* Liczniki bieżące */
    int osoby_na_terenie;           // aktualnie na terenie stacji
    int osoby_na_gorze;             // aktualnie na górze
    int osoby_na_peronie;           // aktualnie na peronie
    int osoby_w_krzesle;            // w trakcie wjazdu (na krzesełku)
    int aktualny_rzad;              // 0-17, który rząd jest gotowy
    
    /* Autoincrement ID */
    int nastepny_id_karnetu;        // następny ID karnetu
    int nastepny_id_klienta;        // następny ID klienta
    
    /* Karnety */
    Karnet karnety[MAX_KARNETOW];
    int liczba_karnetow;
    
    /* Logi przejść */
    LogEntry logi[MAX_LOGOW];
    int liczba_logow;
    
    /* Statystyki */
    Statystyki stats;
    
    /* PIDs procesów stałych (do cleanup) */
    pid_t pid_main;                 // proces główny
    pid_t pid_generator;
    pid_t pid_kasjer;
    pid_t pid_bramki1[LICZBA_BRAMEK1];
    pid_t pid_pracownik1;
    pid_t pid_pracownik2;
    pid_t pid_wyciag;               // proces wyciągu
    
} SharedMemory;

/* ============================================
 * KOMUNIKATY - KOLEJKA DO KASY
 * ============================================ */
typedef struct {
    long mtype;                 // MSG_TYP_NORMALNY lub MSG_TYP_VIP
    pid_t pid_klienta;          // PID procesu klienta
    int id_klienta;             // ID klienta
    int wiek;                   // wiek klienta
    int typ;                    // TYP_PIESZY / TYP_ROWERZYSTA
    int vip;                    // czy VIP
    int liczba_dzieci;          // 0, 1, 2
    int wiek_dzieci[2];         // wiek dzieci
} MsgKasa;

/* ============================================
 * KOMUNIKATY - ODPOWIEDŹ Z KASY
 * ============================================ */
typedef struct {
    long mtype;                 // = pid_klienta (żeby trafić do właściwego)
    int sukces;                 // 0=odmowa, 1=OK
    int id_karnetu;             // przydzielony karnet (-1 jeśli odmowa)
    int id_karnety_dzieci[2];   // karnety dla dzieci (-1 jeśli brak)
    TypKarnetu typ_karnetu;     // jaki typ karnetu kupiono
} MsgKasaOdp;

/* ============================================
 * KOMUNIKATY - KOLEJKA BRAMKA1
 * ============================================ */
typedef struct {
    long mtype;                 // MSG_TYP_VIP (priorytet) lub MSG_TYP_NORMALNY
    pid_t pid_klienta;          // PID procesu klienta
    int id_karnetu;             // ID karnetu do sprawdzenia
    int rozmiar_grupy;          // ile miejsc zajmuje (1-3)
    int numer_bramki;           // do której bramki (1-4)
} MsgBramka1;

/* ============================================
 * KOMUNIKATY - ODPOWIEDŹ Z BRAMKI
 * ============================================ */
typedef struct {
    long mtype;                 // = pid_klienta
    int sukces;                 // 0=odmowa (karnet nieważny), 1=OK
} MsgBramkaOdp;

/* ============================================
 * KOMUNIKATY - KOMUNIKACJA PRACOWNIKÓW
 * ============================================ */
typedef struct {
    long mtype;                 // 1=do P1, 2=do P2
    int typ_komunikatu;         // MSG_TYP_STOP, MSG_TYP_GOTOWY, MSG_TYP_START
    pid_t nadawca;              // kto wysłał
} MsgPracownicy;

/* ============================================
 * KOMUNIKATY - PERON (klient -> pracownik1)
 * ============================================ */
typedef struct {
    long mtype;                 // MSG_TYP_NORMALNY
    pid_t pid_klienta;          // PID klienta
    int id_karnetu;             // ID karnetu
    int miejsca;                // ile miejsc potrzebuje (1-4)
    int numer_bramki2;          // przez którą bramkę2 wchodzi (1-3)
} MsgPeron;

/* ============================================
 * WYCIĄG - TYPY ODPOWIEDZI
 * ============================================ */
typedef enum {
    WYCIAG_ODP_BOARD = 1,       // klient wsiadł (może zwolnić peron)
    WYCIAG_ODP_ARRIVE = 2,      // klient dojechał na górę
    WYCIAG_ODP_KONIEC = 3       // ewakuacja / koniec dnia
} TypWyciagOdp;

/* ============================================
 * KOMUNIKATY - WYCIĄG REQUEST (klient -> wyciąg)
 * ============================================ */
typedef struct {
    long mtype;                 // MSG_TYP_VIP lub MSG_TYP_NORMALNY
    pid_t pid_klienta;          // PID klienta
    int typ_klienta;            // TypKlienta (pieszy/rower)
    int vip;                    // 0/1
    int rozmiar_grupy;          // osoby (dorosły + dzieci)
    int waga_slotow;            // sloty peronu (pieszy=1+dzieci, rower=2+dzieci)
} MsgWyciagReq;

/* ============================================
 * KOMUNIKATY - WYCIĄG RESPONSE (wyciąg -> klient)
 * ============================================ */
typedef struct {
    long mtype;                 // = pid klienta
    TypWyciagOdp typ;           // BOARD/ARRIVE/KONIEC
} MsgWyciagOdp;

#endif /* TYPES_H */
