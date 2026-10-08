// A tiny bytecode interpreter written two ways: with a `switch` and with GNU
// computed goto (`goto *label`), which compiles to an `indirectbr`.
#include <stdio.h>

enum { OP_INC, OP_DEC, OP_DOUBLE, OP_HALT };

static const unsigned char program[] = {OP_INC, OP_INC,    OP_DOUBLE, OP_DEC,
                                        OP_INC, OP_DOUBLE, OP_HALT};

static int run_switch(void) {
  int acc = 0;
  for (int pc = 0;; pc++) {
    switch (program[pc]) {
    case OP_INC:
      acc++;
      break;
    case OP_DEC:
      acc--;
      break;
    case OP_DOUBLE:
      acc *= 2;
      break;
    case OP_HALT:
      return acc;
    }
  }
}

static int run_computed_goto(void) {
  static void *dispatch[] = {&&do_inc, &&do_dec, &&do_double, &&do_halt};
  int acc = 0;
  int pc = 0;
  goto *dispatch[program[pc]];
do_inc:
  acc++;
  goto *dispatch[program[++pc]];
do_dec:
  acc--;
  goto *dispatch[program[++pc]];
do_double:
  acc *= 2;
  goto *dispatch[program[++pc]];
do_halt:
  return acc;
}

int main(void) {
  printf("switch interpreter: %d, computed-goto interpreter: %d\n",
         run_switch(), run_computed_goto());
  return 0;
}
