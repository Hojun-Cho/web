web: web.c
	9c web.c
	9l -o web web.o

clean:
	rm -f web web.o
