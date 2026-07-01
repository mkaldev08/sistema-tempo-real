#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h> /* write() — saída async-signal-safe, sem locks stdio */
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>

#define NANOSEGUNDOS_POR_SEGUNDO 1000000000L
#define MAX_TAREFAS 8

/*
 * Representa uma tarefa periódica de tempo real.
 * Cada campo é lido/escrito exclusivamente pela thread dona da tarefa,
 * excepto após pthread_join() onde o main lê as estatísticas finais.
 */
typedef struct
{
    const char *nome;   /* identificador legível na saída */
    long periodo_ns;    /* intervalo de activação em nanosegundos */
    int prioridade;     /* prioridade SCHED_FIFO pedida (1-99) */
    long numero_ciclos; /* número de activações a executar */

    /* Definido pela thread ao verificar a política real atribuída. */
    int tempo_real_ativo;

    /* Estatísticas de latência preenchidas durante a execução. */
    long latencia_minima_ns; /* menor latência observada */
    long latencia_maxima_ns; /* maior latência observada */
    long soma_latencias_ns;  /* acumulador para cálculo da média */
    long total_amostras;     /* número de ciclos completados */
} Tarefa;

/*
 * Instante de arranque do programa (CLOCK_MONOTONIC).
 * Escrito uma única vez em main() antes de criar qualquer thread.
 * As threads acedem em modo leitura — sem corrida de dados.
 */
static struct timespec instante_inicial;

/* --------------------------------------------------------------------------
 * Funções auxiliares de tempo
 * -------------------------------------------------------------------------- */

/*
 * Avança '*tempo' em 'nanosegundos' ns, normalizando tv_nsec para [0, 10^9).
 * Usa subtracção iterativa para evitar divisão e suportar qualquer delta.
 */
static void avancar_tempo(struct timespec *tempo, long nanosegundos)
{
    tempo->tv_nsec += nanosegundos;
    while (tempo->tv_nsec >= NANOSEGUNDOS_POR_SEGUNDO)
    {
        tempo->tv_nsec -= NANOSEGUNDOS_POR_SEGUNDO;
        tempo->tv_sec += 1;
    }
}

/*
 * Devolve (fim − início) em nanosegundos.
 * Resultado negativo indica que 'fim' é anterior a 'início'.
 */
static long diferenca_em_nanosegundos(const struct timespec *fim,
                                      const struct timespec *inicio)
{
    return (fim->tv_sec - inicio->tv_sec) * NANOSEGUNDOS_POR_SEGUNDO + (fim->tv_nsec - inicio->tv_nsec);
}

/* --------------------------------------------------------------------------
 * Thread da tarefa periódica
 * -------------------------------------------------------------------------- */

/*
 * Executa a tarefa periódica descrita em 'argumento' (ponteiro para Tarefa).
 *
 * Por cada ciclo:
 *   1. Avança o deadline absoluto em periodo_ns.
 *   2. Dorme até esse instante (TIMER_ABSTIME — sem deriva acumulada).
 *   3. Mede a latência real = instante_de_acordar − deadline_previsto.
 *   4. Actualiza estatísticas de mínimo, máximo e soma.
 *   5. Regista a linha do ciclo com write() para evitar locks de stdio
 *      dentro do caminho periódico crítico.
 */
static void *executar_tarefa(void *argumento)
{
    Tarefa *tarefa = (Tarefa *)argumento;

    /* Verifica a política de escalonamento efectivamente atribuída pelo kernel. */
    int politica_atribuida; // 1 - SCHED_FIFO, 2 - SCHED_RR, 0 - SCHED_OTHER
    struct sched_param parametros_escalonamento;
    pthread_getschedparam(pthread_self(), &politica_atribuida,
                          &parametros_escalonamento);
    tarefa->tempo_real_ativo = (politica_atribuida == SCHED_FIFO || politica_atribuida == SCHED_RR);

    /* Mensagem de arranque — fora do loop periódico`. */
    printf("[%-18s] iniciada | politica=%s prioridade=%d | periodo=%ld ms\n",
           tarefa->nome,
           tarefa->tempo_real_ativo ? "SCHED_FIFO" : "SCHED_OTHER",
           parametros_escalonamento.sched_priority,
           tarefa->periodo_ns / 1000000);

    /* Sentinelas de estatísticas: mínimo começa alto, máximo começa a zero. */
    tarefa->latencia_minima_ns = NANOSEGUNDOS_POR_SEGUNDO;
    tarefa->latencia_maxima_ns = 0;
    tarefa->soma_latencias_ns = 0;
    tarefa->total_amostras = 0;

    /*
     * Âncora do deadline absoluto: lida uma vez aqui, depois só avançada.
     * Garante que o primeiro deadline é periodo_ns após o arranque da thread,
     * independentemente do delay de inicialização acima.
     */
    struct timespec proximo_instante, agora;
    clock_gettime(CLOCK_MONOTONIC, &proximo_instante);

    for (long ciclo = 0; ciclo < tarefa->numero_ciclos; ciclo++)
    {
        avancar_tempo(&proximo_instante, tarefa->periodo_ns);

        /*
         * TIMER_ABSTIME: dorme até ao instante absoluto 'proximo_instante'.
         * Mesmo que este ciclo tenha demorado mais do que o previsto, o
         * próximo deadline continua a ser t0 + (ciclo+1)*periodo — sem deriva.
         * Reinicia o sleep se interrompido por um sinal (EINTR).
         */
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                               &proximo_instante, NULL) == EINTR)
            ;

        clock_gettime(CLOCK_MONOTONIC, &agora);

        /* Latência = tempo real de acordar menos o instante planeado. */
        long latencia_ns = diferenca_em_nanosegundos(&agora, &proximo_instante);
        if (latencia_ns < 0)
            latencia_ns = 0; /* defesa contra imprecisão do relógio */

        if (latencia_ns < tarefa->latencia_minima_ns)
            tarefa->latencia_minima_ns = latencia_ns;
        if (latencia_ns > tarefa->latencia_maxima_ns)
            tarefa->latencia_maxima_ns = latencia_ns;
        tarefa->soma_latencias_ns += latencia_ns;
        tarefa->total_amostras += 1;

        /*
         * write() é async-signal-safe e não adquire locks internos de stdio.
         * Evita bloqueio indeterminado dentro do loop periódico crítico,
         * ao contrário de printf/fprintf.
         */
        long tempo_decorrido_ns = diferenca_em_nanosegundos(&agora, &instante_inicial);
        char linha[128];
        int comprimento = snprintf(linha, sizeof(linha),
                                   "[%-18s] ciclo %3ld | t=+%8.1f ms | latencia=%7.1f us\n",
                                   tarefa->nome, ciclo + 1,
                                   tempo_decorrido_ns / 1e6, latencia_ns / 1e3);
        write(STDOUT_FILENO, linha, (size_t)comprimento);
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * Criação da thread com política de tempo real
 * -------------------------------------------------------------------------- */

/*
 * Cria uma pthread com SCHED_FIFO à prioridade definida em 'tarefa'.
 *
 * Se o processo não tiver CAP_SYS_NICE (i.e., não correr com sudo),
 * pthread_create devolve EPERM. Nesse caso, a thread é recriada com
 * PTHREAD_INHERIT_SCHED, herdando SCHED_OTHER do processo principal,
 * o que permite desenvolver e testar sem privilégios.
 *
 * Devolve 0 em sucesso, ou o código de erro de pthread_create.
 */
static int criar_tarefa_tempo_real(pthread_t *identificador, Tarefa *tarefa)
{
    pthread_attr_t atributos;
    struct sched_param parametros = {.sched_priority = tarefa->prioridade};

    pthread_attr_init(&atributos);
    /* PTHREAD_EXPLICIT_SCHED: usa a política aqui configurada em vez de
     * herdar automaticamente a do processo criador. */
    pthread_attr_setinheritsched(&atributos, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&atributos, SCHED_FIFO);
    pthread_attr_setschedparam(&atributos, &parametros);

    int resultado = pthread_create(identificador, &atributos, executar_tarefa, tarefa);
    if (resultado == EPERM)
    {
        /* Sem privilégios suficientes para SCHED_FIFO — recua para SCHED_OTHER. */
        fprintf(stderr,
                "[aviso] '%s': SCHED_FIFO recusado (precisa de sudo). A recuar.\n",
                tarefa->nome);
        pthread_attr_setinheritsched(&atributos, PTHREAD_INHERIT_SCHED);
        resultado = pthread_create(identificador, &atributos, executar_tarefa, tarefa);
    }
    pthread_attr_destroy(&atributos);
    return resultado;
}

int main(int argc, char **argv)
{
    long ciclos_personalizados = -1;

    for (int indice = 1; indice < argc; indice++)
    {
        if (!strcmp(argv[indice], "--ciclos") && indice + 1 < argc)
            ciclos_personalizados = atol(argv[++indice]);
        else
        {
            fprintf(stderr, "utilizacao: %s [--ciclos N]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    Tarefa tarefas[] = {
        {.nome = "Tarefa Critica", .periodo_ns = 500000000L, .prioridade = 80, .numero_ciclos = 20},
        {.nome = "Tarefa Nao-Critica", .periodo_ns = 1000000000L, .prioridade = 40, .numero_ciclos = 10},
    };
    int total_tarefas = sizeof(tarefas) / sizeof(tarefas[0]);

    if (ciclos_personalizados > 0)
        for (int indice = 0; indice < total_tarefas; indice++)
            tarefas[indice].numero_ciclos = ciclos_personalizados;

    /* Bloqueia todas as páginas de memória na RAM (actuais e futuras) para
     * eliminar latências de page fault durante a execução das threads. */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1)
        fprintf(stderr, "[aviso] mlockall falhou: %s\n", strerror(errno));

    /* Referência de tempo global — escrita antes de criar qualquer thread. */
    clock_gettime(CLOCK_MONOTONIC, &instante_inicial);

    pthread_t identificadores[MAX_TAREFAS];
    for (int indice = 0; indice < total_tarefas; indice++)
    {
        if (criar_tarefa_tempo_real(&identificadores[indice], &tarefas[indice]) != 0)
        {
            fprintf(stderr, "falha ao criar a thread de '%s'\n", tarefas[indice].nome);
            return EXIT_FAILURE;
        }
    }

    /* Aguarda o término de todas as threads antes de aceder às estatísticas. */
    for (int indice = 0; indice < total_tarefas; indice++)
        pthread_join(identificadores[indice], NULL);

    printf("\n=== resumo de latencia (microsegundos) ===\n");
    printf("%-20s %8s %8s %8s\n", "tarefa", "min", "media", "max");
    for (int indice = 0; indice < total_tarefas; indice++)
    {
        Tarefa *tarefa = &tarefas[indice];
        /* Guarda contra divisão por zero caso numero_ciclos fosse 0. */
        if (tarefa->total_amostras > 0)
        {
            printf("%-20s %8.1f %8.1f %8.1f\n",
                   tarefa->nome,
                   tarefa->latencia_minima_ns / 1e3,
                   (double)tarefa->soma_latencias_ns / tarefa->total_amostras / 1e3,
                   tarefa->latencia_maxima_ns / 1e3);
        }
        else
        {
            printf("%-20s %8s %8s %8s\n", tarefa->nome, "n/a", "n/a", "n/a");
        }
    }

    munlockall();
    return 0;
}
