#include "expr.h"

namespace slinky {

std::string node_context::name(symbol_id i) const { 
  if (i < id_to_name.size()) {
    return id_to_name[i];
  } else {
    return "<" + std::to_string(i) + ">";
  }
}

symbol_id node_context::insert(const std::string& name) {
  auto i = name_to_id.find(name);
  if (i == name_to_id.end()) {
    symbol_id id = id_to_name.size();
    id_to_name.push_back(name);
    name_to_id[name] = id;
    return id;
  }
  return i->second;
}
symbol_id node_context::insert() {
  symbol_id id = id_to_name.size();
  std::string name = "t" + std::to_string(id);
  id_to_name.push_back(name);
  name_to_id[name] = id;
  return id;
}
symbol_id node_context::lookup(const std::string& name) const {
  auto i = name_to_id.find(name);
  if (i != name_to_id.end()) {
    return i->second;
  } else {
    return -1;
  }
}

template <typename T>
std::shared_ptr<const T> make_bin_op(expr a, expr b) {
  auto n = std::make_shared<T>();
  n->a = std::move(a);
  n->b = std::move(b);
  return n;
}

template <typename T, typename Body>
std::shared_ptr<const T> make_let(symbol_id name, expr value, Body body) {
  auto n = std::make_shared<T>();
  n->name = name;
  n->value = std::move(value);
  n->body = std::move(body);
  return n;
}

expr let::make(symbol_id name, expr value, expr body) {
  return make_let<let>(name, std::move(value), std::move(body)).get();
}

stmt let_stmt::make(symbol_id name, expr value, stmt body) {
  return make_let<let_stmt>(name, std::move(value), std::move(body)).get();
}

expr variable::make(symbol_id name) {
  auto n = std::make_shared<variable>();
  n->name = name;
  return n.get();
}

std::shared_ptr<const constant> make_constant(index_t value) {
  auto n = std::make_shared<constant>();
  n->value = value;
  return n;
}

expr constant::make(index_t value) {
  return make_constant(value).get();
}

expr::expr(index_t value) : expr(make_constant(value).get()) {}

expr constant::make(const void* value) {
  return make(reinterpret_cast<index_t>(value));
}

stmt::stmt(std::initializer_list<stmt> stmts) {
  stmt result;
  for (const stmt& i : stmts) {
    if (result.defined()) {
      result = block::make(std::move(result), std::move(i));
    } else {
      result = std::move(i);
    }
  }
  s = result.s;
}

expr add::make(expr a, expr b) { return make_bin_op<add>(std::move(a), std::move(b)).get(); }
expr sub::make(expr a, expr b) { return make_bin_op<sub>(std::move(a), std::move(b)).get(); }
expr mul::make(expr a, expr b) { return make_bin_op<mul>(std::move(a), std::move(b)).get(); }
expr div::make(expr a, expr b) { return make_bin_op<div>(std::move(a), std::move(b)).get(); }
expr mod::make(expr a, expr b) { return make_bin_op<mod>(std::move(a), std::move(b)).get(); }
expr min::make(expr a, expr b) { return make_bin_op<min>(std::move(a), std::move(b)).get(); }
expr max::make(expr a, expr b) { return make_bin_op<max>(std::move(a), std::move(b)).get(); }
expr equal::make(expr a, expr b) { return make_bin_op<equal>(std::move(a), std::move(b)).get(); }
expr not_equal::make(expr a, expr b) { return make_bin_op<not_equal>(std::move(a), std::move(b)).get(); }
expr less::make(expr a, expr b) { return make_bin_op<less>(std::move(a), std::move(b)).get(); }
expr less_equal::make(expr a, expr b) { return make_bin_op<less_equal>(std::move(a), std::move(b)).get(); }
expr bitwise_and::make(expr a, expr b) { return make_bin_op<bitwise_and>(std::move(a), std::move(b)).get(); }
expr bitwise_or::make(expr a, expr b) { return make_bin_op<bitwise_or>(std::move(a), std::move(b)).get(); }
expr bitwise_xor::make(expr a, expr b) { return make_bin_op<bitwise_xor>(std::move(a), std::move(b)).get(); }
expr logical_and::make(expr a, expr b) { return make_bin_op<logical_and>(std::move(a), std::move(b)).get(); }
expr logical_or::make(expr a, expr b) { return make_bin_op<logical_or>(std::move(a), std::move(b)).get(); }
expr shift_left::make(expr a, expr b) { return make_bin_op<shift_left>(std::move(a), std::move(b)).get(); }
expr shift_right::make(expr a, expr b) { return make_bin_op<shift_right>(std::move(a), std::move(b)).get(); }

expr make_variable(node_context& ctx, const std::string& name) {
  return variable::make(ctx.insert(name));
}

expr operator+(expr a, expr b) { return add::make(std::move(a), std::move(b)); }
expr operator-(expr a, expr b) { return sub::make(std::move(a), std::move(b)); }
expr operator*(expr a, expr b) { return mul::make(std::move(a), std::move(b)); }
expr operator/(expr a, expr b) { return div::make(std::move(a), std::move(b)); }
expr operator%(expr a, expr b) { return mod::make(std::move(a), std::move(b)); }
expr min(expr a, expr b) { return min::make(std::move(a), std::move(b)); }
expr max(expr a, expr b) { return max::make(std::move(a), std::move(b)); }
expr operator==(expr a, expr b) { return equal::make(std::move(a), std::move(b)); }
expr operator!=(expr a, expr b) { return not_equal::make(std::move(a), std::move(b)); }
expr operator<(expr a, expr b) { return less::make(std::move(a), std::move(b)); }
expr operator<=(expr a, expr b) { return less_equal::make(std::move(a), std::move(b)); }
expr operator>(expr a, expr b) { return less::make(std::move(b), std::move(a)); }
expr operator>=(expr a, expr b) { return less_equal::make(std::move(b), std::move(a)); }
expr operator&(expr a, expr b) { return bitwise_and::make(std::move(a), std::move(b)); }
expr operator|(expr a, expr b) { return bitwise_or::make(std::move(a), std::move(b)); }
expr operator^(expr a, expr b) { return bitwise_xor::make(std::move(a), std::move(b)); }
expr operator&&(expr a, expr b) { return logical_and::make(std::move(a), std::move(b)); }
expr operator||(expr a, expr b) { return logical_or::make(std::move(a), std::move(b)); }
expr operator<<(expr a, expr b) { return shift_left::make(std::move(a), std::move(b)); }
expr operator>>(expr a, expr b) { return shift_right::make(std::move(a), std::move(b)); }

expr load_buffer_meta::make(expr buffer, buffer_meta meta, expr dim) {
  auto n = std::make_shared<load_buffer_meta>();
  n->buffer = std::move(buffer);
  n->meta = meta;
  n->dim = std::move(dim);
  return n.get();
}

stmt call::make(call::callable target, std::vector<expr> scalar_args, std::vector<symbol_id> buffer_args, const func* fn) {
  auto n = std::make_shared<call>();
  n->target = std::move(target);
  n->scalar_args = std::move(scalar_args);
  n->buffer_args = std::move(buffer_args);
  n->fn = fn;
  return n.get();
}

stmt block::make(stmt a, stmt b) {
  auto n = std::make_shared<block>();
  n->a = std::move(a);
  n->b = std::move(b);
  return n.get();
}

stmt loop::make(symbol_id name, expr n, stmt body) {
  auto l = std::make_shared<loop>();
  l->name = name;
  l->n = std::move(n);
  l->body = std::move(body);
  return l.get();
}

stmt if_then_else::make(expr condition, stmt true_body, stmt false_body) {
  auto n = std::make_shared<if_then_else>();
  n->condition = std::move(condition);
  n->true_body = std::move(true_body);
  n->false_body = std::move(false_body);
  return n.get();
}

stmt allocate::make(memory_type type, symbol_id name, index_t elem_size, std::vector<dim_expr> dims, stmt body) {
  auto n = std::make_shared<allocate>();
  n->type = type;
  n->name = name;
  n->elem_size = elem_size;
  n->dims = std::move(dims);
  n->body = std::move(body);
  return n.get();
}

stmt check::make(expr condition) {
  auto n = std::make_shared<check>();
  n->condition = std::move(condition);
  return n.get();
}

}  // namespace slinky
