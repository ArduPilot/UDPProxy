# Makefile for UDPProxy

# Compiler settings
CXX ?= g++
CXXFLAGS := -O2 -Wall -g -Werror -Wextra -Werror=format -Wpointer-arith -Wcast-align -Wno-missing-field-initializers -Wno-unused-parameter -Wno-redundant-decls
CXXFLAGS := $(CXXFLAGS) -Wno-unknown-pragmas -Wno-trigraphs -Werror=shadow -Werror=return-type -Werror=unused-result -Werror=unused-variable -Werror=narrowing
CXXFLAGS := $(CXXFLAGS) -Werror=attributes -Werror=overflow -Werror=parentheses -Werror=format-extra-args -Werror=ignored-qualifiers -Werror=undef

# Library settings
LIBS := -ltdb -lssl -lcrypto

# Source files
SOURCES := udpproxy.cpp mavlink.cpp util.cpp keydb.cpp websocket.cpp
OBJECTS := $(SOURCES:.cpp=.o)
TARGET := udpproxy

# Build directories
BUILD_DIR := build
MAVLINK_DIR := libraries/mavlink2/generated

.PHONY: all clean distclean headers modules help test

# Default target
all: modules headers $(TARGET)

# Help target
help:
	@echo "UDPProxy Build System"
	@echo "====================="
	@echo "Available targets:"
	@echo "  all       - Build everything (default)"
	@echo "  headers   - Generate MAVLink headers"
	@echo "  modules   - Initialize git submodules"
	@echo "  clean     - Remove build artifacts"
	@echo "  distclean - Remove all generated files"
	@echo "  test      - Run basic tests"
	@echo "  help      - Show this help message"
	@echo ""
	@echo "Environment variables:"
	@echo "  CXX       - C++ compiler (default: g++)"

# Git submodules
modules: modules/mavlink/message_definitions/v1.0/all.xml

modules/mavlink/message_definitions/v1.0/all.xml:
	@echo "Initializing git submodules..."
	@git submodule update --init --recursive

# MAVLink headers generation
headers: $(MAVLINK_DIR)/protocol.h

$(MAVLINK_DIR)/protocol.h: modules/mavlink/message_definitions/v1.0/all.xml
	@echo "Generating MAVLink headers..."
	@./regen_headers.sh

# Main target
$(TARGET): $(OBJECTS)
	@echo "Linking $(TARGET)..."
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

# Object file compilation
%.o: %.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Special rule for mavlink.o to suppress stringop-truncation warning
# This is needed due to MAVLink library using strncpy for fixed-size character arrays
mavlink.o: mavlink.cpp mavlink.h $(MAVLINK_DIR)/protocol.h
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) -Wno-stringop-truncation -c $< -o $@

# Dependencies
udpproxy.o: udpproxy.cpp mavlink.h util.h keydb.h websocket.h
mavlink.o: mavlink.cpp mavlink.h $(MAVLINK_DIR)/protocol.h
util.o: util.cpp util.h
keydb.o: keydb.cpp keydb.h
websocket.o: websocket.cpp websocket.h util.h

# Testing
test: $(TARGET)
	@echo "Running basic tests..."
	@echo "Checking if binary was built correctly..."
	@file $(TARGET)
	@echo "Checking if keydb.py is executable..."
	@python3 -m py_compile keydb.py
	@echo "Basic tests passed!"

# Cleaning
clean:
	@echo "Cleaning build artifacts..."
	rm -f $(TARGET) $(OBJECTS)

distclean: clean
	@echo "Cleaning all generated files..."
	rm -rf $(MAVLINK_DIR)

