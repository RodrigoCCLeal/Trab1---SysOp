/*
 * Trabalho 1 - INF1316 - Sistemas Operacionais
 * Simulando o escalonamento preemptivo de processos com chamadas de sistema
 *
 * Rodrigo Carvalho       2510091
 *
 * comum.h: definicoes compartilhadas por KernelSim, InterControllerSim e
 * pelos processos de aplicacao.
 */

#ifndef COMUM_H
#define COMUM_H

#include <signal.h>
#include <sys/types.h>

#define MIN_PROCS      3
#define MAX_PROCS      6
#define MAX_SYSCALLS   8

#define TIMESLICE      1    /* segundos entre dois IRQ0 (fim de time slice) */
#define TEMPO_IO       3    /* segundos que D1 leva para atender um pedido   */

/*
 * "Interrupcoes" e syscall sao sinais de tempo real. Eles entram numa fila
 * (dois sinais iguais seguidos nao se perdem) e, quando ha varios pendentes,
 * o de menor numero e entregue primeiro. Por isso a ordem abaixo da
 * prioridade: syscall, depois fim de I/O (IRQ1), depois fim de time slice (IRQ0).
 */
#define SIG_SYSCALL    (SIGRTMIN)
#define SIG_IRQ1       (SIGRTMIN + 1)
#define SIG_IRQ0       (SIGRTMIN + 2)

/* Area de cada processo de aplicacao na memoria compartilhada. */
typedef struct {
    int  pc;               /* contador de iteracoes: o "registrador" PC      */
    int  max;              /* numero total de iteracoes (MAX)                */
    int  em_syscall;       /* 1 enquanto espera o termino do I/O             */
    char dispositivo[3];   /* parametros da syscall: "D1"                    */
    char operacao;         /*                         'R' ou 'W'             */
} SlotProc;

/* Segmento de memoria compartilhada criado pelo KernelSim. */
typedef struct {
    pid_t    kernel;       /* pid do KernelSim (destino das syscalls)        */
    int      fila_d1;      /* pedidos na fila de D1 (lido pelo InterController) */
    SlotProc proc[MAX_PROCS];
} MemCompartilhada;

#endif
