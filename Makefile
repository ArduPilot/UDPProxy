# Makefile for UDPProxy

.PHONY: headers

all: udpproxy

headers:
	@./regen_headers.sh

CXX=g++
CC=gcc
CFLAGS=-Wall -g -DSTANDALONE
CXXFLAGS=-Wall -g -Werror

LIBS=-ltdb -lssl -lcrypto

udpproxy: udpproxy.o mavlink.o util.o keydb.o websocket.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -rf udpproxy *.o

distclean:
	rm -rf udpproxy *.o libraries/mavlink2/generated

