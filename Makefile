# Makefile for UDPProxy

.PHONY: headers

all: udpproxy

headers:
	@./regen_headers.sh

CXX=g++
CC=gcc
CFLAGS=-Wall -g -DSTANDALONE
CXXFLAGS=-Wall -g -Werror

LIBS="-ltdb"

udpproxy: udpproxy.o mavlink.o util.o keydb.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -rf udpproxy *.o

distclean:
	rm -rf udpproxy *.o libraries/mavlink2/generated

