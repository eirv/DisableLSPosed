// AIGC
// ChatGPT converts Java code into equivalent C++ JNI code.

#include "descriptor_builder.h"

#include <jni.h>

#include <cassert>
#include <mutex>
#include <string>
#include <vector>

enum class ColorScheme : uint32_t {
  kPunctuation = 0xFFD0D0D0u,
  kDescriptorL = 0xFFD0D0D0u,
  kDescriptorPrimitive = 0xFF00AFFFu,
  kDescriptorPackageName = 0xFF949494u,
  kDescriptorPackageNameSynthetic = 0xFF949494u,
  kDescriptorClassName = 0xFF00AFFFu,
  kDescriptorClassNameSynthetic = 0xFFD0D0D0u,
  kDescriptorMethodName = 0xFFFF8700u,
  kDescriptorMethodNameSynthetic = 0xFFD0D0D0u,
  kDescriptorSemicolon = 0xFFD0D0D0u,
  kDescriptorArrow = 0xFFFF0000u
};

// ============ JNI CACHE ============
// A small singleton that stores global refs and method IDs for fast calls.
struct JniCache {
  bool initialized = false;

  // classes
  jclass spannableStringBuilderCls = nullptr;  // android.text.SpannableStringBuilder
  jclass foregroundColorSpanCls = nullptr;     // android.text.style.ForegroundColorSpan
  jclass classCls = nullptr;                   // java.lang.Class
  jclass stringCls = nullptr;                  // java.lang.String
  jclass modifierCls = nullptr;                // java.lang.reflect.Modifier

  // primitive TYPE class objects (global refs)
  jobject booleanTYPE = nullptr;
  jobject byteTYPE = nullptr;
  jobject shortTYPE = nullptr;
  jobject charTYPE = nullptr;
  jobject intTYPE = nullptr;
  jobject floatTYPE = nullptr;
  jobject longTYPE = nullptr;
  jobject doubleTYPE = nullptr;
  jobject voidTYPE = nullptr;

  // methods / constructors
  jmethodID ssb_ctor = nullptr;     // SpannableStringBuilder()
  jmethodID ssb_append = nullptr;   // append(String)
  jmethodID ssb_length = nullptr;   // length()
  jmethodID ssb_setSpan = nullptr;  // setSpan(Object, int, int, int)

  jmethodID fcs_ctor = nullptr;  // ForegroundColorSpan(int)

  jmethodID class_isPrimitive = nullptr;  // Class.isPrimitive()
  jmethodID class_isSynthetic = nullptr;  // Class.isSynthetic()
  jmethodID class_isArray = nullptr;      // Class.isArray()
  jmethodID class_getName = nullptr;      // Class.getName()

  jfieldID modifier_SYNTHETIC = nullptr;  // Modifier.SYNTHETIC int value

  // flags
  jint spanExclusiveExclusive = 0;  // Spannable.SPAN_EXCLUSIVE_EXCLUSIVE

  // mutex for thread-safe init
  std::once_flag initFlag;

  static JniCache& instance() {
    static JniCache s;
    return s;
  }

  void init(JNIEnv* env) {
    std::call_once(initFlag, [&]() {
      // Find classes
      jclass local_ssb = env->FindClass("android/text/SpannableStringBuilder");
      jclass local_fcs = env->FindClass("android/text/style/ForegroundColorSpan");
      jclass local_class = env->FindClass("java/lang/Class");
      jclass local_string = env->FindClass("java/lang/String");
      jclass local_modifier = env->FindClass("java/lang/reflect/Modifier");
      if (!local_ssb || !local_fcs || !local_class || !local_string || !local_modifier) {
        // If you want logging, add ALOG here. For brevity, we'll assert.
        env->ExceptionClear();
        assert(false && "Failed to find required Java classes");
        return;
      }

      // promote to global refs
      spannableStringBuilderCls = (jclass)env->NewGlobalRef(local_ssb);
      foregroundColorSpanCls = (jclass)env->NewGlobalRef(local_fcs);
      classCls = (jclass)env->NewGlobalRef(local_class);
      stringCls = (jclass)env->NewGlobalRef(local_string);
      modifierCls = (jclass)env->NewGlobalRef(local_modifier);

      // SpannableStringBuilder methods
      ssb_ctor = env->GetMethodID(spannableStringBuilderCls, "<init>", "()V");
      ssb_append = env->GetMethodID(
          spannableStringBuilderCls, "append", "(Ljava/lang/CharSequence;)Landroid/text/SpannableStringBuilder;");
      ssb_length = env->GetMethodID(spannableStringBuilderCls, "length", "()I");
      ssb_setSpan = env->GetMethodID(spannableStringBuilderCls, "setSpan", "(Ljava/lang/Object;III)V");
      if (!ssb_ctor || !ssb_append || !ssb_length || !ssb_setSpan) {
        env->ExceptionClear();
        assert(false && "Failed to find SpannableStringBuilder methods");
      }

      // ForegroundColorSpan ctor
      fcs_ctor = env->GetMethodID(foregroundColorSpanCls, "<init>", "(I)V");
      if (!fcs_ctor) {
        env->ExceptionClear();
        assert(false && "Failed to find ForegroundColorSpan ctor");
      }

      // Class methods
      class_isPrimitive = env->GetMethodID(classCls, "isPrimitive", "()Z");
      class_isSynthetic = env->GetMethodID(classCls, "isSynthetic", "()Z");
      class_isArray = env->GetMethodID(classCls, "isArray", "()Z");
      class_getName = env->GetMethodID(classCls, "getName", "()Ljava/lang/String;");
      if (!class_isPrimitive || !class_isSynthetic || !class_isArray || !class_getName) {
        env->ExceptionClear();
        assert(false && "Failed to find Class methods");
      }

      // Modifier SYNTHETIC flag
      modifier_SYNTHETIC = env->GetStaticFieldID(modifierCls, "SYNTHETIC", "I");
      if (modifier_SYNTHETIC) {
        // good
      } else {
        env->ExceptionClear();
        // If not present, default to 0 (rare). Keep it safe.
        modifier_SYNTHETIC = nullptr;
      }

      // Spannable flag constant: Spannable.SPAN_EXCLUSIVE_EXCLUSIVE is an int constant on interface or class
      // It's defined in android.text.Spanned as SPAN_EXCLUSIVE_EXCLUSIVE
      jclass spannedCls = env->FindClass("android/text/Spanned");
      if (spannedCls) {
        jfieldID fid = env->GetStaticFieldID(spannedCls, "SPAN_EXCLUSIVE_EXCLUSIVE", "I");
        if (fid) {
          spanExclusiveExclusive = env->GetStaticIntField(spannedCls, fid);
        } else {
          env->ExceptionClear();
          spanExclusiveExclusive = 0;
        }
        env->DeleteLocalRef(spannedCls);
      } else {
        env->ExceptionClear();
        spanExclusiveExclusive = 0;
      }

      // Cache primitive Class objects via wrapper TYPE fields
      auto getPrimitiveTypeField = [&](const char* wrapperClass) -> jobject {
        jclass tmp = env->FindClass(wrapperClass);
        if (!tmp) {
          env->ExceptionClear();
          return nullptr;
        }
        jfieldID fid = env->GetStaticFieldID(tmp, "TYPE", "Ljava/lang/Class;");
        if (!fid) {
          env->ExceptionClear();
          env->DeleteLocalRef(tmp);
          return nullptr;
        }
        jobject clsObj = env->GetStaticObjectField(tmp, fid);
        jobject g = env->NewGlobalRef(clsObj);
        env->DeleteLocalRef(tmp);
        env->DeleteLocalRef(clsObj);
        return g;
      };

      booleanTYPE = getPrimitiveTypeField("java/lang/Boolean");
      byteTYPE = getPrimitiveTypeField("java/lang/Byte");
      shortTYPE = getPrimitiveTypeField("java/lang/Short");
      charTYPE = getPrimitiveTypeField("java/lang/Character");
      intTYPE = getPrimitiveTypeField("java/lang/Integer");
      floatTYPE = getPrimitiveTypeField("java/lang/Float");
      longTYPE = getPrimitiveTypeField("java/lang/Long");
      doubleTYPE = getPrimitiveTypeField("java/lang/Double");
      voidTYPE = getPrimitiveTypeField("java/lang/Void");

      // cleanup local refs
      env->DeleteLocalRef(local_ssb);
      env->DeleteLocalRef(local_fcs);
      env->DeleteLocalRef(local_class);
      env->DeleteLocalRef(local_string);
      env->DeleteLocalRef(local_modifier);

      initialized = true;
    });
  }

  // utility destructor to release global refs when process unloads (not strictly required but hygienic)
  void destroy(JNIEnv* env) {
    if (!initialized) return;
    env->DeleteGlobalRef(spannableStringBuilderCls);
    env->DeleteGlobalRef(foregroundColorSpanCls);
    env->DeleteGlobalRef(classCls);
    env->DeleteGlobalRef(stringCls);
    env->DeleteGlobalRef(modifierCls);

    auto del = [&](jobject o) {
      if (o) env->DeleteGlobalRef(o);
    };
    del(booleanTYPE);
    del(byteTYPE);
    del(shortTYPE);
    del(charTYPE);
    del(intTYPE);
    del(floatTYPE);
    del(longTYPE);
    del(doubleTYPE);
    del(voidTYPE);
    initialized = false;
  }
};

// ============ Helper functions ============

static inline jstring newJStringUtf(JNIEnv* env, const std::string& s) { return env->NewStringUTF(s.c_str()); }

// Append a string with color span: obtains start = ssb.length(); ssb.append(str); create ForegroundColorSpan(color);
// ssb.setSpan(span, start, start+len, flags)
static void appendStringWithColor(JNIEnv* env, jobject ssb, const std::string& str, ColorScheme color) {
  JniCache& C = JniCache::instance();
  // start index
  jint start = env->CallIntMethod(ssb, C.ssb_length);
  jstring jstr = newJStringUtf(env, str);
  // append
  env->CallObjectMethod(ssb, C.ssb_append, jstr);
  // create span object
  jobject spanObj = env->NewObject(C.foregroundColorSpanCls, C.fcs_ctor, static_cast<jint>(color));
  // set span (what, start, end, flags)
  jint end = start + static_cast<jint>(str.size());
  env->CallVoidMethod(ssb, C.ssb_setSpan, spanObj, start, end, C.spanExclusiveExclusive);
  // cleanup locals
  env->DeleteLocalRef(jstr);
  env->DeleteLocalRef(spanObj);
}

// Append primitive descriptor (single-letter) like "I", "Z", etc.
static void appendPrimitiveDescriptor(JNIEnv* env, jobject ssb, jobject clazz) {
  JniCache& C = JniCache::instance();
  // compare clazz to cached TYPE objects
  if (env->IsSameObject(clazz, C.booleanTYPE)) appendStringWithColor(env, ssb, "Z", ColorScheme::kDescriptorPrimitive);
  else if (env->IsSameObject(clazz, C.byteTYPE))
    appendStringWithColor(env, ssb, "B", ColorScheme::kDescriptorPrimitive);
  else if (env->IsSameObject(clazz, C.shortTYPE))
    appendStringWithColor(env, ssb, "S", ColorScheme::kDescriptorPrimitive);
  else if (env->IsSameObject(clazz, C.charTYPE))
    appendStringWithColor(env, ssb, "C", ColorScheme::kDescriptorPrimitive);
  else if (env->IsSameObject(clazz, C.intTYPE)) appendStringWithColor(env, ssb, "I", ColorScheme::kDescriptorPrimitive);
  else if (env->IsSameObject(clazz, C.floatTYPE))
    appendStringWithColor(env, ssb, "F", ColorScheme::kDescriptorPrimitive);
  else if (env->IsSameObject(clazz, C.longTYPE))
    appendStringWithColor(env, ssb, "J", ColorScheme::kDescriptorPrimitive);
  else if (env->IsSameObject(clazz, C.doubleTYPE))
    appendStringWithColor(env, ssb, "D", ColorScheme::kDescriptorPrimitive);
  else appendStringWithColor(env, ssb, "V", ColorScheme::kDescriptorPrimitive);  // void or fallback
}

// Split a fully-qualified class name by '.' into parts without allocations per piece (returns vector<string>).
static std::vector<std::string> splitClassNameToParts(const std::string& name) {
  std::vector<std::string> parts;
  parts.reserve(6);
  size_t off = 0;
  for (size_t i = 0; i < name.size(); ++i) {
    if (name[i] == '.') {
      parts.emplace_back(name.data() + off, i - off);
      off = i + 1;
    }
  }
  if (off <= name.size()) parts.emplace_back(name.data() + off, name.size() - off);
  return parts;
}

// Append class name representation like: Lcom/example/Foo;
static void appendClassName(JNIEnv* env, jobject ssb, const std::string& className, bool syntheticFlag) {
  // append 'L'
  appendStringWithColor(env, ssb, "L", ColorScheme::kDescriptorL);
  // split on dot and append each package segment with "/" punctuation
  auto parts = splitClassNameToParts(className);
  size_t last = parts.size();
  for (size_t i = 0; i + 1 < last; ++i) {
    appendStringWithColor(
        env,
        ssb,
        parts[i],
        syntheticFlag ? ColorScheme::kDescriptorPackageNameSynthetic : ColorScheme::kDescriptorPackageName);
    appendStringWithColor(env, ssb, "/", ColorScheme::kPunctuation);
  }
  if (!parts.empty()) {
    appendStringWithColor(
        env,
        ssb,
        parts.back(),
        syntheticFlag ? ColorScheme::kDescriptorClassNameSynthetic : ColorScheme::kDescriptorClassName);
  }
  appendStringWithColor(env, ssb, ";", ColorScheme::kDescriptorSemicolon);
}

// Append a Class<?> descriptor: handles primitive, array, or object
static void appendClassDescriptor(JNIEnv* env, jobject ssb, jobject clazz) {
  JniCache& C = JniCache::instance();
  // call isPrimitive()
  jboolean isPrim = env->CallBooleanMethod(clazz, C.class_isPrimitive);
  if (isPrim) {
    appendPrimitiveDescriptor(env, ssb, clazz);
    return;
  }

  // handle arrays / objects
  jboolean isArray = env->CallBooleanMethod(clazz, C.class_isArray);
  jboolean synthetic = env->CallBooleanMethod(clazz, C.class_isSynthetic);
  auto nameStr = (jstring)env->CallObjectMethod(clazz, C.class_getName);
  const char* cname = env->GetStringUTFChars(nameStr, nullptr);
  std::string name(cname);
  env->ReleaseStringUTFChars(nameStr, cname);
  env->DeleteLocalRef(nameStr);

  if (isArray) {
    // Java name for array classes looks like "[I", "[[Ljava.lang.String;" etc.
    // Find last '[' (should be leading)
    // size_t idx = name.find_last_of('[');
    // size_t index = (idx == std::string::npos) ? 0 : idx;
    // We want to append the leading '[' characters (there may be multiple)
    // In your Java code you appended substring(0, ++index) and if char at index == 'L' handle specially.
    // So compute count of '[':
    size_t countBrackets = 0;
    while (countBrackets < name.size() && name[countBrackets] == '[')
      ++countBrackets;
    // append that many '[' as punctuation (original used substring of name)
    std::string brackets(countBrackets, '[');
    appendStringWithColor(env, ssb, brackets, ColorScheme::kPunctuation);

    if (countBrackets < name.size()) {
      char typeChar = name[countBrackets];
      if (typeChar == 'L') {
        // then the rest is like Ljava.lang.String; -> we need inner class name between L and ;
        // remove leading 'L' and trailing ';' if present
        std::string inner = name.substr(countBrackets + 1);
        if (!inner.empty() && inner.back() == ';') inner.pop_back();
        appendClassName(env, ssb, inner, synthetic);
      } else {
        // primitive code like 'I', 'B', etc.
        std::string primChar(1, typeChar);
        appendStringWithColor(env, ssb, primChar, ColorScheme::kDescriptorPrimitive);
      }
    }
  } else {
    // normal class name, e.g., java.lang.String
    appendClassName(env, ssb, name, synthetic);
  }
}

// ============ Native method implementation ============
// Signature assumes Java side will declare native as:
// public static native SpannableStringBuilder getDescriptorNative(Class<?> declaringClass, String name, Class<?>[]
// parameterTypes, Class<?> returnType, int modifiers);
jobject DescriptorBuilder::GetDescriptor(JNIEnv* env,
                                         jobject declaringClass,
                                         jstring name,
                                         jobjectArray parameterTypes,
                                         jobject returnType,
                                         jint modifiers) {
  JniCache& C = JniCache::instance();
  C.init(env);

  // Create SpannableStringBuilder instance
  jobject ssb = env->NewObject(C.spannableStringBuilderCls, C.ssb_ctor);

  // append declaring class
  if (declaringClass != nullptr) {
    appendClassDescriptor(env, ssb, declaringClass);
  }

  // append arrow "->"
  appendStringWithColor(env, ssb, "->", ColorScheme::kDescriptorArrow);

  // append method name
  const char* nameChars = env->GetStringUTFChars(name, nullptr);
  std::string methodName(nameChars);
  env->ReleaseStringUTFChars(name, nameChars);

  // check synthetic modifier from Modifier.SYNTHETIC if available
  bool methodSynthetic = false;
  if (C.modifier_SYNTHETIC) {
    jint synVal = env->GetStaticIntField(C.modifierCls, C.modifier_SYNTHETIC);
    methodSynthetic = ((modifiers & synVal) != 0);
  } else {
    // fallback: user passed modifiers; if bit 0x00001000 maybe synthetic - but safer default false
    methodSynthetic = ((modifiers & 0x00001000) != 0);
  }
  appendStringWithColor(
      env,
      ssb,
      methodName,
      methodSynthetic ? ColorScheme::kDescriptorMethodNameSynthetic : ColorScheme::kDescriptorMethodName);

  // append "("
  appendStringWithColor(env, ssb, "(", ColorScheme::kPunctuation);

  // parameters
  if (parameterTypes != nullptr) {
    jsize paramCount = env->GetArrayLength(parameterTypes);
    for (jsize i = 0; i < paramCount; ++i) {
      jobject pClazz = env->GetObjectArrayElement(parameterTypes, i);  // local ref
      appendClassDescriptor(env, ssb, pClazz);
      env->DeleteLocalRef(pClazz);
    }
  }

  // append ")"
  appendStringWithColor(env, ssb, ")", ColorScheme::kPunctuation);

  // return type
  if (returnType != nullptr) {
    appendClassDescriptor(env, ssb, returnType);
  }

  // return SpannableStringBuilder (local ref ok)
  return ssb;
}

// ============ JNI OnLoad / OnUnload for init/cleanup ============
DescriptorBuilder::DescriptorBuilder(JNIEnv* env) : env_{env} {
  // initialize cache
  JniCache::instance().init(env);
}

DescriptorBuilder::~DescriptorBuilder() { JniCache::instance().destroy(env_); }
