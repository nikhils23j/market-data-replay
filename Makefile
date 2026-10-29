CXX ?= clang++
CXXFLAGS ?= -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic
CPPFLAGS ?= -Iinclude

LIBSRC = src/book.cpp src/io.cpp src/replay.cpp
BUILDDIR = build

.PHONY: all test bench demo clean

all: $(BUILDDIR)/mdr_replay $(BUILDDIR)/mdr_bench $(BUILDDIR)/mdr_tests

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/mdr_replay: src/main.cpp $(LIBSRC) | $(BUILDDIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(BUILDDIR)/mdr_bench: src/bench.cpp $(LIBSRC) | $(BUILDDIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

$(BUILDDIR)/mdr_tests: tests/tests.cpp $(LIBSRC) | $(BUILDDIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@

test: $(BUILDDIR)/mdr_tests
	./$(BUILDDIR)/mdr_tests

demo: $(BUILDDIR)/mdr_replay
	./$(BUILDDIR)/mdr_replay --events samples/events.csv --snapshot samples/snapshot.csv

bench: $(BUILDDIR)/mdr_bench
	./$(BUILDDIR)/mdr_bench --events 100000 --levels 4096 --seed 42

clean:
	rm -rf $(BUILDDIR)
