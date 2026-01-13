#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include "config.h"
#include "types.h"

/*
 * KOLEJ KRZESEŁKOWA - FUNKCJE POMOCNICZE
 * Walidacja, obsługa błędów, losowanie, formatowanie
 */

/* ============================================
 * OBSŁUGA BŁĘDÓW
 * ============================================ */

/* 
 * Wyświetla błąd z perror() i kończy program
 * Używać gdy błąd jest krytyczny
 */
void blad_krytyczny(const char *msg);

/*
 * Wyświetla ostrzeżenie z perror() ale kontynuuje
 * Używać gdy błąd nie jest krytyczny
 */
void blad_ostrzezenie(const char *msg);

/*
 * Loguje komunikat do pliku i na stderr
 * Format: [CZAS] [PID] komunikat
 */
void loguj(const char *format, ...);

/* ============================================
 * WALIDACJA DANYCH
 * ============================================ */

/*
 * Waliduje liczbę całkowitą z zakresu
 * Zwraca: wartość lub -1 przy błędzie
 */
int waliduj_liczbe(const char *str, int min, int max);

/*
 * Waliduje argumenty wiersza poleceń dla main
 * Ustawia N i czas_symulacji
 * Zwraca: 0=OK, -1=błąd
 */
int waliduj_argumenty(int argc, char *argv[], int *N, int *czas_symulacji);

/* ============================================
 * LOSOWANIE
 * ============================================ */

/*
 * Inicjalizuje generator liczb losowych
 * Wywołać raz na początku każdego procesu!
 */
void inicjalizuj_losowanie(void);

/*
 * Losuje liczbę z zakresu [min, max] włącznie
 */
int losuj_zakres(int min, int max);

/*
 * Losuje true z podanym prawdopodobieństwem (0-100%)
 */
int losuj_procent(int procent);

/*
 * Losuje typ karnetu według prawdopodobieństw
 */
TypKarnetu losuj_typ_karnetu(void);

/*
 * Losuje trasę dla rowerzysty (T1-T3)
 */
Trasa losuj_trase_rower(void);

/* ============================================
 * FORMATOWANIE I KONWERSJA
 * ============================================ */

/*
 * Zwraca cenę karnetu w groszach
 */
int pobierz_cene_karnetu(TypKarnetu typ);

/*
 * Zwraca czas ważności karnetu w sekundach
 */
int pobierz_waznosc_karnetu(TypKarnetu typ);

/*
 * Zwraca czas trasy w sekundach
 */
int pobierz_czas_trasy(Trasa trasa);

/*
 * Zwraca nazwę typu karnetu jako string
 */
const char* nazwa_karnetu(TypKarnetu typ);

/*
 * Zwraca nazwę trasy jako string
 */
const char* nazwa_trasy(Trasa trasa);

/*
 * Formatuje czas jako HH:MM:SS
 * Bufor musi mieć min. 9 znaków
 */
void formatuj_czas(time_t czas, char *bufor);

/*
 * Formatuje kwotę z groszy na "XX.XX zł"
 * Bufor musi mieć min. 16 znaków
 */
void formatuj_kwote(int grosze, char *bufor);

/* ============================================
 * OBLICZENIA
 * ============================================ */

/*
 * Oblicza cenę ze zniżką
 */
int oblicz_cene_ze_znizka(int cena_gr, int wiek);

/*
 * Sprawdza czy karnet jest ważny
 * Zwraca: 1=ważny, 0=nieważny
 */
int czy_karnet_wazny(Karnet *karnet, time_t aktualny_czas);

/*
 * Oblicza ile miejsc zajmuje klient na krzesełku
 * Rowerzysta = 2, pieszy = 1, + dzieci
 */
int oblicz_miejsca_krzeselko(TypKlienta typ, int liczba_dzieci);

/* ============================================
 * CZAS SYMULACJI
 * ============================================ */

/*
 * Pobiera aktualny czas symulacji (sekundy od startu)
 */
time_t czas_symulacji(time_t czas_startu);

/*
 * Sprawdza czy czas symulacji się skończył
 */
int czy_koniec_symulacji(time_t czas_startu, int max_czas);

#endif /* UTILS_H */
