#ifndef SLINKY_BUILDER_TEST_SIMPLIFY_RULE_TESTER_H
#define SLINKY_BUILDER_TEST_SIMPLIFY_RULE_TESTER_H

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cassert>

#include "base/test/seeded_test.h"
#include "builder/rewrite.h"
#include "builder/simplify.h"
#include "builder/substitute.h"
#include "builder/test/simplify/expr_generator.h"
#include "runtime/evaluate.h"
#include "runtime/expr.h"
#include "runtime/print.h"

namespace slinky {

namespace {

bool contains_infinity(expr x) {
  expr no_infinity = x;
  no_infinity = substitute(no_infinity, positive_infinity(), std::numeric_limits<index_t>::max());
  no_infinity = substitute(no_infinity, negative_infinity(), std::numeric_limits<index_t>::min());
  return !no_infinity.same_as(x);
}

}  // namespace

class rule_tester {
  static constexpr std::size_t var_count = 6;

  gtest_seeded_mt19937 rng_;
  expr_generator<gtest_seeded_mt19937> expr_gen_;

  std::array<expr, rewrite::symbol_count> exprs;
  rewrite::match_context m;

  void init_match_context() {
    for (std::size_t i = 0; i < m.constants.size(); ++i) {
      m.constants[i] = expr_gen_.random_constant();
    }
    for (std::size_t i = 0; i < rewrite::symbol_count; ++i) {
      exprs[i] = expr_gen_.random_expr(0);
      m.vars[i] = exprs[i].get();
    }
  }

public:
  rule_tester() : expr_gen_(rng_, var_count) { init_match_context(); }

  SLINKY_NO_INLINE void test_expr(expr pattern, expr replacement, const std::string& rule_str) {
    if (contains_infinity(pattern)) {
      // TODO: Maybe there's a way to test this...
      return;
    }

    expr simplified = simplify(pattern);
    ASSERT_FALSE(pattern.same_as(simplified)) << "Rule did not apply: " << rule_str << "\nTo: " << pattern << "\n";

    eval_context ctx;
    for (int test = 0; test < 100; ++test) {
      for (std::size_t i = 0; i < var_count; ++i) {
        ctx[var(i)] = expr_gen_.random_constant();
      }

      auto dump_ctx = [&]() { 
        std::stringstream ss;
        for (std::size_t i = 0; i < var_count; ++i) {
          ss << ", " << var(i) << "=" << ctx[var(i)];
        }
        return ss.str();
      };

      index_t value = evaluate(pattern, ctx);
      index_t replacement_value = evaluate(replacement, ctx);
      index_t simplified_value = evaluate(simplified, ctx);
      ASSERT_EQ(value, replacement_value) << "Incorrect rule: " << rule_str << "\n"
                                          << pattern << " -> " << replacement << dump_ctx() << "\n";
      ASSERT_EQ(value, simplified_value) << "Incorrect simplification: " << rule_str << "\n"
                                         << pattern << " -> " << simplified << dump_ctx() << "\n";
    }
  }

  template <typename Pattern, typename Replacement>
  bool operator()(const Pattern& p, const Replacement& r) {
    // This function needs to be kept small and simple, because it is instantiated by hundreds of different rules.
    std::stringstream rule_str;
    rule_str << p << " -> " << r;

    bool overflowed = false;
    expr pattern = expr(substitute(p, m, overflowed));
    assert(!overflowed);
    expr replacement = expr(substitute(r, m, overflowed));
    assert(!overflowed);

    // Make sure the expressions have the same value when evaluated.
    test_expr(pattern, replacement, rule_str.str());

    // Returning false means the rule applicator will continue to the next rule.
    return false;
  }

  template <typename Pattern, typename Replacement, typename Predicate>
  bool operator()(const Pattern& p, const Replacement& r, const Predicate& pr) {
    // This function needs to be kept small and simple, because it is instantiated by hundreds of different rules.
    std::stringstream rule_str;
    rule_str << p << " -> " << r << " if " << pr;

    // Some rules are very picky about a large number of constants, which makes it very unlikely to generate an
    // expression that the rule applies to.
    for (int test = 0; test < 100000; ++test) {
      init_match_context();
      bool overflowed = false;
      if (substitute(pr, m, overflowed) && !overflowed) {
        expr pattern = expr(substitute(p, m, overflowed));
        assert(!overflowed);
        expr replacement = expr(substitute(r, m, overflowed));
        assert(!overflowed);

        // Make sure the expressions have the same value when evaluated.
        test_expr(pattern, replacement, rule_str.str());

        // Returning false means the rule applicator will continue to the next rule.
        return false;
      }
    }
    const bool rule_applied = false;
    // We failed to apply the rule to an expression.
    EXPECT_TRUE(rule_applied) << rule_str.str();
    // Returning true stops any more tests.
    return true;
  }

  template <typename Pattern, typename Replacement, typename Predicate, typename... ReplacementPredicate>
  bool operator()(const Pattern& p, const Replacement& r, const Predicate& pr, ReplacementPredicate... r_pr) {
    return operator()(p, r, pr) && operator()(p, r_pr...);
  }
};

}  // namespace slinky

#endif  // SLINKY_BUILDER_TEST_SIMPLIFY_RULE_TESTER_H