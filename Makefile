all: kilo

kilo: kilo.c
	# $(CC) -o kilo kilo.c -Wall -g -W -pedantic -std=c99
	$(CC) -o kilo kilo.c -Wall -g -Wextra -pedantic -std=c99

clean:
	rm kilo
