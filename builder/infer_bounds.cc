#include "builder/infer_bounds.h"

#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <utility>
#include <vector>

#include "runtime/depends_on.h"
#include "runtime/expr.h"
#include "builder/node_mutator.h"
#include "builder/optimizations.h"
#include "builder/simplify.h"
#include "builder/substitute.h"
#include "runtime/util.h"

namespace slinky {

namespace {

// Get a reference to `n`th vector element of v, resizing the vector if necessary.
template <typename T>
T& vector_at(std::vector<T>& v, std::size_t n) {
  if (n >= v.size()) {
    v.resize(n + 1);
  }
  return v[n];
}
template <typename T>
T& vector_at(std::optional<std::vector<T>>& v, std::size_t n) {
  if (!v) {
    v = std::vector<T>(n + 1);
  }
  return vector_at(*v, n);
}

void merge_crop(std::optional<box_expr>& bounds, int dim, const interval_expr& new_bounds) {
  if (new_bounds.min.defined()) {
    vector_at(bounds, dim).min = new_bounds.min;
  }
  if (new_bounds.max.defined()) {
    vector_at(bounds, dim).max = new_bounds.max;
  }
}

void merge_crop(std::optional<box_expr>& bounds, const box_expr& new_bounds) {
  for (int d = 0; d < static_cast<int>(new_bounds.size()); ++d) {
    merge_crop(bounds, d, new_bounds[d]);
  }
}

class input_crop_remover : public node_mutator {
  symbol_map<bool> used_as_output;

public:
  void visit(const call_stmt* op) {
    for (symbol_id i : op->outputs) {
      used_as_output[i] = true;
    }
    set_result(op);
  }
  void visit(const copy_stmt* op) {
    used_as_output[op->dst] = true;
    set_result(op);
  }

  template <typename T>
  void visit_crop(const T* op) {
    std::optional<bool> old_value = used_as_output[op->sym];
    used_as_output[op->sym] = false;
    stmt body = mutate(op->body);

    if (!*used_as_output[op->sym]) {
      used_as_output[op->sym] = old_value;
      set_result(body);
      return;
    }
    used_as_output[op->sym] = true;

    if (body.same_as(op->body)) {
      set_result(op);
    } else {
      set_result(clone_with_new_body(op, std::move(body)));
    }
  }

  void visit(const crop_buffer* op) { visit_crop(op); }
  void visit(const crop_dim* op) { visit_crop(op); }
};

// Keep substituting substitutions until nothing happens.
std::vector<dim_expr> recursive_substitute(
    std::vector<dim_expr> dims, span<const std::pair<expr, expr>> substitutions) {
  while (true) {
    bool changed = false;
    for (dim_expr& dim : dims) {
      dim_expr new_dim = dim;
      for (const std::pair<expr, expr>& j : substitutions) {
        new_dim.bounds.min = substitute(new_dim.bounds.min, j.first, j.second);
        new_dim.bounds.max = substitute(new_dim.bounds.max, j.first, j.second);
        new_dim.stride = substitute(new_dim.stride, j.first, j.second);
        new_dim.fold_factor = substitute(new_dim.fold_factor, j.first, j.second);
      }
      if (!new_dim.same_as(dim)) {
        changed = true;
        dim = new_dim;
      }
    }
    if (!changed) return dims;
  }
}

// This pass tries to identify where call_stmt operations need to run to satisfy the requirements of their consumers (or
// the output buffers). It updates `allocate` nodes to allocate enough memory for the uses of the allocation, and crops
// producers to the required region.
class bounds_inferrer : public node_mutator {
public:
  symbol_map<box_expr> infer;
  symbol_map<box_expr> crops;

  void visit(const allocate* op) override {
    auto set_bounds = set_value_in_scope(infer, op->sym, box_expr());
    stmt body = mutate(op->body);

    // When we constructed the pipeline, the buffer dimensions were set to buffer_* calls.
    // (This is a little janky because the buffers they are loading from don't exist where they are used.)
    // Here, we are building a list of replacements for those expressions. This way, if the user did something
    // like buf->dim(0).extent = buf->dim(0).extent + 10 (i.e. pad the extent by 10), we'll add 10 to our
    // inferred value.
    // TODO: Is this actually a good design...?
    std::vector<std::pair<expr, expr>> substitutions;

    expr alloc_var = variable::make(op->sym);

    box_expr& bounds = *infer[op->sym];
    expr stride = static_cast<index_t>(op->elem_size);
    for (index_t d = 0; d < static_cast<index_t>(bounds.size()); ++d) {
      const interval_expr& bounds_d = bounds[d];

      substitutions.emplace_back(buffer_min(alloc_var, d), bounds_d.min);
      substitutions.emplace_back(buffer_max(alloc_var, d), bounds_d.max);
      substitutions.emplace_back(buffer_stride(alloc_var, d), stride);

      // We didn't initially set up the buffer with an extent, but the user might have used it.
      expr extent = bounds_d.extent();
      substitutions.emplace_back(buffer_extent(alloc_var, d), extent);
      stride *= min(extent, buffer_fold_factor(alloc_var, d));
    }
    std::vector<dim_expr> dims = recursive_substitute(op->dims, substitutions);

    // Check that the actual bounds we generated are bigger than the inferred bounds (in case the
    // user set the bounds to something too small).
    std::vector<stmt> checks;
    for (std::size_t d = 0; d < dims.size(); ++d) {
      if (d < bounds.size()) {
        // TODO: These checks fail in a case that seems reasonable (setting the bounds of a buffer that
        // is going to be copied for the purposes of adding padding). Maybe the checks aren't valid?
        // Maybe they should be skipped if the consumer is a copy? That seems like a hack.
        // checks.push_back(check::make(dims[d].min() <= bounds[d].min));
        // checks.push_back(check::make(dims[d].max() >= bounds[d].max));
      }
    }

    // Substitute the allocation bounds in any remaining inferred bounds.
    for (std::optional<box_expr>& i : infer) {
      if (!i) continue;
      for (interval_expr& j : *i) {
        for (const auto& k : substitutions) {
          j.min = substitute(j.min, k.first, k.second);
          j.max = substitute(j.max, k.first, k.second);
        }
      }
    }

    stmt s = allocate::make(op->sym, op->storage, op->elem_size, std::move(dims), body);
    set_result(block::make(block::make(checks), s));
  }

  void visit(const call_stmt* op) override {
    // Record the bounds we currently have from the crops.
    for (symbol_id input : op->inputs) {
      if (infer.contains(input)) {
        infer[input] = crops[input];
      }
    }
    set_result(op);
  }

  void visit(const copy_stmt* op) override {
    // Record the bounds we currently have from the crops.
    if (infer.contains(op->src)) {
      infer[op->src] = crops[op->src];
    }
    set_result(op);
  }

  void visit(const crop_buffer* op) override {
    std::optional<box_expr> crop = crops[op->sym];
    merge_crop(crop, op->bounds);
    auto set_crop = set_value_in_scope(crops, op->sym, crop);
    node_mutator::visit(op);
  }

  void visit(const crop_dim* op) override {
    std::optional<box_expr> crop = crops[op->sym];
    merge_crop(crop, op->dim, op->bounds);
    auto set_crop = set_value_in_scope(crops, op->sym, crop);
    node_mutator::visit(op);
  }

  // TODO: Need to handle this?
  void visit(const slice_buffer*) override { std::abort(); }
  void visit(const slice_dim*) override { std::abort(); }
  void visit(const truncate_rank*) override { std::abort(); }

  void visit(const loop* op) override {
    stmt body = mutate(op->body);

    stmt result;
    if (body.same_as(op->body)) {
      result = op;
    } else {
      // We rewrote the loop min.
      result = loop::make(op->sym, op->mode, op->bounds, op->step, std::move(body));
    }

    // We're leaving the body of op. If any of the bounds used that loop variable, we need
    // to replace those uses with the bounds of the loop.
    for (symbol_id buf = 0; buf < infer.size(); ++buf) {
      std::optional<box_expr>& inferring = infer[buf];
      if (!inferring) continue;

      for (interval_expr& j : *inferring) {
        // We need to be careful of the case where min > max, such as when a pipeline
        // flips a dimension.
        // TODO: This seems janky/possibly not right.
        if (depends_on(j.min, op->sym)) {
          j.min = simplify(static_cast<const class min*>(nullptr), substitute(j.min, op->sym, op->bounds.min),
              substitute(j.min, op->sym, op->bounds.max));
        }
        if (depends_on(j.max, op->sym)) {
          j.max = simplify(static_cast<const class max*>(nullptr), substitute(j.max, op->sym, op->bounds.min),
              substitute(j.max, op->sym, op->bounds.max));
        }
      }
      result = crop_buffer::make(buf, *inferring, result);
    }
    set_result(result);
  }
};

void substitute_bounds(box_expr& bounds, const symbol_map<box_expr>& buffers) {
  for (symbol_id i = 0; i < buffers.size(); ++i) {
    if (!buffers[i]) continue;
    for (interval_expr& j : bounds) {
      if (j.min.defined()) j.min = substitute_bounds(j.min, i, *buffers[i]);
      if (j.max.defined()) j.max = substitute_bounds(j.max, i, *buffers[i]);
    }
  }
}

// Try to find cases where we can do "sliding window" or "line buffering" optimizations. When there
// is a producer that is consumed by a stencil operation in a loop, the producer can incrementally produce
// only the values required by the next iteration, and re-use the rest of the values from the previous iteration.
class slide_and_fold_storage : public node_mutator {
public:
  node_context& ctx;
  symbol_map<box_expr> buffer_bounds;
  symbol_map<std::pair<int, expr>> fold_factors;
  struct loop_info {
    symbol_id sym;
    expr orig_min;
    interval_expr bounds;
    expr step;
  };
  std::vector<loop_info> loops;

  // We need an unknown to make equations of.
  var x;

  slide_and_fold_storage(node_context& ctx) : ctx(ctx), x(ctx.insert_unique("_x")) {}

  void visit(const allocate* op) override {
    box_expr bounds;
    bounds.reserve(op->dims.size());
    for (const dim_expr& d : op->dims) {
      bounds.push_back(d.bounds);
    }
    auto set_buffer_bounds = set_value_in_scope(buffer_bounds, op->sym, bounds);
    stmt body = mutate(op->body);

    // When we constructed the pipeline, the buffer dimensions were set to buffer_* calls.
    // (This is a little janky because the buffers they are loading from don't exist where they are used.)
    // Here, we are building a list of replacements for those expressions. This way, if the user did something
    // like buf->dim(0).extent = buf->dim(0).extent + 10 (i.e. pad the extent by 10), we'll add 10 to our
    // inferred value.
    // TODO: Is this actually a good design...?
    std::optional<std::pair<int, expr>> fold_info = fold_factors[op->sym];
    std::vector<std::pair<expr, expr>> replacements;
    for (index_t d = 0; d < static_cast<index_t>(op->dims.size()); ++d) {
      expr alloc_var = variable::make(op->sym);
      if (fold_info && fold_info->first == d) {
        replacements.emplace_back(buffer_fold_factor(alloc_var, d), fold_info->second);
      } else {
        // Treat the fold factor as infinity for now.
        replacements.emplace_back(buffer_fold_factor(alloc_var, d), positive_infinity());
      }
    }
    std::vector<dim_expr> dims = recursive_substitute(op->dims, replacements);
    // Replace infinite fold factors with undefined.
    for (dim_expr& d : dims) {
      if (is_positive_infinity(d.fold_factor)) d.fold_factor = expr();
    }

    set_result(allocate::make(op->sym, op->storage, op->elem_size, std::move(dims), body));
  }

  template <typename T>
  void visit_call_or_copy(const T* op, span<const symbol_id> outputs) {
    stmt result = op;
    for (symbol_id output : outputs) {
      std::optional<box_expr>& bounds = buffer_bounds[output];
      if (!bounds) continue;

      for (std::size_t op = 0; op < loops.size(); ++op) {
        symbol_id loop_sym = loops[op].sym;
        expr loop_var = variable::make(loop_sym);
        const expr& loop_max = loops[op].bounds.max;
        const expr& loop_step = loops[op].step;

        for (int d = 0; d < static_cast<int>(bounds->size()); ++d) {
          interval_expr cur_bounds_d = (*bounds)[d];
          if (!depends_on(cur_bounds_d, loop_sym)) {
            // TODO: In this case, the func is entirely computed redundantly on every iteration. We should be able to
            // just compute it once.
            continue;
          }

          interval_expr prev_bounds_d = {
              substitute(cur_bounds_d.min, loop_sym, loop_var - loop_step),
              substitute(cur_bounds_d.max, loop_sym, loop_var - loop_step),
          };

          // A few things here struggle to simplify when there is a min(loop_max, x) expression involved, where x is
          // some expression that is bounded by the loop bounds. This min simplifies away if we know that x <= loop_max,
          // but the simplifier can't figure that out. As a hopefully temporary workaround, we can just substitute
          // infinity for the loop max.
          auto ignore_loop_max = [=](const expr& e) { return substitute(e, loop_max, positive_infinity()); };

          interval_expr overlap = prev_bounds_d & cur_bounds_d;
          if (prove_true(ignore_loop_max(overlap.empty()))) {
            // The bounds of each loop iteration do not overlap. We can't re-use work between loop iterations, but we
            // can fold the storage.
            expr fold_factor = simplify(bounds_of(ignore_loop_max(cur_bounds_d.extent())).max);
            if (!depends_on(fold_factor, loop_sym)) {
              fold_factors[output] = {d, fold_factor};
            } else {
              // The fold factor didn't simplify to something that doesn't depend on the loop variable.
            }
            continue;
          }

          // Allowing the leading edge to not change means that some calls may ask for empty buffers.
          expr is_monotonic_increasing = prev_bounds_d.min <= cur_bounds_d.min && prev_bounds_d.max <= cur_bounds_d.max;
          expr is_monotonic_decreasing = prev_bounds_d.min >= cur_bounds_d.min && prev_bounds_d.max >= cur_bounds_d.max;
          if (prove_true(ignore_loop_max(is_monotonic_increasing))) {
            // The bounds for each loop iteration overlap and are monotonically increasing,
            // so we can incrementally compute only the newly required bounds.
            expr old_min = cur_bounds_d.min;
            expr new_min = simplify(prev_bounds_d.max + 1);

            expr fold_factor = simplify(bounds_of(ignore_loop_max(cur_bounds_d.extent())).max);
            if (!depends_on(fold_factor, loop_sym)) {
              // Align the fold factor to the loop step size, so it doesn't try to crop across a folding boundary.
              fold_factor = simplify(align_up(fold_factor, loop_step));
              fold_factors[output] = {d, fold_factor};
            } else {
              // The fold factor didn't simplify to something that doesn't depend on the loop variable.
            }

            // Now that we're only computing the newly required parts of the domain, we need
            // to move the loop min back so we compute the whole required region. We'll insert
            // ifs around the other parts of the loop to avoid expanding the bounds that those
            // run on.
            expr new_min_at_new_loop_min = substitute(new_min, loop_sym, x);
            expr old_min_at_loop_min = substitute(old_min, loop_sym, loops[op].bounds.min);
            expr new_loop_min =
                where_true(ignore_loop_max(new_min_at_new_loop_min <= old_min_at_loop_min), x.sym()).max;
            if (!is_negative_infinity(new_loop_min)) {
              loops[op].bounds.min = new_loop_min;

              (*bounds)[d].min = new_min;
            } else {
              // We couldn't find the new loop min. We need to warm up the loop on the first iteration.
              // TODO: If another loop or func adjusts the loop min, we're going to run before the original min... that
              // seems like it might be fine anyways here, but pretty janky.
              (*bounds)[d].min = select(loop_var == loops[op].orig_min, old_min, new_min);
            }
            break;
          } else if (prove_true(ignore_loop_max(is_monotonic_decreasing))) {
            // TODO: We could also try to slide when the bounds are monotonically
            // decreasing, but this is an unusual case.
          }
        }
      }
    }

    // Insert ifs around these calls, in case the loop min shifts later.
    // TODO: We don't actually need to do this, because the crops applied to this buffer will be clamped, and we'll end
    // up with an empty production. It's a little janky, but it handles the case where the warmup is not aligned to the
    // loop step size.
    // for (const auto& op : loops) {
    //  result = if_then_else::make(variable::make(op.sym) >= op.bounds.min, result, stmt());
    //}
    set_result(result);
  }

  void visit(const call_stmt* op) override { visit_call_or_copy(op, op->outputs); }
  void visit(const copy_stmt* op) override { visit_call_or_copy(op, {&op->dst, 1}); }

  void visit(const crop_buffer* op) override {
    std::optional<box_expr> bounds = buffer_bounds[op->sym];
    merge_crop(bounds, op->bounds);
    if (bounds) {
      substitute_bounds(*bounds, buffer_bounds);
    }
    auto set_bounds = set_value_in_scope(buffer_bounds, op->sym, bounds);
    stmt body = mutate(op->body);
    if (buffer_bounds[op->sym]) {
      box_expr new_bounds = *buffer_bounds[op->sym];
      set_result(crop_buffer::make(op->sym, std::move(new_bounds), std::move(body)));
    } else {
      set_result(crop_buffer::make(op->sym, op->bounds, std::move(body)));
    }
  }

  void visit(const crop_dim* op) override {
    std::optional<box_expr> bounds = buffer_bounds[op->sym];
    merge_crop(bounds, op->dim, op->bounds);
    substitute_bounds(*bounds, buffer_bounds);
    auto set_bounds = set_value_in_scope(buffer_bounds, op->sym, bounds);
    stmt body = mutate(op->body);
    interval_expr new_bounds = (*buffer_bounds[op->sym])[op->dim];

    if (body.same_as(op->body) && new_bounds.same_as(op->bounds)) {
      set_result(op);
    } else {
      set_result(crop_dim::make(op->sym, op->dim, std::move(new_bounds), std::move(body)));
    }
  }

  // TODO: Need to handle this?
  void visit(const slice_buffer*) override { std::abort(); }
  void visit(const slice_dim*) override { std::abort(); }
  void visit(const truncate_rank*) override { std::abort(); }

  void visit(const loop* op) override {
    if (op->mode == loop_mode::parallel) {
      // Don't try sliding window or storage folding on parallel loops.
      node_mutator::visit(op);
      return;
    }
    var orig_min(ctx, ctx.name(op->sym) + ".min_orig");

    loops.push_back({op->sym, orig_min, bounds(orig_min, op->bounds.max), op->step});
    stmt body = mutate(op->body);
    expr loop_min = loops.back().bounds.min;
    loops.pop_back();

    if (loop_min.same_as(orig_min)) {
      loop_min = op->bounds.min;
    }

    if (!is_variable(loop_min, orig_min.sym()) || depends_on(body, orig_min.sym())) {
      // We rewrote or used the loop min.
      stmt result = loop::make(op->sym, op->mode, {loop_min, op->bounds.max}, op->step, std::move(body));
      set_result(let_stmt::make(orig_min.sym(), op->bounds.min, result));
      return;
    }

    if (body.same_as(op->body)) {
      set_result(op);
    } else {
      set_result(loop::make(op->sym, op->mode, op->bounds, op->step, std::move(body)));
    }
  }

  void visit(const block* op) override {
    // Visit blocks in reverse order. TODO: Is this really sufficient?
    stmt b = mutate(op->b);
    stmt a = mutate(op->a);
    if (a.same_as(op->a) && b.same_as(op->b)) {
      set_result(op);
    } else {
      set_result(block::make(a, b));
    }
  }
};

stmt infer_bounds(const stmt& s, const std::vector<symbol_id>& inputs) {
  // Tell the bounds inferrer that we are inferring the bounds of the inputs too.
  bounds_inferrer infer;
  for (symbol_id i : inputs) {
    infer.infer[i] = box_expr();
  }
  stmt result = infer.mutate(s);

  // Now we should know the bounds required of the inputs. Add checks that the inputs are sufficient.
  std::vector<stmt> checks;
  for (symbol_id i : inputs) {
    expr buf_var = variable::make(i);
    const box_expr& bounds = *infer.infer[i];
    for (int d = 0; d < static_cast<int>(bounds.size()); ++d) {
      checks.push_back(check::make(buffer_min(buf_var, d) <= bounds[d].min));
      checks.push_back(check::make(buffer_max(buf_var, d) >= bounds[d].max));
      checks.push_back(check::make(bounds[d].extent() <= buffer_fold_factor(buf_var, d)));
    }
  }
  return block::make(block::make(checks), result);
}

}  // namespace

stmt infer_bounds(const stmt& s, node_context& ctx, const std::vector<symbol_id>& inputs) {
  stmt result = s;

  result = infer_bounds(s, inputs);
  // We cannot simplify between infer_bounds and fold_storage, because we need to be able to rewrite the bounds
  // of producers while we still understand the dependencies between stages.
  result = slide_and_fold_storage(ctx).mutate(result);

  // At this point, crops of input buffers are unnecessary.
  // TODO: This is actually necessary for correctness in the case of folded buffers, but this shouldn't
  // be the case.
  // TODO: This is now somewhat redundant with the simplifier, but what the simplifier does is more correct.
  // Unfortunately, we need the more aggressive incorrect crop removal here! This needs to be fixed, and this
  // should be removed completely.
  result = input_crop_remover().mutate(result);

  // Now we can simplify.
  result = simplify(result);
  result = reduce_scopes(result);

  // Try to reuse buffers and eliminate copies where possible.
  result = alias_buffers(result);
  result = optimize_copies(result);

  result = simplify(result);
  result = reduce_scopes(result);

  return result;
}

}  // namespace slinky
