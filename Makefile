# Makefile to build the man clone.

CC = gcc
CPPFLAGS = -I.
CFLAGS = -O3 -s

man.exe: man.c fnmatch.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^

.PHONY: clean
clean:
	@rm -fv *.o *.exe
