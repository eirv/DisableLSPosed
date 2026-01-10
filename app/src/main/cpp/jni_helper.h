#pragma once

#include <jni.h>

#define ExceptionOccurred() \
  ExceptionCheck() ? env->functions->ExceptionOccurred(env) : jthrowable {}
#include "jni_helper.hpp"
#undef ExceptionOccurred

namespace lsplant {

template <ScopeOrClass Class>
[[maybe_unused]] inline auto JNI_GetSuperclass(JNIEnv* env, Class&& clazz) {
  return JNI_SafeInvoke(env, &JNIEnv::GetSuperclass, std::forward<Class>(clazz));
}

template <ScopeOrClass Class>
[[maybe_unused]] inline auto JNI_AllocObject(JNIEnv* env, Class&& clazz) {
  return JNI_SafeInvoke(env, &JNIEnv::AllocObject, std::forward<Class>(clazz));
}

template <ScopeOrObject Object>
[[maybe_unused]] inline auto JNI_FromReflectedField(JNIEnv* env, Object&& field) {
  return JNI_SafeInvoke(env, &JNIEnv::FromReflectedField, std::forward<Object>(field));
}

}  // namespace lsplant
