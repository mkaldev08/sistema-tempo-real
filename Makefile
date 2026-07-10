#TODO: create a make file compatible with ubuntu gcc compiler
# Variáveis de Configuração
CC = gcc
CFLAGS = -O2 -Wall
LIBS = -lpthread
TARGET = simulador_rt
SRC = main.c

# Quantidade padrão de ciclos (caso o usuário não passe no terminal)
CICLOS ?= 50

.PHONY: all clean run run-rt

# 1. Alvo padrão: Compilar o programa
all: $(TARGET)

$(TARGET): $(SRC)
	@echo "Compilando $(SRC) com otimização -O2..."
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LIBS)
	@echo "Compilação concluída com sucesso!"

# 2. Executar SEM sudo (Apenas permissões normais - SCHED_OTHER)
run: $(TARGET)
	@echo "--------------------------------------------------------"
	@echo "Executando SEM privilégios de root (Modo Normal)"
	@echo "Ciclos configurados: $(CICLOS)"
	@echo "--------------------------------------------------------"
	./$(TARGET) --ciclos $(CICLOS) --verbose

# 3. Executar COM sudo (Modo Tempo Real Real - SCHED_FIFO)
run-rt: $(TARGET)
	@echo "--------------------------------------------------------"
	@echo "Executando COM privilégios de root (Modo Real-Time)"
	@echo "Ciclos configurados: $(CICLOS)"
	@echo "--------------------------------------------------------"
	sudo ./$(TARGET) --ciclos $(CICLOS) --verbose

# Limpar arquivos gerados
clean:
	@echo "Limpando o executável..."
	rm -f $(TARGET)