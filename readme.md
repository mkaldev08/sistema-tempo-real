# Simulação de tarefas periódicas com escalonamento de tempo real (Linux)

*
* Cada tarefa corre numa pthread configurada com SCHED_FIFO e acorda em
* instantes absolutos via clock_nanosleep(TIMER_ABSTIME), garantindo
* deadlines sem deriva acumulada. Sem privilégios, recai para SCHED_OTHER.
*
* Compilação: gcc -O2 -Wall -Wextra -pthread -o tarefas_tempo_real main.c
* Execução  : sudo ./tarefas_tempo_real [--ciclos N]
