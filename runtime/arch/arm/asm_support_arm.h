/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ART_RUNTIME_ARCH_ARM_ASM_SUPPORT_ARM_H_
#define ART_RUNTIME_ARCH_ARM_ASM_SUPPORT_ARM_H_

#include "asm_support.h"

#define FRAME_SIZE_SAVE_ALL_CALLEE_SAVE 112
#define FRAME_SIZE_REFS_ONLY_CALLEE_SAVE 32
#define FRAME_SIZE_REFS_AND_ARGS_CALLEE_SAVE 112

// Flag for enabling R4 optimization in arm runtime
#define ARM_R4_SUSPEND_FLAG

#endif  // ART_RUNTIME_ARCH_ARM_ASM_SUPPORT_ARM_H_
