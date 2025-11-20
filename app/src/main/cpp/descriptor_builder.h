#pragma once

#include <jni.h>

class DescriptorBuilder {
 public:
  explicit DescriptorBuilder(JNIEnv* env);

  static jobject GetDescriptor(JNIEnv* env,
                               jobject declaringClass,
                               jstring name,
                               jobjectArray parameterTypes,
                               jobject returnType,
                               jint modifiers);

  ~DescriptorBuilder();

 private:
  JNIEnv* env_;
};
