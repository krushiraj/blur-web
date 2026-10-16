OPT ?= -O1 -fno-strict-aliasing
OBJDIR ?= obj4
GEN ?= gen_all
SRC=$(wildcard $(GEN)/part_*.c) $(GEN)/table.c
OBJ=$(patsubst $(GEN)/%.c,$(OBJDIR)/%.o,$(SRC))
all: $(OBJ)
$(OBJDIR)/%.o: $(GEN)/%.c rt.h
	@mkdir -p $(OBJDIR)
	clang $(OPT) -w -ffp-contract=off -fno-unwind-tables -fno-asynchronous-unwind-tables -I. -c $< -o $@
