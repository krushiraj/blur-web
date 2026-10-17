OPT ?= -O1 -fno-strict-aliasing
OBJDIR ?= obj4
GEN ?= gen_all
SRC=$(wildcard $(GEN)/part_*.c) $(GEN)/table.c
OBJ=$(patsubst $(GEN)/%.c,$(OBJDIR)/%.o,$(SRC))
all: $(OBJ)
PRE ?=
$(OBJDIR)/%.o: $(GEN)/%.c rt.h
	@mkdir -p $(OBJDIR)
	$(if $(PRE),$(PRE) $< | clang $(OPT) -w -ffp-contract=off -fno-unwind-tables -fno-asynchronous-unwind-tables -I. -x c -c - -o $@,clang $(OPT) -w -ffp-contract=off -fno-unwind-tables -fno-asynchronous-unwind-tables -I. -c $< -o $@)
