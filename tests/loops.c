// for, while and do-while loops plus if/else: no `goto` in the source, yet
// the compiled IR is full of unconditional branches.
#include <stdio.h>

static int count_primes(int limit) {
  int count = 0;
  for (int n = 2; n < limit; n++) {
    int prime = 1;
    for (int d = 2; d * d <= n; d++) {
      if (n % d == 0) {
        prime = 0;
        break;
      }
    }
    if (prime)
      count++;
  }
  return count;
}

static int collatz_steps(int x) {
  int steps = 0;
  while (x != 1) {
    if (x % 2 == 0)
      x /= 2;
    else
      x = 3 * x + 1;
    steps++;
  }
  return steps;
}

int main(void) {
  int primes = count_primes(100);
  int steps = collatz_steps(27);
  int k = 0;
  do {
    k++;
  } while (k < 5);
  printf("primes below 100: %d, collatz(27): %d steps, k = %d\n", primes,
         steps, k);
  return 0;
}
