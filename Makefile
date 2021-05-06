# Makefile to build the man clone.

CC = gcc
CPPFLAGS = -I.
CFLAGS = -Wall -O2 -ggdb -g3

default: man

man: man.c fnmatch.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^
