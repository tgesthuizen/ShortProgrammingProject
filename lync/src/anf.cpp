#include "anf.h"
#include "expr.h"
#include "passes.h"
#include "symbol_table.h"

#include <algorithm>
#include <utility>

namespace lyn {

namespace {

struct fun_info {
  std::string_view name;
  const lambda_expr &expr;
  bool global;
};

class anf_generator {
public:
  explicit anf_generator(string_table &stbl, const symbol_table &symtab)
      : stbl{stbl}, symtab{symtab}, next_id{symtab.get_next_id()} {}

  void push_func(std::string_view name, lambda_expr *ptr) {
    funcs_to_generate.push_back(fun_info{name, *ptr, true});
  }

  void run();
  int get_next_id() const { return next_id; }
  anf_context &&get_context() && { return std::move(ctx); }

private:
  int visit_expr(const lyn::expr &value);

  string_table &stbl;
  const symbol_table &symtab;
  int next_id;

  std::vector<fun_info> funcs_to_generate = {};
  anf_context ctx = {};
  anf_def *current_def = nullptr;
  basic_block *current_block = nullptr;
  bool tail_pos = true;
};

void anf_generator::run() {
  for (std::size_t i = 0; i < std::size(funcs_to_generate); ++i) {
    auto &&info = funcs_to_generate[i];
    anf_def new_def;
    new_def.name = info.name;
    anf_receive prologue;
    prologue.args.reserve(std::size(info.expr.params));
    std::transform(std::begin(info.expr.params), std::end(info.expr.params),
                   std::back_inserter(prologue.args),
                   [](const variable_expr &expr) { return expr.id; });
    new_def.blocks.emplace_back();
    new_def.blocks.back().content.emplace_back(std::move(prologue));
    new_def.blocks.back().content.emplace_back(anf_adjust_stack{});
    current_def = &new_def;
    current_block = &new_def.blocks.back();
    tail_pos = true;
    visit_expr(*info.expr.body);
    ctx.defs.emplace_back(std::move(new_def));
  }
}

int anf_generator::visit_expr(const lyn::expr &value) {
  const auto visit_fun = [this](auto &&expr) {
    using expr_t = std::decay_t<decltype(expr)>;
    if constexpr (std::is_same_v<expr_t, constant_expr>) {
      const int constant_id = next_id++;
      current_block->content.emplace_back(
          anf_expr{anf_constant{expr.value, constant_id}});
      if (tail_pos)
        current_block->content.emplace_back(anf_expr{anf_return{constant_id}});
      return constant_id;
    }
    if constexpr (std::is_same_v<expr_t, variable_expr>) {
      if (expr.id >= symtab.get_first_local_id()) {
        if (tail_pos)
          current_block->content.emplace_back(anf_return{expr.id});
        return expr.id;
      }
      const auto global_id = next_id++;
      current_block->content.emplace_back(anf_global{expr.name, global_id});
      return global_id;
    }
    if constexpr (std::is_same_v<expr_t, apply_expr>) {
      const bool tail_pos_saved = std::exchange(tail_pos, false);
      const int fid = visit_expr(*expr.func);
      std::vector<int> args;
      args.reserve(std::size(expr.args));
      std::transform(std::begin(expr.args), std::end(expr.args),
                     std::back_inserter(args),
                     [this](lyn::expr *arg) { return visit_expr(*arg); });
      tail_pos = tail_pos_saved;
      int call_id = 0;
      if (!tail_pos)
        call_id = next_id++;
      current_block->content.emplace_back(
          anf_call{fid, std::move(args), call_id, tail_pos});
      return call_id;
    }
    if constexpr (std::is_same_v<expr_t, lambda_expr>) {
      const int lambda_id = next_id++;
      const auto fun_name = stbl.store("fun" + std::to_string(lambda_id));
      funcs_to_generate.push_back(fun_info{fun_name, expr, false});
      current_block->content.emplace_back(anf_global{fun_name, lambda_id});
      return lambda_id;
    }
    if constexpr (std::is_same_v<expr_t, let_expr>) {
      const auto tail_pos_saved = std::exchange(tail_pos, false);
      for (auto &&binding : expr.bindings) {
        const int bid = visit_expr(*binding.body);
        current_block->content.emplace_back(anf_assoc{bid, binding.id});
      }
      tail_pos = tail_pos_saved;
      if (std::empty(expr.body)) {
        return 0;
      }
      std::for_each(std::begin(expr.body), std::end(expr.body) - 1,
                    [this](lyn::expr *ptr) { visit_expr(*ptr); });
      return visit_expr(*expr.body.back());
    }
    if constexpr (std::is_same_v<expr_t, if_expr>) {
      const auto tail_pos_saved = std::exchange(tail_pos, false);
      const int cond_id = visit_expr(*expr.cond);
      tail_pos = tail_pos_saved;
      // Please be very aware in the below section that inserting into
      // current_def->blocks might get you a dangling current_block
      const std::size_t this_block_idx =
          current_block - current_def->blocks.data();
      current_def->blocks.emplace_back();
      current_def->blocks.emplace_back();
      if (!tail_pos)
        current_def->blocks.emplace_back();
      const int total_blocks = std::size(current_def->blocks);
      const int then_block = total_blocks - 2;
      const int else_block = total_blocks - 1;
      const int cont_block = total_blocks - 3;
      current_def->blocks[this_block_idx].content.push_back(
          anf_cond{cond_id, then_block, else_block});
      current_block = &current_def->blocks[then_block];
      current_block->content.emplace_back(anf_adjust_stack{});
      const auto then_id = visit_expr(*expr.then);
      current_block = &current_def->blocks[else_block];
      current_block->content.emplace_back(anf_adjust_stack{});
      const auto else_id = visit_expr(*expr.els);
      if (tail_pos)
        return 0;
      const int ret_id = next_id++;
      current_def->blocks[then_block].content.emplace_back(
          anf_assoc{then_id, ret_id});
      current_def->blocks[then_block].content.emplace_back(
          anf_jump{cont_block});
      current_def->blocks[else_block].content.emplace_back(
          anf_assoc{else_id, ret_id});
      current_def->blocks[else_block].content.emplace_back(
          anf_jump{cont_block});
      current_block = &current_def->blocks[cont_block];
      return ret_id;
    }
  };
  return std::visit(visit_fun, value.content);
}

} // namespace

std::unique_ptr<anf_context, delete_anf>
genanf(std::vector<toplevel_expr> &exprs, string_table &stbl,
       const symbol_table &symtab) {
  anf_generator gen(stbl, symtab);
  for (auto &&expr : exprs) {
    if (!expr.value)
      continue;
    if (!std::holds_alternative<lambda_expr>(expr.value->content)) {
      fprintf(stderr, "Will not generate anything for %.*s!\n",
              static_cast<int>(std::size(expr.name)), expr.name.data());
      continue;
    }
    gen.push_func(expr.name, &std::get<lambda_expr>(expr.value->content));
  }
  gen.run();
  return std::unique_ptr<anf_context, delete_anf>{
      new anf_context{std::move(gen).get_context()}};
}

void delete_anf::operator()(anf_context *ctx) { delete ctx; }

} // namespace lyn
