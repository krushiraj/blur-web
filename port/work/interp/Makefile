RECOMP = ../../recomp
CFLAGS = -O2 -g -w -ffp-contract=off -I. -I$(RECOMP) -I/opt/homebrew/include
all: difftest unit_test
unit: unit_test
	./unit_test
unit_test: unit_test.c interp.c interp.h $(RECOMP)/rt_vec.c
	clang $(CFLAGS) unit_test.c interp.c $(RECOMP)/rt_vec.c -lm -o $@
difftest: difftest.c interp.c interp.h $(RECOMP)/rt_vec.c
	clang $(CFLAGS) difftest.c interp.c $(RECOMP)/rt_vec.c -L/opt/homebrew/lib -lunicorn -lm -o $@
probe: libinterp_probe.dylib
libinterp_probe.dylib: interp.c interp.h
	clang -O2 -w -ffp-contract=off -DINTERP_TEST_API -shared -fPIC -I. -I$(RECOMP) interp.c tools/probe_stubs.c $(RECOMP)/rt_vec.c -o $@

# differential test with half of the corpus running as lifted C and the rest interpreted (needs mixed/hot.c from mixed/gen_hot.py)
difftest_mixed: difftest.c interp.c interp.h $(RECOMP)/rt_vec.c mixed/hot.c mixed/hot_table.c
	clang -O1 -g -w -ffp-contract=off -DMIXED -I. -I$(RECOMP) -I/opt/homebrew/include difftest.c interp.c $(RECOMP)/rt_vec.c mixed/hot.c mixed/hot_table.c -L/opt/homebrew/lib -lunicorn -lm -o $@

# WebAssembly builds of the interpreter benchmark (needs emcc + node): size and speed under node
wasm: bench/bench_wasm.js
bench/bench_wasm.js: bench/bench.c bench/bench_code.h interp.c interp.h $(RECOMP)/rt_vec.c
	emcc -Os -mtail-call -w -ffp-contract=off -I. -I$(RECOMP) bench/bench.c interp.c $(RECOMP)/rt_vec.c -sINITIAL_MEMORY=100663296 -sENVIRONMENT=node -sEXIT_RUNTIME=1 -o $@
	emcc -Os -mtail-call -w -ffp-contract=off -I. -I$(RECOMP) -c interp.c -o bench/interp_wasm.o && /opt/homebrew/opt/llvm/bin/llvm-size bench/interp_wasm.o
	node bench/bench_wasm.js 12000
bench/bench_native: bench/bench.c bench/bench_code.h interp.c interp.h $(RECOMP)/rt_vec.c
	clang -O2 -w -ffp-contract=off -I. -I$(RECOMP) bench/bench.c interp.c $(RECOMP)/rt_vec.c -lm -o $@

# bench/bench_code.h is generated from bench/bench.s (32-bit x86, assembled with llvm-mc)
LLVM = /opt/homebrew/opt/llvm/bin
bench/bench_code.h: bench/bench.s
	$(LLVM)/llvm-mc -triple=i386-unknown-linux-gnu -filetype=obj bench/bench.s -o bench/bench.o && $(LLVM)/llvm-objcopy -O binary --only-section=.text bench/bench.o bench/bench.bin && cd bench && xxd -i bench.bin > bench_code.h
