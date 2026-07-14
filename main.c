#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/time.h> /* Necessário para gettimeofday */
#include <sys/mman.h>
#include <sys/syscall.h>

#define MICROSEGUNDOS_POR_SEGUNDO 1000000L
#define MAX_TAREFAS 8

typedef struct
{
    const char *nome;
    long periodo_us; /* Alterado para microsegundos */
    int prioridade;
    long numero_ciclos;
    int tempo_real_ativo;
    int verbose;

    long latencia_minima_us;
    long latencia_maxima_us;
    long soma_latencias_us;
    long total_amostras;
} Tarefa;

static struct timeval instante_inicial;

/* Calcula a diferença em microsegundos (fim - inicio) */
static long diferenca_em_microsegundos(const struct timeval *fim, const struct timeval *inicio)
{
    return (fim->tv_sec - inicio->tv_sec) * MICROSEGUNDOS_POR_SEGUNDO + (fim->tv_usec - inicio->tv_usec);
}

/* Avança uma estrutura timeval em um número de microsegundos */
static void avancar_tempo_us(struct timeval *tempo, long microsegundos)
{
    tempo->tv_usec += microsegundos;
    while (tempo->tv_usec >= MICROSEGUNDOS_POR_SEGUNDO)
    {
        tempo->tv_usec -= MICROSEGUNDOS_POR_SEGUNDO;
        tempo->tv_sec += 1;
    }
}

/* --------------------------------------------------------------------------
 * Thread da tarefa periódica
 * -------------------------------------------------------------------------- */
static void *executar_tarefa(void *argumento)
{
    Tarefa *tarefa = (Tarefa *)argumento;
    pid_t tid = (pid_t)syscall(SYS_gettid);

    /* 1. USO DO SCHED_SETSCHEDULER AQUI DENTRO */
    struct sched_param parametros_escalonamento;
    parametros_escalonamento.sched_priority = tarefa->prioridade;

    if (sched_setscheduler(0, SCHED_FIFO, &parametros_escalonamento) == 0)
    {
        tarefa->tempo_real_ativo = 1;
    }
    else
    {
        tarefa->tempo_real_ativo = 0;
        /* Se falhar (ex: sem sudo), avisa e continua como SCHED_OTHER */
        if (tarefa->verbose)
        {
            fprintf(stderr, "[AVISO] '%s': sched_setscheduler falhou (precisa de sudo). Rodando sem prioridade real.\n", tarefa->nome);
        }
    }

    printf("[%-18s] Inicializada | Linux TID: %d | Politica: %s | Prioridade: %d\n",
           tarefa->nome, tid,
           tarefa->tempo_real_ativo ? "SCHED_FIFO" : "SCHED_OTHER",
           tarefa->prioridade);

    tarefa->latencia_minima_us = MICROSEGUNDOS_POR_SEGUNDO * 10; // Valor inicial alto
    tarefa->latencia_maxima_us = 0;
    tarefa->soma_latencias_us = 0;
    tarefa->total_amostras = 0;

    struct timeval proximo_instante, agora;

    /* 2. USO DO GETTIMEOFDAY */
    gettimeofday(&proximo_instante, NULL);

    for (long ciclo = 0; ciclo < tarefa->numero_ciclos; ciclo++)
    {
        avancar_tempo_us(&proximo_instante, tarefa->periodo_us);

        /* Calcula quanto tempo dormir para atingir o proximo_instante */
        gettimeofday(&agora, NULL);
        long tempo_para_dormir_us = diferenca_em_microsegundos(&proximo_instante, &agora);

        /* 3. USO DO SLEEP (usleep para microsegundos) */
        if (tempo_para_dormir_us > 0)
        {
            usleep(tempo_para_dormir_us);
        }

        gettimeofday(&agora, NULL);

        long latencia_us = diferenca_em_microsegundos(&agora, &proximo_instante);
        if (latencia_us < 0)
            latencia_us = 0;

        if (latencia_us < tarefa->latencia_minima_us)
            tarefa->latencia_minima_us = latencia_us;
        if (latencia_us > tarefa->latencia_maxima_us)
            tarefa->latencia_maxima_us = latencia_us;

        tarefa->soma_latencias_us += latencia_us;
        tarefa->total_amostras += 1;

        if (tarefa->verbose)
        {
            long tempo_decorrido_us = diferenca_em_microsegundos(&agora, &instante_inicial);
            char linha[128];
            int comprimento = snprintf(linha, sizeof(linha),
                                       "[%-18s] Ciclo %3ld | t=+%8.1f ms | Latencia=%7.1f us\n",
                                       tarefa->nome, ciclo + 1,
                                       tempo_decorrido_us / 1000.0, (double)latencia_us);

            ssize_t bytes_escritos = write(STDOUT_FILENO, linha, (size_t)comprimento);
            if (bytes_escritos < 0)
            {
                (void)bytes_escritos;
            }
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    long ciclos_personalizados = -1;
    int modo_verboso = 0;

    for (int indice = 1; indice < argc; indice++)
    {
        if (!strcmp(argv[indice], "--ciclos") && indice + 1 < argc)
            ciclos_personalizados = atol(argv[++indice]);
        else if (!strcmp(argv[indice], "--verbose"))
            modo_verboso = 1;
        else
        {
            fprintf(stderr, "Uso: %s [--ciclos N] [--verbose]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    /* Tempos convertidos de nanosegundos (100000000L) para microsegundos (100000L) */
    Tarefa tarefas[] = {
        {.nome = "Tarefa Critica", .periodo_us = 100000L, .prioridade = 80, .numero_ciclos = 50, .verbose = modo_verboso},
        {.nome = "Tarefa Nao-Critica", .periodo_us = 200000L, .prioridade = 40, .numero_ciclos = 25, .verbose = modo_verboso},
    };
    int total_tarefas = sizeof(tarefas) / sizeof(tarefas[0]);

    if (ciclos_personalizados > 0)
        for (int indice = 0; indice < total_tarefas; indice++)
            tarefas[indice].numero_ciclos = ciclos_personalizados;

    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1)
        fprintf(stderr, "[AVISO] mlockall falhou: %s\n", strerror(errno));

    printf("============================================================\n");
    printf("SISTEMAS OPERACIONAIS - SIMULADOR RTOS\n");
    printf("PID Principal do Processo: %d\n", getpid());
    printf("============================================================\n");
    printf("Dando 3 segundos para preparar a inspecao visual externa...\n");
    sleep(3); /* Uso tradicional do sleep */

    gettimeofday(&instante_inicial, NULL);

    pthread_t identificadores[MAX_TAREFAS];
    for (int indice = 0; indice < total_tarefas; indice++)
    {
        /* Criação simples da thread, o escalonamento agora é feito dentro dela */
        if (pthread_create(&identificadores[indice], NULL, executar_tarefa, &tarefas[indice]) != 0)
        {
            fprintf(stderr, "Falha ao criar a thread de '%s'\n", tarefas[indice].nome);
            return EXIT_FAILURE;
        }
    }

    for (int indice = 0; indice < total_tarefas; indice++)
        pthread_join(identificadores[indice], NULL);

    printf("\n=== RESUMO FINAL DE LATENCIA (MICROSEGUNDOS) ===\n");
    printf("%-20s %8s %8s %8s\n", "Tarefa", "Min", "Media", "Max");
    for (int indice = 0; indice < total_tarefas; indice++)
    {
        Tarefa *tarefa = &tarefas[indice];
        if (tarefa->total_amostras > 0)
        {
            printf("%-20s %8.1f %8.1f %8.1f\n",
                   tarefa->nome,
                   (double)tarefa->latencia_minima_us,
                   (double)tarefa->soma_latencias_us / tarefa->total_amostras,
                   (double)tarefa->latencia_maxima_us);
        }
    }

    munlockall();
    return 0;
}