/*
 * Trabalho 1 - INF1316 - Sistemas Operacionais
 *
 * Rodrigo Carvalho       2510091
 *
 * intercontroller.c: InterControllerSim, o controlador de interrupcoes.
 *
 * Processo infinito que, a cada TIMESLICE segundos:
 *   - gera IRQ0 (fim de time slice) para o KernelSim;
 *   - se houver pedidos na fila de D1, conta o tempo do pedido atual e,
 *     depois de TEMPO_IO segundos, gera IRQ1 (fim do I/O em D1).
 *
 * O tamanho da fila de D1 e lido da memoria compartilhada (o kernel o mantem).
 * Um pedido so comeca a contar no primeiro tick em que aparece na fila, entao
 * cada I/O leva pelo menos TEMPO_IO segundos.
 *
 * Uso: intercontroller <pid_kernel> <shmid>
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/shm.h>
#include "comum.h"

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <pid_kernel> <shmid>\n", argv[0]);
        return 1;
    }
    pid_t kernel = (pid_t) atoi(argv[1]);
    int shmid = atoi(argv[2]);

    MemCompartilhada *shm = shmat(shmid, NULL, SHM_RDONLY);
    if (shm == (void *) -1) {
        perror("shmat");
        return 1;
    }
    volatile int *fila_d1 = &shm->fila_d1;

    int atendendo = 0;   /* D1 esta atendendo um pedido? */
    int tempo_d1 = 0;    /* segundos ja gastos no pedido atual */

    for (;;) {
        sleep(TIMESLICE);
        if (getppid() != kernel)   /* o kernel terminou: encerra tambem */
            break;

        if (*fila_d1 > 0) {
            if (!atendendo) {
                atendendo = 1;
                tempo_d1 = 0;
            } else {
                tempo_d1 += TIMESLICE;
                if (tempo_d1 >= TEMPO_IO) {
                    kill(kernel, SIG_IRQ1);   /* IRQ1: terminou o I/O em D1 */
                    tempo_d1 = 0;             /* o proximo pedido comeca agora */
                }
            }
        } else {
            atendendo = 0;
        }

        kill(kernel, SIG_IRQ0);               /* IRQ0: fim do time slice */
    }

    shmdt(shm);
    return 0;
}
