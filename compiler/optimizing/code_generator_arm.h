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

#ifndef ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM_H_
#define ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM_H_

#include "code_generator.h"
#include "nodes.h"
#include "parallel_move_resolver.h"
#include "utils/arm/assembler_thumb2.h"

namespace art {
namespace arm {

class CodeGeneratorARM;

static constexpr size_t kArmWordSize = 4;

static constexpr Register kParameterCoreRegisters[] = { R1, R2, R3 };
static constexpr RegisterPair kParameterCorePairRegisters[] = { R1_R2, R2_R3 };
static constexpr size_t kParameterCoreRegistersLength = arraysize(kParameterCoreRegisters);
static constexpr DRegister kParameterFpuRegisters[] = { };
static constexpr size_t kParameterFpuRegistersLength = 0;

class InvokeDexCallingConvention : public CallingConvention<Register, DRegister> {
 public:
  InvokeDexCallingConvention()
      : CallingConvention(kParameterCoreRegisters,
                          kParameterCoreRegistersLength,
                          kParameterFpuRegisters,
                          kParameterFpuRegistersLength) {}

  RegisterPair GetRegisterPairAt(size_t argument_index) {
    DCHECK_LT(argument_index + 1, GetNumberOfRegisters());
    return kParameterCorePairRegisters[argument_index];
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConvention);
};

class InvokeDexCallingConventionVisitor {
 public:
  InvokeDexCallingConventionVisitor() : gp_index_(0) {}

  Location GetNextLocation(Primitive::Type type);

 private:
  InvokeDexCallingConvention calling_convention;
  uint32_t gp_index_;

  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConventionVisitor);
};

class ParallelMoveResolverARM : public ParallelMoveResolver {
 public:
  ParallelMoveResolverARM(ArenaAllocator* allocator, CodeGeneratorARM* codegen)
      : ParallelMoveResolver(allocator), codegen_(codegen) {}

  virtual void EmitMove(size_t index) OVERRIDE;
  virtual void EmitSwap(size_t index) OVERRIDE;
  virtual void SpillScratch(int reg) OVERRIDE;
  virtual void RestoreScratch(int reg) OVERRIDE;

  ArmAssembler* GetAssembler() const;

 private:
  void Exchange(Register reg, int mem);
  void Exchange(int mem1, int mem2);

  CodeGeneratorARM* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(ParallelMoveResolverARM);
};

class LocationsBuilderARM : public HGraphVisitor {
 public:
  LocationsBuilderARM(HGraph* graph, CodeGeneratorARM* codegen)
      : HGraphVisitor(graph), codegen_(codegen) {}

#define DECLARE_VISIT_INSTRUCTION(name, super)     \
  virtual void Visit##name(H##name* instr);

  FOR_EACH_CONCRETE_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

  void HandleInvoke(HInvoke* invoke);

 private:
  CodeGeneratorARM* const codegen_;
  InvokeDexCallingConventionVisitor parameter_visitor_;

  DISALLOW_COPY_AND_ASSIGN(LocationsBuilderARM);
};

class InstructionCodeGeneratorARM : public HGraphVisitor {
 public:
  InstructionCodeGeneratorARM(HGraph* graph, CodeGeneratorARM* codegen);

#define DECLARE_VISIT_INSTRUCTION(name, super)     \
  virtual void Visit##name(H##name* instr);

  FOR_EACH_CONCRETE_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

  ArmAssembler* GetAssembler() const { return assembler_; }
  void LoadCurrentMethod(Register reg);

 private:
  // Generate code for the given suspend check. If not null, `successor`
  // is the block to branch to if the suspend check is not needed, and after
  // the suspend call.
  void GenerateSuspendCheck(HSuspendCheck* check, HBasicBlock* successor);

  ArmAssembler* const assembler_;
  CodeGeneratorARM* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(InstructionCodeGeneratorARM);
};

class CodeGeneratorARM : public CodeGenerator {
 public:
  explicit CodeGeneratorARM(HGraph* graph);
  virtual ~CodeGeneratorARM() {}

  virtual void GenerateFrameEntry() OVERRIDE;
  virtual void GenerateFrameExit() OVERRIDE;
  virtual void Bind(HBasicBlock* block) OVERRIDE;
  virtual void Move(HInstruction* instruction, Location location, HInstruction* move_for) OVERRIDE;
  virtual size_t SaveCoreRegister(size_t stack_index, uint32_t reg_id) OVERRIDE;
  virtual size_t RestoreCoreRegister(size_t stack_index, uint32_t reg_id) OVERRIDE;

  virtual size_t GetWordSize() const OVERRIDE {
    return kArmWordSize;
  }

  virtual size_t FrameEntrySpillSize() const OVERRIDE;

  virtual HGraphVisitor* GetLocationBuilder() OVERRIDE {
    return &location_builder_;
  }

  virtual HGraphVisitor* GetInstructionVisitor() OVERRIDE {
    return &instruction_visitor_;
  }

  virtual ArmAssembler* GetAssembler() OVERRIDE {
    return &assembler_;
  }

  virtual void SetupBlockedRegisters() const OVERRIDE;

  virtual Location AllocateFreeRegister(Primitive::Type type) const OVERRIDE;

  virtual Location GetStackLocation(HLoadLocal* load) const OVERRIDE;

  virtual void DumpCoreRegister(std::ostream& stream, int reg) const OVERRIDE;
  virtual void DumpFloatingPointRegister(std::ostream& stream, int reg) const OVERRIDE;

  // Blocks all register pairs made out of blocked core registers.
  void UpdateBlockedPairRegisters() const;

  ParallelMoveResolverARM* GetMoveResolver() {
    return &move_resolver_;
  }

  virtual InstructionSet GetInstructionSet() const OVERRIDE {
    return InstructionSet::kThumb2;
  }

  // Helper method to move a 32bits value between two locations.
  void Move32(Location destination, Location source);
  // Helper method to move a 64bits value between two locations.
  void Move64(Location destination, Location source);

  // Emit a write barrier.
  void MarkGCCard(Register temp, Register card, Register object, Register value);

  Label* GetLabelOf(HBasicBlock* block) const {
    return block_labels_.GetRawStorage() + block->GetBlockId();
  }

  virtual void Initialize() OVERRIDE {
    block_labels_.SetSize(GetGraph()->GetBlocks().Size());
  }

 private:
  // Labels for each block that will be compiled.
  GrowableArray<Label> block_labels_;
  LocationsBuilderARM location_builder_;
  InstructionCodeGeneratorARM instruction_visitor_;
  ParallelMoveResolverARM move_resolver_;
  Thumb2Assembler assembler_;

  DISALLOW_COPY_AND_ASSIGN(CodeGeneratorARM);
};

}  // namespace arm
}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_ARM_H_
