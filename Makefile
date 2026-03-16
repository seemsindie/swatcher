##
# Project Title
#
# @file
# @version 0.1

CC=gcc
CFLAGS=-Wall -Wextra -Werror -std=c99 -pedantic -g
LDFLAGS=-lm
SOURCES=main.c
OBJECTS=$(SOURCES:.c=.o)
EXECUTABLE=main

all: $(SOURCES) $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@ $(LDFLAGS)

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(EXECUTABLE)

# .PHONY: all clean

# end
