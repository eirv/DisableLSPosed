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
#include <functional>

#include "object_reference.h"

namespace art {

namespace mirror {
class Object;
}

enum RootType {
  kRootUnknown = 0,
  kRootJNIGlobal,
  kRootJNILocal,
  kRootJavaFrame,
  kRootNativeStack,
  kRootStickyClass,
  kRootThreadBlock,
  kRootMonitorUsed,
  kRootThreadObject,
  kRootInternedString,
  kRootFinalizing,  // used for HPROF's conversion to HprofHeapTag
  kRootDebugger,
  kRootReferenceCleanup,  // used for HPROF's conversion to HprofHeapTag
  kRootVMInternal,
  kRootJNIMonitor,
};

// Only used by hprof. thread_id_ and type_ are only used by hprof.
class RootInfo {
 public:
  // Thread id 0 is for non thread roots.
  explicit RootInfo(RootType type, uint32_t thread_id = 0) : type_(type), thread_id_(thread_id) {}
  RootInfo(const RootInfo&) = default;
  virtual ~RootInfo() {}
  RootType GetType() const { return type_; }
  uint32_t GetThreadId() const { return thread_id_; }

 private:
  const RootType type_;
  const uint32_t thread_id_;
};

class RootVisitor {
 public:
  virtual ~RootVisitor() {}

  // Single root version, not overridable.
  [[gnu::always_inline]] void VisitRoot(mirror::Object** root, const RootInfo& info) { VisitRoots(&root, 1, info); }

  // Single root version, not overridable.
  [[gnu::always_inline]] void VisitRootIfNonNull(mirror::Object** root, const RootInfo& info) {
    if (*root != nullptr) {
      VisitRoot(root, info);
    }
  }

  virtual void VisitRoots(mirror::Object*** roots, size_t count, const RootInfo& info) = 0;

  virtual void VisitRoots(mirror::CompressedReference<mirror::Object>** roots, size_t count, const RootInfo& info) = 0;
};

// Only visits roots one at a time, doesn't handle updating roots. Used when performance isn't
// critical.
class SingleRootVisitor : public RootVisitor {
 private:
  void VisitRoots(mirror::Object*** roots, size_t count, const RootInfo& info) override {
    for (size_t i = 0; i < count; ++i) {
      VisitRoot(*roots[i], info);
    }
  }

  void VisitRoots(mirror::CompressedReference<mirror::Object>** roots, size_t count, const RootInfo& info) override {
    for (size_t i = 0; i < count; ++i) {
      VisitRoot(roots[i]->AsMirrorPtr(), info);
    }
  }

  virtual void VisitRoot(mirror::Object* root, const RootInfo& info) = 0;
};

class LambdaRootVisitor : public SingleRootVisitor {
 public:
  using Type = const std::function<void(mirror::Object* root, const RootInfo& info)>&;

  LambdaRootVisitor(Type visitor) : visitor_{visitor} {}

 private:
  virtual void VisitRoot(mirror::Object* root, const RootInfo& info) override { visitor_(root, info); }

  Type visitor_;
};

}  // namespace art
