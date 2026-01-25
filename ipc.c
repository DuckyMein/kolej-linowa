#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/prctl.h>
#include <signal.h>
#include "ipc.h"
#include "utils.h"

/*
 * KOLEJ KRZESEŁKOWA - IMPLEMENTACJA IPC
 */

/* ============================================
 * ZMIENNE GLOBALNE
 * ============================================ */
int g_sem_id = -1;
int g_shm_id = -1;
SharedMemory *g_shm = NULL;
int g_mq_kasa = -1;
int g_mq_kasa_odp = -1;
int g_mq_bramka = -1;
int g_mq_bramka_odp = -1;  /* NOWA - osobna kolejka odpowiedzi bramek */
int g_mq_prac = -1;

/* Klucz bazowy (ustawiany przy init) */
static key_t g_klucz_bazowy = -1;

/* PID rodzica (zapisany przy starcie) */
static pid_t g_parent_pid = 0;

/* Unia dla semctl (wymagana przez niektóre systemy) */
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

/* ============================================
 * OCHRONA PROCESÓW POTOMNYCH
 * ============================================ */

void ustaw_smierc_z_rodzicem(void) {
    /* Zapisz PID rodzica */
    g_parent_pid = getppid();
    
    /* Ustaw aby dostać SIGTERM gdy rodzic umrze */
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) == -1) {
        blad_ostrzezenie("prctl PR_SET_PDEATHSIG");
    }
    
    /* Sprawdź czy rodzic nie umarł w międzyczasie (race condition) */
    if (getppid() != g_parent_pid) {
        loguj("Rodzic umarł podczas startu - kończę");
        exit(EXIT_SUCCESS);
    }
}

int czy_rodzic_zyje(void) {
    /* Sprawdź czy główny proces (main) jeszcze żyje */
    /* Używamy g_shm->pid_main zamiast getppid() bo niektóre procesy */
    /* są dziećmi generatora, nie maina */
    if (g_shm == NULL) return 0;
    
    pid_t main_pid = g_shm->pid_main;
    if (main_pid <= 0) return 0;
    
    /* kill z sygnałem 0 sprawdza tylko czy proces istnieje */
    if (kill(main_pid, 0) == -1) {
        if (errno == ESRCH) {
            /* Proces nie istnieje */
            return 0;
        }
    }
    return 1;
}

/* ============================================
 * FUNKCJE POMOCNICZE (prywatne)
 * ============================================ */

static key_t generuj_klucz(int offset) {
    if (g_klucz_bazowy == -1) {
        g_klucz_bazowy = ftok(".", IPC_KEY_BASE);
        if (g_klucz_bazowy == -1) {
            blad_krytyczny("ftok");
        }
    }
    return g_klucz_bazowy + offset;
}

/* ============================================
 * INICJALIZACJA IPC (tylko main)
 * ============================================ */

int init_ipc(int N) {
    loguj("Inicjalizacja IPC (N=%d)...", N);
    
    /* 1. Generuj klucz bazowy */
    g_klucz_bazowy = ftok(".", IPC_KEY_BASE);
    if (g_klucz_bazowy == -1) {
        blad_ostrzezenie("ftok");
        return -1;
    }
    loguj("Klucz bazowy: 0x%x", g_klucz_bazowy);
    
    /* 2. Utwórz semafory */
    g_sem_id = semget(generuj_klucz(IPC_KEY_SEM), SEM_COUNT, IPC_CREAT | IPC_EXCL | IPC_PERMS);
    if (g_sem_id == -1) {
        if (errno == EEXIST) {
            loguj("Semafory już istnieją - próba usunięcia...");
            g_sem_id = semget(generuj_klucz(IPC_KEY_SEM), SEM_COUNT, IPC_PERMS);
            if (g_sem_id != -1) {
                semctl(g_sem_id, 0, IPC_RMID);
            }
            g_sem_id = semget(generuj_klucz(IPC_KEY_SEM), SEM_COUNT, IPC_CREAT | IPC_EXCL | IPC_PERMS);
        }
        if (g_sem_id == -1) {
            blad_ostrzezenie("semget");
            return -1;
        }
    }
    
    /* Inicjalizuj wartości semaforów */
    unsigned short sem_init_vals[SEM_COUNT] = {
        [SEM_TEREN]          = N,    // limit osób
        [SEM_MUTEX_SHM]      = 1,    // mutex
        [SEM_MUTEX_KASA]     = 1,    // mutex
        [SEM_MUTEX_LOG]      = 1,    // mutex
        [SEM_PERON]          = KRZESLA_W_RZEDZIE, // miejsca w rzędzie
        [SEM_PRACOWNIK1]     = 0,    // sygnalizacja
        [SEM_PRACOWNIK2]     = 0,    // sygnalizacja
        [SEM_GOTOWY_P1]      = 0,    // gotowość
        [SEM_GOTOWY_P2]      = 0,    // gotowość
        [SEM_KONIEC]         = 0,    // zakończenie
        [SEM_BARIERA_AWARIA] = 0     // bariera awarii (procesy czekają tu)
    };
    
    union semun arg;
    arg.array = sem_init_vals;
    if (semctl(g_sem_id, 0, SETALL, arg) == -1) {
        blad_ostrzezenie("semctl SETALL");
        return -1;
    }
    loguj("Semafory utworzone (id=%d)", g_sem_id);
    
    /* 3. Utwórz pamięć współdzieloną */
    g_shm_id = shmget(generuj_klucz(IPC_KEY_SHM), sizeof(SharedMemory), IPC_CREAT | IPC_EXCL | IPC_PERMS);
    if (g_shm_id == -1) {
        if (errno == EEXIST) {
            loguj("Pamięć współdzielona już istnieje - próba usunięcia...");
            g_shm_id = shmget(generuj_klucz(IPC_KEY_SHM), sizeof(SharedMemory), IPC_PERMS);
            if (g_shm_id != -1) {
                shmctl(g_shm_id, IPC_RMID, NULL);
            }
            g_shm_id = shmget(generuj_klucz(IPC_KEY_SHM), sizeof(SharedMemory), IPC_CREAT | IPC_EXCL | IPC_PERMS);
        }
        if (g_shm_id == -1) {
            blad_ostrzezenie("shmget");
            return -1;
        }
    }
    
    g_shm = (SharedMemory *)shmat(g_shm_id, NULL, 0);
    if (g_shm == (void *)-1) {
        blad_ostrzezenie("shmat");
        g_shm = NULL;
        return -1;
    }
    
    /* Wyzeruj pamięć */
    memset(g_shm, 0, sizeof(SharedMemory));
    g_shm->kolej_aktywna = 1;
    g_shm->czas_startu = time(NULL);
    g_shm->nastepny_id_karnetu = 1;
    g_shm->nastepny_id_klienta = 1;
    g_shm->pid_main = getpid();
    
    loguj("Pamięć współdzielona utworzona (id=%d, size=%zu)", g_shm_id, sizeof(SharedMemory));
    
    /* 4. Utwórz kolejki komunikatów */
    g_mq_kasa = msgget(generuj_klucz(IPC_KEY_MQ_KASA), IPC_CREAT | IPC_EXCL | IPC_PERMS);
    if (g_mq_kasa == -1 && errno == EEXIST) {
        g_mq_kasa = msgget(generuj_klucz(IPC_KEY_MQ_KASA), IPC_PERMS);
        if (g_mq_kasa != -1) msgctl(g_mq_kasa, IPC_RMID, NULL);
        g_mq_kasa = msgget(generuj_klucz(IPC_KEY_MQ_KASA), IPC_CREAT | IPC_EXCL | IPC_PERMS);
    }
    if (g_mq_kasa == -1) {
        blad_ostrzezenie("msgget kasa");
        return -1;
    }
    
    g_mq_kasa_odp = msgget(generuj_klucz(IPC_KEY_MQ_KASA_ODP), IPC_CREAT | IPC_EXCL | IPC_PERMS);
    if (g_mq_kasa_odp == -1 && errno == EEXIST) {
        g_mq_kasa_odp = msgget(generuj_klucz(IPC_KEY_MQ_KASA_ODP), IPC_PERMS);
        if (g_mq_kasa_odp != -1) msgctl(g_mq_kasa_odp, IPC_RMID, NULL);
        g_mq_kasa_odp = msgget(generuj_klucz(IPC_KEY_MQ_KASA_ODP), IPC_CREAT | IPC_EXCL | IPC_PERMS);
    }
    if (g_mq_kasa_odp == -1) {
        blad_ostrzezenie("msgget kasa_odp");
        return -1;
    }
    
    g_mq_bramka = msgget(generuj_klucz(IPC_KEY_MQ_BRAMKA), IPC_CREAT | IPC_EXCL | IPC_PERMS);
    if (g_mq_bramka == -1 && errno == EEXIST) {
        g_mq_bramka = msgget(generuj_klucz(IPC_KEY_MQ_BRAMKA), IPC_PERMS);
        if (g_mq_bramka != -1) msgctl(g_mq_bramka, IPC_RMID, NULL);
        g_mq_bramka = msgget(generuj_klucz(IPC_KEY_MQ_BRAMKA), IPC_CREAT | IPC_EXCL | IPC_PERMS);
    }
    if (g_mq_bramka == -1) {
        blad_ostrzezenie("msgget bramka");
        return -1;
    }
    
    /* NOWA - osobna kolejka odpowiedzi bramek (mniej kontencji) */
    g_mq_bramka_odp = msgget(generuj_klucz(IPC_KEY_MQ_BRAMKA_ODP), IPC_CREAT | IPC_EXCL | IPC_PERMS);
    if (g_mq_bramka_odp == -1 && errno == EEXIST) {
        g_mq_bramka_odp = msgget(generuj_klucz(IPC_KEY_MQ_BRAMKA_ODP), IPC_PERMS);
        if (g_mq_bramka_odp != -1) msgctl(g_mq_bramka_odp, IPC_RMID, NULL);
        g_mq_bramka_odp = msgget(generuj_klucz(IPC_KEY_MQ_BRAMKA_ODP), IPC_CREAT | IPC_EXCL | IPC_PERMS);
    }
    if (g_mq_bramka_odp == -1) {
        blad_ostrzezenie("msgget bramka_odp");
        return -1;
    }
    
    g_mq_prac = msgget(generuj_klucz(IPC_KEY_MQ_PRAC), IPC_CREAT | IPC_EXCL | IPC_PERMS);
    if (g_mq_prac == -1 && errno == EEXIST) {
        g_mq_prac = msgget(generuj_klucz(IPC_KEY_MQ_PRAC), IPC_PERMS);
        if (g_mq_prac != -1) msgctl(g_mq_prac, IPC_RMID, NULL);
        g_mq_prac = msgget(generuj_klucz(IPC_KEY_MQ_PRAC), IPC_CREAT | IPC_EXCL | IPC_PERMS);
    }
    if (g_mq_prac == -1) {
        blad_ostrzezenie("msgget prac");
        return -1;
    }
    
    loguj("Kolejki komunikatów utworzone (kasa=%d, odp=%d, bramka=%d, bramka_odp=%d, prac=%d)",
          g_mq_kasa, g_mq_kasa_odp, g_mq_bramka, g_mq_bramka_odp, g_mq_prac);
    
    loguj("Inicjalizacja IPC zakończona pomyślnie");
    return 0;
}

/* ============================================
 * CLEANUP IPC (tylko main)
 * ============================================ */

void cleanup_ipc(void) {
    loguj("Czyszczenie zasobów IPC...");
    
    /* Odłącz pamięć współdzieloną */
    if (g_shm != NULL) {
        if (shmdt(g_shm) == -1) {
            blad_ostrzezenie("shmdt");
        }
        g_shm = NULL;
    }
    
    /* Usuń pamięć współdzieloną */
    if (g_shm_id != -1) {
        if (shmctl(g_shm_id, IPC_RMID, NULL) == -1) {
            blad_ostrzezenie("shmctl IPC_RMID");
        }
        loguj("Pamięć współdzielona usunięta");
        g_shm_id = -1;
    }
    
    /* Usuń semafory */
    if (g_sem_id != -1) {
        if (semctl(g_sem_id, 0, IPC_RMID) == -1) {
            blad_ostrzezenie("semctl IPC_RMID");
        }
        loguj("Semafory usunięte");
        g_sem_id = -1;
    }
    
    /* Usuń kolejki komunikatów */
    if (g_mq_kasa != -1) {
        msgctl(g_mq_kasa, IPC_RMID, NULL);
        g_mq_kasa = -1;
    }
    if (g_mq_kasa_odp != -1) {
        msgctl(g_mq_kasa_odp, IPC_RMID, NULL);
        g_mq_kasa_odp = -1;
    }
    if (g_mq_bramka != -1) {
        msgctl(g_mq_bramka, IPC_RMID, NULL);
        g_mq_bramka = -1;
    }
    if (g_mq_bramka_odp != -1) {
        msgctl(g_mq_bramka_odp, IPC_RMID, NULL);
        g_mq_bramka_odp = -1;
    }
    if (g_mq_prac != -1) {
        msgctl(g_mq_prac, IPC_RMID, NULL);
        g_mq_prac = -1;
    }
    loguj("Kolejki komunikatów usunięte");
    
    loguj("Czyszczenie IPC zakończone");
}

/* ============================================
 * DOŁĄCZANIE DO IPC (procesy potomne)
 * ============================================ */

int attach_ipc(void) {
    /* Generuj klucz bazowy */
    g_klucz_bazowy = ftok(".", IPC_KEY_BASE);
    if (g_klucz_bazowy == -1) {
        blad_ostrzezenie("ftok (attach)");
        return -1;
    }
    
    /* Pobierz semafory */
    g_sem_id = semget(generuj_klucz(IPC_KEY_SEM), SEM_COUNT, 0);
    if (g_sem_id == -1) {
        blad_ostrzezenie("semget (attach)");
        return -1;
    }
    
    /* Pobierz pamięć współdzieloną */
    g_shm_id = shmget(generuj_klucz(IPC_KEY_SHM), sizeof(SharedMemory), 0);
    if (g_shm_id == -1) {
        blad_ostrzezenie("shmget (attach)");
        return -1;
    }
    
    g_shm = (SharedMemory *)shmat(g_shm_id, NULL, 0);
    if (g_shm == (void *)-1) {
        blad_ostrzezenie("shmat (attach)");
        g_shm = NULL;
        return -1;
    }
    
    /* Pobierz kolejki komunikatów */
    g_mq_kasa = msgget(generuj_klucz(IPC_KEY_MQ_KASA), 0);
    g_mq_kasa_odp = msgget(generuj_klucz(IPC_KEY_MQ_KASA_ODP), 0);
    g_mq_bramka = msgget(generuj_klucz(IPC_KEY_MQ_BRAMKA), 0);
    g_mq_bramka_odp = msgget(generuj_klucz(IPC_KEY_MQ_BRAMKA_ODP), 0);
    g_mq_prac = msgget(generuj_klucz(IPC_KEY_MQ_PRAC), 0);
    
    if (g_mq_kasa == -1 || g_mq_kasa_odp == -1 || 
        g_mq_bramka == -1 || g_mq_bramka_odp == -1 || g_mq_prac == -1) {
        blad_ostrzezenie("msgget (attach)");
        return -1;
    }
    
    return 0;
}

void detach_ipc(void) {
    if (g_shm != NULL) {
        shmdt(g_shm);
        g_shm = NULL;
    }
}

/* ============================================
 * OPERACJE NA SEMAFORACH
 * ============================================ */

/* Mutex z SEM_UNDO - automatyczne odkręcenie przy śmierci procesu */
int mutex_lock(int sem_num) {
    if (g_sem_id == -1) return -2;
    
    struct sembuf op = {sem_num, -1, SEM_UNDO};
    if (semop(g_sem_id, &op, 1) == -1) {
        if (errno == EINTR) return -1;
        if (errno == EIDRM || errno == EINVAL) return -2;
        return -2;
    }
    return 0;
}

void mutex_unlock(int sem_num) {
    if (g_sem_id == -1) return;
    
    struct sembuf op = {sem_num, +1, SEM_UNDO};
    while (semop(g_sem_id, &op, 1) == -1) {
        if (errno == EINTR) continue;
        if (errno == EIDRM || errno == EINVAL) return;
        return;
    }
}

/* Zwraca: 0=OK, -1=przerwane sygnałem, -2=IPC usunięte */
int sem_wait_ipc(int sem_num) {
    if (g_sem_id == -1) return -2;
    
    struct sembuf op = {sem_num, -1, 0};
    if (semop(g_sem_id, &op, 1) == -1) {
        if (errno == EINTR) return -1; /* Przerwane sygnałem */
        if (errno == EIDRM || errno == EINVAL) return -2; /* IPC usunięte */
        blad_ostrzezenie("semop P()");
        return -2;
    }
    return 0; /* Sukces */
}

void sem_signal_ipc(int sem_num) {
    if (g_sem_id == -1) return;
    
    struct sembuf op = {sem_num, +1, 0};
    while (semop(g_sem_id, &op, 1) == -1) {
        if (errno == EINTR) continue;
        if (errno == EIDRM || errno == EINVAL) return;
        blad_ostrzezenie("semop V()");
        return;
    }
}

/* Zwraca: 0=OK, -1=przerwane sygnałem, -2=IPC usunięte */
int sem_wait_n(int sem_num, int n) {
    if (g_sem_id == -1 || n <= 0) return -2;
    
    /* BEZ SEM_UNDO - bramka przekazuje własność semafora klientowi */
    struct sembuf op = {sem_num, -n, 0};
    if (semop(g_sem_id, &op, 1) == -1) {
        if (errno == EINTR) return -1; /* Przerwane sygnałem */
        if (errno == EIDRM || errno == EINVAL) return -2; /* IPC usunięte */
        blad_ostrzezenie("semop P(n)");
        return -2;
    }
    return 0; /* Sukces */
}

void sem_signal_n(int sem_num, int n) {
    if (g_sem_id == -1 || n <= 0) return;
    
    struct sembuf op = {sem_num, +n, 0};
    while (semop(g_sem_id, &op, 1) == -1) {
        if (errno == EINTR) continue;
        if (errno == EIDRM || errno == EINVAL) return;
        blad_ostrzezenie("semop V(n)");
        return;
    }
}

int sem_trywait_ipc(int sem_num) {
    if (g_sem_id == -1) return 0;
    
    struct sembuf op = {sem_num, -1, IPC_NOWAIT};
    if (semop(g_sem_id, &op, 1) == -1) {
        if (errno == EAGAIN) return 0; /* semafor = 0 */
        if (errno == EINTR) return 0;  /* przerwane */
        if (errno == EIDRM || errno == EINVAL) return 0;
        blad_ostrzezenie("semop tryP()");
        return 0;
    }
    return 1;
}

int sem_getval_ipc(int sem_num) {
    if (g_sem_id == -1) return -1;
    
    int val = semctl(g_sem_id, sem_num, GETVAL);
    if (val == -1) {
        if (errno != EIDRM && errno != EINVAL) {
            blad_ostrzezenie("semctl GETVAL");
        }
        return -1;
    }
    return val;
}

/* ============================================
 * OPERACJE NA KOLEJKACH KOMUNIKATÓW
 * ============================================ */

int msg_send(int mq_id, void *msg, size_t size) {
    while (msgsnd(mq_id, msg, size - sizeof(long), 0) == -1) {
        /* BEZ retry na EINTR: pozwól procesom zakończyć się po SIGTERM/SIGINT.
         * Jeśli syscall został przerwany sygnałem, zwróć do wywołującego,
         * żeby mógł sprawdzić flagi końca dnia / g_koniec. */
        if (errno == EINTR) return -1;
        if (errno == EINVAL || errno == EIDRM) return -2; /* IPC usunięte */
        blad_ostrzezenie("msgsnd");
        return -1;
    }
    return 0;
}

int msg_send_nowait(int mq_id, void *msg, size_t size) {
    if (msgsnd(mq_id, msg, size - sizeof(long), IPC_NOWAIT) == -1) {
        if (errno == EAGAIN) return -1; /* kolejka pełna */
        if (errno == EINTR) return -1;
        if (errno == EINVAL || errno == EIDRM) return -2; /* IPC usunięte */
        blad_ostrzezenie("msgsnd nowait");
        return -1;
    }
    return 0;
}

int msg_recv(int mq_id, void *msg, size_t size, long mtype) {
    ssize_t ret;
    while ((ret = msgrcv(mq_id, msg, size - sizeof(long), mtype, 0)) == -1) {
        if (errno == EINTR) return -1; /* Przerwane sygnałem - pozwól sprawdzić g_koniec */
        if (errno == EINVAL || errno == EIDRM) return -2; /* IPC usunięte */
        blad_ostrzezenie("msgrcv");
        return -1;
    }
    return (int)ret;
}

int msg_recv_nowait(int mq_id, void *msg, size_t size, long mtype) {
    ssize_t ret = msgrcv(mq_id, msg, size - sizeof(long), mtype, IPC_NOWAIT);
    if (ret == -1) {
        if (errno == ENOMSG || errno == EAGAIN) return -1; /* brak wiadomości */
        if (errno == EINTR) return -1;
        /* IPC usunięte - zwróć -2 bez ostrzeżenia (normalne przy cleanup) */
        if (errno == EINVAL || errno == EIDRM) return -2;
        blad_ostrzezenie("msgrcv nowait");
        return -1;
    }
    return (int)ret;
}

/* ============================================
 * FUNKCJE POMOCNICZE DLA KARNETÓW - O(1) dostęp
 * ID karnetu = index + 1 (nigdy nie usuwamy karnetów)
 * ============================================ */

int utworz_karnet(TypKarnetu typ, int cena_gr, int vip) {
    MUTEX_SHM_LOCK();
    
    if (g_shm->liczba_karnetow >= MAX_KARNETOW) {
        MUTEX_SHM_UNLOCK();
        return -1;  /* Bez logowania w hot-path */
    }
    
    int idx = g_shm->liczba_karnetow++;
    int id = idx + 1;  /* ID = index + 1 (O(1) dostęp) */
    
    Karnet *k = &g_shm->karnety[idx];
    k->id = id;
    k->typ = typ;
    k->czas_waznosci_sek = pobierz_waznosc_karnetu(typ);
    k->czas_aktywacji = 0;
    k->cena_gr = cena_gr;
    k->uzyty = 0;
    k->vip = vip;
    k->aktywny = 1;
    
    /* Aktualizuj statystyki */
    g_shm->stats.sprzedane_karnety[typ - 1]++;
    g_shm->stats.przychod_gr += cena_gr;
    
    MUTEX_SHM_UNLOCK();
    
    return id;
}

/* O(1) dostęp - idx = id - 1, BEZ mutexa (tylko odczyt) */
Karnet* pobierz_karnet(int id_karnetu) {
    if (id_karnetu <= 0 || id_karnetu > g_shm->liczba_karnetow) return NULL;
    return &g_shm->karnety[id_karnetu - 1];
}

/* O(1) dostęp - mutex tylko przy zapisie */
void aktywuj_karnet(int id_karnetu) {
    if (id_karnetu <= 0 || id_karnetu > g_shm->liczba_karnetow) return;
    
    int idx = id_karnetu - 1;
    Karnet *k = &g_shm->karnety[idx];
    
    /* Sprawdź bez mutexa czy już aktywowany */
    if (k->czas_aktywacji != 0) return;
    
    MUTEX_SHM_LOCK();
    
    /* Double-check po wzięciu mutexa */
    if (k->czas_aktywacji == 0) {
        time_t teraz = time(NULL);
        k->czas_aktywacji = teraz;
        
        /* UCINANIE DO KOŃCA DNIA */
        if (g_shm->czas_konca_dnia > 0 && k->typ != KARNET_JEDNORAZOWY) {
            int pozostalo = (int)(g_shm->czas_konca_dnia - teraz);
            if (pozostalo < 0) pozostalo = 0;
            
            if (pozostalo < k->czas_waznosci_sek) {
                k->czas_waznosci_sek = pozostalo;
            }
        }
    }
    
    MUTEX_SHM_UNLOCK();
}

/* O(1) dostęp */
void uzyj_karnet_jednorazowy(int id_karnetu) {
    if (id_karnetu <= 0 || id_karnetu > g_shm->liczba_karnetow) return;
    g_shm->karnety[id_karnetu - 1].uzyty = 1;  /* Atomic write, bez mutexa */
}

/* ============================================
 * FUNKCJE POMOCNICZE DLA LOGÓW - ATOMOWE
 * ============================================ */

void dodaj_log(int id_karnetu, TypLogu typ, int numer_bramki) {
    /* Atomowe indeksowanie - BEZ mutexa (unika thundering herd) */
    int idx = __sync_fetch_and_add(&g_shm->liczba_logow, 1);
    
    if (idx < MAX_LOGOW) {
        LogEntry *log = &g_shm->logi[idx];
        log->id_karnetu = id_karnetu;
        log->typ_bramki = typ;
        log->numer_bramki = numer_bramki;
        log->czas = time(NULL);
    }
    /* Jeśli idx >= MAX_LOGOW, log jest "zgubiony" - to OK przy przepełnieniu */
}

/* ============================================
 * OBSŁUGA AWARII
 * ============================================ */

void czekaj_na_wznowienie(const char *kto) {
    /* Zarejestruj się jako czekający */
    MUTEX_SHM_LOCK();
    g_shm->czekajacych_na_wznowienie++;
    int numer = g_shm->czekajacych_na_wznowienie;
    MUTEX_SHM_UNLOCK();
    
    loguj("%s: Awaria - czekam na wznowienie (pozycja %d)", kto, numer);
    
    /* Czekaj na semaforze (blokujące) */
    sem_wait_ipc(SEM_BARIERA_AWARIA);
    
    loguj("%s: Wznowiono - kontynuuję", kto);
}

void odblokuj_czekajacych(void) {
    MUTEX_SHM_LOCK();
    int ile = g_shm->czekajacych_na_wznowienie;
    g_shm->czekajacych_na_wznowienie = 0;
    MUTEX_SHM_UNLOCK();
    
    if (ile > 0) {
        loguj("Odblokowuję %d czekających procesów", ile);
        sem_signal_n(SEM_BARIERA_AWARIA, ile);
    }
}
