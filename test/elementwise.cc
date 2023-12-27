#include "expr.h"
#include "funcs.h"
#include "pipeline.h"
#include "print.h"
#include "test.h"

#include <cassert>

using namespace slinky;

// Compute max(a + b, 0) * c
void test_elementwise(const int W, const int H) {
  // Make the pipeline
  node_context ctx;

  auto a = buffer_expr::make(ctx, "a", sizeof(int), 2);
  auto b = buffer_expr::make(ctx, "b", sizeof(int), 2);
  auto c = buffer_expr::make(ctx, "c", sizeof(int), 2);

  auto ab = buffer_expr::make(ctx, "ab", sizeof(int), 2);
  auto maxab0 = buffer_expr::make(ctx, "maxab0", sizeof(int), 2);
  auto out = buffer_expr::make(ctx, "out", sizeof(int), 2);

  expr x = make_variable(ctx, "x");
  expr y = make_variable(ctx, "y");

  func f_ab = func::make<const int, const int, int>(
      add<int>, {a, {interval(x), interval(y)}}, {b, {interval(x), interval(y)}}, {ab, {x, y}});
  func f_maxab0 = func::make<const int, int>(max_0<int>, {ab, {interval(x), interval(y)}}, {maxab0, {x, y}});
  func f_maxab0c = func::make<const int, const int, int>(
      multiply<int>, {maxab0, {interval(x), interval(y)}}, {c, {interval(x), interval(y)}}, {out, {x, y}});

  pipeline p(ctx, {a, b, c}, {out});

  // Run the pipeline
  buffer<int, 2> a_buf({W, H});
  buffer<int, 2> b_buf({W, H});
  buffer<int, 2> c_buf({W, H});
  init_random(a_buf);
  init_random(b_buf);
  init_random(c_buf);

  buffer<int, 2> out_buf({W, H});
  out_buf.allocate();

  // Not having std::span(std::initializer_list<T>) is unfortunate.
  const buffer_base* inputs[] = {&a_buf, &b_buf, &c_buf};
  const buffer_base* outputs[] = {&out_buf};
  p.evaluate(inputs, outputs);

  for_each_index(
      out_buf, [&](std::span<index_t> i) { ASSERT_EQ(out_buf(i), std::max(a_buf(i) + b_buf(i), 0) * c_buf(i)); });
}

TEST(elementwise) { test_elementwise(40, 30); }