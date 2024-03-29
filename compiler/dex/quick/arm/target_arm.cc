/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "codegen_arm.h"

#include <inttypes.h>

#include <string>

#include "backend_arm.h"
#include "dex/compiler_internals.h"
#include "dex/quick/mir_to_lir-inl.h"

namespace art {

#ifdef ARM_R4_SUSPEND_FLAG
static constexpr RegStorage core_regs_arr[] =
    {rs_r0, rs_r1, rs_r2, rs_r3, rs_rARM_SUSPEND, rs_r5, rs_r6, rs_r7, rs_r8, rs_rARM_SELF,
     rs_r10, rs_r11, rs_r12, rs_rARM_SP, rs_rARM_LR, rs_rARM_PC};
#else
static constexpr RegStorage core_regs_arr[] =
    {rs_r0, rs_r1, rs_r2, rs_r3, rs_r4, rs_r5, rs_r6, rs_r7, rs_r8, rs_rARM_SELF,
     rs_r10, rs_r11, rs_r12, rs_rARM_SP, rs_rARM_LR, rs_rARM_PC};
#endif
static constexpr RegStorage sp_regs_arr[] =
    {rs_fr0, rs_fr1, rs_fr2, rs_fr3, rs_fr4, rs_fr5, rs_fr6, rs_fr7, rs_fr8, rs_fr9, rs_fr10,
     rs_fr11, rs_fr12, rs_fr13, rs_fr14, rs_fr15, rs_fr16, rs_fr17, rs_fr18, rs_fr19, rs_fr20,
     rs_fr21, rs_fr22, rs_fr23, rs_fr24, rs_fr25, rs_fr26, rs_fr27, rs_fr28, rs_fr29, rs_fr30,
     rs_fr31};
static constexpr RegStorage dp_regs_arr[] =
    {rs_dr0, rs_dr1, rs_dr2, rs_dr3, rs_dr4, rs_dr5, rs_dr6, rs_dr7, rs_dr8, rs_dr9, rs_dr10,
     rs_dr11, rs_dr12, rs_dr13, rs_dr14, rs_dr15};
#ifdef ARM_R4_SUSPEND_FLAG
static constexpr RegStorage reserved_regs_arr[] =
    {rs_rARM_SUSPEND, rs_rARM_SELF, rs_rARM_SP, rs_rARM_LR, rs_rARM_PC};
static constexpr RegStorage core_temps_arr[] = {rs_r0, rs_r1, rs_r2, rs_r3, rs_r12};
#else
static constexpr RegStorage reserved_regs_arr[] =
    {rs_rARM_SELF, rs_rARM_SP, rs_rARM_LR, rs_rARM_PC};
static constexpr RegStorage core_temps_arr[] = {rs_r0, rs_r1, rs_r2, rs_r3, rs_r4, rs_r12};
#endif
static constexpr RegStorage sp_temps_arr[] =
    {rs_fr0, rs_fr1, rs_fr2, rs_fr3, rs_fr4, rs_fr5, rs_fr6, rs_fr7, rs_fr8, rs_fr9, rs_fr10,
     rs_fr11, rs_fr12, rs_fr13, rs_fr14, rs_fr15};
static constexpr RegStorage dp_temps_arr[] =
    {rs_dr0, rs_dr1, rs_dr2, rs_dr3, rs_dr4, rs_dr5, rs_dr6, rs_dr7};

static constexpr ArrayRef<const RegStorage> empty_pool;
static constexpr ArrayRef<const RegStorage> core_regs(core_regs_arr);
static constexpr ArrayRef<const RegStorage> sp_regs(sp_regs_arr);
static constexpr ArrayRef<const RegStorage> dp_regs(dp_regs_arr);
static constexpr ArrayRef<const RegStorage> reserved_regs(reserved_regs_arr);
static constexpr ArrayRef<const RegStorage> core_temps(core_temps_arr);
static constexpr ArrayRef<const RegStorage> sp_temps(sp_temps_arr);
static constexpr ArrayRef<const RegStorage> dp_temps(dp_temps_arr);

RegLocation ArmMir2Lir::LocCReturn() {
  return arm_loc_c_return;
}

RegLocation ArmMir2Lir::LocCReturnRef() {
  return arm_loc_c_return;
}

RegLocation ArmMir2Lir::LocCReturnWide() {
  return arm_loc_c_return_wide;
}

RegLocation ArmMir2Lir::LocCReturnFloat() {
  return arm_loc_c_return_float;
}

RegLocation ArmMir2Lir::LocCReturnDouble() {
  return arm_loc_c_return_double;
}

// Return a target-dependent special register.
RegStorage ArmMir2Lir::TargetReg(SpecialTargetRegister reg) {
  RegStorage res_reg;
  switch (reg) {
    case kSelf: res_reg = rs_rARM_SELF; break;
#ifdef ARM_R4_SUSPEND_FLAG
    case kSuspend: res_reg =  rs_rARM_SUSPEND; break;
#else
    case kSuspend: res_reg = RegStorage::InvalidReg(); break;
#endif
    case kLr: res_reg =  rs_rARM_LR; break;
    case kPc: res_reg =  rs_rARM_PC; break;
    case kSp: res_reg =  rs_rARM_SP; break;
    case kArg0: res_reg = rs_r0; break;
    case kArg1: res_reg = rs_r1; break;
    case kArg2: res_reg = rs_r2; break;
    case kArg3: res_reg = rs_r3; break;
    case kFArg0: res_reg = kArm32QuickCodeUseSoftFloat ? rs_r0 : rs_fr0; break;
    case kFArg1: res_reg = kArm32QuickCodeUseSoftFloat ? rs_r1 : rs_fr1; break;
    case kFArg2: res_reg = kArm32QuickCodeUseSoftFloat ? rs_r2 : rs_fr2; break;
    case kFArg3: res_reg = kArm32QuickCodeUseSoftFloat ? rs_r3 : rs_fr3; break;
    case kFArg4: res_reg = kArm32QuickCodeUseSoftFloat ? RegStorage::InvalidReg() : rs_fr4; break;
    case kFArg5: res_reg = kArm32QuickCodeUseSoftFloat ? RegStorage::InvalidReg() : rs_fr5; break;
    case kFArg6: res_reg = kArm32QuickCodeUseSoftFloat ? RegStorage::InvalidReg() : rs_fr6; break;
    case kFArg7: res_reg = kArm32QuickCodeUseSoftFloat ? RegStorage::InvalidReg() : rs_fr7; break;
    case kFArg8: res_reg = kArm32QuickCodeUseSoftFloat ? RegStorage::InvalidReg() : rs_fr8; break;
    case kFArg9: res_reg = kArm32QuickCodeUseSoftFloat ? RegStorage::InvalidReg() : rs_fr9; break;
    case kFArg10: res_reg = kArm32QuickCodeUseSoftFloat ? RegStorage::InvalidReg() : rs_fr10; break;
    case kFArg11: res_reg = kArm32QuickCodeUseSoftFloat ? RegStorage::InvalidReg() : rs_fr11; break;
    case kFArg12: res_reg = kArm32QuickCodeUseSoftFloat ? RegStorage::InvalidReg() : rs_fr12; break;
    case kFArg13: res_reg = kArm32QuickCodeUseSoftFloat ? RegStorage::InvalidReg() : rs_fr13; break;
    case kFArg14: res_reg = kArm32QuickCodeUseSoftFloat ? RegStorage::InvalidReg() : rs_fr14; break;
    case kFArg15: res_reg = kArm32QuickCodeUseSoftFloat ? RegStorage::InvalidReg() : rs_fr15; break;
    case kRet0: res_reg = rs_r0; break;
    case kRet1: res_reg = rs_r1; break;
    case kInvokeTgt: res_reg = rs_rARM_LR; break;
    case kHiddenArg: res_reg = rs_r12; break;
    case kHiddenFpArg: res_reg = RegStorage::InvalidReg(); break;
    case kCount: res_reg = RegStorage::InvalidReg(); break;
    default: res_reg = RegStorage::InvalidReg();
  }
  return res_reg;
}

/*
 * Decode the register id.
 */
ResourceMask ArmMir2Lir::GetRegMaskCommon(const RegStorage& reg) const {
  return GetRegMaskArm(reg);
}

constexpr ResourceMask ArmMir2Lir::GetRegMaskArm(RegStorage reg) {
  return reg.IsDouble()
      /* Each double register is equal to a pair of single-precision FP registers */
      ? ResourceMask::TwoBits(reg.GetRegNum() * 2 + kArmFPReg0)
      : ResourceMask::Bit(reg.IsSingle() ? reg.GetRegNum() + kArmFPReg0 : reg.GetRegNum());
}

constexpr ResourceMask ArmMir2Lir::EncodeArmRegList(int reg_list) {
  return ResourceMask::RawMask(static_cast<uint64_t >(reg_list), 0u);
}

constexpr ResourceMask ArmMir2Lir::EncodeArmRegFpcsList(int reg_list) {
  return ResourceMask::RawMask(static_cast<uint64_t >(reg_list) << kArmFPReg16, 0u);
}

ResourceMask ArmMir2Lir::GetPCUseDefEncoding() const {
  return ResourceMask::Bit(kArmRegPC);
}

// Thumb2 specific setup.  TODO: inline?:
void ArmMir2Lir::SetupTargetResourceMasks(LIR* lir, uint64_t flags,
                                          ResourceMask* use_mask, ResourceMask* def_mask) {
  DCHECK_EQ(cu_->instruction_set, kThumb2);
  DCHECK(!lir->flags.use_def_invalid);

  int opcode = lir->opcode;

  // These flags are somewhat uncommon - bypass if we can.
  if ((flags & (REG_DEF_SP | REG_USE_SP | REG_DEF_LIST0 | REG_DEF_LIST1 |
                REG_DEF_FPCS_LIST0 | REG_DEF_FPCS_LIST2 | REG_USE_PC | IS_IT | REG_USE_LIST0 |
                REG_USE_LIST1 | REG_USE_FPCS_LIST0 | REG_USE_FPCS_LIST2 | REG_DEF_LR)) != 0) {
    if (flags & REG_DEF_SP) {
      def_mask->SetBit(kArmRegSP);
    }

    if (flags & REG_USE_SP) {
      use_mask->SetBit(kArmRegSP);
    }

    if (flags & REG_DEF_LIST0) {
      def_mask->SetBits(EncodeArmRegList(lir->operands[0]));
    }

    if (flags & REG_DEF_LIST1) {
      def_mask->SetBits(EncodeArmRegList(lir->operands[1]));
    }

    if (flags & REG_DEF_FPCS_LIST0) {
      def_mask->SetBits(EncodeArmRegList(lir->operands[0]));
    }

    if (flags & REG_DEF_FPCS_LIST2) {
      for (int i = 0; i < lir->operands[2]; i++) {
        SetupRegMask(def_mask, lir->operands[1] + i);
      }
    }

    if (flags & REG_USE_PC) {
      use_mask->SetBit(kArmRegPC);
    }

    /* Conservatively treat the IT block */
    if (flags & IS_IT) {
      *def_mask = kEncodeAll;
    }

    if (flags & REG_USE_LIST0) {
      use_mask->SetBits(EncodeArmRegList(lir->operands[0]));
    }

    if (flags & REG_USE_LIST1) {
      use_mask->SetBits(EncodeArmRegList(lir->operands[1]));
    }

    if (flags & REG_USE_FPCS_LIST0) {
      use_mask->SetBits(EncodeArmRegList(lir->operands[0]));
    }

    if (flags & REG_USE_FPCS_LIST2) {
      for (int i = 0; i < lir->operands[2]; i++) {
        SetupRegMask(use_mask, lir->operands[1] + i);
      }
    }
    /* Fixup for kThumbPush/lr and kThumbPop/pc */
    if (opcode == kThumbPush || opcode == kThumbPop) {
      constexpr ResourceMask r8Mask = GetRegMaskArm(rs_r8);
      if ((opcode == kThumbPush) && (use_mask->Intersects(r8Mask))) {
        use_mask->ClearBits(r8Mask);
        use_mask->SetBit(kArmRegLR);
      } else if ((opcode == kThumbPop) && (def_mask->Intersects(r8Mask))) {
        def_mask->ClearBits(r8Mask);
        def_mask->SetBit(kArmRegPC);;
      }
    }
    if (flags & REG_DEF_LR) {
      def_mask->SetBit(kArmRegLR);
    }
  }
}

ArmConditionCode ArmMir2Lir::ArmConditionEncoding(ConditionCode ccode) {
  ArmConditionCode res;
  switch (ccode) {
    case kCondEq: res = kArmCondEq; break;
    case kCondNe: res = kArmCondNe; break;
    case kCondCs: res = kArmCondCs; break;
    case kCondCc: res = kArmCondCc; break;
    case kCondUlt: res = kArmCondCc; break;
    case kCondUge: res = kArmCondCs; break;
    case kCondMi: res = kArmCondMi; break;
    case kCondPl: res = kArmCondPl; break;
    case kCondVs: res = kArmCondVs; break;
    case kCondVc: res = kArmCondVc; break;
    case kCondHi: res = kArmCondHi; break;
    case kCondLs: res = kArmCondLs; break;
    case kCondGe: res = kArmCondGe; break;
    case kCondLt: res = kArmCondLt; break;
    case kCondGt: res = kArmCondGt; break;
    case kCondLe: res = kArmCondLe; break;
    case kCondAl: res = kArmCondAl; break;
    case kCondNv: res = kArmCondNv; break;
    default:
      LOG(FATAL) << "Bad condition code " << ccode;
      res = static_cast<ArmConditionCode>(0);  // Quiet gcc
  }
  return res;
}

static const char* core_reg_names[16] = {
  "r0",
  "r1",
  "r2",
  "r3",
  "r4",
  "r5",
  "r6",
  "r7",
  "r8",
  "rSELF",
  "r10",
  "r11",
  "r12",
  "sp",
  "lr",
  "pc",
};


static const char* shift_names[4] = {
  "lsl",
  "lsr",
  "asr",
  "ror"};

/* Decode and print a ARM register name */
static char* DecodeRegList(int opcode, int vector, char* buf, size_t buf_size) {
  int i;
  bool printed = false;
  buf[0] = 0;
  for (i = 0; i < 16; i++, vector >>= 1) {
    if (vector & 0x1) {
      int reg_id = i;
      if (opcode == kThumbPush && i == 8) {
        reg_id = rs_rARM_LR.GetRegNum();
      } else if (opcode == kThumbPop && i == 8) {
        reg_id = rs_rARM_PC.GetRegNum();
      }
      if (printed) {
        snprintf(buf + strlen(buf), buf_size - strlen(buf), ", r%d", reg_id);
      } else {
        printed = true;
        snprintf(buf, buf_size, "r%d", reg_id);
      }
    }
  }
  return buf;
}

static char*  DecodeFPCSRegList(int count, int base, char* buf, size_t buf_size) {
  snprintf(buf, buf_size, "s%d", base);
  for (int i = 1; i < count; i++) {
    snprintf(buf + strlen(buf), buf_size - strlen(buf), ", s%d", base + i);
  }
  return buf;
}

static int32_t ExpandImmediate(int value) {
  int32_t mode = (value & 0xf00) >> 8;
  uint32_t bits = value & 0xff;
  switch (mode) {
    case 0:
      return bits;
     case 1:
      return (bits << 16) | bits;
     case 2:
      return (bits << 24) | (bits << 8);
     case 3:
      return (bits << 24) | (bits << 16) | (bits << 8) | bits;
    default:
      break;
  }
  bits = (bits | 0x80) << 24;
  return bits >> (((value & 0xf80) >> 7) - 8);
}

const char* cc_names[] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                         "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};
/*
 * Interpret a format string and build a string no longer than size
 * See format key in Assemble.c.
 */
std::string ArmMir2Lir::BuildInsnString(const char* fmt, LIR* lir, unsigned char* base_addr) {
  std::string buf;
  int i;
  const char* fmt_end = &fmt[strlen(fmt)];
  char tbuf[256];
  const char* name;
  char nc;
  while (fmt < fmt_end) {
    int operand;
    if (*fmt == '!') {
      fmt++;
      DCHECK_LT(fmt, fmt_end);
      nc = *fmt++;
      if (nc == '!') {
        strcpy(tbuf, "!");
      } else {
         DCHECK_LT(fmt, fmt_end);
         DCHECK_LT(static_cast<unsigned>(nc-'0'), 4U);
         operand = lir->operands[nc-'0'];
         switch (*fmt++) {
           case 'H':
             if (operand != 0) {
               snprintf(tbuf, arraysize(tbuf), ", %s %d", shift_names[operand & 0x3], operand >> 2);
             } else {
               strcpy(tbuf, "");
             }
             break;
           case 'B':
             switch (operand) {
               case kSY:
                 name = "sy";
                 break;
               case kST:
                 name = "st";
                 break;
               case kISH:
                 name = "ish";
                 break;
               case kISHST:
                 name = "ishst";
                 break;
               case kNSH:
                 name = "nsh";
                 break;
               case kNSHST:
                 name = "shst";
                 break;
               default:
                 name = "DecodeError2";
                 break;
             }
             strcpy(tbuf, name);
             break;
           case 'b':
             strcpy(tbuf, "0000");
             for (i = 3; i >= 0; i--) {
               tbuf[i] += operand & 1;
               operand >>= 1;
             }
             break;
           case 'n':
             operand = ~ExpandImmediate(operand);
             snprintf(tbuf, arraysize(tbuf), "%d [%#x]", operand, operand);
             break;
           case 'm':
             operand = ExpandImmediate(operand);
             snprintf(tbuf, arraysize(tbuf), "%d [%#x]", operand, operand);
             break;
           case 's':
             snprintf(tbuf, arraysize(tbuf), "s%d", RegStorage::RegNum(operand));
             break;
           case 'S':
             snprintf(tbuf, arraysize(tbuf), "d%d", RegStorage::RegNum(operand));
             break;
           case 'h':
             snprintf(tbuf, arraysize(tbuf), "%04x", operand);
             break;
           case 'M':
           case 'd':
             snprintf(tbuf, arraysize(tbuf), "%d", operand);
             break;
           case 'C':
             operand = RegStorage::RegNum(operand);
             DCHECK_LT(operand, static_cast<int>(
                 sizeof(core_reg_names)/sizeof(core_reg_names[0])));
             snprintf(tbuf, arraysize(tbuf), "%s", core_reg_names[operand]);
             break;
           case 'E':
             snprintf(tbuf, arraysize(tbuf), "%d", operand*4);
             break;
           case 'F':
             snprintf(tbuf, arraysize(tbuf), "%d", operand*2);
             break;
           case 'c':
             strcpy(tbuf, cc_names[operand]);
             break;
           case 't':
             snprintf(tbuf, arraysize(tbuf), "0x%08" PRIxPTR " (L%p)",
                 reinterpret_cast<uintptr_t>(base_addr) + lir->offset + 4 + (operand << 1),
                 lir->target);
             break;
           case 'T':
             snprintf(tbuf, arraysize(tbuf), "%s", PrettyMethod(
                 static_cast<uint32_t>(lir->operands[1]),
                 *reinterpret_cast<const DexFile*>(UnwrapPointer(lir->operands[2]))).c_str());
             break;
           case 'u': {
             int offset_1 = lir->operands[0];
             int offset_2 = NEXT_LIR(lir)->operands[0];
             uintptr_t target =
                 (((reinterpret_cast<uintptr_t>(base_addr) + lir->offset + 4) &
                 ~3) + (offset_1 << 21 >> 9) + (offset_2 << 1)) &
                 0xfffffffc;
             snprintf(tbuf, arraysize(tbuf), "%p", reinterpret_cast<void *>(target));
             break;
          }

           /* Nothing to print for BLX_2 */
           case 'v':
             strcpy(tbuf, "see above");
             break;
           case 'R':
             DecodeRegList(lir->opcode, operand, tbuf, arraysize(tbuf));
             break;
           case 'P':
             DecodeFPCSRegList(operand, 16, tbuf, arraysize(tbuf));
             break;
           case 'Q':
             DecodeFPCSRegList(operand, 0, tbuf, arraysize(tbuf));
             break;
           default:
             strcpy(tbuf, "DecodeError1");
             break;
        }
        buf += tbuf;
      }
    } else {
       buf += *fmt++;
    }
  }
  return buf;
}

void ArmMir2Lir::DumpResourceMask(LIR* arm_lir, const ResourceMask& mask, const char* prefix) {
  char buf[256];
  buf[0] = 0;

  if (mask.Equals(kEncodeAll)) {
    strcpy(buf, "all");
  } else {
    char num[8];
    int i;

    for (i = 0; i < kArmRegEnd; i++) {
      if (mask.HasBit(i)) {
        snprintf(num, arraysize(num), "%d ", i);
        strcat(buf, num);
      }
    }

    if (mask.HasBit(ResourceMask::kCCode)) {
      strcat(buf, "cc ");
    }
    if (mask.HasBit(ResourceMask::kFPStatus)) {
      strcat(buf, "fpcc ");
    }

    /* Memory bits */
    if (arm_lir && (mask.HasBit(ResourceMask::kDalvikReg))) {
      snprintf(buf + strlen(buf), arraysize(buf) - strlen(buf), "dr%d%s",
               DECODE_ALIAS_INFO_REG(arm_lir->flags.alias_info),
               DECODE_ALIAS_INFO_WIDE(arm_lir->flags.alias_info) ? "(+1)" : "");
    }
    if (mask.HasBit(ResourceMask::kLiteral)) {
      strcat(buf, "lit ");
    }

    if (mask.HasBit(ResourceMask::kHeapRef)) {
      strcat(buf, "heap ");
    }
    if (mask.HasBit(ResourceMask::kMustNotAlias)) {
      strcat(buf, "noalias ");
    }
  }
  if (buf[0]) {
    LOG(INFO) << prefix << ": " << buf;
  }
}

bool ArmMir2Lir::IsUnconditionalBranch(LIR* lir) {
  return ((lir->opcode == kThumbBUncond) || (lir->opcode == kThumb2BUncond));
}

RegisterClass ArmMir2Lir::RegClassForFieldLoadStore(OpSize size, bool is_volatile) {
  if (UNLIKELY(is_volatile)) {
    // On arm, atomic 64-bit load/store requires a core register pair.
    // Smaller aligned load/store is atomic for both core and fp registers.
    if (size == k64 || size == kDouble) {
      return kCoreReg;
    }
  }
  return RegClassBySize(size);
}

ArmMir2Lir::ArmMir2Lir(CompilationUnit* cu, MIRGraph* mir_graph, ArenaAllocator* arena)
    : Mir2Lir(cu, mir_graph, arena),
      call_method_insns_(arena->Adapter()) {
  call_method_insns_.reserve(100);
  // Sanity check - make sure encoding map lines up.
  for (int i = 0; i < kArmLast; i++) {
    if (ArmMir2Lir::EncodingMap[i].opcode != i) {
      LOG(FATAL) << "Encoding order for " << ArmMir2Lir::EncodingMap[i].name
                 << " is wrong: expecting " << i << ", seeing "
                 << static_cast<int>(ArmMir2Lir::EncodingMap[i].opcode);
    }
  }
}

Mir2Lir* ArmCodeGenerator(CompilationUnit* const cu, MIRGraph* const mir_graph,
                          ArenaAllocator* const arena) {
  return new ArmMir2Lir(cu, mir_graph, arena);
}

void ArmMir2Lir::CompilerInitializeRegAlloc() {
  reg_pool_.reset(new (arena_) RegisterPool(this, arena_, core_regs, empty_pool /* core64 */,
                                            sp_regs, dp_regs,
                                            reserved_regs, empty_pool /* reserved64 */,
                                            core_temps, empty_pool /* core64_temps */,
                                            sp_temps, dp_temps));

  // Target-specific adjustments.

  // Alias single precision floats to appropriate half of overlapping double.
  for (RegisterInfo* info : reg_pool_->sp_regs_) {
    int sp_reg_num = info->GetReg().GetRegNum();
    int dp_reg_num = sp_reg_num >> 1;
    RegStorage dp_reg = RegStorage::Solo64(RegStorage::kFloatingPoint | dp_reg_num);
    RegisterInfo* dp_reg_info = GetRegInfo(dp_reg);
    // Double precision register's master storage should refer to itself.
    DCHECK_EQ(dp_reg_info, dp_reg_info->Master());
    // Redirect single precision's master storage to master.
    info->SetMaster(dp_reg_info);
    // Singles should show a single 32-bit mask bit, at first referring to the low half.
    DCHECK_EQ(info->StorageMask(), RegisterInfo::kLowSingleStorageMask);
    if (sp_reg_num & 1) {
      // For odd singles, change to use the high word of the backing double.
      info->SetStorageMask(RegisterInfo::kHighSingleStorageMask);
    }
  }

#ifdef ARM_R4_SUSPEND_FLAG
  // TODO: re-enable this when we can safely save r4 over the suspension code path.
  bool no_suspend = NO_SUSPEND;  // || !Runtime::Current()->ExplicitSuspendChecks();
  if (no_suspend) {
    GetRegInfo(rs_rARM_SUSPEND)->MarkFree();
  }
#endif

  // Don't start allocating temps at r0/s0/d0 or you may clobber return regs in early-exit methods.
  // TODO: adjust when we roll to hard float calling convention.
  reg_pool_->next_core_reg_ = 2;
  reg_pool_->next_sp_reg_ = 0;
  reg_pool_->next_dp_reg_ = 0;
}

/*
 * TUNING: is true leaf?  Can't just use METHOD_IS_LEAF to determine as some
 * instructions might call out to C/assembly helper functions.  Until
 * machinery is in place, always spill lr.
 */

void ArmMir2Lir::AdjustSpillMask() {
  core_spill_mask_ |= (1 << rs_rARM_LR.GetRegNum());
  num_core_spills_++;
}

/*
 * Mark a callee-save fp register as promoted.  Note that
 * vpush/vpop uses contiguous register lists so we must
 * include any holes in the mask.  Associate holes with
 * Dalvik register INVALID_VREG (0xFFFFU).
 */
void ArmMir2Lir::MarkPreservedSingle(int v_reg, RegStorage reg) {
  DCHECK_GE(reg.GetRegNum(), ARM_FP_CALLEE_SAVE_BASE);
  int adjusted_reg_num = reg.GetRegNum() - ARM_FP_CALLEE_SAVE_BASE;
  // Ensure fp_vmap_table is large enough
  int table_size = fp_vmap_table_.size();
  for (int i = table_size; i < (adjusted_reg_num + 1); i++) {
    fp_vmap_table_.push_back(INVALID_VREG);
  }
  // Add the current mapping
  fp_vmap_table_[adjusted_reg_num] = v_reg;
  // Size of fp_vmap_table is high-water mark, use to set mask
  num_fp_spills_ = fp_vmap_table_.size();
  fp_spill_mask_ = ((1 << num_fp_spills_) - 1) << ARM_FP_CALLEE_SAVE_BASE;
}

void ArmMir2Lir::MarkPreservedDouble(int v_reg, RegStorage reg) {
  // TEMP: perform as 2 singles.
  int reg_num = reg.GetRegNum() << 1;
  RegStorage lo = RegStorage::Solo32(RegStorage::kFloatingPoint | reg_num);
  RegStorage hi = RegStorage::Solo32(RegStorage::kFloatingPoint | reg_num | 1);
  MarkPreservedSingle(v_reg, lo);
  MarkPreservedSingle(v_reg + 1, hi);
}

/* Clobber all regs that might be used by an external C call */
void ArmMir2Lir::ClobberCallerSave() {
  // TODO: rework this - it's gotten even more ugly.
  Clobber(rs_r0);
  Clobber(rs_r1);
  Clobber(rs_r2);
  Clobber(rs_r3);
  Clobber(rs_r12);
  Clobber(rs_r14lr);
  Clobber(rs_fr0);
  Clobber(rs_fr1);
  Clobber(rs_fr2);
  Clobber(rs_fr3);
  Clobber(rs_fr4);
  Clobber(rs_fr5);
  Clobber(rs_fr6);
  Clobber(rs_fr7);
  Clobber(rs_fr8);
  Clobber(rs_fr9);
  Clobber(rs_fr10);
  Clobber(rs_fr11);
  Clobber(rs_fr12);
  Clobber(rs_fr13);
  Clobber(rs_fr14);
  Clobber(rs_fr15);
  Clobber(rs_dr0);
  Clobber(rs_dr1);
  Clobber(rs_dr2);
  Clobber(rs_dr3);
  Clobber(rs_dr4);
  Clobber(rs_dr5);
  Clobber(rs_dr6);
  Clobber(rs_dr7);
}

RegLocation ArmMir2Lir::GetReturnWideAlt() {
  RegLocation res = LocCReturnWide();
  res.reg.SetLowReg(rs_r2.GetReg());
  res.reg.SetHighReg(rs_r3.GetReg());
  Clobber(rs_r2);
  Clobber(rs_r3);
  MarkInUse(rs_r2);
  MarkInUse(rs_r3);
  MarkWide(res.reg);
  return res;
}

RegLocation ArmMir2Lir::GetReturnAlt() {
  RegLocation res = LocCReturn();
  res.reg.SetReg(rs_r1.GetReg());
  Clobber(rs_r1);
  MarkInUse(rs_r1);
  return res;
}

/* To be used when explicitly managing register use */
void ArmMir2Lir::LockCallTemps() {
  LockTemp(rs_r0);
  LockTemp(rs_r1);
  LockTemp(rs_r2);
  LockTemp(rs_r3);
  if (!kArm32QuickCodeUseSoftFloat) {
    LockTemp(rs_fr0);
    LockTemp(rs_fr1);
    LockTemp(rs_fr2);
    LockTemp(rs_fr3);
    LockTemp(rs_fr4);
    LockTemp(rs_fr5);
    LockTemp(rs_fr6);
    LockTemp(rs_fr7);
    LockTemp(rs_fr8);
    LockTemp(rs_fr9);
    LockTemp(rs_fr10);
    LockTemp(rs_fr11);
    LockTemp(rs_fr12);
    LockTemp(rs_fr13);
    LockTemp(rs_fr14);
    LockTemp(rs_fr15);
    LockTemp(rs_dr0);
    LockTemp(rs_dr1);
    LockTemp(rs_dr2);
    LockTemp(rs_dr3);
    LockTemp(rs_dr4);
    LockTemp(rs_dr5);
    LockTemp(rs_dr6);
    LockTemp(rs_dr7);
  }
}

/* To be used when explicitly managing register use */
void ArmMir2Lir::FreeCallTemps() {
  FreeTemp(rs_r0);
  FreeTemp(rs_r1);
  FreeTemp(rs_r2);
  FreeTemp(rs_r3);
  if (!kArm32QuickCodeUseSoftFloat) {
    FreeTemp(rs_fr0);
    FreeTemp(rs_fr1);
    FreeTemp(rs_fr2);
    FreeTemp(rs_fr3);
    FreeTemp(rs_fr4);
    FreeTemp(rs_fr5);
    FreeTemp(rs_fr6);
    FreeTemp(rs_fr7);
    FreeTemp(rs_fr8);
    FreeTemp(rs_fr9);
    FreeTemp(rs_fr10);
    FreeTemp(rs_fr11);
    FreeTemp(rs_fr12);
    FreeTemp(rs_fr13);
    FreeTemp(rs_fr14);
    FreeTemp(rs_fr15);
    FreeTemp(rs_dr0);
    FreeTemp(rs_dr1);
    FreeTemp(rs_dr2);
    FreeTemp(rs_dr3);
    FreeTemp(rs_dr4);
    FreeTemp(rs_dr5);
    FreeTemp(rs_dr6);
    FreeTemp(rs_dr7);
  }
}

RegStorage ArmMir2Lir::LoadHelper(QuickEntrypointEnum trampoline) {
  LoadWordDisp(rs_rARM_SELF, GetThreadOffset<4>(trampoline).Int32Value(), rs_rARM_LR);
  return rs_rARM_LR;
}

LIR* ArmMir2Lir::CheckSuspendUsingLoad() {
  RegStorage tmp = rs_r0;
  Load32Disp(rs_rARM_SELF, Thread::ThreadSuspendTriggerOffset<4>().Int32Value(), tmp);
  LIR* load2 = Load32Disp(tmp, 0, tmp);
  return load2;
}

uint64_t ArmMir2Lir::GetTargetInstFlags(int opcode) {
  DCHECK(!IsPseudoLirOp(opcode));
  return ArmMir2Lir::EncodingMap[opcode].flags;
}

const char* ArmMir2Lir::GetTargetInstName(int opcode) {
  DCHECK(!IsPseudoLirOp(opcode));
  return ArmMir2Lir::EncodingMap[opcode].name;
}

const char* ArmMir2Lir::GetTargetInstFmt(int opcode) {
  DCHECK(!IsPseudoLirOp(opcode));
  return ArmMir2Lir::EncodingMap[opcode].fmt;
}

/*
 * Somewhat messy code here.  We want to allocate a pair of contiguous
 * physical single-precision floating point registers starting with
 * an even numbered reg.  It is possible that the paired s_reg (s_reg+1)
 * has already been allocated - try to fit if possible.  Fail to
 * allocate if we can't meet the requirements for the pair of
 * s_reg<=sX[even] & (s_reg+1)<= sX+1.
 */
// TODO: needs rewrite to support non-backed 64-bit float regs.
RegStorage ArmMir2Lir::AllocPreservedDouble(int s_reg) {
  RegStorage res;
  int v_reg = mir_graph_->SRegToVReg(s_reg);
  int p_map_idx = SRegToPMap(s_reg);
  if (promotion_map_[p_map_idx+1].fp_location == kLocPhysReg) {
    // Upper reg is already allocated.  Can we fit?
    int high_reg = promotion_map_[p_map_idx+1].fp_reg;
    if ((high_reg & 1) == 0) {
      // High reg is even - fail.
      return res;  // Invalid.
    }
    // Is the low reg of the pair free?
    // FIXME: rework.
    RegisterInfo* p = GetRegInfo(RegStorage::FloatSolo32(high_reg - 1));
    if (p->InUse() || p->IsTemp()) {
      // Already allocated or not preserved - fail.
      return res;  // Invalid.
    }
    // OK - good to go.
    res = RegStorage::FloatSolo64(p->GetReg().GetRegNum() >> 1);
    p->MarkInUse();
    MarkPreservedSingle(v_reg, p->GetReg());
  } else {
    /*
     * TODO: until runtime support is in, make sure we avoid promoting the same vreg to
     * different underlying physical registers.
     */
    for (RegisterInfo* info : reg_pool_->dp_regs_) {
      if (!info->IsTemp() && !info->InUse()) {
        res = info->GetReg();
        info->MarkInUse();
        MarkPreservedDouble(v_reg, info->GetReg());
        break;
      }
    }
  }
  if (res.Valid()) {
    RegisterInfo* info = GetRegInfo(res);
    promotion_map_[p_map_idx].fp_location = kLocPhysReg;
    promotion_map_[p_map_idx].fp_reg =
        info->FindMatchingView(RegisterInfo::kLowSingleStorageMask)->GetReg().GetReg();
    promotion_map_[p_map_idx+1].fp_location = kLocPhysReg;
    promotion_map_[p_map_idx+1].fp_reg =
        info->FindMatchingView(RegisterInfo::kHighSingleStorageMask)->GetReg().GetReg();
  }
  return res;
}

// Reserve a callee-save sp single register.
RegStorage ArmMir2Lir::AllocPreservedSingle(int s_reg) {
  RegStorage res;
  for (RegisterInfo* info : reg_pool_->sp_regs_) {
    if (!info->IsTemp() && !info->InUse()) {
      res = info->GetReg();
      int p_map_idx = SRegToPMap(s_reg);
      int v_reg = mir_graph_->SRegToVReg(s_reg);
      GetRegInfo(res)->MarkInUse();
      MarkPreservedSingle(v_reg, res);
      promotion_map_[p_map_idx].fp_location = kLocPhysReg;
      promotion_map_[p_map_idx].fp_reg = res.GetReg();
      break;
    }
  }
  return res;
}

void ArmMir2Lir::InstallLiteralPools() {
  // PC-relative calls to methods.
  patches_.reserve(call_method_insns_.size());
  for (LIR* p : call_method_insns_) {
      DCHECK_EQ(p->opcode, kThumb2Bl);
      uint32_t target_method_idx = p->operands[1];
      const DexFile* target_dex_file =
          reinterpret_cast<const DexFile*>(UnwrapPointer(p->operands[2]));

      patches_.push_back(LinkerPatch::RelativeCodePatch(p->offset,
                                                        target_dex_file, target_method_idx));
  }

  // And do the normal processing.
  Mir2Lir::InstallLiteralPools();
}

RegStorage ArmMir2Lir::InToRegStorageArmMapper::GetNextReg(bool is_double_or_float, bool is_wide) {
  const RegStorage coreArgMappingToPhysicalReg[] =
      {rs_r1, rs_r2, rs_r3};
  const int coreArgMappingToPhysicalRegSize = arraysize(coreArgMappingToPhysicalReg);
  const RegStorage fpArgMappingToPhysicalReg[] =
      {rs_fr0, rs_fr1, rs_fr2, rs_fr3, rs_fr4, rs_fr5, rs_fr6, rs_fr7,
       rs_fr8, rs_fr9, rs_fr10, rs_fr11, rs_fr12, rs_fr13, rs_fr14, rs_fr15};
  const uint32_t fpArgMappingToPhysicalRegSize = arraysize(fpArgMappingToPhysicalReg);
  COMPILE_ASSERT(fpArgMappingToPhysicalRegSize % 2 == 0, knum_of_fp_arg_regs_not_even);

  if (kArm32QuickCodeUseSoftFloat) {
    is_double_or_float = false;  // Regard double as long, float as int.
    is_wide = false;  // Map long separately.
  }

  RegStorage result = RegStorage::InvalidReg();
  if (is_double_or_float) {
    // TODO: Remove "cur_fp_double_reg_ % 2 != 0" when we return double as double.
    if (is_wide || cur_fp_double_reg_ % 2 != 0) {
      cur_fp_double_reg_ = std::max(cur_fp_double_reg_, RoundUp(cur_fp_reg_, 2));
      if (cur_fp_double_reg_ < fpArgMappingToPhysicalRegSize) {
        // TODO: Replace by following code in the branch when FlushIns() support 64-bit registers.
        // result = RegStorage::MakeRegPair(fpArgMappingToPhysicalReg[cur_fp_double_reg_],
        //                                  fpArgMappingToPhysicalReg[cur_fp_double_reg_ + 1]);
        // result = As64BitFloatReg(result);
        // cur_fp_double_reg_ += 2;
        result = fpArgMappingToPhysicalReg[cur_fp_double_reg_];
        cur_fp_double_reg_++;
      }
    } else {
      // TODO: Remove the check when we return double as double.
      DCHECK_EQ(cur_fp_double_reg_ % 2, 0U);
      if (cur_fp_reg_ % 2 == 0) {
        cur_fp_reg_ = std::max(cur_fp_double_reg_, cur_fp_reg_);
      }
      if (cur_fp_reg_ < fpArgMappingToPhysicalRegSize) {
        result = fpArgMappingToPhysicalReg[cur_fp_reg_];
        cur_fp_reg_++;
      }
    }
  } else {
    if (cur_core_reg_ < coreArgMappingToPhysicalRegSize) {
      result = coreArgMappingToPhysicalReg[cur_core_reg_++];
      // TODO: Enable following code when FlushIns() support 64-bit registers.
      // if (is_wide && cur_core_reg_ < coreArgMappingToPhysicalRegSize) {
      //   result = RegStorage::MakeRegPair(result, coreArgMappingToPhysicalReg[cur_core_reg_++]);
      // }
    }
  }
  return result;
}

RegStorage ArmMir2Lir::InToRegStorageMapping::Get(int in_position) const {
  DCHECK(IsInitialized());
  auto res = mapping_.find(in_position);
  return res != mapping_.end() ? res->second : RegStorage::InvalidReg();
}

void ArmMir2Lir::InToRegStorageMapping::Initialize(RegLocation* arg_locs, int count,
                                                   InToRegStorageMapper* mapper) {
  DCHECK(mapper != nullptr);
  max_mapped_in_ = -1;
  is_there_stack_mapped_ = false;
  for (int in_position = 0; in_position < count; in_position++) {
     RegStorage reg = mapper->GetNextReg(arg_locs[in_position].fp,
                                         arg_locs[in_position].wide);
     if (reg.Valid()) {
       mapping_[in_position] = reg;
       // TODO: Enable the following code when FlushIns() support 64-bit argument registers.
       // if (arg_locs[in_position].wide) {
       //  if (reg.Is32Bit()) {
       //    // As it is a split long, the hi-part is on stack.
       //    is_there_stack_mapped_ = true;
       //  }
       //  // We covered 2 v-registers, so skip the next one
       //  in_position++;
       // }
       max_mapped_in_ = std::max(max_mapped_in_, in_position);
     } else {
       is_there_stack_mapped_ = true;
     }
  }
  initialized_ = true;
}

// TODO: Should be able to return long, double registers.
// Need check some common code as it will break some assumption.
RegStorage ArmMir2Lir::GetArgMappingToPhysicalReg(int arg_num) {
  if (!in_to_reg_storage_mapping_.IsInitialized()) {
    int start_vreg = mir_graph_->GetFirstInVR();
    RegLocation* arg_locs = &mir_graph_->reg_location_[start_vreg];

    InToRegStorageArmMapper mapper;
    in_to_reg_storage_mapping_.Initialize(arg_locs, mir_graph_->GetNumOfInVRs(), &mapper);
  }
  return in_to_reg_storage_mapping_.Get(arg_num);
}

int ArmMir2Lir::GenDalvikArgsNoRange(CallInfo* info,
                                     int call_state, LIR** pcrLabel, NextCallInsn next_call_insn,
                                     const MethodReference& target_method,
                                     uint32_t vtable_idx, uintptr_t direct_code,
                                     uintptr_t direct_method, InvokeType type, bool skip_this) {
  if (kArm32QuickCodeUseSoftFloat) {
    return Mir2Lir::GenDalvikArgsNoRange(info, call_state, pcrLabel, next_call_insn, target_method,
                                         vtable_idx, direct_code, direct_method, type, skip_this);
  } else {
    return GenDalvikArgsRange(info, call_state, pcrLabel, next_call_insn, target_method, vtable_idx,
                              direct_code, direct_method, type, skip_this);
  }
}

int ArmMir2Lir::GenDalvikArgsRange(CallInfo* info, int call_state,
                                   LIR** pcrLabel, NextCallInsn next_call_insn,
                                   const MethodReference& target_method,
                                   uint32_t vtable_idx, uintptr_t direct_code,
                                   uintptr_t direct_method, InvokeType type, bool skip_this) {
  if (kArm32QuickCodeUseSoftFloat) {
    return Mir2Lir::GenDalvikArgsRange(info, call_state, pcrLabel, next_call_insn, target_method,
                                       vtable_idx, direct_code, direct_method, type, skip_this);
  }

  // TODO: Rework the implementation when argument register can be long or double.

  /* If no arguments, just return */
  if (info->num_arg_words == 0) {
    return call_state;
  }

  const int start_index = skip_this ? 1 : 0;

  InToRegStorageArmMapper mapper;
  InToRegStorageMapping in_to_reg_storage_mapping;
  in_to_reg_storage_mapping.Initialize(info->args, info->num_arg_words, &mapper);
  const int last_mapped_in = in_to_reg_storage_mapping.GetMaxMappedIn();
  int regs_left_to_pass_via_stack = info->num_arg_words - (last_mapped_in + 1);

  // First of all, check whether it makes sense to use bulk copying.
  // Bulk copying is done only for the range case.
  // TODO: make a constant instead of 2
  if (info->is_range && regs_left_to_pass_via_stack >= 2) {
    // Scan the rest of the args - if in phys_reg flush to memory
    for (int next_arg = last_mapped_in + 1; next_arg < info->num_arg_words;) {
      RegLocation loc = info->args[next_arg];
      if (loc.wide) {
        // TODO: Only flush hi-part.
        if (loc.high_word) {
          loc = info->args[--next_arg];
        }
        loc = UpdateLocWide(loc);
        if (loc.location == kLocPhysReg) {
          ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
          StoreBaseDisp(TargetPtrReg(kSp), SRegOffset(loc.s_reg_low), loc.reg, k64, kNotVolatile);
        }
        next_arg += 2;
      } else {
        loc = UpdateLoc(loc);
        if (loc.location == kLocPhysReg) {
          ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
          if (loc.ref) {
            StoreRefDisp(TargetPtrReg(kSp), SRegOffset(loc.s_reg_low), loc.reg, kNotVolatile);
          } else {
            StoreBaseDisp(TargetPtrReg(kSp), SRegOffset(loc.s_reg_low), loc.reg, k32,
                          kNotVolatile);
          }
        }
        next_arg++;
      }
    }

    // The rest can be copied together
    int start_offset = SRegOffset(info->args[last_mapped_in + 1].s_reg_low);
    int outs_offset = StackVisitor::GetOutVROffset(last_mapped_in + 1,
                                                   cu_->instruction_set);

    int current_src_offset = start_offset;
    int current_dest_offset = outs_offset;

    // Only davik regs are accessed in this loop; no next_call_insn() calls.
    ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
    while (regs_left_to_pass_via_stack > 0) {
      /*
       * TODO: Improve by adding block copy for large number of arguments.  This
       * should be done, if possible, as a target-depending helper.  For now, just
       * copy a Dalvik vreg at a time.
       */
      // Moving 32-bits via general purpose register.
      size_t bytes_to_move = sizeof(uint32_t);

      // Instead of allocating a new temp, simply reuse one of the registers being used
      // for argument passing.
      RegStorage temp = TargetReg(kArg3, kNotWide);

      // Now load the argument VR and store to the outs.
      Load32Disp(TargetPtrReg(kSp), current_src_offset, temp);
      Store32Disp(TargetPtrReg(kSp), current_dest_offset, temp);

      current_src_offset += bytes_to_move;
      current_dest_offset += bytes_to_move;
      regs_left_to_pass_via_stack -= (bytes_to_move >> 2);
    }
    DCHECK_EQ(regs_left_to_pass_via_stack, 0);
  }

  // Now handle rest not registers if they are
  if (in_to_reg_storage_mapping.IsThereStackMapped()) {
    RegStorage regWide = TargetReg(kArg2, kWide);
    for (int i = start_index; i <= last_mapped_in + regs_left_to_pass_via_stack; i++) {
      RegLocation rl_arg = info->args[i];
      rl_arg = UpdateRawLoc(rl_arg);
      RegStorage reg = in_to_reg_storage_mapping.Get(i);
      // TODO: Only pass split wide hi-part via stack.
      if (!reg.Valid() || rl_arg.wide) {
        int out_offset = StackVisitor::GetOutVROffset(i, cu_->instruction_set);

        {
          ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
          if (rl_arg.wide) {
            if (rl_arg.location == kLocPhysReg) {
              StoreBaseDisp(TargetPtrReg(kSp), out_offset, rl_arg.reg, k64, kNotVolatile);
            } else {
              LoadValueDirectWideFixed(rl_arg, regWide);
              StoreBaseDisp(TargetPtrReg(kSp), out_offset, regWide, k64, kNotVolatile);
            }
          } else {
            if (rl_arg.location == kLocPhysReg) {
              if (rl_arg.ref) {
                StoreRefDisp(TargetPtrReg(kSp), out_offset, rl_arg.reg, kNotVolatile);
              } else {
                StoreBaseDisp(TargetPtrReg(kSp), out_offset, rl_arg.reg, k32, kNotVolatile);
              }
            } else {
              if (rl_arg.ref) {
                RegStorage regSingle = TargetReg(kArg2, kRef);
                LoadValueDirectFixed(rl_arg, regSingle);
                StoreRefDisp(TargetPtrReg(kSp), out_offset, regSingle, kNotVolatile);
              } else {
                RegStorage regSingle = TargetReg(kArg2, kNotWide);
                LoadValueDirectFixed(rl_arg, regSingle);
                StoreBaseDisp(TargetPtrReg(kSp), out_offset, regSingle, k32, kNotVolatile);
              }
            }
          }
        }

        call_state = next_call_insn(cu_, info, call_state, target_method,
                                    vtable_idx, direct_code, direct_method, type);
      }
      if (rl_arg.wide) {
        i++;
      }
    }
  }

  // Finish with mapped registers
  for (int i = start_index; i <= last_mapped_in; i++) {
    RegLocation rl_arg = info->args[i];
    rl_arg = UpdateRawLoc(rl_arg);
    RegStorage reg = in_to_reg_storage_mapping.Get(i);
    if (reg.Valid()) {
      if (reg.Is64Bit()) {
        LoadValueDirectWideFixed(rl_arg, reg);
      } else {
        // TODO: Only split long should be the case we need to care about.
        if (rl_arg.wide) {
          ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
          int high_word = rl_arg.high_word ? 1 : 0;
          rl_arg = high_word ? info->args[i - 1] : rl_arg;
          if (rl_arg.location == kLocPhysReg) {
            RegStorage rs_arg = rl_arg.reg;
            if (rs_arg.IsDouble() && rs_arg.Is64BitSolo()) {
              rs_arg = As64BitFloatRegPair(rs_arg);
            }
            RegStorage rs_arg_low = rs_arg.GetLow();
            RegStorage rs_arg_high = rs_arg.GetHigh();
            OpRegCopy(reg, high_word ? rs_arg_high : rs_arg_low);
          } else {
            Load32Disp(TargetPtrReg(kSp), SRegOffset(rl_arg.s_reg_low + high_word), reg);
          }
        } else {
          LoadValueDirectFixed(rl_arg, reg);
        }
      }
      call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                                  direct_code, direct_method, type);
    }
    if (reg.Is64Bit()) {
      i++;
    }
  }

  call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                           direct_code, direct_method, type);
  if (pcrLabel) {
    if (!cu_->compiler_driver->GetCompilerOptions().GetImplicitNullChecks()) {
      *pcrLabel = GenExplicitNullCheck(TargetReg(kArg1, kRef), info->opt_flags);
    } else {
      *pcrLabel = nullptr;
      // In lieu of generating a check for kArg1 being null, we need to
      // perform a load when doing implicit checks.
      RegStorage tmp = AllocTemp();
      Load32Disp(TargetReg(kArg1, kRef), 0, tmp);
      MarkPossibleNullPointerException(info->opt_flags);
      FreeTemp(tmp);
    }
  }
  return call_state;
}

}  // namespace art
