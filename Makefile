CC        = g++
CFLAGS    = -c -Wall -std=c++0x
LIBPATH   = -L/usr/lib
TARGETDIR = /usr/local/bin

.PHONY: clean install

all: silence
	
silence: silence.o
	$(CC) silence.o -o $@ $(LIBPATH) -lsndfile

.cpp.o:
	$(CC) $(CFLAGS) $< -o $@

install: silence silence.py
	install -p -t $(TARGETDIR) $^

clean: 
	-rm -f silence *.o
