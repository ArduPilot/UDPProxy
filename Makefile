# Makefile for UDPProxy

.PHONY: headers

all: udpproxy set_key

headers:
	@./regen_headers.sh

CXX=g++
CC=gcc
CFLAGS=-Wall -g -DSTANDALONE
CXXFLAGS=-Wall -g

udpproxy: udpproxy.o mavlink.o tdb.o spinlock.o
	$(CXX) $(CXXFLAGS) -o $@ $^

set_key: set_key.o tdb.o spinlock.o mavlink.o sha256.o
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -rf udpproxy *.o

distclean:
	rm -rf udpproxy *.o libraries/mavlink2/generated

