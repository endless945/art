/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ssa_liveness_analysis.h"

#include "base/bit_vector-inl.h"
#include "code_generator.h"
#include "nodes.h"

namespace art {

void SsaLivenessAnalysis::Analyze() {
  LinearizeGraph();
  NumberInstructions();
  ComputeLiveness();
}

static bool IsLoopExit(HLoopInformation* current, HLoopInformation* to) {
  // `to` is either not part of a loop, or `current` is an inner loop of `to`.
  return to == nullptr || (current != to && current->IsIn(*to));
}

static bool IsLoop(HLoopInformation* info) {
  return info != nullptr;
}

static bool InSameLoop(HLoopInformation* first_loop, HLoopInformation* second_loop) {
  return first_loop == second_loop;
}

static bool IsInnerLoop(HLoopInformation* outer, HLoopInformation* inner) {
  return (inner != outer)
      && (inner != nullptr)
      && (outer != nullptr)
      && inner->IsIn(*outer);
}

static void VisitBlockForLinearization(HBasicBlock* block,
                                       GrowableArray<HBasicBlock*>* order,
                                       ArenaBitVector* visited) {
  if (visited->IsBitSet(block->GetBlockId())) {
    return;
  }
  visited->SetBit(block->GetBlockId());
  size_t number_of_successors = block->GetSuccessors().Size();
  if (number_of_successors == 0) {
    // Nothing to do.
  } else if (number_of_successors == 1) {
    VisitBlockForLinearization(block->GetSuccessors().Get(0), order, visited);
  } else {
    DCHECK_EQ(number_of_successors, 2u);
    HBasicBlock* first_successor = block->GetSuccessors().Get(0);
    HBasicBlock* second_successor = block->GetSuccessors().Get(1);
    HLoopInformation* my_loop = block->GetLoopInformation();
    HLoopInformation* first_loop = first_successor->GetLoopInformation();
    HLoopInformation* second_loop = second_successor->GetLoopInformation();

    if (!IsLoop(my_loop)) {
      // Nothing to do. Current order is fine.
    } else if (IsLoopExit(my_loop, second_loop) && InSameLoop(my_loop, first_loop)) {
      // Visit the loop exit first in post order.
      std::swap(first_successor, second_successor);
    } else if (IsInnerLoop(my_loop, first_loop) && !IsInnerLoop(my_loop, second_loop)) {
      // Visit the inner loop last in post order.
      std::swap(first_successor, second_successor);
    }
    VisitBlockForLinearization(first_successor, order, visited);
    VisitBlockForLinearization(second_successor, order, visited);
  }
  order->Add(block);
}

void SsaLivenessAnalysis::LinearizeGraph() {
  // For simplicity of the implementation, we create post linear order. The order for
  // computing live ranges is the reverse of that order.
  ArenaBitVector visited(graph_.GetArena(), graph_.GetBlocks().Size(), false);
  VisitBlockForLinearization(graph_.GetEntryBlock(), &linear_post_order_, &visited);
}

void SsaLivenessAnalysis::NumberInstructions() {
  int ssa_index = 0;
  size_t lifetime_position = 0;
  // Each instruction gets a lifetime position, and a block gets a lifetime
  // start and end position. Non-phi instructions have a distinct lifetime position than
  // the block they are in. Phi instructions have the lifetime start of their block as
  // lifetime position.
  //
  // Because the register allocator will insert moves in the graph, we need
  // to differentiate between the start and end of an instruction. Adding 2 to
  // the lifetime position for each instruction ensures the start of an
  // instruction is different than the end of the previous instruction.
  HGraphVisitor* location_builder = codegen_->GetLocationBuilder();
  for (HLinearOrderIterator it(*this); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    block->SetLifetimeStart(lifetime_position);

    for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
      HInstruction* current = it.Current();
      current->Accept(location_builder);
      LocationSummary* locations = current->GetLocations();
      if (locations != nullptr && locations->Out().IsValid()) {
        instructions_from_ssa_index_.Add(current);
        current->SetSsaIndex(ssa_index++);
        current->SetLiveInterval(
            new (graph_.GetArena()) LiveInterval(graph_.GetArena(), current->GetType(), current));
      }
      current->SetLifetimePosition(lifetime_position);
    }
    lifetime_position += 2;

    // Add a null marker to notify we are starting a block.
    instructions_from_lifetime_position_.Add(nullptr);

    for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
      HInstruction* current = it.Current();
      current->Accept(codegen_->GetLocationBuilder());
      LocationSummary* locations = current->GetLocations();
      if (locations != nullptr && locations->Out().IsValid()) {
        instructions_from_ssa_index_.Add(current);
        current->SetSsaIndex(ssa_index++);
        current->SetLiveInterval(
            new (graph_.GetArena()) LiveInterval(graph_.GetArena(), current->GetType(), current));
      }
      instructions_from_lifetime_position_.Add(current);
      current->SetLifetimePosition(lifetime_position);
      lifetime_position += 2;
    }

    block->SetLifetimeEnd(lifetime_position);
  }
  number_of_ssa_values_ = ssa_index;
}

void SsaLivenessAnalysis::ComputeLiveness() {
  for (HLinearOrderIterator it(*this); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    block_infos_.Put(
        block->GetBlockId(),
        new (graph_.GetArena()) BlockInfo(graph_.GetArena(), *block, number_of_ssa_values_));
  }

  // Compute the live ranges, as well as the initial live_in, live_out, and kill sets.
  // This method does not handle backward branches for the sets, therefore live_in
  // and live_out sets are not yet correct.
  ComputeLiveRanges();

  // Do a fixed point calculation to take into account backward branches,
  // that will update live_in of loop headers, and therefore live_out and live_in
  // of blocks in the loop.
  ComputeLiveInAndLiveOutSets();
}

void SsaLivenessAnalysis::ComputeLiveRanges() {
  // Do a post order visit, adding inputs of instructions live in the block where
  // that instruction is defined, and killing instructions that are being visited.
  for (HLinearPostOrderIterator it(*this); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();

    BitVector* kill = GetKillSet(*block);
    BitVector* live_in = GetLiveInSet(*block);

    // Set phi inputs of successors of this block corresponding to this block
    // as live_in.
    for (size_t i = 0, e = block->GetSuccessors().Size(); i < e; ++i) {
      HBasicBlock* successor = block->GetSuccessors().Get(i);
      live_in->Union(GetLiveInSet(*successor));
      size_t phi_input_index = successor->GetPredecessorIndexOf(block);
      for (HInstructionIterator it(successor->GetPhis()); !it.Done(); it.Advance()) {
        HInstruction* phi = it.Current();
        HInstruction* input = phi->InputAt(phi_input_index);
        input->GetLiveInterval()->AddPhiUse(phi, phi_input_index, block);
        // A phi input whose last user is the phi dies at the end of the predecessor block,
        // and not at the phi's lifetime position.
        live_in->SetBit(input->GetSsaIndex());
      }
    }

    // Add a range that covers this block to all instructions live_in because of successors.
    // Instructions defined in this block will have their start of the range adjusted.
    for (uint32_t idx : live_in->Indexes()) {
      HInstruction* current = instructions_from_ssa_index_.Get(idx);
      current->GetLiveInterval()->AddRange(block->GetLifetimeStart(), block->GetLifetimeEnd());
    }

    for (HBackwardInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
      HInstruction* current = it.Current();
      if (current->HasSsaIndex()) {
        // Kill the instruction and shorten its interval.
        kill->SetBit(current->GetSsaIndex());
        live_in->ClearBit(current->GetSsaIndex());
        current->GetLiveInterval()->SetFrom(current->GetLifetimePosition());
      }

      // All inputs of an instruction must be live.
      for (size_t i = 0, e = current->InputCount(); i < e; ++i) {
        HInstruction* input = current->InputAt(i);
        // Some instructions 'inline' their inputs, that is they do not need
        // to be materialized.
        if (input->HasSsaIndex()) {
          live_in->SetBit(input->GetSsaIndex());
          input->GetLiveInterval()->AddUse(current, i, false);
        }
      }

      if (current->HasEnvironment()) {
        // All instructions in the environment must be live.
        GrowableArray<HInstruction*>* environment = current->GetEnvironment()->GetVRegs();
        for (size_t i = 0, e = environment->Size(); i < e; ++i) {
          HInstruction* instruction = environment->Get(i);
          if (instruction != nullptr) {
            DCHECK(instruction->HasSsaIndex());
            live_in->SetBit(instruction->GetSsaIndex());
            instruction->GetLiveInterval()->AddUse(current, i, true);
          }
        }
      }
    }

    // Kill phis defined in this block.
    for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
      HInstruction* current = it.Current();
      if (current->HasSsaIndex()) {
        kill->SetBit(current->GetSsaIndex());
        live_in->ClearBit(current->GetSsaIndex());
        LiveInterval* interval = current->GetLiveInterval();
        DCHECK((interval->GetFirstRange() == nullptr)
               || (interval->GetStart() == current->GetLifetimePosition()));
        interval->SetFrom(current->GetLifetimePosition());
      }
    }

    if (block->IsLoopHeader()) {
      HBasicBlock* back_edge = block->GetLoopInformation()->GetBackEdges().Get(0);
      // For all live_in instructions at the loop header, we need to create a range
      // that covers the full loop.
      for (uint32_t idx : live_in->Indexes()) {
        HInstruction* current = instructions_from_ssa_index_.Get(idx);
        current->GetLiveInterval()->AddLoopRange(block->GetLifetimeStart(),
                                                 back_edge->GetLifetimeEnd());
      }
    }
  }
}

void SsaLivenessAnalysis::ComputeLiveInAndLiveOutSets() {
  bool changed;
  do {
    changed = false;

    for (HPostOrderIterator it(graph_); !it.Done(); it.Advance()) {
      const HBasicBlock& block = *it.Current();

      // The live_in set depends on the kill set (which does not
      // change in this loop), and the live_out set.  If the live_out
      // set does not change, there is no need to update the live_in set.
      if (UpdateLiveOut(block) && UpdateLiveIn(block)) {
        changed = true;
      }
    }
  } while (changed);
}

bool SsaLivenessAnalysis::UpdateLiveOut(const HBasicBlock& block) {
  BitVector* live_out = GetLiveOutSet(block);
  bool changed = false;
  // The live_out set of a block is the union of live_in sets of its successors.
  for (size_t i = 0, e = block.GetSuccessors().Size(); i < e; ++i) {
    HBasicBlock* successor = block.GetSuccessors().Get(i);
    if (live_out->Union(GetLiveInSet(*successor))) {
      changed = true;
    }
  }
  return changed;
}


bool SsaLivenessAnalysis::UpdateLiveIn(const HBasicBlock& block) {
  BitVector* live_out = GetLiveOutSet(block);
  BitVector* kill = GetKillSet(block);
  BitVector* live_in = GetLiveInSet(block);
  // If live_out is updated (because of backward branches), we need to make
  // sure instructions in live_out are also in live_in, unless they are killed
  // by this block.
  return live_in->UnionIfNotIn(live_out, kill);
}

int LiveInterval::FindFirstRegisterHint(size_t* free_until) const {
  if (GetParent() == this && defined_by_ != nullptr) {
    // This is the first interval for the instruction. Try to find
    // a register based on its definition.
    DCHECK_EQ(defined_by_->GetLiveInterval(), this);
    int hint = FindHintAtDefinition();
    if (hint != kNoRegister && free_until[hint] > GetStart()) {
      return hint;
    }
  }

  UsePosition* use = first_use_;
  size_t start = GetStart();
  size_t end = GetEnd();
  while (use != nullptr && use->GetPosition() <= end) {
    size_t use_position = use->GetPosition();
    if (use_position >= start && !use->GetIsEnvironment()) {
      HInstruction* user = use->GetUser();
      size_t input_index = use->GetInputIndex();
      if (user->IsPhi()) {
        // If the phi has a register, try to use the same.
        Location phi_location = user->GetLiveInterval()->ToLocation();
        if (SameRegisterKind(phi_location) && free_until[phi_location.reg()] >= use_position) {
          return phi_location.reg();
        }
        const GrowableArray<HBasicBlock*>& predecessors = user->GetBlock()->GetPredecessors();
        // If the instruction dies at the phi assignment, we can try having the
        // same register.
        if (end == predecessors.Get(input_index)->GetLifetimeEnd()) {
          for (size_t i = 0, e = user->InputCount(); i < e; ++i) {
            if (i == input_index) {
              continue;
            }
            HInstruction* input = user->InputAt(i);
            Location location = input->GetLiveInterval()->GetLocationAt(
                predecessors.Get(i)->GetLifetimeEnd() - 1);
            if (location.IsRegister() && free_until[location.reg()] >= use_position) {
              return location.reg();
            }
          }
        }
      } else {
        // If the instruction is expected in a register, try to use it.
        LocationSummary* locations = user->GetLocations();
        Location expected = locations->InAt(use->GetInputIndex());
        // We use the user's lifetime position - 1 (and not `use_position`) because the
        // register is blocked at the beginning of the user.
        size_t position = user->GetLifetimePosition() - 1;
        if (SameRegisterKind(expected) && free_until[expected.reg()] >= position) {
          return expected.reg();
        }
      }
    }
    use = use->GetNext();
  }

  return kNoRegister;
}

int LiveInterval::FindHintAtDefinition() const {
  if (defined_by_->IsPhi()) {
    // Try to use the same register as one of the inputs.
    const GrowableArray<HBasicBlock*>& predecessors = defined_by_->GetBlock()->GetPredecessors();
    for (size_t i = 0, e = defined_by_->InputCount(); i < e; ++i) {
      HInstruction* input = defined_by_->InputAt(i);
      size_t end = predecessors.Get(i)->GetLifetimeEnd();
      const LiveInterval& input_interval = input->GetLiveInterval()->GetIntervalAt(end - 1);
      if (input_interval.GetEnd() == end) {
        // If the input dies at the end of the predecessor, we know its register can
        // be reused.
        Location input_location = input_interval.ToLocation();
        if (SameRegisterKind(input_location)) {
          return input_location.reg();
        }
      }
    }
  } else {
    LocationSummary* locations = GetDefinedBy()->GetLocations();
    Location out = locations->Out();
    if (out.IsUnallocated() && out.GetPolicy() == Location::kSameAsFirstInput) {
      // Try to use the same register as the first input.
      const LiveInterval& input_interval =
          GetDefinedBy()->InputAt(0)->GetLiveInterval()->GetIntervalAt(GetStart() - 1);
      if (input_interval.GetEnd() == GetStart()) {
        // If the input dies at the start of this instruction, we know its register can
        // be reused.
        Location location = input_interval.ToLocation();
        if (SameRegisterKind(location)) {
          return location.reg();
        }
      }
    }
  }
  return kNoRegister;
}

bool LiveInterval::SameRegisterKind(Location other) const {
  return IsFloatingPoint()
      ? other.IsFpuRegister()
      : other.IsRegister();
}

bool LiveInterval::NeedsTwoSpillSlots() const {
  return type_ == Primitive::kPrimLong || type_ == Primitive::kPrimDouble;
}

Location LiveInterval::ToLocation() const {
  if (HasRegister()) {
    return IsFloatingPoint()
        ? Location::FpuRegisterLocation(GetRegister())
        : Location::RegisterLocation(GetRegister());
  } else {
    HInstruction* defined_by = GetParent()->GetDefinedBy();
    if (defined_by->IsConstant()) {
      return defined_by->GetLocations()->Out();
    } else if (GetParent()->HasSpillSlot()) {
      if (NeedsTwoSpillSlots()) {
        return Location::DoubleStackSlot(GetParent()->GetSpillSlot());
      } else {
        return Location::StackSlot(GetParent()->GetSpillSlot());
      }
    } else {
      return Location();
    }
  }
}

Location LiveInterval::GetLocationAt(size_t position) const {
  return GetIntervalAt(position).ToLocation();
}

const LiveInterval& LiveInterval::GetIntervalAt(size_t position) const {
  const LiveInterval* current = this;
  while (!current->Covers(position)) {
    current = current->GetNextSibling();
    DCHECK(current != nullptr);
  }
  return *current;
}

}  // namespace art
