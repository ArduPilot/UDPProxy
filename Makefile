# Makefile for UDPProxy

.PHONY: headers

all: modules headers udpproxy

modules: modules/mavlink/message_definitions/v1.0/all.xml

modules/mavlink/message_definitions/v1.0/all.xml:
	@git submodule update --init --recursive

headers: libraries/mavlink2/generated/protocol.h

libraries/mavlink2/generated/protocol.h: modules/mavlink/message_definitions/v1.0/all.xml
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

