# Thin wrapper around CMake. All build output lives under build/.
JOBS ?= 8
REL  = build/release
ASAN = build/asan
TSAN = build/tsan

.PHONY: all test fuzz demo bench eval asan tsan clean

all:
	cmake -S . -B $(REL) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(REL) -j $(JOBS)

test: all
	ctest --test-dir $(REL) --output-on-failure

fuzz: all
	./$(REL)/mdr_fuzz --seeds 8 --events 500000 --sessions 40 --session-messages 20000

demo: all
	./$(REL)/mdr_gen --out build/demo --messages 200000 --seed 7
	./$(REL)/mdr_replay --dir build/demo

bench: all
	./$(REL)/mdr_gen --out build/data --messages 3000000 --seed 42
	./$(REL)/mdr_bench --dir build/data --reps 5 --pace-reps 3

eval:
	./scripts/run_eval.sh

asan:
	cmake -S . -B $(ASAN) -DCMAKE_BUILD_TYPE=Debug -DMDR_SANITIZE=address,undefined
	cmake --build $(ASAN) -j $(JOBS)
	./$(ASAN)/mdr_tests
	./$(ASAN)/mdr_fuzz --seeds 4 --events 100000 --sessions 8 --session-messages 10000

tsan:
	cmake -S . -B $(TSAN) -DCMAKE_BUILD_TYPE=Debug -DMDR_SANITIZE=thread
	cmake --build $(TSAN) -j $(JOBS) --target mdr_tests mdr_gen mdr_replay
	./$(TSAN)/mdr_tests
	./$(TSAN)/mdr_gen --out build/tsan/data --messages 200000 --seed 3
	./$(TSAN)/mdr_replay --dir build/tsan/data --threads 2

clean:
	rm -rf build
