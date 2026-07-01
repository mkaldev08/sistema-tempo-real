## Makefile — compila e corre dentro de um contentor Linux (gcc:latest).
## Necessário porque clock_nanosleep/TIMER_ABSTIME não existem em macOS.
##
## Alvos disponíveis:
##   make          — compila
##   make run      — compila e corre (5 ciclos)
##   make run-rt   — compila e corre com sudo dentro do contentor (SCHED_FIFO real)
##   make clean    — remove o binário local (se existir)

SRC      := main.c
BIN      := tarefas_tempo_real
CFLAGS   := -O2 -Wall -Wextra -pthread
DOCKER   := docker run --rm -v "$(CURDIR)":/src -w /src gcc:latest

.PHONY: all run run-rt clean

all:
	$(DOCKER) gcc $(CFLAGS) -o $(BIN) $(SRC)

run: all
	$(DOCKER) ./$(BIN) --ciclos 5

## Para SCHED_FIFO real é preciso --privileged (CAP_SYS_NICE dentro do contentor)
run-rt: all
	docker run --rm --privileged -v "$(CURDIR)":/src -w /src gcc:latest \
		./$(BIN) --ciclos 5

clean:
	rm -f $(BIN)
