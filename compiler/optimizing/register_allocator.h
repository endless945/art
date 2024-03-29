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

#ifndef ART_COMPILER_OPTIMIZING_REGISTER_ALLOCATOR_H_
#define ART_COMPILER_OPTIMIZING_REGISTER_ALLOCATOR_H_

#include "base/macros.h"
#include "primitive.h"
#include "utils/growable_array.h"

namespace art {

class CodeGenerator;
class HBasicBlock;
class HGraph;
class HInstruction;
class HParallelMove;
class LiveInterval;
class Location;
class SsaLivenessAnalysis;

/**
 * An implementation of a linear scan register allocator on an `HGraph` with SSA form.
 */
class RegisterAllocator {
 public:
  RegisterAllocator(ArenaAllocator* allocator,
                    CodeGenerator* codegen,
                    const SsaLivenessAnalysis& analysis);

  // Main entry point for the register allocator. Given the liveness analysis,
  // allocates registers to live intervals.
  void AllocateRegisters();

  // Validate that the register allocator did not allocate the same register to
  // intervals that intersect each other. Returns false if it did not.
  bool Validate(bool log_fatal_on_failure) {
    processing_core_registers_ = true;
    if (!ValidateInternal(log_fatal_on_failure)) {
      return false;
    }
    processing_core_registers_ = false;
    return ValidateInternal(log_fatal_on_failure);
  }

  // Helper method for validation. Used by unit testing.
  static bool ValidateIntervals(const GrowableArray<LiveInterval*>& intervals,
                                size_t number_of_spill_slots,
                                size_t number_of_out_slots,
                                const CodeGenerator& codegen,
                                ArenaAllocator* allocator,
                                bool processing_core_registers,
                                bool log_fatal_on_failure);

  static bool CanAllocateRegistersFor(const HGraph& graph, InstructionSet instruction_set);
  static bool Supports(InstructionSet instruction_set) {
    return instruction_set == kX86
        || instruction_set == kArm
        || instruction_set == kX86_64
        || instruction_set == kThumb2;
  }

  size_t GetNumberOfSpillSlots() const {
    return spill_slots_.Size();
  }

 private:
  // Main methods of the allocator.
  void LinearScan();
  bool TryAllocateFreeReg(LiveInterval* interval);
  bool AllocateBlockedReg(LiveInterval* interval);
  void Resolve();

  // Add `interval` in the given sorted list.
  static void AddSorted(GrowableArray<LiveInterval*>* array, LiveInterval* interval);

  // Split `interval` at the position `at`. The new interval starts at `at`.
  LiveInterval* Split(LiveInterval* interval, size_t at);

  // Returns whether `reg` is blocked by the code generator.
  bool IsBlocked(int reg) const;

  // Update the interval for the register in `location` to cover [start, end).
  void BlockRegister(Location location, size_t start, size_t end);

  // Allocate a spill slot for the given interval.
  void AllocateSpillSlotFor(LiveInterval* interval);

  // Connect adjacent siblings within blocks.
  void ConnectSiblings(LiveInterval* interval);

  // Connect siblings between block entries and exits.
  void ConnectSplitSiblings(LiveInterval* interval, HBasicBlock* from, HBasicBlock* to) const;

  // Helper methods to insert parallel moves in the graph.
  void InsertParallelMoveAtExitOf(HBasicBlock* block,
                                  HInstruction* instruction,
                                  Location source,
                                  Location destination) const;
  void InsertParallelMoveAtEntryOf(HBasicBlock* block,
                                   HInstruction* instruction,
                                   Location source,
                                   Location destination) const;
  void InsertMoveAfter(HInstruction* instruction, Location source, Location destination) const;
  void AddInputMoveFor(HInstruction* user, Location source, Location destination) const;
  void InsertParallelMoveAt(size_t position,
                            HInstruction* instruction,
                            Location source,
                            Location destination) const;

  // Helper methods.
  void AllocateRegistersInternal();
  void ProcessInstruction(HInstruction* instruction);
  bool ValidateInternal(bool log_fatal_on_failure) const;
  void DumpInterval(std::ostream& stream, LiveInterval* interval) const;

  ArenaAllocator* const allocator_;
  CodeGenerator* const codegen_;
  const SsaLivenessAnalysis& liveness_;

  // List of intervals for core registers that must be processed, ordered by start
  // position. Last entry is the interval that has the lowest start position.
  // This list is initially populated before doing the linear scan.
  GrowableArray<LiveInterval*> unhandled_core_intervals_;

  // List of intervals for floating-point registers. Same comments as above.
  GrowableArray<LiveInterval*> unhandled_fp_intervals_;

  // Currently processed list of unhandled intervals. Either `unhandled_core_intervals_`
  // or `unhandled_fp_intervals_`.
  GrowableArray<LiveInterval*>* unhandled_;

  // List of intervals that have been processed.
  GrowableArray<LiveInterval*> handled_;

  // List of intervals that are currently active when processing a new live interval.
  // That is, they have a live range that spans the start of the new interval.
  GrowableArray<LiveInterval*> active_;

  // List of intervals that are currently inactive when processing a new live interval.
  // That is, they have a lifetime hole that spans the start of the new interval.
  GrowableArray<LiveInterval*> inactive_;

  // Fixed intervals for physical registers. Such intervals cover the positions
  // where an instruction requires a specific register.
  GrowableArray<LiveInterval*> physical_core_register_intervals_;
  GrowableArray<LiveInterval*> physical_fp_register_intervals_;

  // Intervals for temporaries. Such intervals cover the positions
  // where an instruction requires a temporary.
  GrowableArray<LiveInterval*> temp_intervals_;

  // The spill slots allocated for live intervals.
  GrowableArray<size_t> spill_slots_;

  // Instructions that need a safepoint.
  GrowableArray<HInstruction*> safepoints_;

  // True if processing core registers. False if processing floating
  // point registers.
  bool processing_core_registers_;

  // Number of registers for the current register kind (core or floating point).
  size_t number_of_registers_;

  // Temporary array, allocated ahead of time for simplicity.
  size_t* registers_array_;

  // Blocked registers, as decided by the code generator.
  bool* const blocked_core_registers_;
  bool* const blocked_fp_registers_;

  // Slots reserved for out arguments.
  size_t reserved_out_slots_;

  // The maximum live registers at safepoints.
  size_t maximum_number_of_live_registers_;

  ART_FRIEND_TEST(RegisterAllocatorTest, FreeUntil);

  DISALLOW_COPY_AND_ASSIGN(RegisterAllocator);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_REGISTER_ALLOCATOR_H_
