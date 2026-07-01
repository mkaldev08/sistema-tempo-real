# instalar a toolchain e ferramentas (uma vez)

sudo dnf install gcc make util-linux

# compilar — com gcc, como no teu Makefile

gcc -O2 -Wall -Wextra -pthread -o tarefas_tempo_real main.c

# ou com o compilador nativo da distro

clang -O2 -Wall -Wextra -pthread -o tarefas_tempo_real main.c

# correr com privilégios para obter SCHED_FIFO real

sudo ./tarefas_tempo_real --ciclos 5
