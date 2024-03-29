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

#ifndef ART_COMPILER_DRIVER_COMPILER_DRIVER_H_
#define ART_COMPILER_DRIVER_COMPILER_DRIVER_H_

#include <set>
#include <string>
#include <vector>

#include "base/mutex.h"
#include "base/timing_logger.h"
#include "class_reference.h"
#include "compiled_method.h"
#include "compiler.h"
#include "dex_file.h"
#include "driver/compiler_options.h"
#include "instruction_set.h"
#include "invoke_type.h"
#include "method_reference.h"
#include "mirror/class.h"  // For mirror::Class::Status.
#include "os.h"
#include "profiler.h"
#include "runtime.h"
#include "safe_map.h"
#include "thread_pool.h"
#include "utils/arena_allocator.h"
#include "utils/dedupe_set.h"

namespace art {

namespace verifier {
class MethodVerifier;
}  // namespace verifier

class CompiledClass;
class CompilerOptions;
class DexCompilationUnit;
class DexFileToMethodInlinerMap;
struct InlineIGetIPutData;
class OatWriter;
class ParallelCompilationManager;
class ScopedObjectAccess;
template<class T> class Handle;
class TimingLogger;
class VerificationResults;
class VerifiedMethod;

enum EntryPointCallingConvention {
  // ABI of invocations to a method's interpreter entry point.
  kInterpreterAbi,
  // ABI of calls to a method's native code, only used for native methods.
  kJniAbi,
  // ABI of calls to a method's portable code entry point.
  kPortableAbi,
  // ABI of calls to a method's quick code entry point.
  kQuickAbi
};

enum DexToDexCompilationLevel {
  kDontDexToDexCompile,   // Only meaning wrt image time interpretation.
  kRequired,              // Dex-to-dex compilation required for correctness.
  kOptimize               // Perform required transformation and peep-hole optimizations.
};

class CompilerDriver {
 public:
  // Create a compiler targeting the requested "instruction_set".
  // "image" should be true if image specific optimizations should be
  // enabled.  "image_classes" lets the compiler know what classes it
  // can assume will be in the image, with nullptr implying all available
  // classes.
  explicit CompilerDriver(const CompilerOptions* compiler_options,
                          VerificationResults* verification_results,
                          DexFileToMethodInlinerMap* method_inliner_map,
                          Compiler::Kind compiler_kind,
                          InstructionSet instruction_set,
                          const InstructionSetFeatures* instruction_set_features,
                          bool image, std::set<std::string>* image_classes,
                          size_t thread_count, bool dump_stats, bool dump_passes,
                          CumulativeLogger* timer, const std::string& profile_file);

  ~CompilerDriver();

  void CompileAll(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                  TimingLogger* timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Compile a single Method.
  void CompileOne(mirror::ArtMethod* method, TimingLogger* timings)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  VerificationResults* GetVerificationResults() const {
    return verification_results_;
  }

  DexFileToMethodInlinerMap* GetMethodInlinerMap() const {
    return method_inliner_map_;
  }

  InstructionSet GetInstructionSet() const {
    return instruction_set_;
  }

  const InstructionSetFeatures* GetInstructionSetFeatures() const {
    return instruction_set_features_;
  }

  const CompilerOptions& GetCompilerOptions() const {
    return *compiler_options_;
  }

  Compiler* GetCompiler() const {
    return compiler_.get();
  }

  bool ProfilePresent() const {
    return profile_present_;
  }

  // Are we compiling and creating an image file?
  bool IsImage() const {
    return image_;
  }

  const std::set<std::string>* GetImageClasses() const {
    return image_classes_.get();
  }

  CompilerTls* GetTls();

  // Generate the trampolines that are invoked by unresolved direct methods.
  const std::vector<uint8_t>* CreateInterpreterToInterpreterBridge() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const std::vector<uint8_t>* CreateInterpreterToCompiledCodeBridge() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const std::vector<uint8_t>* CreateJniDlsymLookup() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const std::vector<uint8_t>* CreatePortableImtConflictTrampoline() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const std::vector<uint8_t>* CreatePortableResolutionTrampoline() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const std::vector<uint8_t>* CreatePortableToInterpreterBridge() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const std::vector<uint8_t>* CreateQuickGenericJniTrampoline() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const std::vector<uint8_t>* CreateQuickImtConflictTrampoline() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const std::vector<uint8_t>* CreateQuickResolutionTrampoline() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
  const std::vector<uint8_t>* CreateQuickToInterpreterBridge() const
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  CompiledClass* GetCompiledClass(ClassReference ref) const
      LOCKS_EXCLUDED(compiled_classes_lock_);

  CompiledMethod* GetCompiledMethod(MethodReference ref) const
      LOCKS_EXCLUDED(compiled_methods_lock_);
  size_t GetNonRelativeLinkerPatchCount() const
      LOCKS_EXCLUDED(compiled_methods_lock_);

  void AddRequiresConstructorBarrier(Thread* self, const DexFile* dex_file,
                                     uint16_t class_def_index);
  bool RequiresConstructorBarrier(Thread* self, const DexFile* dex_file, uint16_t class_def_index);

  // Callbacks from compiler to see what runtime checks must be generated.

  bool CanAssumeTypeIsPresentInDexCache(const DexFile& dex_file, uint32_t type_idx);

  bool CanAssumeStringIsPresentInDexCache(const DexFile& dex_file, uint32_t string_idx)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Are runtime access checks necessary in the compiled code?
  bool CanAccessTypeWithoutChecks(uint32_t referrer_idx, const DexFile& dex_file,
                                  uint32_t type_idx, bool* type_known_final = nullptr,
                                  bool* type_known_abstract = nullptr,
                                  bool* equals_referrers_class = nullptr)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Are runtime access and instantiable checks necessary in the code?
  bool CanAccessInstantiableTypeWithoutChecks(uint32_t referrer_idx, const DexFile& dex_file,
                                              uint32_t type_idx)
     LOCKS_EXCLUDED(Locks::mutator_lock_);

  bool CanEmbedTypeInCode(const DexFile& dex_file, uint32_t type_idx,
                          bool* is_type_initialized, bool* use_direct_type_ptr,
                          uintptr_t* direct_type_ptr, bool* out_is_finalizable);

  // Query methods for the java.lang.ref.Reference class.
  bool CanEmbedReferenceTypeInCode(ClassReference* ref,
                                   bool* use_direct_type_ptr, uintptr_t* direct_type_ptr);
  uint32_t GetReferenceSlowFlagOffset() const;
  uint32_t GetReferenceDisableFlagOffset() const;

  // Get the DexCache for the
  mirror::DexCache* GetDexCache(const DexCompilationUnit* mUnit)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  mirror::ClassLoader* GetClassLoader(ScopedObjectAccess& soa, const DexCompilationUnit* mUnit)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Resolve compiling method's class. Returns nullptr on failure.
  mirror::Class* ResolveCompilingMethodsClass(
      const ScopedObjectAccess& soa, Handle<mirror::DexCache> dex_cache,
      Handle<mirror::ClassLoader> class_loader, const DexCompilationUnit* mUnit)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Resolve a field. Returns nullptr on failure, including incompatible class change.
  // NOTE: Unlike ClassLinker's ResolveField(), this method enforces is_static.
  mirror::ArtField* ResolveField(
      const ScopedObjectAccess& soa, Handle<mirror::DexCache> dex_cache,
      Handle<mirror::ClassLoader> class_loader, const DexCompilationUnit* mUnit,
      uint32_t field_idx, bool is_static)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Get declaration location of a resolved field.
  void GetResolvedFieldDexFileLocation(
      mirror::ArtField* resolved_field, const DexFile** declaring_dex_file,
      uint16_t* declaring_class_idx, uint16_t* declaring_field_idx)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  bool IsFieldVolatile(mirror::ArtField* field) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Can we fast-path an IGET/IPUT access to an instance field? If yes, compute the field offset.
  std::pair<bool, bool> IsFastInstanceField(
      mirror::DexCache* dex_cache, mirror::Class* referrer_class,
      mirror::ArtField* resolved_field, uint16_t field_idx)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Can we fast-path an SGET/SPUT access to a static field? If yes, compute the field offset,
  // the type index of the declaring class in the referrer's dex file and whether the declaring
  // class is the referrer's class or at least can be assumed to be initialized.
  std::pair<bool, bool> IsFastStaticField(
      mirror::DexCache* dex_cache, mirror::Class* referrer_class,
      mirror::ArtField* resolved_field, uint16_t field_idx, MemberOffset* field_offset,
      uint32_t* storage_index, bool* is_referrers_class, bool* is_initialized)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Resolve a method. Returns nullptr on failure, including incompatible class change.
  mirror::ArtMethod* ResolveMethod(
      ScopedObjectAccess& soa, Handle<mirror::DexCache> dex_cache,
      Handle<mirror::ClassLoader> class_loader, const DexCompilationUnit* mUnit,
      uint32_t method_idx, InvokeType invoke_type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Get declaration location of a resolved field.
  void GetResolvedMethodDexFileLocation(
      mirror::ArtMethod* resolved_method, const DexFile** declaring_dex_file,
      uint16_t* declaring_class_idx, uint16_t* declaring_method_idx)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Get the index in the vtable of the method.
  uint16_t GetResolvedMethodVTableIndex(
      mirror::ArtMethod* resolved_method, InvokeType type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Can we fast-path an INVOKE? If no, returns 0. If yes, returns a non-zero opaque flags value
  // for ProcessedInvoke() and computes the necessary lowering info.
  int IsFastInvoke(
      ScopedObjectAccess& soa, Handle<mirror::DexCache> dex_cache,
      Handle<mirror::ClassLoader> class_loader, const DexCompilationUnit* mUnit,
      mirror::Class* referrer_class, mirror::ArtMethod* resolved_method, InvokeType* invoke_type,
      MethodReference* target_method, const MethodReference* devirt_target,
      uintptr_t* direct_code, uintptr_t* direct_method)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // Does invokation of the resolved method need class initialization?
  bool NeedsClassInitialization(mirror::Class* referrer_class, mirror::ArtMethod* resolved_method)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void ProcessedInstanceField(bool resolved);
  void ProcessedStaticField(bool resolved, bool local);
  void ProcessedInvoke(InvokeType invoke_type, int flags);

  // Can we fast path instance field access? Computes field's offset and volatility.
  bool ComputeInstanceFieldInfo(uint32_t field_idx, const DexCompilationUnit* mUnit, bool is_put,
                                MemberOffset* field_offset, bool* is_volatile)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  mirror::ArtField* ComputeInstanceFieldInfo(uint32_t field_idx,
                                             const DexCompilationUnit* mUnit,
                                             bool is_put,
                                             const ScopedObjectAccess& soa)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);


  // Can we fastpath static field access? Computes field's offset, volatility and whether the
  // field is within the referrer (which can avoid checking class initialization).
  bool ComputeStaticFieldInfo(uint32_t field_idx, const DexCompilationUnit* mUnit, bool is_put,
                              MemberOffset* field_offset, uint32_t* storage_index,
                              bool* is_referrers_class, bool* is_volatile, bool* is_initialized)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Can we fastpath a interface, super class or virtual method call? Computes method's vtable
  // index.
  bool ComputeInvokeInfo(const DexCompilationUnit* mUnit, const uint32_t dex_pc,
                         bool update_stats, bool enable_devirtualization,
                         InvokeType* type, MethodReference* target_method, int* vtable_idx,
                         uintptr_t* direct_code, uintptr_t* direct_method)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  const VerifiedMethod* GetVerifiedMethod(const DexFile* dex_file, uint32_t method_idx) const;
  bool IsSafeCast(const DexCompilationUnit* mUnit, uint32_t dex_pc);

  bool GetSupportBootImageFixup() const {
    return support_boot_image_fixup_;
  }

  void SetSupportBootImageFixup(bool support_boot_image_fixup) {
    support_boot_image_fixup_ = support_boot_image_fixup;
  }

  ArenaPool* GetArenaPool() {
    return &arena_pool_;
  }

  bool WriteElf(const std::string& android_root,
                bool is_host,
                const std::vector<const DexFile*>& dex_files,
                OatWriter* oat_writer,
                File* file);

  // TODO: move to a common home for llvm helpers once quick/portable are merged.
  static void InstructionSetToLLVMTarget(InstructionSet instruction_set,
                                         std::string* target_triple,
                                         std::string* target_cpu,
                                         std::string* target_attr);

  void SetCompilerContext(void* compiler_context) {
    compiler_context_ = compiler_context;
  }

  void* GetCompilerContext() const {
    return compiler_context_;
  }

  size_t GetThreadCount() const {
    return thread_count_;
  }

  bool GetDumpPasses() const {
    return dump_passes_;
  }

  CumulativeLogger* GetTimingsLogger() const {
    return timings_logger_;
  }

  // Checks if class specified by type_idx is one of the image_classes_
  bool IsImageClass(const char* descriptor) const;

  void RecordClassStatus(ClassReference ref, mirror::Class::Status status)
      LOCKS_EXCLUDED(compiled_classes_lock_);

  std::vector<uint8_t>* DeduplicateCode(const std::vector<uint8_t>& code);
  SrcMap* DeduplicateSrcMappingTable(const SrcMap& src_map);
  std::vector<uint8_t>* DeduplicateMappingTable(const std::vector<uint8_t>& code);
  std::vector<uint8_t>* DeduplicateVMapTable(const std::vector<uint8_t>& code);
  std::vector<uint8_t>* DeduplicateGCMap(const std::vector<uint8_t>& code);
  std::vector<uint8_t>* DeduplicateCFIInfo(const std::vector<uint8_t>* cfi_info);

  ProfileFile profile_file_;
  bool profile_present_;

  // Should the compiler run on this method given profile information?
  bool SkipCompilation(const std::string& method_name);

 private:
  // These flags are internal to CompilerDriver for collecting INVOKE resolution statistics.
  // The only external contract is that unresolved method has flags 0 and resolved non-0.
  enum {
    kBitMethodResolved = 0,
    kBitVirtualMadeDirect,
    kBitPreciseTypeDevirtualization,
    kBitDirectCallToBoot,
    kBitDirectMethodToBoot
  };
  static constexpr int kFlagMethodResolved              = 1 << kBitMethodResolved;
  static constexpr int kFlagVirtualMadeDirect           = 1 << kBitVirtualMadeDirect;
  static constexpr int kFlagPreciseTypeDevirtualization = 1 << kBitPreciseTypeDevirtualization;
  static constexpr int kFlagDirectCallToBoot            = 1 << kBitDirectCallToBoot;
  static constexpr int kFlagDirectMethodToBoot          = 1 << kBitDirectMethodToBoot;
  static constexpr int kFlagsMethodResolvedVirtualMadeDirect =
      kFlagMethodResolved | kFlagVirtualMadeDirect;
  static constexpr int kFlagsMethodResolvedPreciseTypeDevirtualization =
      kFlagsMethodResolvedVirtualMadeDirect | kFlagPreciseTypeDevirtualization;

 public:  // TODO make private or eliminate.
  // Compute constant code and method pointers when possible.
  void GetCodeAndMethodForDirectCall(/*out*/InvokeType* type,
                                     InvokeType sharp_type,
                                     bool no_guarantee_of_dex_cache_entry,
                                     const mirror::Class* referrer_class,
                                     mirror::ArtMethod* method,
                                     /*out*/int* stats_flags,
                                     MethodReference* target_method,
                                     uintptr_t* direct_code, uintptr_t* direct_method)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 private:
  void PreCompile(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                  ThreadPool* thread_pool, TimingLogger* timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  void LoadImageClasses(TimingLogger* timings);

  // Attempt to resolve all type, methods, fields, and strings
  // referenced from code in the dex file following PathClassLoader
  // ordering semantics.
  void Resolve(jobject class_loader, const std::vector<const DexFile*>& dex_files,
               ThreadPool* thread_pool, TimingLogger* timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);
  void ResolveDexFile(jobject class_loader, const DexFile& dex_file,
                      const std::vector<const DexFile*>& dex_files,
                      ThreadPool* thread_pool, TimingLogger* timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  void Verify(jobject class_loader, const std::vector<const DexFile*>& dex_files,
              ThreadPool* thread_pool, TimingLogger* timings);
  void VerifyDexFile(jobject class_loader, const DexFile& dex_file,
                     const std::vector<const DexFile*>& dex_files,
                     ThreadPool* thread_pool, TimingLogger* timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  void SetVerified(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                   ThreadPool* thread_pool, TimingLogger* timings);
  void SetVerifiedDexFile(jobject class_loader, const DexFile& dex_file,
                          const std::vector<const DexFile*>& dex_files,
                          ThreadPool* thread_pool, TimingLogger* timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  void InitializeClasses(jobject class_loader, const std::vector<const DexFile*>& dex_files,
                         ThreadPool* thread_pool, TimingLogger* timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);
  void InitializeClasses(jobject class_loader, const DexFile& dex_file,
                         const std::vector<const DexFile*>& dex_files,
                         ThreadPool* thread_pool, TimingLogger* timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_, compiled_classes_lock_);

  void UpdateImageClasses(TimingLogger* timings) LOCKS_EXCLUDED(Locks::mutator_lock_);
  static void FindClinitImageClassesCallback(mirror::Object* object, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  void Compile(jobject class_loader, const std::vector<const DexFile*>& dex_files,
               ThreadPool* thread_pool, TimingLogger* timings);
  void CompileDexFile(jobject class_loader, const DexFile& dex_file,
                      const std::vector<const DexFile*>& dex_files,
                      ThreadPool* thread_pool, TimingLogger* timings)
      LOCKS_EXCLUDED(Locks::mutator_lock_);
  void CompileMethod(const DexFile::CodeItem* code_item, uint32_t access_flags,
                     InvokeType invoke_type, uint16_t class_def_idx, uint32_t method_idx,
                     jobject class_loader, const DexFile& dex_file,
                     DexToDexCompilationLevel dex_to_dex_compilation_level)
      LOCKS_EXCLUDED(compiled_methods_lock_);

  static void CompileClass(const ParallelCompilationManager* context, size_t class_def_index)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  const CompilerOptions* const compiler_options_;
  VerificationResults* const verification_results_;
  DexFileToMethodInlinerMap* const method_inliner_map_;

  std::unique_ptr<Compiler> compiler_;

  const InstructionSet instruction_set_;
  const InstructionSetFeatures* const instruction_set_features_;

  // All class references that require
  mutable ReaderWriterMutex freezing_constructor_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  std::set<ClassReference> freezing_constructor_classes_ GUARDED_BY(freezing_constructor_lock_);

  typedef SafeMap<const ClassReference, CompiledClass*> ClassTable;
  // All class references that this compiler has compiled.
  mutable Mutex compiled_classes_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  ClassTable compiled_classes_ GUARDED_BY(compiled_classes_lock_);

  typedef SafeMap<const MethodReference, CompiledMethod*, MethodReferenceComparator> MethodTable;
  // All method references that this compiler has compiled.
  mutable Mutex compiled_methods_lock_ DEFAULT_MUTEX_ACQUIRED_AFTER;
  MethodTable compiled_methods_ GUARDED_BY(compiled_methods_lock_);
  // Number of non-relative patches in all compiled methods. These patches need space
  // in the .oat_patches ELF section if requested in the compiler options.
  size_t non_relative_linker_patch_count_ GUARDED_BY(compiled_methods_lock_);

  const bool image_;

  // If image_ is true, specifies the classes that will be included in
  // the image. Note if image_classes_ is nullptr, all classes are
  // included in the image.
  std::unique_ptr<std::set<std::string>> image_classes_;

  size_t thread_count_;

  class AOTCompilationStats;
  std::unique_ptr<AOTCompilationStats> stats_;

  bool dump_stats_;
  const bool dump_passes_;

  CumulativeLogger* const timings_logger_;

  typedef void (*CompilerCallbackFn)(CompilerDriver& driver);
  typedef MutexLock* (*CompilerMutexLockFn)(CompilerDriver& driver);

  typedef void (*DexToDexCompilerFn)(CompilerDriver& driver,
                                     const DexFile::CodeItem* code_item,
                                     uint32_t access_flags, InvokeType invoke_type,
                                     uint32_t class_dex_idx, uint32_t method_idx,
                                     jobject class_loader, const DexFile& dex_file,
                                     DexToDexCompilationLevel dex_to_dex_compilation_level);
  DexToDexCompilerFn dex_to_dex_compiler_;

  void* compiler_context_;

  pthread_key_t tls_key_;

  // Arena pool used by the compiler.
  ArenaPool arena_pool_;

  bool support_boot_image_fixup_;

  // DeDuplication data structures, these own the corresponding byte arrays.
  template <typename ByteArray>
  class DedupeHashFunc {
   public:
    size_t operator()(const ByteArray& array) const {
      // For small arrays compute a hash using every byte.
      static const size_t kSmallArrayThreshold = 16;
      size_t hash = 0x811c9dc5;
      if (array.size() <= kSmallArrayThreshold) {
        for (auto b : array) {
          hash = (hash * 16777619) ^ static_cast<uint8_t>(b);
        }
      } else {
        // For larger arrays use the 2 bytes at 6 bytes (the location of a push registers
        // instruction field for quick generated code on ARM) and then select a number of other
        // values at random.
        static const size_t kRandomHashCount = 16;
        for (size_t i = 0; i < 2; ++i) {
          uint8_t b = static_cast<uint8_t>(array[i + 6]);
          hash = (hash * 16777619) ^ b;
        }
        for (size_t i = 2; i < kRandomHashCount; ++i) {
          size_t r = i * 1103515245 + 12345;
          uint8_t b = static_cast<uint8_t>(array[r % array.size()]);
          hash = (hash * 16777619) ^ b;
        }
      }
      hash += hash << 13;
      hash ^= hash >> 7;
      hash += hash << 3;
      hash ^= hash >> 17;
      hash += hash << 5;
      return hash;
    }
  };

  DedupeSet<std::vector<uint8_t>, size_t, DedupeHashFunc<std::vector<uint8_t>>, 4> dedupe_code_;
  DedupeSet<SrcMap, size_t, DedupeHashFunc<SrcMap>, 4> dedupe_src_mapping_table_;
  DedupeSet<std::vector<uint8_t>, size_t, DedupeHashFunc<std::vector<uint8_t>>, 4> dedupe_mapping_table_;
  DedupeSet<std::vector<uint8_t>, size_t, DedupeHashFunc<std::vector<uint8_t>>, 4> dedupe_vmap_table_;
  DedupeSet<std::vector<uint8_t>, size_t, DedupeHashFunc<std::vector<uint8_t>>, 4> dedupe_gc_map_;
  DedupeSet<std::vector<uint8_t>, size_t, DedupeHashFunc<std::vector<uint8_t>>, 4> dedupe_cfi_info_;

  DISALLOW_COPY_AND_ASSIGN(CompilerDriver);
};

}  // namespace art

#endif  // ART_COMPILER_DRIVER_COMPILER_DRIVER_H_
