// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/jump-threading.h"
#include "src/compiler/code-generator-impl.h"

namespace v8 {
namespace internal {
namespace compiler {

typedef BasicBlock::RpoNumber RpoNumber;

#define TRACE(x) \
  if (FLAG_trace_turbo_jt) PrintF x

struct JumpThreadingState {
  bool forwarded;
  ZoneVector<RpoNumber>& result;
  ZoneStack<RpoNumber>& stack;

  void Clear(size_t count) { result.assign(count, unvisited()); }
  void PushIfUnvisited(RpoNumber num) {
    if (result[num.ToInt()] == unvisited()) {
      stack.push(num);
      result[num.ToInt()] = onstack();
    }
  }
  void Forward(RpoNumber to) {
    RpoNumber from = stack.top();
    RpoNumber to_to = result[to.ToInt()];
    bool pop = true;
    if (to == from) {
      TRACE(("  xx %d\n", from.ToInt()));
      result[from.ToInt()] = from;
    } else if (to_to == unvisited()) {
      TRACE(("  fw %d -> %d (recurse)\n", from.ToInt(), to.ToInt()));
      stack.push(to);
      result[to.ToInt()] = onstack();
      pop = false;  // recurse.
    } else if (to_to == onstack()) {
      TRACE(("  fw %d -> %d (cycle)\n", from.ToInt(), to.ToInt()));
      result[from.ToInt()] = to;  // break the cycle.
      forwarded = true;
    } else {
      TRACE(("  fw %d -> %d (forward)\n", from.ToInt(), to.ToInt()));
      result[from.ToInt()] = to_to;  // forward the block.
      forwarded = true;
    }
    if (pop) stack.pop();
  }
  RpoNumber unvisited() { return RpoNumber::FromInt(-1); }
  RpoNumber onstack() { return RpoNumber::FromInt(-2); }
};


bool JumpThreading::ComputeForwarding(Zone* local_zone,
                                      ZoneVector<RpoNumber>& result,
                                      InstructionSequence* code) {
  ZoneStack<RpoNumber> stack(local_zone);
  JumpThreadingState state = {false, result, stack};
  state.Clear(code->InstructionBlockCount());

  // Iterate over the blocks forward, pushing the blocks onto the stack.
  for (auto const block : code->instruction_blocks()) {
    RpoNumber current = block->rpo_number();
    state.PushIfUnvisited(current);

    // Process the stack, which implements DFS through empty blocks.
    while (!state.stack.empty()) {
      InstructionBlock* block = code->InstructionBlockAt(state.stack.top());
      // Process the instructions in a block up to a non-empty instruction.
      TRACE(("jt [%d] B%d RPO%d\n", static_cast<int>(stack.size()),
             block->id().ToInt(), block->rpo_number().ToInt()));
      bool fallthru = true;
      RpoNumber fw = block->rpo_number();
      for (int i = block->code_start(); i < block->code_end(); ++i) {
        Instruction* instr = code->InstructionAt(i);
        if (instr->IsGapMoves() && GapInstruction::cast(instr)->IsRedundant()) {
          // skip redundant gap moves.
          TRACE(("  nop gap\n"));
          continue;
        } else if (instr->IsSourcePosition()) {
          // skip source positions.
          TRACE(("  src pos\n"));
          continue;
        } else if (FlagsModeField::decode(instr->opcode()) != kFlags_none) {
          // can't skip instructions with flags continuations.
          TRACE(("  flags\n"));
          fallthru = false;
        } else if (instr->IsNop()) {
          // skip nops.
          TRACE(("  nop\n"));
          continue;
        } else if (instr->arch_opcode() == kArchJmp) {
          // try to forward the jump instruction.
          TRACE(("  jmp\n"));
          fw = code->InputRpo(instr, 0);
          fallthru = false;
        } else {
          // can't skip other instructions.
          TRACE(("  other\n"));
          fallthru = false;
        }
        break;
      }
      if (fallthru) {
        int next = 1 + block->rpo_number().ToInt();
        if (next < code->InstructionBlockCount()) fw = RpoNumber::FromInt(next);
      }
      state.Forward(fw);
    }
  }

#ifdef DEBUG
  for (RpoNumber num : result) {
    CHECK(num.IsValid());
  }
#endif

  if (FLAG_trace_turbo_jt) {
    for (int i = 0; i < static_cast<int>(result.size()); i++) {
      TRACE(("RPO%d B%d ", i,
             code->InstructionBlockAt(RpoNumber::FromInt(i))->id().ToInt()));
      int to = result[i].ToInt();
      if (i != to) {
        TRACE(("-> B%d\n",
               code->InstructionBlockAt(RpoNumber::FromInt(to))->id().ToInt()));
      } else {
        TRACE(("\n"));
      }
    }
  }

  return state.forwarded;
}


void JumpThreading::ApplyForwarding(ZoneVector<RpoNumber>& result,
                                    InstructionSequence* code) {
  if (!FLAG_turbo_jt) return;

  Zone local_zone(code->zone()->isolate());
  ZoneVector<bool> skip(static_cast<int>(result.size()), false, &local_zone);

  // Skip empty blocks when the previous block doesn't fall through.
  bool prev_fallthru = true;
  for (auto const block : code->instruction_blocks()) {
    int block_num = block->rpo_number().ToInt();
    skip[block_num] = !prev_fallthru && result[block_num].ToInt() != block_num;

    bool fallthru = true;
    for (int i = block->code_start(); i < block->code_end(); ++i) {
      Instruction* instr = code->InstructionAt(i);
      if (FlagsModeField::decode(instr->opcode()) == kFlags_branch) {
        fallthru = false;  // branches don't fall through to the next block.
      } else if (instr->arch_opcode() == kArchJmp) {
        if (skip[block_num]) {
          // Overwrite a redundant jump with a nop.
          TRACE(("jt-fw nop @%d\n", i));
          instr->OverwriteWithNop();
        }
        fallthru = false;  // jumps don't fall through to the next block.
      }
    }
    prev_fallthru = fallthru;
  }

  // Patch RPO immediates.
  InstructionSequence::Immediates& immediates = code->immediates();
  for (size_t i = 0; i < immediates.size(); i++) {
    Constant constant = immediates[i];
    if (constant.type() == Constant::kRpoNumber) {
      RpoNumber rpo = constant.ToRpoNumber();
      RpoNumber fw = result[rpo.ToInt()];
      if (!(fw == rpo)) immediates[i] = Constant(fw);
    }
  }

  // Recompute assembly order numbers.
  int ao = 0;
  for (auto const block : code->instruction_blocks()) {
    if (!block->IsDeferred()) {
      block->set_ao_number(RpoNumber::FromInt(ao));
      if (!skip[block->rpo_number().ToInt()]) ao++;
    }
  }
  for (auto const block : code->instruction_blocks()) {
    if (block->IsDeferred()) {
      block->set_ao_number(RpoNumber::FromInt(ao));
      if (!skip[block->rpo_number().ToInt()]) ao++;
    }
  }
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8
