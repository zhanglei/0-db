EXEC = zdb
SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)

CFLAGS += -g -std=gnu99 -O0 -W -Wall -Wextra -msse4.2 -Wno-implicit-fallthrough
LDFLAGS += -rdynamic

# grab version from git, if possible
REVISION := $(shell git describe --abbrev=8 --dirty --always --tags)
ifneq ($(REVISION),)
	CFLAGS += -DREVISION=\"$(REVISION)\"
endif

ifeq ($(STATIC),1)
	LDFLAGS += -static
endif

ifeq ($(COVERAGE),1)
	CFLAGS += -coverage -fprofile-arcs -ftest-coverage
	LDFLAGS += -lgcov --coverage
endif

all: $(EXEC)

release: CFLAGS += -DRELEASE -O2
release: clean $(EXEC)

$(EXEC): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	$(RM) *.o

mrproper: clean
	$(RM) $(EXEC)
	$(RM) *.gcno *.gcda *.gcov
