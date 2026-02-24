CFLAGS ?= -std=c99

pin: main.o attach.o history.o server.o
	$(CC) $(CFLAGS) $^ -lutil -o $@

%.o: %.c *.h
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f *.o pin
