// C++ input: classes, virtual methods and a range-based for loop. Function
// names in the report are demangled (e.g. Circle::area() const).
#include <cstdio>

struct Shape {
  virtual ~Shape() = default;
  virtual double area() const = 0;
};

struct Square : Shape {
  double side;
  explicit Square(double s) : side(s) {}
  double area() const override { return side * side; }
};

struct Circle : Shape {
  double r;
  explicit Circle(double r) : r(r) {}
  double area() const override { return 3.14159 * r * r; }
};

int main() {
  Square s1(1), s2(2);
  Circle c1(1), c2(3);
  const Shape *shapes[] = {&s1, &c1, &s2, &c2};

  double total = 0;
  int big = 0;
  for (const Shape *s : shapes) {
    total += s->area();
    if (s->area() > 3.5)
      big++;
  }
  std::printf("total area = %.2f, shapes larger than 3.5: %d\n", total, big);
  return 0;
}
