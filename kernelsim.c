/*
 * Trabalho 1 - INF1316 - Sistemas Operacionais
 *
 * Rodrigo Carvalho       2510091
 *
 * kernelsim.c: KernelSim, o nucleo simulado.
 *
 * Cria a memoria compartilhada, os processos de aplicacao A1..An (3 a 6) e o
 * InterControllerSim (fork + exec). Depois fica num laco infinito esperando a
 * proxima "interrupcao" com sigwaitinfo():
 *
 *   SIG_IRQ0    (InterController) fim do time slice: SIGSTOP no processo atual,
 *               que volta para a fila de prontos, e SIGCONT no proximo.
 *   SIG_IRQ1    (InterController) fim do I/O em D1: o primeiro da fila de D1
 *               vai para a fila de prontos com prioridade.
 *   SIG_SYSCALL (aplicacao) syscall(D1, R|W): SIGSTOP no processo, que vai
 *               para a fila de bloqueados de D1; outro processo assume a CPU.
 *   SIGCHLD     um processo de aplicacao terminou as suas MAX iteracoes.
 *   SIGINT      Ctrl+C: encerra tudo e libera a memoria compartilhada.
 *
 * Os sinais ficam bloqueados e sao tratados um de cada vez no laco principal
 * (nao ha tratadores assincronos), entao as filas nunca sao alteradas por
 * dois eventos ao mesmo tempo.
 *
 * Troca de contexto: o contexto de um processo e o seu PC e, se ele parou por
 * causa de uma syscall, os parametros dela (dispositivo e operacao). Ao tirar
 * o processo da CPU o kernel copia esses valores da memoria compartilhada para
 * a sua tabela de processos (salvarContexto); antes do SIGCONT copia o PC de
 * volta (restaurarContexto).
 *
 * Uso: kernelsim [numero_de_processos]   (3 a 6, padrao 4)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include "comum.h"

/* ---------- processos a executar (ja "lidos pela shell") ---------- */

typedef struct {
    const char *nome;
    int max;
    const char *syscalls[MAX_SYSCALLS];   /* "pc:R" ou "pc:W" */
} ConfigProc;

static const ConfigProc CONFIG[MAX_PROCS] = {
    { "A1", 8, { "2:R", "6:W" } },
    { "A2", 6, { "3:W" } },
    { "A3", 7, { "1:R", "4:R" } },
    { "A4", 5, { NULL } },              /* so usa CPU */
    { "A5", 6, { "2:W" } },
    { "A6", 4, { "3:R" } },
};

/* ---------- tabela de processos (PCB) ---------- */

typedef enum { PRONTO, EXECUTANDO, BLOQUEADO, TERMINADO } Estado;

typedef struct {
    char   nome[4];
    pid_t  pid;
    Estado estado;
    int    max;
    int    pc;               /* contexto salvo: PC                  */
    char   dispositivo[3];   /* contexto salvo: parametros da syscall */
    char   operacao;
} PCB;

static PCB tabela[MAX_PROCS];
static int nprocs;
static int atual = -1;       /* processo na CPU; -1 = CPU ociosa */
static struct timespec inicio;   /* instante em que o KernelSim comecou */
static int terminados = 0;

static int shmid = -1;
static MemCompartilhada *shm;
static pid_t pid_inter = -1;

/* ---------- filas ---------- */

typedef struct {
    int itens[MAX_PROCS];
    int tam;
} Fila;

static Fila prontos_io;   /* prontos que acabaram de sair de I/O (prioridade) */
static Fila prontos;      /* prontos que perderam a CPU por fim de time slice */
static Fila fila_d1;      /* bloqueados esperando D1, em ordem de pedido      */

static void inserir(Fila *f, int p) { f->itens[f->tam++] = p; }

static int remover(Fila *f) {
    int p = f->itens[0];
    for (int i = 1; i < f->tam; i++) f->itens[i - 1] = f->itens[i];
    f->tam--;
    return p;
}

static void retirar(Fila *f, int p) {
    for (int i = 0; i < f->tam; i++) {
        if (f->itens[i] == p) {
            for (int j = i + 1; j < f->tam; j++) f->itens[j - 1] = f->itens[j];
            f->tam--;
            return;
        }
    }
}

static void imprimirFila(const char *nome, Fila *f, int com_op) {
    printf("%s[", nome);
    for (int i = 0; i < f->tam; i++) {
        PCB *p = &tabela[f->itens[i]];
        if (com_op) printf("%s%s:%c", i ? " " : "", p->nome, p->operacao);
        else        printf("%s%s", i ? " " : "", p->nome);
    }
    printf("]");
}

/* ---------- troca de contexto ---------- */

static void salvarContexto(int p) {
    tabela[p].pc = shm->proc[p].pc;
    memcpy(tabela[p].dispositivo, shm->proc[p].dispositivo, sizeof tabela[p].dispositivo);
    tabela[p].operacao = shm->proc[p].operacao;
}

static void restaurarContexto(int p) {
    shm->proc[p].pc = tabela[p].pc;
}

/* segundos desde o inicio da simulacao */
static double agora(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - inicio.tv_sec) + (t.tv_nsec - inicio.tv_nsec) / 1e9;
}

static void marcarTerminado(int p) {
    if (tabela[p].estado == TERMINADO) return;
    tabela[p].pc = shm->proc[p].pc;
    retirar(&prontos, p);
    retirar(&prontos_io, p);
    retirar(&fila_d1, p);
    tabela[p].estado = TERMINADO;
    terminados++;
    printf("t=%5.1fs   %s terminou (PC = %d/%d)\n", agora(), tabela[p].nome, tabela[p].pc, tabela[p].max);
    if (atual == p) atual = -1;
}

/* Tira o processo da CPU com SIGSTOP e espera ele parar de fato antes de
 * salvar o contexto. Devolve 0 se ele terminou nesse meio tempo. */
static int parar(int p) {
    int status;
    kill(tabela[p].pid, SIGSTOP);
    if (waitpid(tabela[p].pid, &status, WUNTRACED) == tabela[p].pid && !WIFSTOPPED(status)) {
        marcarTerminado(p);
        return 0;
    }
    salvarContexto(p);
    return 1;
}

/* Escolhe o proximo processo (primeiro os que voltaram de I/O) e o poe na CPU.
 * Devolve 1 se algum processo assumiu a CPU. */
static int despachar(void) {
    if (atual != -1) return 0;
    Fila *f = prontos_io.tam > 0 ? &prontos_io : (prontos.tam > 0 ? &prontos : NULL);
    if (f == NULL) return 0;   /* CPU ociosa */

    int p = remover(f);
    restaurarContexto(p);
    tabela[p].estado = EXECUTANDO;
    atual = p;
    kill(tabela[p].pid, SIGCONT);
    return 1;
}

/* Despacho fora do IRQ0 (depois de syscall, fim de I/O ou termino). */
static void despacharComAviso(void) {
    if (despachar())
        printf("t=%5.1fs   %s assume a CPU (PC restaurado = %d)\n",
               agora(), tabela[atual].nome, tabela[atual].pc);
}

static void bloquear(int p) {
    tabela[p].estado = BLOQUEADO;
    inserir(&fila_d1, p);
    shm->fila_d1 = fila_d1.tam;
    char sc[16];
    snprintf(sc, sizeof sc, "%s,%c", tabela[p].dispositivo, tabela[p].operacao);
    printf("t=%5.1fs   %s syscall(%s) no PC %d -> bloqueado esperando D1\n",
           agora(), tabela[p].nome, sc, tabela[p].pc);
}

/* ---------- tratamento de cada evento ---------- */

static void trataIRQ0(void) {
    if (atual != -1) {
        int p = atual;
        atual = -1;
        if (parar(p)) {
            if (shm->proc[p].em_syscall) {       /* fez syscall junto com o fim do slice */
                bloquear(p);
            } else {
                tabela[p].estado = PRONTO;
                inserir(&prontos, p);
            }
        }
    }
    despachar();
}

static void trataIRQ1(void) {
    if (fila_d1.tam == 0) return;
    int p = remover(&fila_d1);
    shm->fila_d1 = fila_d1.tam;
    shm->proc[p].em_syscall = 0;                 /* "retorno" da syscall */
    tabela[p].estado = PRONTO;
    inserir(&prontos_io, p);
    printf("t=%5.1fs   IRQ1: D1 terminou %c de %s -> pronto (prioridade)\n",
           agora(), tabela[p].operacao, tabela[p].nome);
    despacharComAviso();                         /* se a CPU estava ociosa */
}

static int indiceDe(pid_t pid) {
    for (int i = 0; i < nprocs; i++)
        if (tabela[i].pid == pid) return i;
    return -1;
}

static void trataSyscall(pid_t pid) {
    int p = indiceDe(pid);
    if (p < 0) return;
    if (tabela[p].estado == EXECUTANDO) {
        atual = -1;
        if (parar(p)) bloquear(p);
        despacharComAviso();
    } else if (tabela[p].estado == PRONTO) {    /* ja tinha perdido a CPU */
        retirar(&prontos, p);
        retirar(&prontos_io, p);
        bloquear(p);
    }
    /* BLOQUEADO ou TERMINADO: o pedido ja foi tratado */
}

static void trataSIGCHLD(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == pid_inter) {
            printf("InterControllerSim terminou inesperadamente\n");
            pid_inter = -1;
            continue;
        }
        int p = indiceDe(pid);
        if (p >= 0) marcarTerminado(p);
    }
    despacharComAviso();
}

/* ---------- linha do tempo ---------- */

static const char *letraEstado(Estado e) {
    switch (e) {
        case PRONTO:     return "pronto";
        case EXECUTANDO: return "exec";
        case BLOQUEADO:  return "bloq";
        default:         return "fim";
    }
}

static void imprimirLinhaDoTempo(void) {
    printf("t=%5.1fs | CPU: %-4s| ", agora(), atual == -1 ? "--" : tabela[atual].nome);
    imprimirFila("prontos(I/O)", &prontos_io, 0);
    printf(" ");
    imprimirFila("prontos", &prontos, 0);
    printf(" ");
    imprimirFila("D1", &fila_d1, 1);
    printf(" | PC:");
    for (int i = 0; i < nprocs; i++) {
        int pc = (tabela[i].estado == EXECUTANDO) ? shm->proc[i].pc : tabela[i].pc;
        printf(" %s=%d/%d(%s)", tabela[i].nome, pc, tabela[i].max, letraEstado(tabela[i].estado));
    }
    printf("\n");
}

/* ---------- criacao e encerramento ---------- */

static void encerrar(int codigo) {
    if (pid_inter > 0) kill(pid_inter, SIGKILL);
    for (int i = 0; i < nprocs; i++)
        if (tabela[i].pid > 0 && tabela[i].estado != TERMINADO) kill(tabela[i].pid, SIGKILL);
    while (waitpid(-1, NULL, 0) > 0)
        ;
    if (shm) shmdt(shm);
    if (shmid != -1) shmctl(shmid, IPC_RMID, NULL);
    printf("KernelSim encerrado.\n");
    exit(codigo);
}

static pid_t criarProcesso(char *const args[], const sigset_t *bloqueados) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        encerrar(1);
    }
    if (pid == 0) {
        sigprocmask(SIG_UNBLOCK, bloqueados, NULL);   /* o filho recebe sinais normalmente */
        execv(args[0], args);
        perror("execv");
        _exit(1);
    }
    return pid;
}

int main(int argc, char *argv[]) {
    nprocs = (argc > 1) ? atoi(argv[1]) : 4;
    if (nprocs < MIN_PROCS || nprocs > MAX_PROCS) {
        fprintf(stderr, "Uso: %s [numero de processos: %d a %d]\n", argv[0], MIN_PROCS, MAX_PROCS);
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    clock_gettime(CLOCK_MONOTONIC, &inicio);

    shmid = shmget(IPC_PRIVATE, sizeof(MemCompartilhada), IPC_CREAT | 0600);
    if (shmid == -1) {
        perror("shmget");
        return 1;
    }
    shm = shmat(shmid, NULL, 0);
    if (shm == (void *) -1) {
        perror("shmat");
        shmctl(shmid, IPC_RMID, NULL);
        return 1;
    }
    memset(shm, 0, sizeof *shm);
    shm->kernel = getpid();

    /* sinais tratados de forma sincrona no laco principal */
    sigset_t eventos;
    sigemptyset(&eventos);
    sigaddset(&eventos, SIG_IRQ0);
    sigaddset(&eventos, SIG_IRQ1);
    sigaddset(&eventos, SIG_SYSCALL);
    sigaddset(&eventos, SIGCHLD);
    sigaddset(&eventos, SIGINT);
    sigaddset(&eventos, SIGTERM);
    sigprocmask(SIG_BLOCK, &eventos, NULL);

    char sshmid[16], sidx[8], smax[8], skernel[16];
    snprintf(sshmid, sizeof sshmid, "%d", shmid);

    /* processos de aplicacao: comecam parados, na fila de prontos */
    for (int i = 0; i < nprocs; i++) {
        const ConfigProc *c = &CONFIG[i];
        snprintf(tabela[i].nome, sizeof tabela[i].nome, "%s", c->nome);
        tabela[i].max = c->max;
        tabela[i].estado = PRONTO;
        shm->proc[i].max = c->max;

        snprintf(sidx, sizeof sidx, "%d", i);
        snprintf(smax, sizeof smax, "%d", c->max);
        char *args[6 + MAX_SYSCALLS] = { "./aplicacao", (char *) c->nome, sshmid, sidx, smax };
        int n = 5;
        for (int k = 0; k < MAX_SYSCALLS && c->syscalls[k]; k++) args[n++] = (char *) c->syscalls[k];
        args[n] = NULL;

        tabela[i].pid = criarProcesso(args, &eventos);
        int status;
        if (waitpid(tabela[i].pid, &status, WUNTRACED) != tabela[i].pid || !WIFSTOPPED(status)) {
            fprintf(stderr, "%s nao iniciou corretamente\n", c->nome);
            tabela[i].estado = TERMINADO;
            encerrar(1);
        }
        inserir(&prontos, i);
    }

    snprintf(skernel, sizeof skernel, "%d", getpid());
    char *args_inter[] = { "./intercontroller", skernel, sshmid, NULL };
    pid_inter = criarProcesso(args_inter, &eventos);

    printf("KernelSim (pid %d): %d processos, time slice = %ds, I/O em D1 = %ds\n",
           getpid(), nprocs, TIMESLICE, TEMPO_IO);
    for (int i = 0; i < nprocs; i++) {
        printf("  %s: pid %d, MAX = %d, syscalls:", tabela[i].nome, tabela[i].pid, tabela[i].max);
        if (!CONFIG[i].syscalls[0]) printf(" nenhuma");
        for (int k = 0; k < MAX_SYSCALLS && CONFIG[i].syscalls[k]; k++) printf(" %s", CONFIG[i].syscalls[k]);
        printf("\n");
    }
    printf("(Ctrl+C encerra)\n\n");

    despachar();
    imprimirLinhaDoTempo();

    int avisou_fim = 0;
    for (;;) {
        siginfo_t info;
        int sig = sigwaitinfo(&eventos, &info);
        if (sig == -1) {
            if (errno == EINTR) continue;
            perror("sigwaitinfo");
            encerrar(1);
        }

        if (sig == SIG_IRQ0) {
            trataIRQ0();
            if (terminados < nprocs) imprimirLinhaDoTempo();
        } else if (sig == SIG_IRQ1) {
            trataIRQ1();
        } else if (sig == SIG_SYSCALL) {
            trataSyscall(info.si_pid);
        } else if (sig == SIGCHLD) {
            trataSIGCHLD();
        } else {   /* SIGINT ou SIGTERM */
            printf("\n");
            encerrar(0);
        }

        if (terminados == nprocs && !avisou_fim) {
            avisou_fim = 1;
            imprimirLinhaDoTempo();
            printf("\nTodos os processos de aplicacao terminaram. KernelSim continua ativo (Ctrl+C encerra).\n");
        }
    }
}
