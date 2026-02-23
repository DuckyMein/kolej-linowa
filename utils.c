#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/file.h>
#include "utils.h"
#include "ipc.h"  /* dla g_shm->czas_konca_dnia */

/*
 * KOLEJ KRZESEŁKOWA - IMPLEMENTACJA FUNKCJI POMOCNICZYCH
 */

/* ============================================
 * OBSŁUGA BŁĘDÓW
 * ============================================ */

void blad_krytyczny(const char *msg) {
    loguj("BŁĄD KRYTYCZNY: %s: %s", msg, strerror(errno));
    exit(EXIT_FAILURE);
}

void blad_ostrzezenie(const char *msg) {
    loguj("OSTRZEŻENIE: %s: %s", msg, strerror(errno));
}

void loguj(const char *format, ...) {
    /*
     * Uwaga: wiele procesów loguje do tych samych plików (stdout/stderr są
     * przekierowane w main.c do output/<nazwa>.log). Żeby nie mieszać
     * wpisów, cały wiersz składamy do bufora i zapisujemy pod lockiem.
     */
    char buf[1024];

    time_t teraz = time(NULL);
    struct tm tm_info;
    localtime_r(&teraz, &tm_info);

    char czas_buf[20];
    strftime(czas_buf, sizeof(czas_buf), "%H:%M:%S", &tm_info);

    int n = snprintf(buf, sizeof(buf), "[%s][PID %d] ", czas_buf, (int)getpid());
    if (n < 0) return;

    size_t off = (size_t)n;
    if (off >= sizeof(buf)) off = sizeof(buf) - 1;

    va_list args;
    va_start(args, format);
    int m = vsnprintf(buf + off, sizeof(buf) - off, format, args);
    va_end(args);
    if (m < 0) return;

    size_t len = off + (size_t)m;
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;

    /* Dopilnuj newline */
    if (len == 0 || buf[len - 1] != "\n"[0]) {
        if (len < sizeof(buf) - 1) {
            buf[len++] = "\n"[0];
        } else {
            buf[sizeof(buf) - 1] = "\n"[0];
            len = sizeof(buf);
        }
    }

    int locked = (flock(STDERR_FILENO, LOCK_EX) == 0);

    /* write() best-effort z obsługą EINTR */
    size_t pos = 0;
    while (pos < len) {
        ssize_t w = write(STDERR_FILENO, buf + pos, len - pos);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        pos += (size_t)w;
    }

    if (locked) (void)flock(STDERR_FILENO, LOCK_UN);
}

void loguj_errno(const char *prefix) {
    int e = errno;
    loguj("%s: %s", prefix, strerror(e));
}

/* ============================================
 * WALIDACJA DANYCH
 * ============================================ */

int waliduj_liczbe(const char *str, int min, int max) {
    if (str == NULL || *str == '\0') {
        fprintf(stderr, "Błąd: pusta wartość\n");
        return -1;
    }
    
    char *endptr;
    errno = 0;
    long val = strtol(str, &endptr, 10);
    
    if (errno != 0) {
        perror("strtol");
        return -1;
    }
    
    if (*endptr != '\0') {
        fprintf(stderr, "Błąd: '%s' nie jest liczbą całkowitą\n", str);
        return -1;
    }
    
    if (val < min || val > max) {
        fprintf(stderr, "Błąd: wartość %ld poza zakresem [%d, %d]\n", val, min, max);
        return -1;
    }
    
    return (int)val;
}

int waliduj_argumenty(int argc, char *argv[], int *N, int *czas_symulacji) {
    /* Domyślne wartości */
    *N = N_LIMIT_TERENU;
    *czas_symulacji = CZAS_SYMULACJI;
    
    if (argc >= 2) {
         *N = waliduj_liczbe(argv[1], 1, N_LIMIT_TERENU_MAX);
        if (*N < 0) {
            fprintf(stderr, "Użycie: %s [N] [czas_symulacji]\n", argv[0]);
            fprintf(stderr, "  N - limit osób na terenie stacji (1-%d, domyślnie %d)\n", N_LIMIT_TERENU_MAX, N_LIMIT_TERENU);
            fprintf(stderr, "  czas_symulacji - czas w sekundach (1-3600, domyślnie %d)\n", CZAS_SYMULACJI);
            return -1;
        }
    }
    
    if (argc >= 3) {
        *czas_symulacji = waliduj_liczbe(argv[2], 1, 3600);
        if (*czas_symulacji < 0) {
            return -1;
        }
    }
    
    return 0;
}

/* ============================================
 * LOSOWANIE
 * ============================================ */

void inicjalizuj_losowanie(void) {
    /* Używamy PID + czas dla unikalności w każdym procesie */
    srand((unsigned int)(time(NULL) ^ getpid()));
}

int losuj_zakres(int min, int max) {
    if (min > max) {
        int tmp = min;
        min = max;
        max = tmp;
    }
    return min + rand() % (max - min + 1);
}

int losuj_procent(int procent) {
    if (procent <= 0) return 0;
    if (procent >= 100) return 1;
    return (rand() % 100) < procent;
}

TypKarnetu losuj_typ_karnetu(void) {
    int los = rand() % 100;
    
    /* Rozkład: 40% jednorazowy, 20% TK1, 15% TK2, 10% TK3, 15% dzienny */
    if (los < 40) return KARNET_JEDNORAZOWY;
    if (los < 60) return KARNET_TK1;
    if (los < 75) return KARNET_TK2;
    if (los < 85) return KARNET_TK3;
    return KARNET_DZIENNY;
}

static int mask_has_type(int mask, TypKarnetu typ) {
    switch (typ) {
        case KARNET_JEDNORAZOWY: return (mask & TICKET_MASK_JEDNORAZOWY) != 0;
        case KARNET_TK1:         return (mask & TICKET_MASK_TK1) != 0;
        case KARNET_TK2:         return (mask & TICKET_MASK_TK2) != 0;
        case KARNET_TK3:         return (mask & TICKET_MASK_TK3) != 0;
        case KARNET_DZIENNY:     return (mask & TICKET_MASK_DZIENNY) != 0;
        default:                 return 0;
    }
}

TypKarnetu losuj_typ_karnetu_mask(int mask) {
    if (mask == 0) mask = KASJER_TICKET_MASK_DEFAULT;
    if (mask == 0) mask = TICKET_MASK_ALL;

    /* Wagi jak w losuj_typ_karnetu(): 40/20/15/10/15 */
    struct { TypKarnetu typ; int w; } items[] = {
        {KARNET_JEDNORAZOWY, 40},
        {KARNET_TK1,         20},
        {KARNET_TK2,         15},
        {KARNET_TK3,         10},
        {KARNET_DZIENNY,     15},
    };

    int sum = 0;
    for (size_t i = 0; i < sizeof(items)/sizeof(items[0]); i++) {
        if (mask_has_type(mask, items[i].typ)) sum += items[i].w;
    }

    if (sum <= 0) {
        /* Jeśli ktoś podał maskę=0 lub błędną, wróć do pełnej puli */
        mask = TICKET_MASK_ALL;
        sum = 0;
        for (size_t i = 0; i < sizeof(items)/sizeof(items[0]); i++) sum += items[i].w;
    }

    int los = rand() % sum;
    int acc = 0;
    for (size_t i = 0; i < sizeof(items)/sizeof(items[0]); i++) {
        if (!mask_has_type(mask, items[i].typ)) continue;
        acc += items[i].w;
        if (los < acc) return items[i].typ;
    }

    /* Awaryjnie */
    return KARNET_JEDNORAZOWY;
}

static void str_to_lower(char *s) {
    for (; s && *s; s++) {
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
    }
}

static char *trim(char *s) {
    while (s && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
    if (!s) return s;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) end--;
    *end = '\0';
    return s;
}

int parse_ticket_mask(const char *s) {
    if (s == NULL) return -1;

    /* jeśli liczba */
    if (*s >= '0' && *s <= '9') {
        int v = waliduj_liczbe(s, 0, 31);
        return v;
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "%s", s);
    str_to_lower(buf);

    int mask = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",;|+", &save); tok != NULL; tok = strtok_r(NULL, ",;|+", &save)) {
        tok = trim(tok);
        if (*tok == '\0') continue;

        if (strcmp(tok, "all") == 0 || strcmp(tok, "wszystkie") == 0 || strcmp(tok, "pelne") == 0) {
            mask |= TICKET_MASK_ALL;
            continue;
        }
        if (strcmp(tok, "jednorazowy") == 0 || strcmp(tok, "jednorazowe") == 0 || strcmp(tok, "single") == 0) {
            mask |= TICKET_MASK_JEDNORAZOWY;
            continue;
        }
        if (strcmp(tok, "tk1") == 0) { mask |= TICKET_MASK_TK1; continue; }
        if (strcmp(tok, "tk2") == 0) { mask |= TICKET_MASK_TK2; continue; }
        if (strcmp(tok, "tk3") == 0) { mask |= TICKET_MASK_TK3; continue; }
        if (strcmp(tok, "dzienny") == 0 || strcmp(tok, "day") == 0) { mask |= TICKET_MASK_DZIENNY; continue; }

        /* nieznany token */
        return -1;
    }

    if (mask < 0 || mask > 31) return -1;
    return mask;
}

void format_ticket_mask(int mask, char *buf, size_t buflen) {
    if (!buf || buflen == 0) return;
    buf[0] = '\0';
    int first = 1;

    struct { int bit; const char *name; } items[] = {
        {TICKET_MASK_JEDNORAZOWY, "Jednorazowy"},
        {TICKET_MASK_TK1,         "TK1"},
        {TICKET_MASK_TK2,         "TK2"},
        {TICKET_MASK_TK3,         "TK3"},
        {TICKET_MASK_DZIENNY,     "Dzienny"},
    };

    for (size_t i = 0; i < sizeof(items)/sizeof(items[0]); i++) {
        if (mask & items[i].bit) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%s%s", first ? "" : ",", items[i].name);
            strncat(buf, tmp, buflen - strlen(buf) - 1);
            first = 0;
        }
    }

    if (first) {
        snprintf(buf, buflen, "(brak)");
    }
}

Trasa losuj_trase_rower(void) {
    int los = rand() % 100;
    
    /* Rozkład: 50% T1, 30% T2, 20% T3 */
    if (los < 50) return TRASA_T1;
    if (los < 80) return TRASA_T2;
    return TRASA_T3;
}

/* ============================================
 * FORMATOWANIE I KONWERSJA
 * ============================================ */

int pobierz_cene_karnetu(TypKarnetu typ) {
    switch (typ) {
        case KARNET_JEDNORAZOWY: return CENA_JEDNORAZOWY;
        case KARNET_TK1:         return CENA_TK1;
        case KARNET_TK2:         return CENA_TK2;
        case KARNET_TK3:         return CENA_TK3;
        case KARNET_DZIENNY:     return CENA_DZIENNY;
        default:                 return 0;
    }
}

int pobierz_waznosc_karnetu(TypKarnetu typ) {
    switch (typ) {
        case KARNET_JEDNORAZOWY: return WAZNOSC_JEDNORAZOWY;
        case KARNET_TK1:         return WAZNOSC_TK1;
        case KARNET_TK2:         return WAZNOSC_TK2;
        case KARNET_TK3:         return WAZNOSC_TK3;
        case KARNET_DZIENNY:     return WAZNOSC_DZIENNY;
        default:                 return 0;
    }
}

int pobierz_czas_trasy(Trasa trasa) {
    switch (trasa) {
        case TRASA_T1: return CZAS_T1;
        case TRASA_T2: return CZAS_T2;
        case TRASA_T3: return CZAS_T3;
        case TRASA_T4: return CZAS_T4;
        default:       return CZAS_T4;
    }
}

const char* nazwa_karnetu(TypKarnetu typ) {
    switch (typ) {
        case KARNET_JEDNORAZOWY: return "Jednorazowy";
        case KARNET_TK1:         return "TK1 (30min)";
        case KARNET_TK2:         return "TK2 (60min)";
        case KARNET_TK3:         return "TK3 (120min)";
        case KARNET_DZIENNY:     return "Dzienny";
        default:                 return "Nieznany";
    }
}

const char* nazwa_trasy(Trasa trasa) {
    switch (trasa) {
        case TRASA_T1: return "T1 (łatwa)";
        case TRASA_T2: return "T2 (średnia)";
        case TRASA_T3: return "T3 (trudna)";
        case TRASA_T4: return "T4 (piesza)";
        default:       return "Nieznana";
    }
}

void formatuj_czas(time_t czas, char *bufor) {
    struct tm *tm_info = localtime(&czas);
    strftime(bufor, 9, "%H:%M:%S", tm_info);
}

void formatuj_kwote(int grosze, char *bufor) {
    int zlote = grosze / 100;
    int gr = grosze % 100;
    sprintf(bufor, "%d.%02d zł", zlote, gr);
}

/* ============================================
 * OBLICZENIA
 * ============================================ */

int oblicz_cene_ze_znizka(int cena_gr, int wiek) {
    /* Zniżka 25% dla dzieci <10 lat i seniorów 65+ */
    if (wiek < WIEK_ZNIZKA_DZIECKO || wiek >= WIEK_ZNIZKA_SENIOR) {
        return cena_gr - (cena_gr * ZNIZKA_PROCENT / 100);
    }
    return cena_gr;
}

int czy_karnet_wazny(Karnet *karnet, time_t aktualny_czas) {
    if (karnet == NULL) return 0;
    if (!karnet->aktywny) return 0;
    
    /* GLOBALNA ZASADA: po zamknięciu stacji WSZYSTKIE karnety nieważne */
    if (g_shm != NULL && g_shm->czas_konca_dnia > 0) {
        if (aktualny_czas >= g_shm->czas_konca_dnia) {
            return 0; /* Po zamknięciu */
        }
    }
    
    /* Karnet jednorazowy - sprawdź czy już użyty */
    if (karnet->typ == KARNET_JEDNORAZOWY) {
        return !karnet->uzyty;
    }
    
    /* Karnet czasowy - sprawdź czy nie wygasł */
    if (karnet->czas_aktywacji == 0) {
        /* Jeszcze nieaktywowany - ważny (ale tylko przed zamknięciem) */
        return 1;
    }
    
    time_t czas_uplynal = aktualny_czas - karnet->czas_aktywacji;
    return czas_uplynal < karnet->czas_waznosci_sek;
}

int oblicz_miejsca_krzeselko(TypKlienta typ, int liczba_dzieci) {
    int miejsca = (typ == TYP_ROWERZYSTA) ? 2 : 1;
    miejsca += liczba_dzieci; /* każde dziecko = 1 miejsce */
    return miejsca;
}

/* ============================================
 * CZAS SYMULACJI
 * ============================================ */

time_t czas_symulacji(time_t czas_startu) {
    return time(NULL) - czas_startu;
}

int czy_koniec_symulacji(time_t czas_startu, int max_czas) {
    return czas_symulacji(czas_startu) >= max_czas;
}
