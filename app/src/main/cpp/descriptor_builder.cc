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
  kDescriptorArrow = 0xFF949494u
};

struct Modifier {
  static constexpr jint SYNTHETIC = 0x1000;
};

struct Spanned {
  static constexpr jint SPAN_EXCLUSIVE_EXCLUSIVE = 0x21;
};

// ============ JNI CACHE ============
// A small singleton that stores global refs and method IDs for fast calls.
struct JniCache {
  bool initialized = false;

  // classes
  jclass SpannableStringBuilder_class = nullptr;  // android.text.SpannableStringBuilder
  jclass ForegroundColorSpan_class = nullptr;     // android.text.style.ForegroundColorSpan
  jclass Class_class = nullptr;                   // java.lang.Class

  // primitive TYPE class objects (global refs)
  jobject Boolean_TYPE = nullptr;
  jobject Byte_TYPE = nullptr;
  jobject Short_TYPE = nullptr;
  jobject Character_TYPE = nullptr;
  jobject Integer_TYPE = nullptr;
  jobject Float_TYPE = nullptr;
  jobject Long_TYPE = nullptr;
  jobject Double_TYPE = nullptr;
  jobject Void_TYPE = nullptr;

  // methods / constructors
  jmethodID SpannableStringBuilder_init = nullptr;     // SpannableStringBuilder()
  jmethodID SpannableStringBuilder_append = nullptr;   // append(String)
  jmethodID SpannableStringBuilder_length = nullptr;   // length()
  jmethodID SpannableStringBuilder_setSpan = nullptr;  // setSpan(Object, int, int, int)

  jmethodID ForegroundColorSpan_init = nullptr;  // ForegroundColorSpan(int)

  jmethodID Class_isPrimitive = nullptr;  // Class.isPrimitive()
  jmethodID Class_isSynthetic = nullptr;  // Class.isSynthetic()
  jmethodID Class_isArray = nullptr;      // Class.isArray()
  jmethodID Class_getName = nullptr;      // Class.getName()

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
      if (!local_ssb || !local_fcs || !local_class) {
        // If you want logging, add ALOG here. For brevity, we'll assert.
        env->ExceptionClear();
        assert(false && "Failed to find required Java classes");
        return;
      }

      // promote to global refs
      SpannableStringBuilder_class = local_ssb;
      ForegroundColorSpan_class = local_fcs;
      Class_class = local_class;

      // SpannableStringBuilder methods
      SpannableStringBuilder_init = env->GetMethodID(SpannableStringBuilder_class, "<init>", "()V");
      SpannableStringBuilder_append = env->GetMethodID(
          SpannableStringBuilder_class, "append", "(Ljava/lang/CharSequence;)Landroid/text/SpannableStringBuilder;");
      SpannableStringBuilder_length = env->GetMethodID(SpannableStringBuilder_class, "length", "()I");
      SpannableStringBuilder_setSpan =
          env->GetMethodID(SpannableStringBuilder_class, "setSpan", "(Ljava/lang/Object;III)V");
      if (!SpannableStringBuilder_init || !SpannableStringBuilder_append || !SpannableStringBuilder_length ||
          !SpannableStringBuilder_setSpan) {
        env->ExceptionClear();
        assert(false && "Failed to find SpannableStringBuilder methods");
      }

      // ForegroundColorSpan ctor
      ForegroundColorSpan_init = env->GetMethodID(ForegroundColorSpan_class, "<init>", "(I)V");
      if (!ForegroundColorSpan_init) {
        env->ExceptionClear();
        assert(false && "Failed to find ForegroundColorSpan ctor");
      }

      // Class methods
      Class_isPrimitive = env->GetMethodID(Class_class, "isPrimitive", "()Z");
      Class_isSynthetic = env->GetMethodID(Class_class, "isSynthetic", "()Z");
      Class_isArray = env->GetMethodID(Class_class, "isArray", "()Z");
      Class_getName = env->GetMethodID(Class_class, "getName", "()Ljava/lang/String;");
      if (!Class_isPrimitive || !Class_isSynthetic || !Class_isArray || !Class_getName) {
        env->ExceptionClear();
        assert(false && "Failed to find Class methods");
      }

      // Cache primitive Class objects via wrapper TYPE fields
      auto getPrimitiveTypeField = [&](const char* wrapperClassName) -> jobject {
        auto wrapperClass = env->FindClass(wrapperClassName);
        if (!wrapperClass) {
          env->ExceptionClear();
          return nullptr;
        }
        auto fid = env->GetStaticFieldID(wrapperClass, "TYPE", "Ljava/lang/Class;");
        if (!fid) {
          env->ExceptionClear();
          env->DeleteLocalRef(wrapperClass);
          return nullptr;
        }
        auto clsObj = env->GetStaticObjectField(wrapperClass, fid);
        env->DeleteLocalRef(wrapperClass);
        return clsObj;
      };

      Boolean_TYPE = getPrimitiveTypeField("java/lang/Boolean");
      Byte_TYPE = getPrimitiveTypeField("java/lang/Byte");
      Short_TYPE = getPrimitiveTypeField("java/lang/Short");
      Character_TYPE = getPrimitiveTypeField("java/lang/Character");
      Integer_TYPE = getPrimitiveTypeField("java/lang/Integer");
      Float_TYPE = getPrimitiveTypeField("java/lang/Float");
      Long_TYPE = getPrimitiveTypeField("java/lang/Long");
      Double_TYPE = getPrimitiveTypeField("java/lang/Double");
      Void_TYPE = getPrimitiveTypeField("java/lang/Void");

      initialized = true;
    });
  }

  // utility destructor to release global refs when process unloads (not strictly required but hygienic)
  void destroy(JNIEnv* env) {
    if (!initialized) return;
    env->DeleteLocalRef(SpannableStringBuilder_class);
    env->DeleteLocalRef(ForegroundColorSpan_class);
    env->DeleteLocalRef(Class_class);

    env->DeleteLocalRef(Boolean_TYPE);
    env->DeleteLocalRef(Byte_TYPE);
    env->DeleteLocalRef(Short_TYPE);
    env->DeleteLocalRef(Character_TYPE);
    env->DeleteLocalRef(Integer_TYPE);
    env->DeleteLocalRef(Float_TYPE);
    env->DeleteLocalRef(Long_TYPE);
    env->DeleteLocalRef(Double_TYPE);
    env->DeleteLocalRef(Void_TYPE);
    initialized = false;
  }
};

// ============ Helper functions ============

// Append a string with color span: obtains start = ssb.length(); ssb.append(str); create ForegroundColorSpan(color);
// ssb.setSpan(span, start, start+len, flags)
static void appendStringWithColor(JNIEnv* env, jobject ssb, const std::string& str, ColorScheme color) {
  JniCache& C = JniCache::instance();
  // start index
  jint start = env->CallNonvirtualIntMethod(ssb, C.SpannableStringBuilder_class, C.SpannableStringBuilder_length);
  jstring jstr = env->NewStringUTF(str.c_str());
  // append
  env->CallNonvirtualObjectMethod(ssb, C.SpannableStringBuilder_class, C.SpannableStringBuilder_append, jstr);
  // create span object
  jobject spanObj = env->NewObject(C.ForegroundColorSpan_class, C.ForegroundColorSpan_init, static_cast<jint>(color));
  // set span (what, start, end, flags)
  jint end = start + static_cast<jint>(str.size());
  env->CallNonvirtualVoidMethod(ssb,
                                C.SpannableStringBuilder_class,
                                C.SpannableStringBuilder_setSpan,
                                spanObj,
                                start,
                                end,
                                Spanned::SPAN_EXCLUSIVE_EXCLUSIVE);
  // cleanup locals
  env->DeleteLocalRef(jstr);
  env->DeleteLocalRef(spanObj);
}

// Append primitive descriptor (single-letter) like "I", "Z", etc.
static void appendPrimitiveDescriptor(JNIEnv* env, jobject ssb, jobject clazz) {
  JniCache& C = JniCache::instance();
  // compare clazz to cached TYPE objects
  auto name = "V";
  if (env->IsSameObject(clazz, C.Boolean_TYPE)) name = "Z";
  else if (env->IsSameObject(clazz, C.Byte_TYPE)) name = "B";
  else if (env->IsSameObject(clazz, C.Short_TYPE)) name = "S";
  else if (env->IsSameObject(clazz, C.Character_TYPE)) name = "C";
  else if (env->IsSameObject(clazz, C.Integer_TYPE)) name = "I";
  else if (env->IsSameObject(clazz, C.Float_TYPE)) name = "F";
  else if (env->IsSameObject(clazz, C.Long_TYPE)) name = "J";
  else if (env->IsSameObject(clazz, C.Double_TYPE)) name = "D";
  appendStringWithColor(env, ssb, name, ColorScheme::kDescriptorPrimitive);
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
  jboolean isPrim = env->CallNonvirtualBooleanMethod(clazz, C.Class_class, C.Class_isPrimitive);
  if (isPrim) {
    appendPrimitiveDescriptor(env, ssb, clazz);
    return;
  }

  // handle arrays / objects
  jboolean isArray = env->CallNonvirtualBooleanMethod(clazz, C.Class_class, C.Class_isArray);
  jboolean synthetic = env->CallNonvirtualBooleanMethod(clazz, C.Class_class, C.Class_isSynthetic);
  auto nameStr = reinterpret_cast<jstring>(env->CallNonvirtualObjectMethod(clazz, C.Class_class, C.Class_getName));
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
  jobject ssb = env->NewObject(C.SpannableStringBuilder_class, C.SpannableStringBuilder_init);

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
  bool methodSynthetic = (modifiers & Modifier::SYNTHETIC) != 0;
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
