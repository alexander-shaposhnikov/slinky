#include <gtest/gtest.h>

#include <cassert>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "builder/pipeline.h"
#include "runtime/expr.h"
#include "runtime/pipeline.h"

namespace slinky {

template <typename T, std::size_t N>
void init_random(buffer<T, N>& x) {
  x.allocate();
  for_each_contiguous_slice(x, [&](index_t extent, T* base) {
    for (index_t i = 0; i < extent; ++i) {
      base[i] = (rand() % 20) - 10;
    }
  });
}

// (Ab)use our expression mechanism to make an elementwise "calculator", for the purposes of testing.

template <typename T, std::size_t Rank>
class elementwise_pipeline_builder : public expr_visitor {
  node_context& ctx;

  std::vector<interval_expr> bounds;

  std::string name(const buffer_expr_ptr& b) const { return ctx.name(b->sym()); }

  std::map<var, buffer_expr_ptr> vars;
  std::vector<buffer_expr_ptr> constants;

public:
  elementwise_pipeline_builder(node_context& ctx) : ctx(ctx) {
    for (std::size_t d = 0; d < Rank; ++d) {
      dims.emplace_back(ctx, "d" + std::to_string(d));
      bounds.emplace_back(dims.back(), dims.back());
    }
  }

  std::vector<var> dims;
  std::vector<func> result_funcs;
  std::vector<buffer_expr_ptr> inputs;
  buffer_expr_ptr result;

  void visit(const variable* v) override {
    auto i = vars.find(v->sym);
    if (i != vars.end()) {
      result = i->second;
      return;
    }
    result = buffer_expr::make(v->sym, Rank, sizeof(T));
    inputs.push_back(result);
    vars[v->sym] = result;
  }

  void visit(const constant* c) override {
    slinky::dim dims[Rank];
    for (std::size_t d = 0; d < Rank; ++d) {
      dims[d] = dim::broadcast();
    }

    result = buffer_expr::make_scalar<T>(ctx, "c" + std::to_string(c->value), c->value);
    constants.push_back(result);
  }

  buffer_expr_ptr visit_expr(const expr& e) {
    e.accept(this);
    return result;
  }

  template <typename Impl>
  void visit_binary(const char* fn_name, const buffer_expr_ptr& a, const buffer_expr_ptr& b, const Impl& impl) {
    result = buffer_expr::make(ctx, name(a) + fn_name + name(b), Rank, sizeof(T));
    func::callable<const T, const T, T> fn = [=](const buffer<const T>& a, const buffer<const T>& b,
                                                 const buffer<T>& c) {
      for_each_element([&](T* c, const T* a, const T* b) { *c = impl(*a, *b); }, c, a, b);
      return 0;
    };
    func r = func::make<const T, const T, T>(
        std::move(fn), {{a, bounds}, {b, bounds}}, {{result, dims}}, call_stmt::attributes{.allow_in_place = 0x3});
    result_funcs.push_back(std::move(r));
  }

  template <typename Impl>
  void visit_binary(const char* fn, const expr& a, const expr& b, const Impl& impl) {
    visit_binary(fn, visit_expr(a), visit_expr(b), impl);
  }

  void visit(const class min* op) override {
    visit_binary("min", op->a, op->b, [](T a, T b) { return std::min(a, b); });
  }
  void visit(const class max* op) override {
    visit_binary("max", op->a, op->b, [](T a, T b) { return std::max(a, b); });
  }
  void visit(const class add* op) override { visit_binary("+", op->a, op->b, std::plus<T>()); }
  void visit(const sub* op) override { visit_binary("-", op->a, op->b, std::minus<T>()); }
  void visit(const mul* op) override { visit_binary("*", op->a, op->b, std::multiplies<T>()); }
  void visit(const slinky::div* op) override { visit_binary("/", op->a, op->b, std::divides<T>()); }
  void visit(const slinky::mod* op) override { visit_binary("%", op->a, op->b, std::modulus<T>()); }
  void visit(const less* op) override { visit_binary("<", op->a, op->b, std::less<T>()); }
  void visit(const less_equal* op) override { visit_binary("<=", op->a, op->b, std::less_equal<T>()); }
  void visit(const equal* op) override { visit_binary("==", op->a, op->b, std::equal_to<T>()); }
  void visit(const not_equal* op) override { visit_binary("==", op->a, op->b, std::not_equal_to<T>()); }
  void visit(const logical_and* op) override { visit_binary("&&", op->a, op->b, std::logical_and<T>()); }
  void visit(const logical_or* op) override { visit_binary("||", op->a, op->b, std::logical_or<T>()); }
  void visit(const class select* op) override {
    buffer_expr_ptr c = visit_expr(op->condition);
    buffer_expr_ptr t = visit_expr(op->true_value);
    buffer_expr_ptr f = visit_expr(op->false_value);
    result = buffer_expr::make(ctx, "select_" + name(c) + "_" + name(t) + "_" + name(f), Rank, sizeof(T));
    func::callable<const T, const T, const T, T> fn = [](const buffer<const T>& c, const buffer<const T>& t,
                                                          const buffer<const T>& f, const buffer<T>& r) -> index_t {
      for_each_element([&](T* r, const T* c, const T* t, const T* f) { *r = *c != 0 ? *t : *f; }, r, c, t, f);
      return 0;
    };
    func r = func::make<const T, const T, const T, T>(std::move(fn), {{c, bounds}, {t, bounds}, {f, bounds}},
        {{result, dims}}, call_stmt::attributes{.allow_in_place = 0xf});
    result_funcs.push_back(std::move(r));
  }

  void visit(const let*) override { SLINKY_UNREACHABLE; }
  void visit(const call*) override { SLINKY_UNREACHABLE; }
  void visit(const logical_not*) override { SLINKY_UNREACHABLE; }
};

template <typename T, std::size_t Rank>
class elementwise_pipeline_evaluator : public expr_visitor {
public:
  std::vector<index_t> extents;
  symbol_map<buffer<T, Rank>*> vars;

  buffer<T, Rank> result;

  void init_buffer(buffer<T, Rank>& b) {
    b.free();
    for (std::size_t d = 0; d < Rank; ++d) {
      b.dims[d].set_min_extent(0, extents[d]);
    }
    b.allocate();
  }

  void visit(const variable* v) override {
    const std::optional<buffer<T, Rank>*>& i = vars[v->sym];
    assert(i);
    result.free();
    init_buffer(result);
    copy(**i, result);
  }

  void visit(const constant* c) override {
    result.free();
    init_buffer(result);
    std::fill_n(result.base(), result.elem_count(), c->value);
  }

  void visit_expr(const expr& e, buffer<T, Rank>& r) {
    e.accept(this);
    init_buffer(r);
    copy(result, r);
  }

  template <typename Impl>
  void visit_binary(const char* fn, const expr& a, const expr& b, const Impl& impl) {
    buffer<T, Rank> a_buf;
    visit_expr(a, a_buf);
    b.accept(this);
    for_each_element([&](T* result, const T* a) { *result = impl(*a, *result); }, result, a_buf);
  }

  void visit(const class min* op) override {
    visit_binary("min", op->a, op->b, [](T a, T b) { return std::min(a, b); });
  }
  void visit(const class max* op) override {
    visit_binary("max", op->a, op->b, [](T a, T b) { return std::max(a, b); });
  }
  void visit(const class add* op) override { visit_binary("+", op->a, op->b, std::plus<T>()); }
  void visit(const sub* op) override { visit_binary("-", op->a, op->b, std::minus<T>()); }
  void visit(const mul* op) override { visit_binary("*", op->a, op->b, std::multiplies<T>()); }
  void visit(const slinky::div* op) override { visit_binary("/", op->a, op->b, std::divides<T>()); }
  void visit(const slinky::mod* op) override { visit_binary("%", op->a, op->b, std::modulus<T>()); }
  void visit(const less* op) override { visit_binary("<", op->a, op->b, std::less<T>()); }
  void visit(const less_equal* op) override { visit_binary("<=", op->a, op->b, std::less_equal<T>()); }
  void visit(const equal* op) override { visit_binary("==", op->a, op->b, std::equal_to<T>()); }
  void visit(const not_equal* op) override { visit_binary("==", op->a, op->b, std::not_equal_to<T>()); }
  void visit(const logical_and* op) override { visit_binary("&&", op->a, op->b, std::logical_and<T>()); }
  void visit(const logical_or* op) override { visit_binary("||", op->a, op->b, std::logical_or<T>()); }
  void visit(const class select* op) override {
    buffer<T, Rank> c_buf;
    visit_expr(op->condition, c_buf);
    buffer<T, Rank> t_buf;
    visit_expr(op->true_value, t_buf);
    op->false_value.accept(this);
    for_each_element([&](T* result, const T* c, const T* t) { *result = *c ? *t : *result; }, result, c_buf, t_buf);
  }

  void visit(const let*) override { SLINKY_UNREACHABLE; }
  void visit(const call*) override { SLINKY_UNREACHABLE; }
  void visit(const logical_not*) override { SLINKY_UNREACHABLE; }
};

template <typename T, std::size_t Rank>
void test_expr_pipeline(node_context& ctx, int split, const expr& e) {
  elementwise_pipeline_builder<T, Rank> builder(ctx);
  e.accept(&builder);

  if (split > 0) {
    builder.result_funcs.back().loops({{builder.dims.back(), split}});
  }

  pipeline p = build_pipeline(ctx, builder.inputs, {builder.result});

  std::vector<index_t> extents;
  for (std::size_t i = 0; i < Rank; ++i) {
    extents.push_back(i * 3 + 5);
  }

  std::vector<const raw_buffer*> inputs;
  std::vector<buffer<T, Rank>> input_bufs(p.inputs.size());

  for (std::size_t i = 0; i < p.inputs.size(); ++i) {
    for (std::size_t d = 0; d < Rank; ++d) {
      input_bufs[i].dims[d].set_min_extent(0, extents[d]);
    }
    init_random(input_bufs[i]);
    inputs.push_back(&input_bufs[i]);
  }

  buffer<T, Rank> output_buf(extents);
  output_buf.allocate();

  std::vector<const raw_buffer*> outputs;
  outputs.push_back(&output_buf);

  p.evaluate(inputs, outputs);

  elementwise_pipeline_evaluator<T, Rank> eval;
  eval.extents = extents;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    eval.vars[p.inputs[i]] = &input_bufs[i];
  }
  e.accept(&eval);

  for_each_element([&](T* output, T* eval_result) { ASSERT_EQ(*output, *eval_result); }, output_buf, eval.result);
}

namespace {

node_context ctx;
var x(ctx, "x");
var y(ctx, "y");
var z(ctx, "z");

expr pow(expr x, int n) {
  if (n == 0) {
    return 1;
  } else if (n == 1) {
    return x;
  } else if (n % 2 == 0) {
    return pow(x, n / 2) * pow(x, n / 2);
  } else {
    return x * pow(x, n - 1);
  }
}

}  // namespace

class elementwise : public testing::TestWithParam<int> {};

INSTANTIATE_TEST_SUITE_P(split, elementwise, testing::Range(0, 3));

TEST_P(elementwise, add_xy) { test_expr_pipeline<int, 1>(ctx, GetParam(), x + y); }
TEST_P(elementwise, mul_add) { test_expr_pipeline<int, 1>(ctx, GetParam(), x * y + z); }
TEST_P(elementwise, add_max_mul) { test_expr_pipeline<int, 1>(ctx, GetParam(), max(x + y, 0) * z); }
TEST_P(elementwise, exp1) { test_expr_pipeline<int, 1>(ctx, GetParam(), 1 + x); }
TEST_P(elementwise, exp2) { test_expr_pipeline<int, 1>(ctx, GetParam(), 1 + x + pow(x, 2)); }
TEST_P(elementwise, exp3) { test_expr_pipeline<int, 1>(ctx, GetParam(), 1 + x + pow(x, 2) + pow(x, 3)); }
TEST_P(elementwise, exp4) { test_expr_pipeline<int, 1>(ctx, GetParam(), 1 + x + pow(x, 2) + pow(x, 3) + pow(x, 4)); }
TEST_P(elementwise, exp8) {
  test_expr_pipeline<int, 1>(
      ctx, GetParam(), 1 + x + pow(x, 2) + pow(x, 3) + pow(x, 4) + pow(x, 5) + pow(x, 6) + pow(x, 7) + pow(x, 8));
}

TEST_P(elementwise, exp2_horners) { test_expr_pipeline<int, 1>(ctx, GetParam(), 1 + x * (1 + x)); }
TEST_P(elementwise, exp3_horners) { test_expr_pipeline<int, 1>(ctx, GetParam(), 1 + x * (1 + x * (1 + x))); }
TEST_P(elementwise, exp4_horners) { test_expr_pipeline<int, 1>(ctx, GetParam(), 1 + x * (1 + x * (1 + x * (1 + x)))); }
TEST_P(elementwise, exp8_horners) {
  test_expr_pipeline<int, 1>(
      ctx, GetParam(), 1 + x * (1 + x * (1 + x * (1 + x * (1 + x * (1 + x * (1 + x * (1 + x))))))));
}

}  // namespace slinky
