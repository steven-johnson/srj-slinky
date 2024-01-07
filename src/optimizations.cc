#include "optimizations.h"

#include <cassert>
#include <set>
#include <iostream>

#include "evaluate.h"
#include "node_mutator.h"
#include "print.h"
#include "simplify.h"
#include "substitute.h"

namespace slinky {

namespace {

stmt clone_with_new_body(const let_stmt* op, stmt new_body) {
  return let_stmt::make(op->sym, op->value, std::move(new_body));
}
stmt clone_with_new_body(const allocate* op, stmt new_body) {
  return allocate::make(op->storage, op->sym, op->elem_size, op->dims, std::move(new_body));
}
stmt clone_with_new_body(const make_buffer* op, stmt new_body) {
  return make_buffer::make(op->sym, op->base, op->elem_size, op->dims, std::move(new_body));
}
stmt clone_with_new_body(const crop_buffer* op, stmt new_body) {
  return crop_buffer::make(op->sym, op->bounds, std::move(new_body));
}
stmt clone_with_new_body(const crop_dim* op, stmt new_body) {
  return crop_dim::make(op->sym, op->dim, op->bounds, std::move(new_body));
}
stmt clone_with_new_body(const slice_buffer* op, stmt new_body) {
  return slice_buffer::make(op->sym, op->at, std::move(new_body));
}
stmt clone_with_new_body(const slice_dim* op, stmt new_body) {
  return slice_dim::make(op->sym, op->dim, op->at, std::move(new_body));
}
stmt clone_with_new_body(const truncate_rank* op, stmt new_body) {
  return truncate_rank::make(op->sym, op->rank, std::move(new_body));
}

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

bool is_elementwise(const box_expr& in_x, symbol_id out) {
  expr out_var = variable::make(out);
  for (index_t d = 0; d < static_cast<index_t>(in_x.size()); ++d) {
    // TODO: This is too lax, we really need to check for elementwise before we've computed the bounds of this
    // particular call, so we can check that a single point of the output is a function of the same point in the input
    // (and not a rectangle of output being a function of a rectangle of the input).
    if (!match(in_x[d].min, buffer_min(out_var, d))) return false;
    if (!match(in_x[d].max, buffer_max(out_var, d))) return false;
  }
  return true;
}

class buffer_aliaser : public node_mutator {
  struct buffer_info {
    std::set<symbol_id> alias_candidates;
    bool elementwise = true;
  };
  symbol_map<buffer_info> alias_info;
  symbol_map<box_expr> buffer_bounds;
  symbol_map<symbol_id> aliases;

public:
  void visit(const allocate* op) override {
    box_expr bounds;
    bounds.reserve(op->dims.size());
    for (const dim_expr& d : op->dims) {
      bounds.push_back(d.bounds);
    }
    auto set_buffer_bounds = set_value_in_scope(buffer_bounds, op->sym, bounds);

    // When we allocate a buffer, we can look for all the uses of this buffer. If it is:
    // - consumed elemenwise,
    // - consumed by a producer that has an output that we can re-use,
    // - not consumed after the buffer it aliases to is produced,
    // then we can alias it to the buffer produced by its consumer.

    // Start out by setting it to elementwise.
    auto s = set_value_in_scope(alias_info, op->sym, buffer_info());
    stmt body = mutate(op->body);
    const buffer_info& info = *alias_info[op->sym];

    if (info.elementwise && !info.alias_candidates.empty()) {
      symbol_id target = *info.alias_candidates.begin();
      set_result(let_stmt::make(op->sym, variable::make(target), std::move(body)));
      aliases[op->sym] = target;
      // Remove this as a candidate for other aliases.
      for (std::optional<buffer_info>& i : alias_info) {
        if (!i) continue;
        i->alias_candidates.erase(target);
      }
    } else if (!body.same_as(op->body)) {
      set_result(clone_with_new_body(op, std::move(body)));
    } else {
      set_result(op);
    }
  }

  void visit(const call_stmt* op) override {
    set_result(op);
    for (symbol_id o : op->outputs) {
      for (symbol_id i : op->inputs) {
        const std::optional<box_expr>& in_x = buffer_bounds[i];
        std::optional<buffer_info>& info = alias_info[i];
        if (!in_x || !info) {
          info->elementwise = false;
          return;
        }

        if (!is_elementwise(*in_x, o)) {
          info->elementwise = false;
          return;
        }

        info->alias_candidates.insert(o);
      }
    }
  }

  void visit(const crop_buffer* c) override {
    std::optional<box_expr> bounds = buffer_bounds[c->sym];
    merge_crop(bounds, c->bounds);
    auto set_bounds = set_value_in_scope(buffer_bounds, c->sym, bounds);
    node_mutator::visit(c);
  }

  void visit(const crop_dim* c) override {
    std::optional<box_expr> bounds = buffer_bounds[c->sym];
    merge_crop(bounds, c->dim, c->bounds);
    auto set_bounds = set_value_in_scope(buffer_bounds, c->sym, bounds);
    node_mutator::visit(c);
  }

  // TODO: Need to handle this?
  void visit(const slice_buffer*) override { std::abort(); }
  void visit(const slice_dim*) override { std::abort(); }
  void visit(const truncate_rank*) override { std::abort(); }
};

}  // namespace

stmt alias_buffers(const stmt& s) { return buffer_aliaser().mutate(s); }

namespace {

class copy_implementer : public node_mutator {
  node_context& ctx;

public:
  copy_implementer(node_context& ctx) : ctx(ctx) {}

  void visit(const copy_stmt* c) override { set_result(c); }
};

}  // namespace

stmt implement_copies(const stmt& s, node_context& ctx) { return copy_implementer(ctx).mutate(s); }

namespace {

template <typename Fn>
void for_each_stmt_forward(const stmt& s, const Fn& fn) {
  if (const block* b = s.as<block>()) {
    for_each_stmt_forward(b->a, fn);
    for_each_stmt_forward(b->b, fn);
  } else {
    fn(s);
  }
}

template <typename Fn>
void for_each_stmt_backward(const stmt& s, const Fn& fn) {
  if (const block* b = s.as<block>()) {
    for_each_stmt_backward(b->b, fn);
    for_each_stmt_backward(b->a, fn);
  } else {
    fn(s);
  }
}

class scope_reducer : public node_mutator {
  std::tuple<stmt, stmt, stmt> split_body(const stmt& body, std::span<const symbol_id> vars) {
    stmt before;
    stmt new_body_after;
    bool depended_on = false;
    // First, split the body into the before, and the new body + after.
    for_each_stmt_forward(body, [&](const stmt& s) {
      if (depended_on || depends_on(s, vars)) {
        new_body_after = block::make({new_body_after, s});
        depended_on = true;
      } else {
        before = block::make({before, s});
      }
    });

    // Now, split the new body + after into the new body and the after.
    stmt new_body;
    stmt after;
    depended_on = false;
    for_each_stmt_backward(new_body_after, [&](const stmt& s) {
      if (!depended_on && !depends_on(s, vars)) {
        after = block::make({s, after});
      } else {
        new_body = block::make({s, new_body});
        depended_on = true;
      }
    });

    return {before, new_body, after};
  }
  std::tuple<stmt, stmt, stmt> split_body(const stmt& body, symbol_id var) {
    symbol_id vars[] = {var};
    return split_body(body, vars);
  }

public:
  template <typename T>
  void visit_stmt(const T* op) {
    stmt body = mutate(op->body);

    stmt before, new_body, after;
    std::tie(before, new_body, after) = split_body(body, op->sym);

    if (body.same_as(op->body) && !before.defined() && !after.defined()) {
      set_result(op);
    } else if (new_body.defined()) {
      stmt result = clone_with_new_body(op, std::move(new_body));
      set_result(block::make({before, result, after}));
    } else {
      // The op was dead...?
      set_result(block::make({before, after}));
    }
  }

  void visit(const let_stmt* op) override { visit_stmt(op); }
  void visit(const allocate* op) override { visit_stmt(op); }
  void visit(const make_buffer* op) override { visit_stmt(op); }
  void visit(const crop_buffer* op) override { visit_stmt(op); }
  void visit(const crop_dim* op) override { visit_stmt(op); }
  void visit(const slice_buffer* op) override { visit_stmt(op); }
  void visit(const slice_dim* op) override { visit_stmt(op); }
  void visit(const truncate_rank* op) override { visit_stmt(op); }
};

}  // namespace

stmt reduce_scopes(const stmt& s) { return scope_reducer().mutate(s); }

}  // namespace slinky
