/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_SRC_COMPILER_LLVM_RUNTIME_SUPPORT_BUILDER_H_
#define ART_SRC_COMPILER_LLVM_RUNTIME_SUPPORT_BUILDER_H_

#include "backend_types.h"
#include "logging.h"
#include "runtime_support_func.h"

#include <stdint.h>

namespace llvm {
  class LLVMContext;
  class Module;
  class Function;
  class Type;
  class Value;
}

namespace art {
namespace compiler_llvm {

class IRBuilder;


class RuntimeSupportBuilder {
 public:
  RuntimeSupportBuilder(llvm::LLVMContext& context, llvm::Module& module, IRBuilder& irb);

  /* Thread */
  virtual llvm::Value* EmitGetCurrentThread();
  virtual llvm::Value* EmitLoadFromThreadOffset(int64_t offset, llvm::Type* type,
                                                TBAASpecialType s_ty);
  virtual void EmitStoreToThreadOffset(int64_t offset, llvm::Value* value,
                                       TBAASpecialType s_ty);
  virtual void EmitSetCurrentThread(llvm::Value* thread);

  /* ShadowFrame */
  virtual llvm::Value* EmitPushShadowFrame(llvm::Value* new_shadow_frame,
                                       llvm::Value* method, uint32_t size);
  virtual llvm::Value* EmitPushShadowFrameNoInline(llvm::Value* new_shadow_frame,
                                               llvm::Value* method, uint32_t size);
  virtual void EmitPopShadowFrame(llvm::Value* old_shadow_frame);

  /* Check */
  virtual llvm::Value* EmitIsExceptionPending();
  virtual void EmitTestSuspend();

  /* Monitor */
  virtual void EmitLockObject(llvm::Value* object);
  virtual void EmitUnlockObject(llvm::Value* object);

  llvm::Function* GetRuntimeSupportFunction(runtime_support::RuntimeId id) {
    if (id >= 0 && id < runtime_support::MAX_ID) {
      return runtime_support_func_decls_[id];
    } else {
      LOG(ERROR) << "Unknown runtime function id: " << id;
      return NULL;
    }
  }

  void OptimizeRuntimeSupport();

  virtual ~RuntimeSupportBuilder() {}

 protected:
  // Mark a function as inline function.
  // You should implement the function, if mark as inline.
  void MakeFunctionInline(llvm::Function* function);

  void OverrideRuntimeSupportFunction(runtime_support::RuntimeId id, llvm::Function* function);


 protected:
  llvm::LLVMContext& context_;
  llvm::Module& module_;
  IRBuilder& irb_;

 private:
  llvm::Function* runtime_support_func_decls_[runtime_support::MAX_ID];
  bool target_runtime_support_func_[runtime_support::MAX_ID];
};


} // namespace compiler_llvm
} // namespace art

#endif // ART_SRC_COMPILER_LLVM_RUNTIME_SUPPORT_BUILDER_H_
