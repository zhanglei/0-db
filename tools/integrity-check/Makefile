EXEC = integrity-check
SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)

CFLAGS += -g -std=gnu99 -W -Wall -O2 -msse4.2 -I../../src
LDFLAGS += -rdynamic

all: $(EXEC)

release: CFLAGS += -DRELEASE
release: $(EXEC)

shadump: CFLAGS += -DSHADUMP
shadump: LDFLAGS += -lcrypto
shadump: $(EXEC)

$(EXEC): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	$(RM) *.o

mrproper: clean
	$(RM) $(EXEC)
