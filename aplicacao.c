/*
 * Trabalho 1 - INF1316 - Sistemas Operacionais
 *
 * Rodrigo Carvalho       2510091
 *
 * aplicacao.c: processo de aplicacao Ai.
 *
 * Executa um laco de MAX iteracoes com sleep(1). O contador de iteracoes (PC)
 * fica na memoria compartilhada, que faz o papel do registrador: o KernelSim
 * o salva quando tira o processo da CPU e o restaura antes de devolve-la.
 *
 * Nos instantes configurados (valor do PC), o processo faz syscall(D1, R|W):
 * grava os parametros na memoria compartilhada, avisa o kernel com SIG_SYSCALL
 * e espera o fim do I/O. O kernel o para com SIGSTOP e so o libera depois do
 * IRQ1 que indica o termino da operacao.
 *
 * Uso: aplicacao <nome> <shmid> <indice> <MAX> [pc:R|W ...]
 *      ex.: aplicacao A1 1234 0 8 2:R 5:W
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/shm.h>
#include "comum.h"

int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Uso: %s <nome> <shmid> <indice> <MAX> [pc:R|W ...]\n", argv[0]);
        return 1;
    }

    int shmid  = atoi(argv[2]);
    int indice = atoi(argv[3]);
    int max    = atoi(argv[4]);

    /* instantes (valor do PC) e operacoes das syscalls */
    int  quando[MAX_SYSCALLS];
    char operacao[MAX_SYSCALLS];
    int  n = 0;
    for (int i = 5; i < argc && n < MAX_SYSCALLS; i++) {
        int t;
        char op;
        if (sscanf(argv[i], "%d:%c", &t, &op) == 2 && t > 0 && t < max && (op == 'R' || op == 'W')) {
            quando[n] = t;
            operacao[n] = op;
            n++;
        }
    }

    MemCompartilhada *shm = shmat(shmid, NULL, 0);
    if (shm == (void *) -1) {
        perror("shmat");
        return 1;
    }
    volatile SlotProc *eu = &shm->proc[indice];
    pid_t kernel = shm->kernel;

    raise(SIGSTOP);   /* pronto: espera o kernel escalona-lo pela 1a vez */

    while (eu->pc < max) {
        sleep(1);
        eu->pc++;

        for (int k = 0; k < n; k++) {
            if (quando[k] == eu->pc) {
                eu->dispositivo[0] = 'D';
                eu->dispositivo[1] = '1';
                eu->dispositivo[2] = '\0';
                eu->operacao = operacao[k];
                eu->em_syscall = 1;
                kill(kernel, SIG_SYSCALL);
                while (eu->em_syscall)   /* o kernel para o processo aqui (SIGSTOP) */
                    usleep(10000);       /* e zera a flag quando o I/O termina      */
            }
        }
    }

    shmdt(shm);
    return 0;
}
