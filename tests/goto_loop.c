// A loop written with an explicit `goto`. The number of iterations comes from
// the command line, so the count can only be known by running the program.
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  int n = argc > 1 ? atoi(argv[1]) : 10;
  int i = 0;
  int sum = 0;

loop:
  if (i >= n)
    goto done;
  sum += i;
  i++;
  goto loop;

done:
  printf("sum of 0..%d = %d\n", n - 1, sum);
  return 0;
}
