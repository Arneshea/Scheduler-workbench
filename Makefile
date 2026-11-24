CXX ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic
COMMON = -std=c++17 -pthread

ifeq ($(OS),Windows_NT)
EXE = .exe
endif

.PHONY: all test
all: scheduler$(EXE)

scheduler$(EXE): main.cpp simulator.cpp simulator.hpp resources.hpp
	$(CXX) $(CXXFLAGS) $(COMMON) main.cpp simulator.cpp -o $@

scheduler_tests$(EXE): tests.cpp simulator.cpp simulator.hpp resources.hpp
	$(CXX) $(CXXFLAGS) $(COMMON) tests.cpp simulator.cpp -o $@

test: scheduler_tests$(EXE)
	./scheduler_tests$(EXE)
