#pragma once

#include <jni.h>

class DescriptorBuilder {
 public:
  explicit DescriptorBuilder(JNIEnv* env);

  static auto GetDescriptor(JNIEnv* env,
                            jobject declaringClass,
                            jstring name,
                            jobjectArray parameterTypes,
                            jobject returnType,
                            jint modifiers) -> jobject;

  ~DescriptorBuilder();

 private:
  JNIEnv* env_;
};
