#include "anf.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace lyn {

void genasm(anf_context &ctx, FILE *out) {
  fputs("\t.arch armv5t\n"
        "\t.thumb\n"
        "\t.syntax unified\n"
        "\t.section \".text\", \"ax\"\n",
        out);
  int label_offset = 1;
  for (auto &&def : ctx.defs) {
    if (def.global)
      fprintf(out, "\t.global \"%.*s\"\n",
              static_cast<int>(std::size(def.name)), def.name.data());
    fprintf(out,
            "\t.type \"%.*s\", %%function\n"
            "\t.thumb_func\n"
            "\"%.*s\":\n",
            static_cast<int>(std::size(def.name)), def.name.data(),
            static_cast<int>(std::size(def.name)), def.name.data());

    std::unordered_map<int, int> local_to_stack_slot;
    std::unordered_map<std::size_t, int> used_stack_slots;
    used_stack_slots[0] = 0;
    for (std::size_t block_idx = 0; block_idx != std::size(def.blocks);
         ++block_idx) {
      auto &&block = def.blocks[block_idx];
      int parent_local_count = used_stack_slots.at(block_idx);
      int local_count = parent_local_count;
      int stack_offset = local_count;
      const auto sp_offset_for_local = [&](int id) {
        return (local_count - local_to_stack_slot.at(id) - 1) * 4;
      };

      fprintf(out, ".L%d:\n", static_cast<int>(label_offset + block_idx));
      for (auto &&expr : block.content)
        std::visit(
            [&](auto &&val) {
              using val_t = std::decay_t<decltype(val)>;
              if constexpr (std::is_same_v<val_t, anf_receive>) {
                local_count += std::size(val.args);
                parent_local_count += std::size(val.args);
                stack_offset += std::size(val.args);
              }
              if constexpr (std::is_same_v<val_t, anf_global>) {
                local_count += 1;
              }
              if constexpr (std::is_same_v<val_t, anf_constant>) {
                local_count += 1;
              }
              if constexpr (std::is_same_v<val_t, anf_call>) {
                local_count += 1;
              }
            },
            expr);
      for (auto &&expr : block.content) {
        std::visit(
            [&](auto &&val) {
              using val_t = std::decay_t<decltype(val)>;
              local_count = local_count;
              if constexpr (std::is_same_v<val_t, anf_receive>) {
                if (std::size(val.args) > 4u) {
                  throw std::runtime_error{
                      "Function with more than four arguments are currently "
                      "not supported"};
                }
                for (std::size_t i = 0; i < std::size(val.args); ++i) {
                  local_to_stack_slot[val.args[i]] =
                      std::size(val.args) - i - 1;
                }
                fprintf(out, "\tpush {");
                for (int i = 0;
                     static_cast<std::size_t>(i) < std::size(val.args); ++i) {
                  fprintf(out, "r%d, ", i);
                }
                parent_local_count = std::size(val.args);
                fputs("r6, lr}\n", out);
              }
              if constexpr (std::is_same_v<val_t, anf_adjust_stack>) {
                fprintf(out, "\tsub sp, sp, #%d\n",
                        (local_count - parent_local_count) * 4);
              }
              if constexpr (std::is_same_v<val_t, anf_global>) {
                const int stack_slot = stack_offset++;
                local_to_stack_slot[val.id] = stack_slot;
                fprintf(out,
                        "\tldr r0, =\"%.*s\"\n"
                        "\tstr r0, [sp, #%d]\n",
                        static_cast<int>(std::size(val.name)), val.name.data(),
                        sp_offset_for_local(val.id));
              }
              if constexpr (std::is_same_v<val_t, anf_constant>) {
                const int stack_slot = stack_offset++;
                local_to_stack_slot[val.id] = stack_slot;
                fprintf(out,
                        "\tldr r0, =#%d\n"
                        "\tstr r0, [sp, #%d]\n",
                        val.value, sp_offset_for_local(val.id));
              }
              if constexpr (std::is_same_v<val_t, anf_call>) {
                if (std::size(val.arg_ids) > 4)
                  throw std::runtime_error{"Sorry, more than 4 args are WIP"};
                // Restore lr when tail calling
                if (val.is_tail) {
                  fprintf(out,
                          "\tldr r0, [sp, #%d]\n"
                          "\tmov lr, r0\n",
                          (local_count + 1) * 4);
                }
                if (std::holds_alternative<int>(val.call_target)) {
                  fprintf(out, "\tldr r4, [sp, #%d]\n",
                          sp_offset_for_local(std::get<int>(val.call_target)));
                }
                for (std::size_t i = 0; i < std::size(val.arg_ids); ++i) {
                  fprintf(out, "\tldr r%d, [sp, #%d]\n", static_cast<int>(i),
                          sp_offset_for_local(val.arg_ids[i]));
                }
                if (val.is_tail) {
                  fprintf(out, "\tadd sp, #%d\n", (local_count + 2) * 4);
                  std::visit(
                      [&](auto &&target) {
                        using target_t = std::decay_t<decltype(target)>;
                        if constexpr (std::is_same_v<target_t,
                                                     std::string_view>) {
                          fprintf(out, "\tbx \"%.*s\"\n",
                                  static_cast<int>(std::size(target)),
                                  std::data(target));
                        } else {
                          fputs("\tbx r4\n", out);
                        }
                      },
                      val.call_target);
                } else {
                  const int stack_slot = stack_offset++;
                  local_to_stack_slot[val.res_id] = stack_slot;
                  std::visit(
                      [&](auto &&value) {
                        using value_t = std::decay_t<decltype(value)>;
                        if constexpr (std::is_same_v<value_t,
                                                     std::string_view>) {
                          fprintf(out, "\tblx \"%.*s\"\n",
                                  static_cast<int>(std::size(value)),
                                  std::data(value));
                        } else {
                          fputs("\tblx r4\n", out);
                        }
                      },
                      val.call_target);
                  fprintf(out, "\tstr r0, [sp, #%d]\n",
                          sp_offset_for_local(val.res_id));
                }
              }
              if constexpr (std::is_same_v<val_t, anf_assoc>) {
                local_to_stack_slot[val.id] = local_to_stack_slot[val.alias];
              }
              if constexpr (std::is_same_v<val_t, anf_cond>) {
                used_stack_slots[val.then_block] = local_count;
                used_stack_slots[val.else_block] = local_count;
                fprintf(out,
                        "\tldr r0, [sp, #%d]\n"
                        "\ttst r0, r0\n"
                        "\tbeq .L%d\n"
                        "\tb   .L%d\n",
                        sp_offset_for_local(val.cond_id),
                        val.else_block + label_offset,
                        val.then_block + label_offset);
              }
              if constexpr (std::is_same_v<val_t, anf_return>) {
                fprintf(out,
                        "\tldr r0, [sp, #%d]\n"
                        "\tadd sp, #%d\n"
                        "\tpop {r6, pc}\n",
                        sp_offset_for_local(val.value), local_count * 4);
              }
              if constexpr (std::is_same_v<val_t, anf_jump>) {
                fprintf(out, "\tb .L%d\n", val.target + label_offset);
              }
              if constexpr (std::is_same_v<val_t, anf_global_assign>) {
                fprintf(out,
                        "\tldr r0, \"%.*s\"\n"
                        "\tldr r1, [sp, #%d]\n"
                        "\tstr r1, r0\n",
                        static_cast<int>(std::size(val.name)), val.name.data(),
                        sp_offset_for_local(val.id));
              }
            },
            expr);
      }
    }
    fprintf(out,
            "\t.pool\n"
            "\t.size \"%.*s\", .-\"%.*s\"\n",
            static_cast<int>(std::size(def.name)), def.name.data(),
            static_cast<int>(std::size(def.name)), def.name.data());
    label_offset += std::size(def.blocks);
  }
}

} // namespace lyn
