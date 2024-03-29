/*
 *
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

#include "builder.h"

#include "class_linker.h"
#include "dex_file.h"
#include "dex_file-inl.h"
#include "dex_instruction.h"
#include "dex_instruction-inl.h"
#include "driver/compiler_driver-inl.h"
#include "mirror/art_field.h"
#include "mirror/art_field-inl.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache.h"
#include "nodes.h"
#include "primitive.h"
#include "scoped_thread_state_change.h"
#include "thread.h"

namespace art {

/**
 * Helper class to add HTemporary instructions. This class is used when
 * converting a DEX instruction to multiple HInstruction, and where those
 * instructions do not die at the following instruction, but instead spans
 * multiple instructions.
 */
class Temporaries : public ValueObject {
 public:
  Temporaries(HGraph* graph, size_t count) : graph_(graph), count_(count), index_(0) {
    graph_->UpdateNumberOfTemporaries(count_);
  }

  void Add(HInstruction* instruction) {
    // We currently only support vreg size temps.
    DCHECK(instruction->GetType() != Primitive::kPrimLong
           && instruction->GetType() != Primitive::kPrimDouble);
    HInstruction* temp = new (graph_->GetArena()) HTemporary(index_++);
    instruction->GetBlock()->AddInstruction(temp);
    DCHECK(temp->GetPrevious() == instruction);
  }

 private:
  HGraph* const graph_;

  // The total number of temporaries that will be used.
  const size_t count_;

  // Current index in the temporary stack, updated by `Add`.
  size_t index_;
};

static bool IsTypeSupported(Primitive::Type type) {
  return type != Primitive::kPrimFloat && type != Primitive::kPrimDouble;
}

void HGraphBuilder::InitializeLocals(uint16_t count) {
  graph_->SetNumberOfVRegs(count);
  locals_.SetSize(count);
  for (int i = 0; i < count; i++) {
    HLocal* local = new (arena_) HLocal(i);
    entry_block_->AddInstruction(local);
    locals_.Put(i, local);
  }
}

bool HGraphBuilder::InitializeParameters(uint16_t number_of_parameters) {
  // dex_compilation_unit_ is null only when unit testing.
  if (dex_compilation_unit_ == nullptr) {
    return true;
  }

  graph_->SetNumberOfInVRegs(number_of_parameters);
  const char* shorty = dex_compilation_unit_->GetShorty();
  int locals_index = locals_.Size() - number_of_parameters;
  int parameter_index = 0;

  if (!dex_compilation_unit_->IsStatic()) {
    // Add the implicit 'this' argument, not expressed in the signature.
    HParameterValue* parameter =
        new (arena_) HParameterValue(parameter_index++, Primitive::kPrimNot);
    entry_block_->AddInstruction(parameter);
    HLocal* local = GetLocalAt(locals_index++);
    entry_block_->AddInstruction(new (arena_) HStoreLocal(local, parameter));
    number_of_parameters--;
  }

  uint32_t pos = 1;
  for (int i = 0; i < number_of_parameters; i++) {
    HParameterValue* parameter =
        new (arena_) HParameterValue(parameter_index++, Primitive::GetType(shorty[pos++]));
    entry_block_->AddInstruction(parameter);
    HLocal* local = GetLocalAt(locals_index++);
    // Store the parameter value in the local that the dex code will use
    // to reference that parameter.
    entry_block_->AddInstruction(new (arena_) HStoreLocal(local, parameter));
    bool is_wide = (parameter->GetType() == Primitive::kPrimLong)
        || (parameter->GetType() == Primitive::kPrimDouble);
    if (is_wide) {
      i++;
      locals_index++;
      parameter_index++;
    }
  }
  return true;
}

static bool CanHandleCodeItem(const DexFile::CodeItem& code_item) {
  if (code_item.tries_size_ > 0) {
    return false;
  }
  return true;
}

template<typename T>
void HGraphBuilder::If_22t(const Instruction& instruction, uint32_t dex_offset) {
  int32_t target_offset = instruction.GetTargetOffset();
  PotentiallyAddSuspendCheck(target_offset, dex_offset);
  HInstruction* first = LoadLocal(instruction.VRegA(), Primitive::kPrimInt);
  HInstruction* second = LoadLocal(instruction.VRegB(), Primitive::kPrimInt);
  T* comparison = new (arena_) T(first, second);
  current_block_->AddInstruction(comparison);
  HInstruction* ifinst = new (arena_) HIf(comparison);
  current_block_->AddInstruction(ifinst);
  HBasicBlock* target = FindBlockStartingAt(dex_offset + target_offset);
  DCHECK(target != nullptr);
  current_block_->AddSuccessor(target);
  target = FindBlockStartingAt(dex_offset + instruction.SizeInCodeUnits());
  DCHECK(target != nullptr);
  current_block_->AddSuccessor(target);
  current_block_ = nullptr;
}

template<typename T>
void HGraphBuilder::If_21t(const Instruction& instruction, uint32_t dex_offset) {
  int32_t target_offset = instruction.GetTargetOffset();
  PotentiallyAddSuspendCheck(target_offset, dex_offset);
  HInstruction* value = LoadLocal(instruction.VRegA(), Primitive::kPrimInt);
  T* comparison = new (arena_) T(value, GetIntConstant(0));
  current_block_->AddInstruction(comparison);
  HInstruction* ifinst = new (arena_) HIf(comparison);
  current_block_->AddInstruction(ifinst);
  HBasicBlock* target = FindBlockStartingAt(dex_offset + target_offset);
  DCHECK(target != nullptr);
  current_block_->AddSuccessor(target);
  target = FindBlockStartingAt(dex_offset + instruction.SizeInCodeUnits());
  DCHECK(target != nullptr);
  current_block_->AddSuccessor(target);
  current_block_ = nullptr;
}

HGraph* HGraphBuilder::BuildGraph(const DexFile::CodeItem& code_item) {
  if (!CanHandleCodeItem(code_item)) {
    return nullptr;
  }

  const uint16_t* code_ptr = code_item.insns_;
  const uint16_t* code_end = code_item.insns_ + code_item.insns_size_in_code_units_;
  code_start_ = code_ptr;

  // Setup the graph with the entry block and exit block.
  graph_ = new (arena_) HGraph(arena_);
  entry_block_ = new (arena_) HBasicBlock(graph_, 0);
  graph_->AddBlock(entry_block_);
  exit_block_ = new (arena_) HBasicBlock(graph_, kNoDexPc);
  graph_->SetEntryBlock(entry_block_);
  graph_->SetExitBlock(exit_block_);

  InitializeLocals(code_item.registers_size_);
  graph_->UpdateMaximumNumberOfOutVRegs(code_item.outs_size_);

  // To avoid splitting blocks, we compute ahead of time the instructions that
  // start a new block, and create these blocks.
  ComputeBranchTargets(code_ptr, code_end);

  if (!InitializeParameters(code_item.ins_size_)) {
    return nullptr;
  }

  size_t dex_offset = 0;
  while (code_ptr < code_end) {
    // Update the current block if dex_offset starts a new block.
    MaybeUpdateCurrentBlock(dex_offset);
    const Instruction& instruction = *Instruction::At(code_ptr);
    if (!AnalyzeDexInstruction(instruction, dex_offset)) return nullptr;
    dex_offset += instruction.SizeInCodeUnits();
    code_ptr += instruction.SizeInCodeUnits();
  }

  // Add the exit block at the end to give it the highest id.
  graph_->AddBlock(exit_block_);
  exit_block_->AddInstruction(new (arena_) HExit());
  // Add the suspend check to the entry block.
  entry_block_->AddInstruction(new (arena_) HSuspendCheck(0));
  entry_block_->AddInstruction(new (arena_) HGoto());
  return graph_;
}

void HGraphBuilder::MaybeUpdateCurrentBlock(size_t index) {
  HBasicBlock* block = FindBlockStartingAt(index);
  if (block == nullptr) {
    return;
  }

  if (current_block_ != nullptr) {
    // Branching instructions clear current_block, so we know
    // the last instruction of the current block is not a branching
    // instruction. We add an unconditional goto to the found block.
    current_block_->AddInstruction(new (arena_) HGoto());
    current_block_->AddSuccessor(block);
  }
  graph_->AddBlock(block);
  current_block_ = block;
}

void HGraphBuilder::ComputeBranchTargets(const uint16_t* code_ptr, const uint16_t* code_end) {
  // TODO: Support switch instructions.
  branch_targets_.SetSize(code_end - code_ptr);

  // Create the first block for the dex instructions, single successor of the entry block.
  HBasicBlock* block = new (arena_) HBasicBlock(graph_, 0);
  branch_targets_.Put(0, block);
  entry_block_->AddSuccessor(block);

  // Iterate over all instructions and find branching instructions. Create blocks for
  // the locations these instructions branch to.
  size_t dex_offset = 0;
  while (code_ptr < code_end) {
    const Instruction& instruction = *Instruction::At(code_ptr);
    if (instruction.IsBranch()) {
      int32_t target = instruction.GetTargetOffset() + dex_offset;
      // Create a block for the target instruction.
      if (FindBlockStartingAt(target) == nullptr) {
        block = new (arena_) HBasicBlock(graph_, target);
        branch_targets_.Put(target, block);
      }
      dex_offset += instruction.SizeInCodeUnits();
      code_ptr += instruction.SizeInCodeUnits();
      if ((code_ptr < code_end) && (FindBlockStartingAt(dex_offset) == nullptr)) {
        block = new (arena_) HBasicBlock(graph_, dex_offset);
        branch_targets_.Put(dex_offset, block);
      }
    } else {
      code_ptr += instruction.SizeInCodeUnits();
      dex_offset += instruction.SizeInCodeUnits();
    }
  }
}

HBasicBlock* HGraphBuilder::FindBlockStartingAt(int32_t index) const {
  DCHECK_GE(index, 0);
  return branch_targets_.Get(index);
}

template<typename T>
void HGraphBuilder::Unop_12x(const Instruction& instruction, Primitive::Type type) {
  HInstruction* first = LoadLocal(instruction.VRegB(), type);
  current_block_->AddInstruction(new (arena_) T(type, first));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}

template<typename T>
void HGraphBuilder::Binop_23x(const Instruction& instruction, Primitive::Type type) {
  HInstruction* first = LoadLocal(instruction.VRegB(), type);
  HInstruction* second = LoadLocal(instruction.VRegC(), type);
  current_block_->AddInstruction(new (arena_) T(type, first, second));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}

template<typename T>
void HGraphBuilder::Binop_12x(const Instruction& instruction, Primitive::Type type) {
  HInstruction* first = LoadLocal(instruction.VRegA(), type);
  HInstruction* second = LoadLocal(instruction.VRegB(), type);
  current_block_->AddInstruction(new (arena_) T(type, first, second));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}

template<typename T>
void HGraphBuilder::Binop_22s(const Instruction& instruction, bool reverse) {
  HInstruction* first = LoadLocal(instruction.VRegB(), Primitive::kPrimInt);
  HInstruction* second = GetIntConstant(instruction.VRegC_22s());
  if (reverse) {
    std::swap(first, second);
  }
  current_block_->AddInstruction(new (arena_) T(Primitive::kPrimInt, first, second));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}

template<typename T>
void HGraphBuilder::Binop_22b(const Instruction& instruction, bool reverse) {
  HInstruction* first = LoadLocal(instruction.VRegB(), Primitive::kPrimInt);
  HInstruction* second = GetIntConstant(instruction.VRegC_22b());
  if (reverse) {
    std::swap(first, second);
  }
  current_block_->AddInstruction(new (arena_) T(Primitive::kPrimInt, first, second));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
}

void HGraphBuilder::BuildReturn(const Instruction& instruction, Primitive::Type type) {
  if (type == Primitive::kPrimVoid) {
    current_block_->AddInstruction(new (arena_) HReturnVoid());
  } else {
    HInstruction* value = LoadLocal(instruction.VRegA(), type);
    current_block_->AddInstruction(new (arena_) HReturn(value));
  }
  current_block_->AddSuccessor(exit_block_);
  current_block_ = nullptr;
}

bool HGraphBuilder::BuildInvoke(const Instruction& instruction,
                                uint32_t dex_offset,
                                uint32_t method_idx,
                                uint32_t number_of_vreg_arguments,
                                bool is_range,
                                uint32_t* args,
                                uint32_t register_index) {
  Instruction::Code opcode = instruction.Opcode();
  InvokeType invoke_type;
  switch (opcode) {
    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_STATIC_RANGE:
      invoke_type = kStatic;
      break;
    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_DIRECT_RANGE:
      invoke_type = kDirect;
      break;
    case Instruction::INVOKE_VIRTUAL:
    case Instruction::INVOKE_VIRTUAL_RANGE:
      invoke_type = kVirtual;
      break;
    case Instruction::INVOKE_INTERFACE:
    case Instruction::INVOKE_INTERFACE_RANGE:
      invoke_type = kInterface;
      break;
    case Instruction::INVOKE_SUPER_RANGE:
    case Instruction::INVOKE_SUPER:
      invoke_type = kSuper;
      break;
    default:
      LOG(FATAL) << "Unexpected invoke op: " << opcode;
      return false;
  }

  const DexFile::MethodId& method_id = dex_file_->GetMethodId(method_idx);
  const DexFile::ProtoId& proto_id = dex_file_->GetProtoId(method_id.proto_idx_);
  const char* descriptor = dex_file_->StringDataByIdx(proto_id.shorty_idx_);
  Primitive::Type return_type = Primitive::GetType(descriptor[0]);
  bool is_instance_call = invoke_type != kStatic;
  const size_t number_of_arguments = strlen(descriptor) - (is_instance_call ? 0 : 1);

  HInvoke* invoke = nullptr;
  if (invoke_type == kVirtual) {
    MethodReference target_method(dex_file_, method_idx);
    uintptr_t direct_code;
    uintptr_t direct_method;
    int vtable_index;
    // TODO: Add devirtualization support.
    compiler_driver_->ComputeInvokeInfo(dex_compilation_unit_, dex_offset, true, true,
                                        &invoke_type, &target_method, &vtable_index,
                                        &direct_code, &direct_method);
    if (vtable_index == -1) {
      return false;
    }
    invoke = new (arena_) HInvokeVirtual(
        arena_, number_of_arguments, return_type, dex_offset, vtable_index);
  } else {
    // Treat invoke-direct like static calls for now.
    invoke = new (arena_) HInvokeStatic(
        arena_, number_of_arguments, return_type, dex_offset, method_idx);
  }

  size_t start_index = 0;
  Temporaries temps(graph_, is_instance_call ? 1 : 0);
  if (is_instance_call) {
    HInstruction* arg = LoadLocal(is_range ? register_index : args[0], Primitive::kPrimNot);
    HNullCheck* null_check = new (arena_) HNullCheck(arg, dex_offset);
    current_block_->AddInstruction(null_check);
    temps.Add(null_check);
    invoke->SetArgumentAt(0, null_check);
    start_index = 1;
  }

  uint32_t descriptor_index = 1;
  uint32_t argument_index = start_index;
  for (size_t i = start_index; i < number_of_vreg_arguments; i++, argument_index++) {
    Primitive::Type type = Primitive::GetType(descriptor[descriptor_index++]);
    bool is_wide = (type == Primitive::kPrimLong) || (type == Primitive::kPrimDouble);
    if (!is_range && is_wide && args[i] + 1 != args[i + 1]) {
      LOG(WARNING) << "Non sequential register pair in " << dex_compilation_unit_->GetSymbol()
                   << " at " << dex_offset;
      // We do not implement non sequential register pair.
      return false;
    }
    HInstruction* arg = LoadLocal(is_range ? register_index + i : args[i], type);
    invoke->SetArgumentAt(argument_index, arg);
    if (is_wide) {
      i++;
    }
  }

  DCHECK_EQ(argument_index, number_of_arguments);
  current_block_->AddInstruction(invoke);
  latest_result_ = invoke;
  return true;
}

bool HGraphBuilder::BuildFieldAccess(const Instruction& instruction,
                                     uint32_t dex_offset,
                                     bool is_put) {
  uint32_t source_or_dest_reg = instruction.VRegA_22c();
  uint32_t obj_reg = instruction.VRegB_22c();
  uint16_t field_index = instruction.VRegC_22c();

  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ArtField> resolved_field(hs.NewHandle(
      compiler_driver_->ComputeInstanceFieldInfo(field_index, dex_compilation_unit_, is_put, soa)));

  if (resolved_field.Get() == nullptr) {
    return false;
  }
  if (resolved_field->IsVolatile()) {
    return false;
  }

  Primitive::Type field_type = resolved_field->GetTypeAsPrimitiveType();
  if (!IsTypeSupported(field_type)) {
    return false;
  }

  HInstruction* object = LoadLocal(obj_reg, Primitive::kPrimNot);
  current_block_->AddInstruction(new (arena_) HNullCheck(object, dex_offset));
  if (is_put) {
    Temporaries temps(graph_, 1);
    HInstruction* null_check = current_block_->GetLastInstruction();
    // We need one temporary for the null check.
    temps.Add(null_check);
    HInstruction* value = LoadLocal(source_or_dest_reg, field_type);
    current_block_->AddInstruction(new (arena_) HInstanceFieldSet(
        null_check,
        value,
        field_type,
        resolved_field->GetOffset()));
  } else {
    current_block_->AddInstruction(new (arena_) HInstanceFieldGet(
        current_block_->GetLastInstruction(),
        field_type,
        resolved_field->GetOffset()));

    UpdateLocal(source_or_dest_reg, current_block_->GetLastInstruction());
  }
  return true;
}

void HGraphBuilder::BuildArrayAccess(const Instruction& instruction,
                                     uint32_t dex_offset,
                                     bool is_put,
                                     Primitive::Type anticipated_type) {
  uint8_t source_or_dest_reg = instruction.VRegA_23x();
  uint8_t array_reg = instruction.VRegB_23x();
  uint8_t index_reg = instruction.VRegC_23x();

  DCHECK(IsTypeSupported(anticipated_type));

  // We need one temporary for the null check, one for the index, and one for the length.
  Temporaries temps(graph_, 3);

  HInstruction* object = LoadLocal(array_reg, Primitive::kPrimNot);
  object = new (arena_) HNullCheck(object, dex_offset);
  current_block_->AddInstruction(object);
  temps.Add(object);

  HInstruction* length = new (arena_) HArrayLength(object);
  current_block_->AddInstruction(length);
  temps.Add(length);
  HInstruction* index = LoadLocal(index_reg, Primitive::kPrimInt);
  index = new (arena_) HBoundsCheck(index, length, dex_offset);
  current_block_->AddInstruction(index);
  temps.Add(index);
  if (is_put) {
    HInstruction* value = LoadLocal(source_or_dest_reg, anticipated_type);
    // TODO: Insert a type check node if the type is Object.
    current_block_->AddInstruction(new (arena_) HArraySet(
        object, index, value, anticipated_type, dex_offset));
  } else {
    current_block_->AddInstruction(new (arena_) HArrayGet(object, index, anticipated_type));
    UpdateLocal(source_or_dest_reg, current_block_->GetLastInstruction());
  }
}

void HGraphBuilder::BuildFilledNewArray(uint32_t dex_offset,
                                        uint32_t type_index,
                                        uint32_t number_of_vreg_arguments,
                                        bool is_range,
                                        uint32_t* args,
                                        uint32_t register_index) {
  HInstruction* length = GetIntConstant(number_of_vreg_arguments);
  HInstruction* object = new (arena_) HNewArray(length, dex_offset, type_index);
  current_block_->AddInstruction(object);

  const char* descriptor = dex_file_->StringByTypeIdx(type_index);
  DCHECK_EQ(descriptor[0], '[') << descriptor;
  char primitive = descriptor[1];
  DCHECK(primitive == 'I'
      || primitive == 'L'
      || primitive == '[') << descriptor;
  bool is_reference_array = (primitive == 'L') || (primitive == '[');
  Primitive::Type type = is_reference_array ? Primitive::kPrimNot : Primitive::kPrimInt;

  Temporaries temps(graph_, 1);
  temps.Add(object);
  for (size_t i = 0; i < number_of_vreg_arguments; ++i) {
    HInstruction* value = LoadLocal(is_range ? register_index + i : args[i], type);
    HInstruction* index = GetIntConstant(i);
    current_block_->AddInstruction(
        new (arena_) HArraySet(object, index, value, type, dex_offset));
  }
  latest_result_ = object;
}

template <typename T>
void HGraphBuilder::BuildFillArrayData(HInstruction* object,
                                       const T* data,
                                       uint32_t element_count,
                                       Primitive::Type anticipated_type,
                                       uint32_t dex_offset) {
  for (uint32_t i = 0; i < element_count; ++i) {
    HInstruction* index = GetIntConstant(i);
    HInstruction* value = GetIntConstant(data[i]);
    current_block_->AddInstruction(new (arena_) HArraySet(
      object, index, value, anticipated_type, dex_offset));
  }
}

void HGraphBuilder::BuildFillWideArrayData(HInstruction* object,
                                           const int64_t* data,
                                           uint32_t element_count,
                                           uint32_t dex_offset) {
  for (uint32_t i = 0; i < element_count; ++i) {
    HInstruction* index = GetIntConstant(i);
    HInstruction* value = GetLongConstant(data[i]);
    current_block_->AddInstruction(new (arena_) HArraySet(
      object, index, value, Primitive::kPrimLong, dex_offset));
  }
}

void HGraphBuilder::PotentiallyAddSuspendCheck(int32_t target_offset, uint32_t dex_offset) {
  if (target_offset <= 0) {
    // Unconditionnally add a suspend check to backward branches. We can remove
    // them after we recognize loops in the graph.
    current_block_->AddInstruction(new (arena_) HSuspendCheck(dex_offset));
  }
}

bool HGraphBuilder::AnalyzeDexInstruction(const Instruction& instruction, uint32_t dex_offset) {
  if (current_block_ == nullptr) {
    return true;  // Dead code
  }

  switch (instruction.Opcode()) {
    case Instruction::CONST_4: {
      int32_t register_index = instruction.VRegA();
      HIntConstant* constant = GetIntConstant(instruction.VRegB_11n());
      UpdateLocal(register_index, constant);
      break;
    }

    case Instruction::CONST_16: {
      int32_t register_index = instruction.VRegA();
      HIntConstant* constant = GetIntConstant(instruction.VRegB_21s());
      UpdateLocal(register_index, constant);
      break;
    }

    case Instruction::CONST: {
      int32_t register_index = instruction.VRegA();
      HIntConstant* constant = GetIntConstant(instruction.VRegB_31i());
      UpdateLocal(register_index, constant);
      break;
    }

    case Instruction::CONST_HIGH16: {
      int32_t register_index = instruction.VRegA();
      HIntConstant* constant = GetIntConstant(instruction.VRegB_21h() << 16);
      UpdateLocal(register_index, constant);
      break;
    }

    case Instruction::CONST_WIDE_16: {
      int32_t register_index = instruction.VRegA();
      // Get 16 bits of constant value, sign extended to 64 bits.
      int64_t value = instruction.VRegB_21s();
      value <<= 48;
      value >>= 48;
      HLongConstant* constant = GetLongConstant(value);
      UpdateLocal(register_index, constant);
      break;
    }

    case Instruction::CONST_WIDE_32: {
      int32_t register_index = instruction.VRegA();
      // Get 32 bits of constant value, sign extended to 64 bits.
      int64_t value = instruction.VRegB_31i();
      value <<= 32;
      value >>= 32;
      HLongConstant* constant = GetLongConstant(value);
      UpdateLocal(register_index, constant);
      break;
    }

    case Instruction::CONST_WIDE: {
      int32_t register_index = instruction.VRegA();
      HLongConstant* constant = GetLongConstant(instruction.VRegB_51l());
      UpdateLocal(register_index, constant);
      break;
    }

    case Instruction::CONST_WIDE_HIGH16: {
      int32_t register_index = instruction.VRegA();
      int64_t value = static_cast<int64_t>(instruction.VRegB_21h()) << 48;
      HLongConstant* constant = GetLongConstant(value);
      UpdateLocal(register_index, constant);
      break;
    }

    // TODO: these instructions are also used to move floating point values, so what is
    // the type (int or float)?
    case Instruction::MOVE:
    case Instruction::MOVE_FROM16:
    case Instruction::MOVE_16: {
      HInstruction* value = LoadLocal(instruction.VRegB(), Primitive::kPrimInt);
      UpdateLocal(instruction.VRegA(), value);
      break;
    }

    // TODO: these instructions are also used to move floating point values, so what is
    // the type (long or double)?
    case Instruction::MOVE_WIDE:
    case Instruction::MOVE_WIDE_FROM16:
    case Instruction::MOVE_WIDE_16: {
      HInstruction* value = LoadLocal(instruction.VRegB(), Primitive::kPrimLong);
      UpdateLocal(instruction.VRegA(), value);
      break;
    }

    case Instruction::MOVE_OBJECT:
    case Instruction::MOVE_OBJECT_16:
    case Instruction::MOVE_OBJECT_FROM16: {
      HInstruction* value = LoadLocal(instruction.VRegB(), Primitive::kPrimNot);
      UpdateLocal(instruction.VRegA(), value);
      break;
    }

    case Instruction::RETURN_VOID: {
      BuildReturn(instruction, Primitive::kPrimVoid);
      break;
    }

#define IF_XX(comparison, cond) \
    case Instruction::IF_##cond: If_22t<comparison>(instruction, dex_offset); break; \
    case Instruction::IF_##cond##Z: If_21t<comparison>(instruction, dex_offset); break

    IF_XX(HEqual, EQ);
    IF_XX(HNotEqual, NE);
    IF_XX(HLessThan, LT);
    IF_XX(HLessThanOrEqual, LE);
    IF_XX(HGreaterThan, GT);
    IF_XX(HGreaterThanOrEqual, GE);

    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32: {
      int32_t offset = instruction.GetTargetOffset();
      PotentiallyAddSuspendCheck(offset, dex_offset);
      HBasicBlock* target = FindBlockStartingAt(offset + dex_offset);
      DCHECK(target != nullptr);
      current_block_->AddInstruction(new (arena_) HGoto());
      current_block_->AddSuccessor(target);
      current_block_ = nullptr;
      break;
    }

    case Instruction::RETURN: {
      DCHECK_NE(return_type_, Primitive::kPrimNot);
      DCHECK_NE(return_type_, Primitive::kPrimLong);
      DCHECK_NE(return_type_, Primitive::kPrimDouble);
      BuildReturn(instruction, return_type_);
      break;
    }

    case Instruction::RETURN_OBJECT: {
      DCHECK(return_type_ == Primitive::kPrimNot);
      BuildReturn(instruction, return_type_);
      break;
    }

    case Instruction::RETURN_WIDE: {
      DCHECK(return_type_ == Primitive::kPrimDouble || return_type_ == Primitive::kPrimLong);
      BuildReturn(instruction, return_type_);
      break;
    }

    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_VIRTUAL: {
      uint32_t method_idx = instruction.VRegB_35c();
      uint32_t number_of_vreg_arguments = instruction.VRegA_35c();
      uint32_t args[5];
      instruction.GetVarArgs(args);
      if (!BuildInvoke(instruction, dex_offset, method_idx, number_of_vreg_arguments, false, args, -1)) {
        return false;
      }
      break;
    }

    case Instruction::INVOKE_STATIC_RANGE:
    case Instruction::INVOKE_DIRECT_RANGE:
    case Instruction::INVOKE_VIRTUAL_RANGE: {
      uint32_t method_idx = instruction.VRegB_3rc();
      uint32_t number_of_vreg_arguments = instruction.VRegA_3rc();
      uint32_t register_index = instruction.VRegC();
      if (!BuildInvoke(instruction, dex_offset, method_idx,
                       number_of_vreg_arguments, true, nullptr, register_index)) {
        return false;
      }
      break;
    }

    case Instruction::NEG_INT: {
      Unop_12x<HNeg>(instruction, Primitive::kPrimInt);
      break;
    }

    case Instruction::NEG_LONG: {
      Unop_12x<HNeg>(instruction, Primitive::kPrimLong);
      break;
    }

    case Instruction::NOT_INT: {
      Unop_12x<HNot>(instruction, Primitive::kPrimInt);
      break;
    }

    case Instruction::ADD_INT: {
      Binop_23x<HAdd>(instruction, Primitive::kPrimInt);
      break;
    }

    case Instruction::ADD_LONG: {
      Binop_23x<HAdd>(instruction, Primitive::kPrimLong);
      break;
    }

    case Instruction::ADD_DOUBLE: {
      Binop_23x<HAdd>(instruction, Primitive::kPrimDouble);
      break;
    }

    case Instruction::ADD_FLOAT: {
      Binop_23x<HAdd>(instruction, Primitive::kPrimFloat);
      break;
    }

    case Instruction::SUB_INT: {
      Binop_23x<HSub>(instruction, Primitive::kPrimInt);
      break;
    }

    case Instruction::SUB_LONG: {
      Binop_23x<HSub>(instruction, Primitive::kPrimLong);
      break;
    }

    case Instruction::SUB_FLOAT: {
      Binop_23x<HSub>(instruction, Primitive::kPrimFloat);
      break;
    }

    case Instruction::SUB_DOUBLE: {
      Binop_23x<HSub>(instruction, Primitive::kPrimDouble);
      break;
    }

    case Instruction::ADD_INT_2ADDR: {
      Binop_12x<HAdd>(instruction, Primitive::kPrimInt);
      break;
    }

    case Instruction::MUL_INT: {
      Binop_23x<HMul>(instruction, Primitive::kPrimInt);
      break;
    }

    case Instruction::MUL_LONG: {
      Binop_23x<HMul>(instruction, Primitive::kPrimLong);
      break;
    }

    case Instruction::MUL_FLOAT: {
      Binop_23x<HMul>(instruction, Primitive::kPrimFloat);
      break;
    }

    case Instruction::MUL_DOUBLE: {
      Binop_23x<HMul>(instruction, Primitive::kPrimDouble);
      break;
    }

    case Instruction::ADD_LONG_2ADDR: {
      Binop_12x<HAdd>(instruction, Primitive::kPrimLong);
      break;
    }

    case Instruction::ADD_DOUBLE_2ADDR: {
      Binop_12x<HAdd>(instruction, Primitive::kPrimDouble);
      break;
    }

    case Instruction::ADD_FLOAT_2ADDR: {
      Binop_12x<HAdd>(instruction, Primitive::kPrimFloat);
      break;
    }

    case Instruction::SUB_INT_2ADDR: {
      Binop_12x<HSub>(instruction, Primitive::kPrimInt);
      break;
    }

    case Instruction::SUB_LONG_2ADDR: {
      Binop_12x<HSub>(instruction, Primitive::kPrimLong);
      break;
    }

    case Instruction::SUB_FLOAT_2ADDR: {
      Binop_12x<HSub>(instruction, Primitive::kPrimFloat);
      break;
    }

    case Instruction::SUB_DOUBLE_2ADDR: {
      Binop_12x<HSub>(instruction, Primitive::kPrimDouble);
      break;
    }

    case Instruction::MUL_INT_2ADDR: {
      Binop_12x<HMul>(instruction, Primitive::kPrimInt);
      break;
    }

    case Instruction::MUL_LONG_2ADDR: {
      Binop_12x<HMul>(instruction, Primitive::kPrimLong);
      break;
    }

    case Instruction::MUL_FLOAT_2ADDR: {
      Binop_12x<HMul>(instruction, Primitive::kPrimFloat);
      break;
    }

    case Instruction::MUL_DOUBLE_2ADDR: {
      Binop_12x<HMul>(instruction, Primitive::kPrimDouble);
      break;
    }

    case Instruction::ADD_INT_LIT16: {
      Binop_22s<HAdd>(instruction, false);
      break;
    }

    case Instruction::RSUB_INT: {
      Binop_22s<HSub>(instruction, true);
      break;
    }

    case Instruction::MUL_INT_LIT16: {
      Binop_22s<HMul>(instruction, false);
      break;
    }

    case Instruction::ADD_INT_LIT8: {
      Binop_22b<HAdd>(instruction, false);
      break;
    }

    case Instruction::RSUB_INT_LIT8: {
      Binop_22b<HSub>(instruction, true);
      break;
    }

    case Instruction::MUL_INT_LIT8: {
      Binop_22b<HMul>(instruction, false);
      break;
    }

    case Instruction::NEW_INSTANCE: {
      current_block_->AddInstruction(
          new (arena_) HNewInstance(dex_offset, instruction.VRegB_21c()));
      UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction());
      break;
    }

    case Instruction::NEW_ARRAY: {
      HInstruction* length = LoadLocal(instruction.VRegB_22c(), Primitive::kPrimInt);
      current_block_->AddInstruction(
          new (arena_) HNewArray(length, dex_offset, instruction.VRegC_22c()));
      UpdateLocal(instruction.VRegA_22c(), current_block_->GetLastInstruction());
      break;
    }

    case Instruction::FILLED_NEW_ARRAY: {
      uint32_t number_of_vreg_arguments = instruction.VRegA_35c();
      uint32_t type_index = instruction.VRegB_35c();
      uint32_t args[5];
      instruction.GetVarArgs(args);
      BuildFilledNewArray(dex_offset, type_index, number_of_vreg_arguments, false, args, 0);
      break;
    }

    case Instruction::FILLED_NEW_ARRAY_RANGE: {
      uint32_t number_of_vreg_arguments = instruction.VRegA_3rc();
      uint32_t type_index = instruction.VRegB_3rc();
      uint32_t register_index = instruction.VRegC_3rc();
      BuildFilledNewArray(
          dex_offset, type_index, number_of_vreg_arguments, true, nullptr, register_index);
      break;
    }

    case Instruction::FILL_ARRAY_DATA: {
      Temporaries temps(graph_, 1);
      HInstruction* array = LoadLocal(instruction.VRegA_31t(), Primitive::kPrimNot);
      HNullCheck* null_check = new (arena_) HNullCheck(array, dex_offset);
      current_block_->AddInstruction(null_check);
      temps.Add(null_check);

      HInstruction* length = new (arena_) HArrayLength(null_check);
      current_block_->AddInstruction(length);

      int32_t payload_offset = instruction.VRegB_31t() + dex_offset;
      const Instruction::ArrayDataPayload* payload =
          reinterpret_cast<const Instruction::ArrayDataPayload*>(code_start_ + payload_offset);
      const uint8_t* data = payload->data;
      uint32_t element_count = payload->element_count;

      // Implementation of this DEX instruction seems to be that the bounds check is
      // done before doing any stores.
      HInstruction* last_index = GetIntConstant(payload->element_count - 1);
      current_block_->AddInstruction(new (arena_) HBoundsCheck(last_index, length, dex_offset));

      switch (payload->element_width) {
        case 1:
          BuildFillArrayData(null_check,
                             reinterpret_cast<const int8_t*>(data),
                             element_count,
                             Primitive::kPrimByte,
                             dex_offset);
          break;
        case 2:
          BuildFillArrayData(null_check,
                             reinterpret_cast<const int16_t*>(data),
                             element_count,
                             Primitive::kPrimShort,
                             dex_offset);
          break;
        case 4:
          BuildFillArrayData(null_check,
                             reinterpret_cast<const int32_t*>(data),
                             element_count,
                             Primitive::kPrimInt,
                             dex_offset);
          break;
        case 8:
          BuildFillWideArrayData(null_check,
                                 reinterpret_cast<const int64_t*>(data),
                                 element_count,
                                 dex_offset);
          break;
        default:
          LOG(FATAL) << "Unknown element width for " << payload->element_width;
      }
      break;
    }

    case Instruction::MOVE_RESULT:
    case Instruction::MOVE_RESULT_WIDE:
    case Instruction::MOVE_RESULT_OBJECT:
      UpdateLocal(instruction.VRegA(), latest_result_);
      latest_result_ = nullptr;
      break;

    case Instruction::CMP_LONG: {
      Binop_23x<HCompare>(instruction, Primitive::kPrimLong);
      break;
    }

    case Instruction::NOP:
      break;

    case Instruction::IGET:
    case Instruction::IGET_WIDE:
    case Instruction::IGET_OBJECT:
    case Instruction::IGET_BOOLEAN:
    case Instruction::IGET_BYTE:
    case Instruction::IGET_CHAR:
    case Instruction::IGET_SHORT: {
      if (!BuildFieldAccess(instruction, dex_offset, false)) {
        return false;
      }
      break;
    }

    case Instruction::IPUT:
    case Instruction::IPUT_WIDE:
    case Instruction::IPUT_OBJECT:
    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BYTE:
    case Instruction::IPUT_CHAR:
    case Instruction::IPUT_SHORT: {
      if (!BuildFieldAccess(instruction, dex_offset, true)) {
        return false;
      }
      break;
    }

#define ARRAY_XX(kind, anticipated_type)                                          \
    case Instruction::AGET##kind: {                                               \
      BuildArrayAccess(instruction, dex_offset, false, anticipated_type);         \
      break;                                                                      \
    }                                                                             \
    case Instruction::APUT##kind: {                                               \
      BuildArrayAccess(instruction, dex_offset, true, anticipated_type);          \
      break;                                                                      \
    }

    ARRAY_XX(, Primitive::kPrimInt);
    ARRAY_XX(_WIDE, Primitive::kPrimLong);
    ARRAY_XX(_OBJECT, Primitive::kPrimNot);
    ARRAY_XX(_BOOLEAN, Primitive::kPrimBoolean);
    ARRAY_XX(_BYTE, Primitive::kPrimByte);
    ARRAY_XX(_CHAR, Primitive::kPrimChar);
    ARRAY_XX(_SHORT, Primitive::kPrimShort);

    case Instruction::ARRAY_LENGTH: {
      HInstruction* object = LoadLocal(instruction.VRegB_12x(), Primitive::kPrimNot);
      current_block_->AddInstruction(new (arena_) HArrayLength(object));
      UpdateLocal(instruction.VRegA_12x(), current_block_->GetLastInstruction());
      break;
    }

    default:
      return false;
  }
  return true;
}

HIntConstant* HGraphBuilder::GetIntConstant0() {
  if (constant0_ != nullptr) {
    return constant0_;
  }
  constant0_ = new(arena_) HIntConstant(0);
  entry_block_->AddInstruction(constant0_);
  return constant0_;
}

HIntConstant* HGraphBuilder::GetIntConstant1() {
  if (constant1_ != nullptr) {
    return constant1_;
  }
  constant1_ = new(arena_) HIntConstant(1);
  entry_block_->AddInstruction(constant1_);
  return constant1_;
}

HIntConstant* HGraphBuilder::GetIntConstant(int32_t constant) {
  switch (constant) {
    case 0: return GetIntConstant0();
    case 1: return GetIntConstant1();
    default: {
      HIntConstant* instruction = new (arena_) HIntConstant(constant);
      entry_block_->AddInstruction(instruction);
      return instruction;
    }
  }
}

HLongConstant* HGraphBuilder::GetLongConstant(int64_t constant) {
  HLongConstant* instruction = new (arena_) HLongConstant(constant);
  entry_block_->AddInstruction(instruction);
  return instruction;
}

HLocal* HGraphBuilder::GetLocalAt(int register_index) const {
  return locals_.Get(register_index);
}

void HGraphBuilder::UpdateLocal(int register_index, HInstruction* instruction) const {
  HLocal* local = GetLocalAt(register_index);
  current_block_->AddInstruction(new (arena_) HStoreLocal(local, instruction));
}

HInstruction* HGraphBuilder::LoadLocal(int register_index, Primitive::Type type) const {
  HLocal* local = GetLocalAt(register_index);
  current_block_->AddInstruction(new (arena_) HLoadLocal(local, type));
  return current_block_->GetLastInstruction();
}

}  // namespace art
