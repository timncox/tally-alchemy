# Native tests for the Tally engine (host compiler, no hardware).
#   make            ASan+UBSan build, run
#   make fast       -O2 without sanitizers (for the CPU number)
CXX      ?= c++
SRC      := ../src/tally/tally_engine.cpp
CXXFLAGS := -std=gnu++17 -O2 -g -I../src/tally -Wall -Wextra
SAN      := -fsanitize=address,undefined -fno-omit-frame-pointer
BIN      := bin

.PHONY: all fast
all: $(BIN)/test_tally
	./$(BIN)/test_tally

fast: $(BIN)/test_tally_fast
	./$(BIN)/test_tally_fast

$(BIN):
	mkdir -p $(BIN)

$(BIN)/test_tally: test_tally.cpp $(SRC) ../src/tally/tally_engine.h | $(BIN)
	$(CXX) $(CXXFLAGS) $(SAN) test_tally.cpp $(SRC) -o $@

$(BIN)/test_tally_fast: test_tally.cpp $(SRC) ../src/tally/tally_engine.h | $(BIN)
	$(CXX) $(CXXFLAGS) test_tally.cpp $(SRC) -o $@
