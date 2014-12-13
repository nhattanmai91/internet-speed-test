OS := $(shell uname -s)
CFLAGS := -Wall -Wextra
#CFLAGS += -Werror
CFLAGS += -O2
#CFLAGS += -O0 -g

ifneq ($(OS),Linux)
CFLAGS += -DNO_SCHED_FIFO=1
endif

main: main.c GNUmakefile
	gcc $(CFLAGS) main.c -o main

.PHONY: clean
clean:
	rm -fv main
