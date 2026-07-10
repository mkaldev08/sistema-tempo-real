#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/syscall.h> /* Necessário para obter o TID real do Linux para o top */

#define NANOSEGUNDOS_POR_SEGUNDO 1000000000L
#define MAX_TAREFAS 8

typedef struct
{
    const char *nome;
    long periodo_ns;
    int prioridade;
    long numero_ciclos;
    int tempo_real_ativo;
    int verbose; /* Se 1, exibe logs detalhados ciclo a ciclo */

    /* Estatísticas de latência */
    long latencia_minima_ns;
    long latencia_maxima_ns;
    long soma_latencias_ns;
    long total_amostras;
} Tarefa;

static struct timespec instante_inicial;

static void avancar_tempo(struct timespec *tempo, long nanosegundos)
{
    tempo->tv_nsec += nanosegundos;
    while (tempo->tv_nsec >= NANOSEGUNDOS_POR_SEGUNDO)
    {
        tempo->tv_nsec -= NANOSEGUNDOS_POR_SEGUNDO;
        tempo->tv_sec += 1;
    }
}

static long diferenca_em_nanosegundos(const struct timespec *fim,
                                      const struct timespec *inicio)
{
    return (fim->tv_sec - inicio->tv_sec) * NANOSEGUNDOS_POR_SEGUNDO + (fim->tv_nsec - inicio->tv_nsec);
}

/* --------------------------------------------------------------------------
 * Thread da tarefa periódica
 * -------------------------------------------------------------------------- */
static void *executar_tarefa(void *argumento)
{
    Tarefa *tarefa = (Tarefa *)argumento;

    /* Obtém o ID real da thread reconhecido pelo comando 'top -H' */
    pid_t tid = (pid_t)syscall(SYS_gettid);

    int politica_atribuida;
    struct sched_param parametros_escalonamento;
    pthread_getschedparam(pthread_self(), &politica_atribuida, &parametros_escalonamento);
    tarefa->tempo_real_ativo = (politica_atribuida == SCHED_FIFO || politica_atribuida == SCHED_RR);

    /* Mensagem estruturada para vistoria com top/chrt */
    printf("[%-18s] Inicializada | Linux TID: %d | Politica: %s | Prioridade: %d\n",
           tarefa->nome, tid,
           tarefa->tempo_real_ativo ? "SCHED_FIFO" : "SCHED_OTHER",
           parametros_escalonamento.sched_priority);

    tarefa->latencia_minima_ns = NANOSEGUNDOS_POR_SEGUNDO;
    tarefa->latencia_maxima_ns = 0;
    tarefa->soma_latencias_ns = 0;
    tarefa->total_amostras = 0;

    struct timespec proximo_instante, agora;
    clock_gettime(CLOCK_MONOTONIC, &proximo_instante);

    for (long ciclo = 0; ciclo < tarefa->numero_ciclos; ciclo++)
    {
        avancar_tempo(&proximo_instante, tarefa->periodo_ns);

        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &proximo_instante, NULL) == EINTR)
            ;

        clock_gettime(CLOCK_MONOTONIC, &agora);

        long latencia_ns = diferenca_em_nanosegundos(&agora, &proximo_instante);
        if (latencia_ns < 0)
            latencia_ns = 0;

        if (latencia_ns < tarefa->latencia_minima_ns)
            tarefa->latencia_minima_ns = latencia_ns;
        if (latencia_ns > tarefa->latencia_maxima_ns)
            tarefa->latencia_maxima_ns = latencia_ns;
        tarefa->soma_latencias_ns += latencia_ns;
        tarefa->total_amostras += 1;

        /* Apenas executa a escrita segura via write se o modo verboso estiver ativo */
        if (tarefa->verbose)
        {
            long tempo_decorrido_ns = diferenca_em_nanosegundos(&agora, &instante_inicial);
            char linha[128];
            int comprimento = snprintf(linha, sizeof(linha),
                                       "[%-18s] Ciclo %3ld | t=+%8.1f ms | Latencia=%7.1f us\n",
                                       tarefa->nome, ciclo + 1,
                                       tempo_decorrido_ns / 1e6, latencia_ns / 1e3);
            /* Captura o retorno para silenciar o warning e garantir consistência */
            ssize_t bytes_escritos = write(STDOUT_FILENO, linha, (size_t)comprimento);
            if (bytes_escritos < 0)
            {

                (void)bytes_escritos;
            }
        }
    }
    return NULL;
}

static int criar_tarefa_tempo_real(pthread_t *identificador, Tarefa *tarefa)
{
    pthread_attr_t atributos;
    struct sched_param parametros = {.sched_priority = tarefa->prioridade};

    pthread_attr_init(&atributos);
    pthread_attr_setinheritsched(&atributos, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&atributos, SCHED_FIFO);
    pthread_attr_setschedparam(&atributos, &parametros);

    int resultado = pthread_create(identificador, &atributos, executar_tarefa, tarefa);
    if (resultado == EPERM)
    {
        fprintf(stderr, "[AVISO] '%s': SCHED_FIFO negado (precisa de sudo). Usando de SCHED_OTHER.\n", tarefa->nome);
        pthread_attr_setinheritsched(&atributos, PTHREAD_INHERIT_SCHED);
        resultado = pthread_create(identificador, &atributos, executar_tarefa, tarefa);
    }
    pthread_attr_destroy(&atributos);
    return resultado;
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

    Tarefa tarefas[] = {
        {.nome = "Tarefa Critica", .periodo_ns = 100000000L, .prioridade = 80, .numero_ciclos = 50, .verbose = modo_verboso},     // 100ms
        {.nome = "Tarefa Nao-Critica", .periodo_ns = 200000000L, .prioridade = 40, .numero_ciclos = 25, .verbose = modo_verboso}, // 200ms
    };
    int total_tarefas = sizeof(tarefas) / sizeof(tarefas[0]);

    if (ciclos_personalizados > 0)
        for (int indice = 0; indice < total_tarefas; indice++)
            tarefas[indice].numero_ciclos = ciclos_personalizados;

    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1)
        fprintf(stderr, "[AVISO] mlockall falhou: %s\n", strerror(errno));

    // Exibe o PID principal para monitoramento externo rápido */
    printf("============================================================\n");
    printf("SISTEMAS OPERACIONAIS - SIMULADOR RTOS\n");
    printf("PID Principal do Processo: %d\n", getpid());
    printf("Comando para inspecionar no top: top -H -p %d\n", getpid());
    printf("Comando para inspecionar politicas: ps -mLo pid,tid,class,rtprio,comm -p %d\n", getpid());
    printf("============================================================\n");

    printf("Dando 3 segundos para preparar a inspecao visual externa...\n");
    sleep(3);

    clock_gettime(CLOCK_MONOTONIC, &instante_inicial);

    pthread_t identificadores[MAX_TAREFAS];
    for (int indice = 0; indice < total_tarefas; indice++)
    {
        if (criar_tarefa_tempo_real(&identificadores[indice], &tarefas[indice]) != 0)
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
                   tarefa->latencia_minima_ns / 1e3,
                   (double)tarefa->soma_latencias_ns / tarefa->total_amostras / 1e3,
                   tarefa->latencia_maxima_ns / 1e3);
        }
    }

    munlockall();
    return 0;
}