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

#include "nodes.h"
#include "ssa_builder.h"
#include "utils/growable_array.h"

namespace art {

void HGraph::AddBlock(HBasicBlock* block) {
  block->SetBlockId(blocks_.Size());
  blocks_.Add(block);
}

void HGraph::FindBackEdges(ArenaBitVector* visited) {
  ArenaBitVector visiting(arena_, blocks_.Size(), false);
  VisitBlockForBackEdges(entry_block_, visited, &visiting);
}

void HGraph::RemoveDeadBlocks(const ArenaBitVector& visited) const {
  for (size_t i = 0; i < blocks_.Size(); ++i) {
    if (!visited.IsBitSet(i)) {
      HBasicBlock* block = blocks_.Get(i);
      for (size_t j = 0; j < block->GetSuccessors().Size(); ++j) {
        block->GetSuccessors().Get(j)->RemovePredecessor(block);
      }
      for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
        block->RemovePhi(it.Current()->AsPhi());
      }
      for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
        block->RemoveInstruction(it.Current());
      }
    }
  }
}

void HGraph::VisitBlockForBackEdges(HBasicBlock* block,
                                    ArenaBitVector* visited,
                                    ArenaBitVector* visiting) {
  int id = block->GetBlockId();
  if (visited->IsBitSet(id)) return;

  visited->SetBit(id);
  visiting->SetBit(id);
  for (size_t i = 0; i < block->GetSuccessors().Size(); i++) {
    HBasicBlock* successor = block->GetSuccessors().Get(i);
    if (visiting->IsBitSet(successor->GetBlockId())) {
      successor->AddBackEdge(block);
    } else {
      VisitBlockForBackEdges(successor, visited, visiting);
    }
  }
  visiting->ClearBit(id);
}

void HGraph::BuildDominatorTree() {
  ArenaBitVector visited(arena_, blocks_.Size(), false);

  // (1) Find the back edges in the graph doing a DFS traversal.
  FindBackEdges(&visited);

  // (2) Remove blocks not visited during the initial DFS.
  //     Step (3) requires dead blocks to be removed from the
  //     predecessors list of live blocks.
  RemoveDeadBlocks(visited);

  // (3) Simplify the CFG now, so that we don't need to recompute
  //     dominators and the reverse post order.
  SimplifyCFG();

  // (4) Compute the immediate dominator of each block. We visit
  //     the successors of a block only when all its forward branches
  //     have been processed.
  GrowableArray<size_t> visits(arena_, blocks_.Size());
  visits.SetSize(blocks_.Size());
  reverse_post_order_.Add(entry_block_);
  for (size_t i = 0; i < entry_block_->GetSuccessors().Size(); i++) {
    VisitBlockForDominatorTree(entry_block_->GetSuccessors().Get(i), entry_block_, &visits);
  }
}

HBasicBlock* HGraph::FindCommonDominator(HBasicBlock* first, HBasicBlock* second) const {
  ArenaBitVector visited(arena_, blocks_.Size(), false);
  // Walk the dominator tree of the first block and mark the visited blocks.
  while (first != nullptr) {
    visited.SetBit(first->GetBlockId());
    first = first->GetDominator();
  }
  // Walk the dominator tree of the second block until a marked block is found.
  while (second != nullptr) {
    if (visited.IsBitSet(second->GetBlockId())) {
      return second;
    }
    second = second->GetDominator();
  }
  LOG(ERROR) << "Could not find common dominator";
  return nullptr;
}

void HGraph::VisitBlockForDominatorTree(HBasicBlock* block,
                                        HBasicBlock* predecessor,
                                        GrowableArray<size_t>* visits) {
  if (block->GetDominator() == nullptr) {
    block->SetDominator(predecessor);
  } else {
    block->SetDominator(FindCommonDominator(block->GetDominator(), predecessor));
  }

  visits->Increment(block->GetBlockId());
  // Once all the forward edges have been visited, we know the immediate
  // dominator of the block. We can then start visiting its successors.
  if (visits->Get(block->GetBlockId()) ==
      block->GetPredecessors().Size() - block->NumberOfBackEdges()) {
    block->GetDominator()->AddDominatedBlock(block);
    reverse_post_order_.Add(block);
    for (size_t i = 0; i < block->GetSuccessors().Size(); i++) {
      VisitBlockForDominatorTree(block->GetSuccessors().Get(i), block, visits);
    }
  }
}

void HGraph::TransformToSSA() {
  DCHECK(!reverse_post_order_.IsEmpty());
  SsaBuilder ssa_builder(this);
  ssa_builder.BuildSsa();
}

void HGraph::SplitCriticalEdge(HBasicBlock* block, HBasicBlock* successor) {
  // Insert a new node between `block` and `successor` to split the
  // critical edge.
  HBasicBlock* new_block = new (arena_) HBasicBlock(this, successor->GetDexPc());
  AddBlock(new_block);
  new_block->AddInstruction(new (arena_) HGoto());
  block->ReplaceSuccessor(successor, new_block);
  new_block->AddSuccessor(successor);
  if (successor->IsLoopHeader()) {
    // If we split at a back edge boundary, make the new block the back edge.
    HLoopInformation* info = successor->GetLoopInformation();
    if (info->IsBackEdge(block)) {
      info->RemoveBackEdge(block);
      info->AddBackEdge(new_block);
    }
  }
}

void HGraph::SimplifyLoop(HBasicBlock* header) {
  HLoopInformation* info = header->GetLoopInformation();

  // If there are more than one back edge, make them branch to the same block that
  // will become the only back edge. This simplifies finding natural loops in the
  // graph.
  // Also, if the loop is a do/while (that is the back edge is an if), change the
  // back edge to be a goto. This simplifies code generation of suspend cheks.
  if (info->NumberOfBackEdges() > 1 || info->GetBackEdges().Get(0)->GetLastInstruction()->IsIf()) {
    HBasicBlock* new_back_edge = new (arena_) HBasicBlock(this, header->GetDexPc());
    AddBlock(new_back_edge);
    new_back_edge->AddInstruction(new (arena_) HGoto());
    for (size_t pred = 0, e = info->GetBackEdges().Size(); pred < e; ++pred) {
      HBasicBlock* back_edge = info->GetBackEdges().Get(pred);
      back_edge->ReplaceSuccessor(header, new_back_edge);
    }
    info->ClearBackEdges();
    info->AddBackEdge(new_back_edge);
    new_back_edge->AddSuccessor(header);
  }

  // Make sure the loop has only one pre header. This simplifies SSA building by having
  // to just look at the pre header to know which locals are initialized at entry of the
  // loop.
  size_t number_of_incomings = header->GetPredecessors().Size() - info->NumberOfBackEdges();
  if (number_of_incomings != 1) {
    HBasicBlock* pre_header = new (arena_) HBasicBlock(this, header->GetDexPc());
    AddBlock(pre_header);
    pre_header->AddInstruction(new (arena_) HGoto());

    ArenaBitVector back_edges(arena_, GetBlocks().Size(), false);
    HBasicBlock* back_edge = info->GetBackEdges().Get(0);
    for (size_t pred = 0; pred < header->GetPredecessors().Size(); ++pred) {
      HBasicBlock* predecessor = header->GetPredecessors().Get(pred);
      if (predecessor != back_edge) {
        predecessor->ReplaceSuccessor(header, pre_header);
        pred--;
      }
    }
    pre_header->AddSuccessor(header);
  }

  // Make sure the second predecessor of a loop header is the back edge.
  if (header->GetPredecessors().Get(1) != info->GetBackEdges().Get(0)) {
    header->SwapPredecessors();
  }

  // Place the suspend check at the beginning of the header, so that live registers
  // will be known when allocating registers. Note that code generation can still
  // generate the suspend check at the back edge, but needs to be careful with
  // loop phi spill slots (which are not written to at back edge).
  HInstruction* first_instruction = header->GetFirstInstruction();
  if (!first_instruction->IsSuspendCheck()) {
    HSuspendCheck* check = new (arena_) HSuspendCheck(header->GetDexPc());
    header->InsertInstructionBefore(check, first_instruction);
    first_instruction = check;
  }
  info->SetSuspendCheck(first_instruction->AsSuspendCheck());
}

void HGraph::SimplifyCFG() {
  // Simplify the CFG for future analysis, and code generation:
  // (1): Split critical edges.
  // (2): Simplify loops by having only one back edge, and one preheader.
  for (size_t i = 0; i < blocks_.Size(); ++i) {
    HBasicBlock* block = blocks_.Get(i);
    if (block->GetSuccessors().Size() > 1) {
      for (size_t j = 0; j < block->GetSuccessors().Size(); ++j) {
        HBasicBlock* successor = block->GetSuccessors().Get(j);
        if (successor->GetPredecessors().Size() > 1) {
          SplitCriticalEdge(block, successor);
          --j;
        }
      }
    }
    if (block->IsLoopHeader()) {
      SimplifyLoop(block);
    }
  }
}

bool HGraph::FindNaturalLoops() const {
  for (size_t i = 0; i < blocks_.Size(); ++i) {
    HBasicBlock* block = blocks_.Get(i);
    if (block->IsLoopHeader()) {
      HLoopInformation* info = block->GetLoopInformation();
      if (!info->Populate()) {
        // Abort if the loop is non natural. We currently bailout in such cases.
        return false;
      }
    }
  }
  return true;
}

void HLoopInformation::PopulateRecursive(HBasicBlock* block) {
  if (blocks_.IsBitSet(block->GetBlockId())) {
    return;
  }

  blocks_.SetBit(block->GetBlockId());
  block->SetInLoop(this);
  for (size_t i = 0, e = block->GetPredecessors().Size(); i < e; ++i) {
    PopulateRecursive(block->GetPredecessors().Get(i));
  }
}

bool HLoopInformation::Populate() {
  DCHECK_EQ(GetBackEdges().Size(), 1u);
  HBasicBlock* back_edge = GetBackEdges().Get(0);
  DCHECK(back_edge->GetDominator() != nullptr);
  if (!header_->Dominates(back_edge)) {
    // This loop is not natural. Do not bother going further.
    return false;
  }

  // Populate this loop: starting with the back edge, recursively add predecessors
  // that are not already part of that loop. Set the header as part of the loop
  // to end the recursion.
  // This is a recursive implementation of the algorithm described in
  // "Advanced Compiler Design & Implementation" (Muchnick) p192.
  blocks_.SetBit(header_->GetBlockId());
  PopulateRecursive(back_edge);
  return true;
}

HBasicBlock* HLoopInformation::GetPreHeader() const {
  DCHECK_EQ(header_->GetPredecessors().Size(), 2u);
  return header_->GetDominator();
}

bool HLoopInformation::Contains(const HBasicBlock& block) const {
  return blocks_.IsBitSet(block.GetBlockId());
}

bool HLoopInformation::IsIn(const HLoopInformation& other) const {
  return other.blocks_.IsBitSet(header_->GetBlockId());
}

bool HBasicBlock::Dominates(HBasicBlock* other) const {
  // Walk up the dominator tree from `other`, to find out if `this`
  // is an ancestor.
  HBasicBlock* current = other;
  while (current != nullptr) {
    if (current == this) {
      return true;
    }
    current = current->GetDominator();
  }
  return false;
}

static void UpdateInputsUsers(HInstruction* instruction) {
  for (size_t i = 0, e = instruction->InputCount(); i < e; ++i) {
    instruction->InputAt(i)->AddUseAt(instruction, i);
  }
  // Environment should be created later.
  DCHECK(!instruction->HasEnvironment());
}

void HBasicBlock::InsertInstructionBefore(HInstruction* instruction, HInstruction* cursor) {
  DCHECK(!cursor->IsPhi());
  DCHECK(!instruction->IsPhi());
  DCHECK_EQ(instruction->GetId(), -1);
  DCHECK_NE(cursor->GetId(), -1);
  DCHECK_EQ(cursor->GetBlock(), this);
  DCHECK(!instruction->IsControlFlow());
  instruction->next_ = cursor;
  instruction->previous_ = cursor->previous_;
  cursor->previous_ = instruction;
  if (GetFirstInstruction() == cursor) {
    instructions_.first_instruction_ = instruction;
  } else {
    instruction->previous_->next_ = instruction;
  }
  instruction->SetBlock(this);
  instruction->SetId(GetGraph()->GetNextInstructionId());
  UpdateInputsUsers(instruction);
}

void HBasicBlock::ReplaceAndRemoveInstructionWith(HInstruction* initial,
                                                  HInstruction* replacement) {
  DCHECK(initial->GetBlock() == this);
  InsertInstructionBefore(replacement, initial);
  initial->ReplaceWith(replacement);
  RemoveInstruction(initial);
}

static void Add(HInstructionList* instruction_list,
                HBasicBlock* block,
                HInstruction* instruction) {
  DCHECK(instruction->GetBlock() == nullptr);
  DCHECK_EQ(instruction->GetId(), -1);
  instruction->SetBlock(block);
  instruction->SetId(block->GetGraph()->GetNextInstructionId());
  UpdateInputsUsers(instruction);
  instruction_list->AddInstruction(instruction);
}

void HBasicBlock::AddInstruction(HInstruction* instruction) {
  Add(&instructions_, this, instruction);
}

void HBasicBlock::AddPhi(HPhi* phi) {
  Add(&phis_, this, phi);
}

void HBasicBlock::InsertPhiAfter(HPhi* phi, HPhi* cursor) {
  DCHECK_EQ(phi->GetId(), -1);
  DCHECK_NE(cursor->GetId(), -1);
  DCHECK_EQ(cursor->GetBlock(), this);
  if (cursor->next_ == nullptr) {
    cursor->next_ = phi;
    phi->previous_ = cursor;
    DCHECK(phi->next_ == nullptr);
  } else {
    phi->next_ = cursor->next_;
    phi->previous_ = cursor;
    cursor->next_ = phi;
    phi->next_->previous_ = phi;
  }
  phi->SetBlock(this);
  phi->SetId(GetGraph()->GetNextInstructionId());
  UpdateInputsUsers(phi);
}

static void Remove(HInstructionList* instruction_list,
                   HBasicBlock* block,
                   HInstruction* instruction) {
  DCHECK_EQ(block, instruction->GetBlock());
  DCHECK(instruction->GetUses() == nullptr);
  DCHECK(instruction->GetEnvUses() == nullptr);
  instruction->SetBlock(nullptr);
  instruction_list->RemoveInstruction(instruction);

  for (size_t i = 0; i < instruction->InputCount(); i++) {
    instruction->InputAt(i)->RemoveUser(instruction, i);
  }

  HEnvironment* environment = instruction->GetEnvironment();
  if (environment != nullptr) {
    for (size_t i = 0, e = environment->Size(); i < e; ++i) {
      HInstruction* vreg = environment->GetInstructionAt(i);
      if (vreg != nullptr) {
        vreg->RemoveEnvironmentUser(environment, i);
      }
    }
  }
}

void HBasicBlock::RemoveInstruction(HInstruction* instruction) {
  Remove(&instructions_, this, instruction);
}

void HBasicBlock::RemovePhi(HPhi* phi) {
  Remove(&phis_, this, phi);
}

template <typename T>
static void RemoveFromUseList(T* user,
                              size_t input_index,
                              HUseListNode<T>** list) {
  HUseListNode<T>* previous = nullptr;
  HUseListNode<T>* current = *list;
  while (current != nullptr) {
    if (current->GetUser() == user && current->GetIndex() == input_index) {
      if (previous == NULL) {
        *list = current->GetTail();
      } else {
        previous->SetTail(current->GetTail());
      }
    }
    previous = current;
    current = current->GetTail();
  }
}

void HInstruction::RemoveUser(HInstruction* user, size_t input_index) {
  RemoveFromUseList(user, input_index, &uses_);
}

void HInstruction::RemoveEnvironmentUser(HEnvironment* user, size_t input_index) {
  RemoveFromUseList(user, input_index, &env_uses_);
}

void HInstructionList::AddInstruction(HInstruction* instruction) {
  if (first_instruction_ == nullptr) {
    DCHECK(last_instruction_ == nullptr);
    first_instruction_ = last_instruction_ = instruction;
  } else {
    last_instruction_->next_ = instruction;
    instruction->previous_ = last_instruction_;
    last_instruction_ = instruction;
  }
}

void HInstructionList::RemoveInstruction(HInstruction* instruction) {
  if (instruction->previous_ != nullptr) {
    instruction->previous_->next_ = instruction->next_;
  }
  if (instruction->next_ != nullptr) {
    instruction->next_->previous_ = instruction->previous_;
  }
  if (instruction == first_instruction_) {
    first_instruction_ = instruction->next_;
  }
  if (instruction == last_instruction_) {
    last_instruction_ = instruction->previous_;
  }
}

bool HInstructionList::Contains(HInstruction* instruction) const {
  for (HInstructionIterator it(*this); !it.Done(); it.Advance()) {
    if (it.Current() == instruction) {
      return true;
    }
  }
  return false;
}

bool HInstructionList::FoundBefore(const HInstruction* instruction1,
                                   const HInstruction* instruction2) const {
  DCHECK_EQ(instruction1->GetBlock(), instruction2->GetBlock());
  for (HInstructionIterator it(*this); !it.Done(); it.Advance()) {
    if (it.Current() == instruction1) {
      return true;
    }
    if (it.Current() == instruction2) {
      return false;
    }
  }
  LOG(FATAL) << "Did not find an order between two instructions of the same block.";
  return true;
}

bool HInstruction::StrictlyDominates(HInstruction* other_instruction) const {
  if (other_instruction == this) {
    // An instruction does not strictly dominate itself.
    return false;
  }
  HBasicBlock* block = GetBlock();
  HBasicBlock* other_block = other_instruction->GetBlock();
  if (block != other_block) {
    return GetBlock()->Dominates(other_instruction->GetBlock());
  } else {
    // If both instructions are in the same block, ensure this
    // instruction comes before `other_instruction`.
    if (IsPhi()) {
      if (!other_instruction->IsPhi()) {
        // Phis appear before non phi-instructions so this instruction
        // dominates `other_instruction`.
        return true;
      } else {
        // There is no order among phis.
        LOG(FATAL) << "There is no dominance between phis of a same block.";
        return false;
      }
    } else {
      // `this` is not a phi.
      if (other_instruction->IsPhi()) {
        // Phis appear before non phi-instructions so this instruction
        // does not dominate `other_instruction`.
        return false;
      } else {
        // Check whether this instruction comes before
        // `other_instruction` in the instruction list.
        return block->GetInstructions().FoundBefore(this, other_instruction);
      }
    }
  }
}

void HInstruction::ReplaceWith(HInstruction* other) {
  DCHECK(other != nullptr);
  for (HUseIterator<HInstruction> it(GetUses()); !it.Done(); it.Advance()) {
    HUseListNode<HInstruction>* current = it.Current();
    HInstruction* user = current->GetUser();
    size_t input_index = current->GetIndex();
    user->SetRawInputAt(input_index, other);
    other->AddUseAt(user, input_index);
  }

  for (HUseIterator<HEnvironment> it(GetEnvUses()); !it.Done(); it.Advance()) {
    HUseListNode<HEnvironment>* current = it.Current();
    HEnvironment* user = current->GetUser();
    size_t input_index = current->GetIndex();
    user->SetRawEnvAt(input_index, other);
    other->AddEnvUseAt(user, input_index);
  }

  uses_ = nullptr;
  env_uses_ = nullptr;
}

void HInstruction::ReplaceInput(HInstruction* replacement, size_t index) {
  InputAt(index)->RemoveUser(this, index);
  SetRawInputAt(index, replacement);
  replacement->AddUseAt(this, index);
}

size_t HInstruction::EnvironmentSize() const {
  return HasEnvironment() ? environment_->Size() : 0;
}

void HPhi::AddInput(HInstruction* input) {
  DCHECK(input->GetBlock() != nullptr);
  inputs_.Add(input);
  input->AddUseAt(this, inputs_.Size() - 1);
}

#define DEFINE_ACCEPT(name, super)                                             \
void H##name::Accept(HGraphVisitor* visitor) {                                 \
  visitor->Visit##name(this);                                                  \
}

FOR_EACH_INSTRUCTION(DEFINE_ACCEPT)

#undef DEFINE_ACCEPT

void HGraphVisitor::VisitInsertionOrder() {
  const GrowableArray<HBasicBlock*>& blocks = graph_->GetBlocks();
  for (size_t i = 0 ; i < blocks.Size(); i++) {
    VisitBasicBlock(blocks.Get(i));
  }
}

void HGraphVisitor::VisitReversePostOrder() {
  for (HReversePostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
    VisitBasicBlock(it.Current());
  }
}

void HGraphVisitor::VisitBasicBlock(HBasicBlock* block) {
  for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
    it.Current()->Accept(this);
  }
  for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
    it.Current()->Accept(this);
  }
}

HConstant* HUnaryOperation::TryStaticEvaluation() const {
  if (GetInput()->IsIntConstant()) {
    int32_t value = Evaluate(GetInput()->AsIntConstant()->GetValue());
    return new(GetBlock()->GetGraph()->GetArena()) HIntConstant(value);
  } else if (GetInput()->IsLongConstant()) {
    // TODO: Implement static evaluation of long unary operations.
    //
    // Do not exit with a fatal condition here.  Instead, simply
    // return `nullptr' to notify the caller that this instruction
    // cannot (yet) be statically evaluated.
    return nullptr;
  }
  return nullptr;
}

HConstant* HBinaryOperation::TryStaticEvaluation() const {
  if (GetLeft()->IsIntConstant() && GetRight()->IsIntConstant()) {
    int32_t value = Evaluate(GetLeft()->AsIntConstant()->GetValue(),
                             GetRight()->AsIntConstant()->GetValue());
    return new(GetBlock()->GetGraph()->GetArena()) HIntConstant(value);
  } else if (GetLeft()->IsLongConstant() && GetRight()->IsLongConstant()) {
    int64_t value = Evaluate(GetLeft()->AsLongConstant()->GetValue(),
                             GetRight()->AsLongConstant()->GetValue());
    return new(GetBlock()->GetGraph()->GetArena()) HLongConstant(value);
  }
  return nullptr;
}

bool HCondition::IsBeforeWhenDisregardMoves(HIf* if_) const {
  HInstruction* previous = if_->GetPrevious();
  while (previous != nullptr && previous->IsParallelMove()) {
    previous = previous->GetPrevious();
  }
  return previous == this;
}

bool HInstruction::Equals(HInstruction* other) const {
  if (!InstructionTypeEquals(other)) return false;
  DCHECK_EQ(GetKind(), other->GetKind());
  if (!InstructionDataEquals(other)) return false;
  if (GetType() != other->GetType()) return false;
  if (InputCount() != other->InputCount()) return false;

  for (size_t i = 0, e = InputCount(); i < e; ++i) {
    if (InputAt(i) != other->InputAt(i)) return false;
  }
  DCHECK_EQ(ComputeHashCode(), other->ComputeHashCode());
  return true;
}

}  // namespace art
