#ifndef IPC_H
#define IPC_H

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/prctl.h>
#include <signal.h>
#include "config.h"
#include "types.h"

/*
 * KOLEJ KRZESEŁKOWA - FUNKCJE IPC
 * Semafory, pamięć współdzielona, kolejki komunikatów
 */

/* ============================================
 * GLOBALNE IDENTYFIKATORY IPC
 * (ustawiane przez init_ipc lub attach_ipc)
 * ============================================ */
extern int g_sem_id;            // ID zestawu semaforów
extern int g_shm_id;            // ID pamięci współdzielonej
extern SharedMemory *g_shm;     // wskaźnik na pamięć współdzieloną
extern int g_mq_kasa;           // kolejka do kasy
extern int g_mq_kasa_odp;       // kolejka odpowiedzi z kasy
extern int g_mq_bramka;         // kolejka do bramek
extern int g_mq_prac;           // kolejka pracowników

/* ============================================
 * OCHRONA PROCESÓW POTOMNYCH
 * ============================================ */

/*
 * Ustawia proces potomny tak, aby dostał SIGTERM gdy rodzic umiera.
 * Wywołać NA POCZĄTKU każdego procesu potomnego!
 */
void ustaw_smierc_z_rodzicem(void);

/*
 * Sprawdza czy rodzic jeszcze żyje (getppid() != 1)
 * Zwraca: 1=żyje, 0=umarł (jesteśmy osieroceni)
 */
int czy_rodzic_zyje(void);

/* ============================================
 * INICJALIZACJA I CLEANUP (tylko main)
 * ============================================ */

/*
 * Tworzy wszystkie zasoby IPC
 * Wywołać TYLKO w procesie main!
 * N - limit osób na terenie (wartość początkowa semafora SEM_TEREN)
 * Zwraca: 0=OK, -1=błąd
 */
int init_ipc(int N);

/*
 * Usuwa wszystkie zasoby IPC
 * Wywołać TYLKO w procesie main przed zakończeniem!
 */
void cleanup_ipc(void);

/* ============================================
 * DOŁĄCZANIE DO IPC (procesy potomne)
 * ============================================ */

/*
 * Dołącza do istniejących zasobów IPC
 * Wywołać w każdym procesie potomnym po exec()
 * Zwraca: 0=OK, -1=błąd
 */
int attach_ipc(void);

/*
 * Odłącza od pamięci współdzielonej
 * Wywołać przed exit() w procesach potomnych
 */
void detach_ipc(void);

/* ============================================
 * OPERACJE NA SEMAFORACH
 * ============================================ */

/*
 * P() - wait/dekrementacja semafora
 * Blokuje jeśli wartość = 0
 * Zwraca: 0=OK, -1=przerwane sygnałem, -2=IPC usunięte
 */
int sem_wait_ipc(int sem_num);

/*
 * V() - signal/inkrementacja semafora
 */
void sem_signal_ipc(int sem_num);

/*
 * P() dla N jednostek (np. dla grup)
 * Zwraca: 0=OK, -1=przerwane sygnałem, -2=IPC usunięte
 */
int sem_wait_n(int sem_num, int n);

/*
 * V() dla N jednostek
 */
void sem_signal_n(int sem_num, int n);

/*
 * Próba P() bez blokowania
 * Zwraca: 1=udało się, 0=semafor=0
 */
int sem_trywait_ipc(int sem_num);

/*
 * Pobiera aktualną wartość semafora
 */
int sem_getval_ipc(int sem_num);

/* ============================================
 * OPERACJE NA KOLEJKACH KOMUNIKATÓW
 * ============================================ */

/*
 * Wysyła komunikat do kolejki
 * Blokuje jeśli kolejka pełna
 */
int msg_send(int mq_id, void *msg, size_t size);

/*
 * Wysyła komunikat bez blokowania
 * Zwraca: 0=OK, -1=błąd (kolejka pełna)
 */
int msg_send_nowait(int mq_id, void *msg, size_t size);

/*
 * Odbiera komunikat z kolejki (blokujący)
 * mtype: 0=dowolny, >0=konkretny typ, <0=priorytet
 */
int msg_recv(int mq_id, void *msg, size_t size, long mtype);

/*
 * Odbiera komunikat bez blokowania
 * Zwraca: >0=OK (rozmiar), -1=brak wiadomości
 */
int msg_recv_nowait(int mq_id, void *msg, size_t size, long mtype);

/* ============================================
 * BEZPIECZNY DOSTĘP DO PAMIĘCI WSPÓŁDZIELONEJ
 * ============================================ */

/*
 * Blokuje mutex SHM, wykonuje operację, odblokowuje
 * Makra dla wygody
 */
#define MUTEX_SHM_LOCK()   sem_wait_ipc(SEM_MUTEX_SHM)
#define MUTEX_SHM_UNLOCK() sem_signal_ipc(SEM_MUTEX_SHM)

/*
 * Bezpieczny odczyt zmiennej z shm
 */
#define SHM_READ(var) ({ \
    MUTEX_SHM_LOCK(); \
    typeof(g_shm->var) _tmp = g_shm->var; \
    MUTEX_SHM_UNLOCK(); \
    _tmp; \
})

/*
 * Bezpieczny zapis zmiennej do shm
 */
#define SHM_WRITE(var, val) do { \
    MUTEX_SHM_LOCK(); \
    g_shm->var = (val); \
    MUTEX_SHM_UNLOCK(); \
} while(0)

/*
 * Bezpieczna inkrementacja
 */
#define SHM_INC(var) do { \
    MUTEX_SHM_LOCK(); \
    g_shm->var++; \
    MUTEX_SHM_UNLOCK(); \
} while(0)

/*
 * Bezpieczna dekrementacja
 */
#define SHM_DEC(var) do { \
    MUTEX_SHM_LOCK(); \
    g_shm->var--; \
    MUTEX_SHM_UNLOCK(); \
} while(0)

/* ============================================
 * FUNKCJE POMOCNICZE DLA KARNETÓW
 * ============================================ */

/*
 * Tworzy nowy karnet w pamięci współdzielonej
 * Zwraca: ID karnetu lub -1 przy błędzie
 */
int utworz_karnet(TypKarnetu typ, int cena_gr, int vip);

/*
 * Pobiera wskaźnik do karnetu (bezpieczne)
 * Zwraca: wskaźnik lub NULL
 */
Karnet* pobierz_karnet(int id_karnetu);

/*
 * Aktywuje karnet (ustawia czas_aktywacji)
 */
void aktywuj_karnet(int id_karnetu);

/*
 * Oznacza karnet jednorazowy jako użyty
 */
void uzyj_karnet_jednorazowy(int id_karnetu);

/* ============================================
 * FUNKCJE POMOCNICZE DLA LOGÓW
 * ============================================ */

/*
 * Dodaje wpis do logu przejść
 */
void dodaj_log(int id_karnetu, TypLogu typ, int numer_bramki);

/* ============================================
 * OBSŁUGA AWARII
 * ============================================ */

/*
 * Czeka na wznowienie po awarii (blokuje na semaforze)
 * Rejestruje się w liczniku, czeka, wyrejestrowuje
 * Wywołać gdy g_shm->awaria == 1
 */
void czekaj_na_wznowienie(const char *kto);

/*
 * Odblokuj wszystkich czekających na wznowienie
 * Wywołać w main przy SIGUSR2 (START)
 */
void odblokuj_czekajacych(void);

#endif /* IPC_H */
