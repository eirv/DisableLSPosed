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

#pragma once

#include <cstdint>

namespace art::mirror {

class Object;

template <bool kPoisonReferences, class MirrorType>
class PtrCompression {
 public:
  // Compress reference to its bit representation.
  static uint32_t Compress(MirrorType* mirror_ptr) {
    uint32_t as_bits = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mirror_ptr));
    return kPoisonReferences ? -as_bits : as_bits;
  }

  // Uncompress an encoded reference from its bit representation.
  static MirrorType* Decompress(uint32_t ref) {
    uint32_t as_bits = kPoisonReferences ? -ref : ref;
    return reinterpret_cast<MirrorType*>(static_cast<uintptr_t>(as_bits));
  }
};

// Value type representing a reference to a mirror::Object of type MirrorType.
template <bool kPoisonReferences, class MirrorType>
class alignas(4) [[gnu::packed]] ObjectReference {
 private:
  using Compression = PtrCompression<kPoisonReferences, MirrorType>;

 public:
  /*
   * Returns a pointer to the mirror of the managed object this reference is for.
   *
   * This does NOT return the current object (which isn't derived from, and
   * therefor cannot be a mirror::Object) as a mirror pointer.  Instead, this
   * returns a pointer to the mirror of the managed object this refers to.
   *
   * TODO (chriswailes): Rename to GetPtr().
   */
  MirrorType* AsMirrorPtr() const { return Compression::Decompress(reference_); }

  void Assign(MirrorType* other) { reference_ = Compression::Compress(other); }

  void Clear() {
    reference_ = 0;
    // DCHECK(IsNull());
  }

  bool IsNull() const { return reference_ == 0; }

  static ObjectReference<kPoisonReferences, MirrorType> FromMirrorPtr(MirrorType* mirror_ptr) {
    return ObjectReference<kPoisonReferences, MirrorType>(mirror_ptr);
  }

 protected:
  explicit ObjectReference(MirrorType* mirror_ptr) : reference_(Compression::Compress(mirror_ptr)) {}
  ObjectReference() : reference_(0u) {
    // DCHECK(IsNull());
  }

  // The encoded reference to a mirror::Object.
  uint32_t reference_;
};

// Standard compressed reference used in the runtime. Used for StackReference and GC roots.
template <class MirrorType>
class alignas(4) [[gnu::packed]] CompressedReference : public mirror::ObjectReference<false, MirrorType> {
 public:
  CompressedReference<MirrorType>() : mirror::ObjectReference<false, MirrorType>() {}

  static CompressedReference<MirrorType> FromMirrorPtr(MirrorType* p) { return CompressedReference<MirrorType>(p); }

  static CompressedReference<MirrorType> FromVRegValue(uint32_t vreg_value) {
    CompressedReference<MirrorType> result;
    result.reference_ = vreg_value;
    return result;
  }

  uint32_t AsVRegValue() const { return this->reference_; }

 private:
  explicit CompressedReference(MirrorType* p) : ObjectReference<false, MirrorType>(p) {}
};

}  // namespace art::mirror
