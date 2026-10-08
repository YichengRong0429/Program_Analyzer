// The same function body runs many times through recursion, so its branches
// are executed far more often than they appear in the source.
#include <stdio.h>

static int fib(int n) {
  if (n < 2)
    return n;
  return fib(n - 1) + fib(n - 2);
}

int main(void) {
  printf("fib(15) = %d\n", fib(15));
  return 0;
}
