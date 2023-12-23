#include "pipeline.h"

#include <cassert>
#include <set>
#include <iostream>

#include "evaluate.h"
#include "print.h"
#include "infer_allocate_bounds.h"
#include "simplify.h"
#include "substitute.h"

namespace slinky {

buffer_expr::buffer_expr(symbol_id name, index_t elem_size, std::size_t rank) : name_(name), elem_size_(elem_size), producer_(nullptr) {
  dims_.reserve(rank);
  auto var = variable::make(name);
  for (index_t i = 0; i < static_cast<index_t>(rank); ++i) {
    expr min = load_buffer_meta::make(var, buffer_meta::min, i);
    expr extent = load_buffer_meta::make(var, buffer_meta::extent, i);
    expr stride_bytes = load_buffer_meta::make(var, buffer_meta::stride_bytes, i);
    expr fold_factor = load_buffer_meta::make(var, buffer_meta::fold_factor, i);
    dims_.emplace_back(min, extent, stride_bytes, fold_factor);
  }
}

buffer_expr_ptr buffer_expr::make(symbol_id name, index_t elem_size, std::size_t rank) {
  return buffer_expr_ptr(new buffer_expr(name, elem_size, rank));
}

buffer_expr_ptr buffer_expr::make(node_context& ctx, const std::string& name, index_t elem_size, std::size_t rank) {
  return buffer_expr_ptr(new buffer_expr(ctx.insert(name), elem_size, rank));
}

void buffer_expr::add_producer(func* f) {
  assert(producer_ == nullptr);
  producer_ = f;
}

void buffer_expr::add_consumer(func* f) {
  assert(std::find(consumers_.begin(), consumers_.end(), f) == consumers_.end());
  consumers_.push_back(f);
}

func::func(callable impl, std::vector<input> inputs, std::vector<output> outputs)
  : impl_(std::move(impl)), inputs_(std::move(inputs)), outputs_(std::move(outputs)) {
  for (auto& i : inputs_) {
    i.buffer->add_consumer(this);
  }
  for (auto& i : outputs_) {
    i.buffer->add_producer(this);
  }
}

namespace {

class pipeline_builder {
  // We're going to incrementally build the body, starting at the end of the pipeline and adding producers as necessary.
  std::set<buffer_expr_ptr> to_produce;
  std::set<buffer_expr_ptr> produced;
  std::set<buffer_expr_ptr> allocated;
  stmt result;

public:
  pipeline_builder(const std::vector<buffer_expr_ptr>& inputs, const std::vector<buffer_expr_ptr>& outputs) {
    // To start with, we need to produce the outputs.
    for (auto& i : outputs) {
      to_produce.insert(i);
      allocated.insert(i);
    }
    for (auto& i : inputs) {
      produced.insert(i);
    }

    // Find all the buffers we need to produce.
    while (true) {
      std::set<buffer_expr_ptr> produce_next;
      for (const buffer_expr_ptr& i : to_produce) {
        if (!i->producer()) {
          // Must be an input.
          continue;
        }

        for (const func::input& j : i->producer()->inputs()) {
          if (!to_produce.count(j.buffer)) {
            produce_next.insert(j.buffer);
          }
        }
      }
      if (produce_next.empty()) {
        break;
      }
      to_produce.insert(produce_next.begin(), produce_next.end());
    }
  }

  // Find the func f to run next. This is the func that produces a buffer we need that we have not yet produced,
  // and all the buffers produced by f are ready to be consumed.
  const func* find_next_producer() const {
    const func* f;
    for (const buffer_expr_ptr& i : to_produce) {
      if (produced.count(i)) {
        // Already produced.
        continue;
      }
      f = i->producer();

      for (const func* j : i->consumers()) {
        for (const func::output& k : j->outputs()) {
          if (k.buffer == i) {
            // This is the buffer we are proposing to produce now.
            continue;
          }
          if (!produced.count(k.buffer)) {
            // f produces a buffer that is needed by another func that has not yet run.
            f = nullptr;
            break;
          }
        }
      }
      if (f) {
        return f;
      }
    }
    return nullptr;
  }

  bool complete() const {
    return produced.size() == to_produce.size(); 
  }

  void produce(stmt& result, const func* f) {
    // TODO: We shouldn't need this wrapper, it might add measureable overhead.
    // All it does is split a span of buffers into two spans of buffers.
    std::size_t input_count = f->inputs().size();
    std::size_t output_count = f->outputs().size();
    auto wrapper = [impl = f->impl(), input_count, output_count](std::span<const index_t>, std::span<buffer_base*> buffers) -> index_t {
      assert(buffers.size() == input_count + output_count);
      return impl(buffers.subspan(0, input_count), buffers.subspan(input_count, output_count));
    };
    std::vector<symbol_id> buffer_args;
    buffer_args.reserve(input_count + output_count);
    std::vector<buffer_expr_ptr> allocations;
    allocations.reserve(output_count);
    for (const func::input& i : f->inputs()) {
      buffer_args.push_back(i.buffer->name());
    }
    for (const func::output& i : f->outputs()) {
      buffer_args.push_back(i.buffer->name());
      if (!allocated.count(i.buffer)) {
        allocations.push_back(i.buffer);
        allocated.insert(i.buffer);
      }
    }
    stmt call_f = call::make(std::move(wrapper), {}, std::move(buffer_args), f);

    // Generate the loops that we want to be explicit. 
    for (const auto& loop : f->loops()) {
      interval bounds;
      std::vector<std::pair<int, buffer_expr_ptr>> to_crop;
      for (const auto& o : f->outputs()) {
        for (int d = 0; d < o.dims.size(); ++d) {
          if (match(o.dims[d], loop)) {
            to_crop.emplace_back(d, o.buffer);
            // This output uses this loop. Add it to the bounds.
            interval bounds_d(o.buffer->dim(d).min, o.buffer->dim(d).max());
            if (bounds.min.defined() && bounds.max.defined()) {
              bounds |= bounds_d;
            } else {
              bounds = bounds_d;
            }
          }
        }
      }

      for (const auto& i : to_crop) {
        call_f = crop::make(i.second->name(), i.first, loop, 1, call_f);
      }

      // Before making this loop, see if there are any producers we need to insert here.
      for (const buffer_expr_ptr& i : to_produce) {
        if (!i->producer()) {
          // Must be an input.
          continue;
        }
        const func::loop_id& compute_at = i->producer()->compute_at();
        if (compute_at.f == f && *as_variable(compute_at.loop) == *as_variable(loop)) {
          produce(call_f, i->producer());
        }
      }

      call_f = loop::make(*as_variable(loop), bounds.min, bounds.max + 1, call_f);
    }
    result = result.defined() ? block::make(call_f, result) : call_f;
    for (const auto& i : allocations) {
      result = allocate::make(i->storage(), i->name(), i->elem_size(), i->dims(), result);
    }

    for (const func::output& i : f->outputs()) {
      produced.insert(i.buffer);
    }
  }
};

stmt build_pipeline(node_context& ctx, const std::vector<buffer_expr_ptr>& inputs, const std::vector<buffer_expr_ptr>& outputs) {
  pipeline_builder builder(inputs, outputs);

  stmt result;

  while (!builder.complete()) {
    // Find a buffer to produce.
    const func* f = builder.find_next_producer();

    // Call the producer.
    if (!f) {
      // TODO: Make a better error here.
      std::cerr << "Problem in dependency graph" << std::endl;
      std::abort();
    }

    builder.produce(result, f);
  }

  result = infer_allocate_bounds(result, ctx);

  result = simplify(result);
  print(std::cerr, result, &ctx);

  return result;
}

}  // namespace

pipeline::pipeline(node_context& ctx, std::vector<buffer_expr_ptr> inputs, std::vector<buffer_expr_ptr> outputs)
  : inputs_(std::move(inputs)), outputs_(std::move(outputs)) {
  body = build_pipeline(ctx, inputs_, outputs_);
}

namespace {

void set_buffer(eval_context& ctx, const buffer_expr_ptr& buf_expr, const buffer_base* buf) {
  assert(buf_expr->rank() == buf->rank);

  ctx.set(buf_expr->name(), reinterpret_cast<index_t>(buf));

  for (std::size_t i = 0; i < buf->rank; ++i) {
    // If these asserts fail, it's because the user has added constraints to the buffer_expr,
    // e.g. buf.dim[0].stride_bytes = 4, and the buffer passed in does not satisfy that
    // constraint.
    assert(evaluate(buf_expr->dim(i).min, ctx) == buf->dims[i].min);
    assert(evaluate(buf_expr->dim(i).extent, ctx) == buf->dims[i].extent);
    assert(evaluate(buf_expr->dim(i).stride_bytes, ctx) == buf->dims[i].stride_bytes);
    assert(evaluate(buf_expr->dim(i).fold_factor, ctx) == buf->dims[i].fold_factor);
  }
}

}  // namespace

index_t pipeline::evaluate(std::span<buffer_base*> inputs, std::span<buffer_base*> outputs) {
  assert(inputs.size() == inputs_.size());
  assert(outputs.size() == outputs_.size());

  eval_context ctx;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    set_buffer(ctx, inputs_[i], inputs[i]);
  }
  for (std::size_t i = 0; i < outputs.size(); ++i) {
    set_buffer(ctx, outputs_[i], outputs[i]);
  }

  return slinky::evaluate(body, ctx);
}

}  // namespace slinky