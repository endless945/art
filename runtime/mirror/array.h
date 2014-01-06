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

#ifndef ART_RUNTIME_MIRROR_ARRAY_H_
#define ART_RUNTIME_MIRROR_ARRAY_H_

#include "object.h"
#include "gc/heap.h"
#include "thread.h"

namespace art {
namespace mirror {

class MANAGED Array : public Object {
 public:
  // A convenience for code that doesn't know the component size, and doesn't want to have to work
  // it out itself.
  template <bool kIsInstrumented>
  static Array* Alloc(Thread* self, Class* array_class, int32_t component_count,
                      gc::AllocatorType allocator_type)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  template <bool kIsInstrumented>
  static Array* Alloc(Thread* self, Class* array_class, int32_t component_count,
                      size_t component_size, gc::AllocatorType allocator_type)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  template <bool kIsInstrumented>
  static Array* Alloc(Thread* self, Class* array_class, int32_t component_count)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  template <bool kIsInstrumented>
  static Array* Alloc(Thread* self, Class* array_class, int32_t component_count,
                      size_t component_size)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static Array* CreateMultiArray(Thread* self, Class* element_class, IntArray* dimensions)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  size_t SizeOf() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  int32_t GetLength() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetField32(OFFSET_OF_OBJECT_MEMBER(Array, length_), false);
  }

  void SetLength(int32_t length) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    CHECK_GE(length, 0);
    SetField32(OFFSET_OF_OBJECT_MEMBER(Array, length_), length, false, false);
  }

  static MemberOffset LengthOffset() {
    return OFFSET_OF_OBJECT_MEMBER(Array, length_);
  }

  static MemberOffset DataOffset(size_t component_size) {
    if (component_size != sizeof(int64_t)) {
      return OFFSET_OF_OBJECT_MEMBER(Array, first_element_);
    } else {
      // Align longs and doubles.
      return MemberOffset(OFFSETOF_MEMBER(Array, first_element_) + 4);
    }
  }

  void* GetRawData(size_t component_size, int32_t index)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    intptr_t data = reinterpret_cast<intptr_t>(this) + DataOffset(component_size).Int32Value() +
        + (index * component_size);
    return reinterpret_cast<void*>(data);
  }

  const void* GetRawData(size_t component_size, int32_t index) const {
    intptr_t data = reinterpret_cast<intptr_t>(this) + DataOffset(component_size).Int32Value() +
        + (index * component_size);
    return reinterpret_cast<void*>(data);
  }

  // Returns true if the index is valid. If not, throws an ArrayIndexOutOfBoundsException and
  // returns false.
  bool CheckIsValidIndex(int32_t index) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (UNLIKELY(static_cast<uint32_t>(index) >= static_cast<uint32_t>(GetLength()))) {
      ThrowArrayIndexOutOfBoundsException(index);
      return false;
    }
    return true;
  }

 protected:
  void ThrowArrayStoreException(Object* object) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 private:
  void ThrowArrayIndexOutOfBoundsException(int32_t index)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  // The number of array elements.
  int32_t length_;
  // Marker for the data (used by generated code)
  uint32_t first_element_[0];

  DISALLOW_IMPLICIT_CONSTRUCTORS(Array);
};

template<class T>
class MANAGED PrimitiveArray : public Array {
 public:
  typedef T ElementType;

  static PrimitiveArray<T>* Alloc(Thread* self, size_t length)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  const T* GetData() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return reinterpret_cast<const T*>(GetRawData(sizeof(T), 0));
  }

  T* GetData() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return reinterpret_cast<T*>(GetRawData(sizeof(T), 0));
  }

  T Get(int32_t i) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (UNLIKELY(!CheckIsValidIndex(i))) {
      DCHECK(Thread::Current()->IsExceptionPending());
      return T(0);
    }
    return GetWithoutChecks(i);
  }

  T GetWithoutChecks(int32_t i) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(CheckIsValidIndex(i));
    return GetData()[i];
  }

  void Set(int32_t i, T value) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (LIKELY(CheckIsValidIndex(i))) {
      SetWithoutChecks(i, value);
    } else {
      DCHECK(Thread::Current()->IsExceptionPending());
    }
  }

  void SetWithoutChecks(int32_t i, T value) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(CheckIsValidIndex(i));
    GetData()[i] = value;
  }

  /*
   * Works like memmove(), except we guarantee not to allow tearing of array values (ie using
   * smaller than element size copies). Arguments are assumed to be within the bounds of the array
   * and the arrays non-null.
   */
  void Memmove(int32_t dst_pos, PrimitiveArray<T>* src, int32_t src_pos, int32_t count)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  /*
   * Works like memcpy(), except we guarantee not to allow tearing of array values (ie using
   * smaller than element size copies). Arguments are assumed to be within the bounds of the array
   * and the arrays non-null.
   */
  void Memcpy(int32_t dst_pos, PrimitiveArray<T>* src, int32_t src_pos, int32_t count)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  static void SetArrayClass(Class* array_class) {
    CHECK(array_class_ == NULL);
    CHECK(array_class != NULL);
    array_class_ = array_class;
  }

  static void ResetArrayClass() {
    CHECK(array_class_ != NULL);
    array_class_ = NULL;
  }

  static void VisitRoots(RootVisitor* visitor, void* arg)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 private:
  static Class* array_class_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(PrimitiveArray);
};

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_ARRAY_H_
