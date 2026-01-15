#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "descriptor_builder.h"
#include "gc_root.h"
#include "jni_helper.h"
#include "linux_syscall_support.h"
#include "maps_parser.h"
#include "xdl.h"

using namespace std::string_literals;
using namespace std::string_view_literals;
using namespace lsplant;
using namespace io::proc;

using art::LambdaRootVisitor;
using art::RootInfo;
using art::RootVisitor;
using art::mirror::Object;

using LambdaRootVisitorType = typename LambdaRootVisitor::Type;

namespace {
#ifdef USE_SPANNABLE_STRING_BUILDER
auto unhooked_method_list_ = jobject{};
#else
auto unhooked_methods_ = std::vector<std::string>{};
#endif

auto cleared_callbacks_ = std::vector<std::string>{};
auto framework_name_ = "LSPosed"s;

auto is_lsposed_disabled_ = bool{};
auto is_art_restored_ = bool{};

template <typename T>
void InsertUnique(std::vector<T>& vec, const T& value) {
  if (std::find(vec.begin(), vec.end(), value) == vec.end()) {
    vec.push_back(value);
  }
}

template <typename... Args>
auto FormatString(std::string_view fmt, Args... args) -> std::string {
  std::array<char, 1024> buffer{};
  snprintf(buffer.data(), buffer.size(), fmt.data(), args...);
  return buffer.data();
}

class XposedCallbackHelper {
 public:
  explicit XposedCallbackHelper(JNIEnv* env)
      : env_{env},
        class_cls_{JNI_FindClass(env, "java/lang/Class")},
        field_cls_{JNI_FindClass(env, "java/lang/reflect/Field")},
        collection_cls_{JNI_FindClass(env, "java/util/Collection")},
        key_set_view_cls_{JNI_FindClass(env, "java/util/concurrent/ConcurrentHashMap$KeySetView")},
        system_cls_{JNI_FindClass(env, "java/lang/System")},
        xposed_interface_cls_{env} {
    class_getDeclaredFields_ = JNI_GetMethodID(env_, class_cls_, "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
    class_getName_ = JNI_GetMethodID(env_, class_cls_, "getName", "()Ljava/lang/String;");
    class_getSimpleName_ = JNI_GetMethodID(env_, class_cls_, "getSimpleName", "()Ljava/lang/String;");
    class_getInterfaces_ = JNI_GetMethodID(env_, class_cls_, "getInterfaces", "()[Ljava/lang/Class;");

    field_setAccessible_ = JNI_GetMethodID(env_, field_cls_, "setAccessible", "(Z)V");
    field_get_ = JNI_GetMethodID(env_, field_cls_, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
    field_getModifiers_ = JNI_GetMethodID(env_, field_cls_, "getModifiers", "()I");

    collection_clear_ = JNI_GetMethodID(env_, collection_cls_, "clear", "()V");

    auto iterable_cls = JNI_FindClass(env, "java/lang/Iterable");
    iterable_iterator_ = JNI_GetMethodID(env, iterable_cls, "iterator", "()Ljava/util/Iterator;");

    auto iterator_cls = JNI_FindClass(env, "java/util/Iterator");
    iterator_hasNext_ = JNI_GetMethodID(env, iterator_cls, "hasNext", "()Z");
    iterator_next_ = JNI_GetMethodID(env, iterator_cls, "next", "()Ljava/lang/Object;");

    system_identityHashCode_ = JNI_GetStaticMethodID(env, system_cls_, "identityHashCode", "(Ljava/lang/Object;)I");
  }

  void ClearXposedCallbacks(ScopedLocalRef<jclass>& cls) {
    if (legacy_cleared_ && modern_cleared_) return;

    auto name_jstr = JNI_Cast<jstring>(JNI_CallNonvirtualObjectMethod(env_, cls, class_cls_, class_getSimpleName_));
    if (!name_jstr) return;
    auto name = JUTFString{name_jstr};

    if (!modern_cleared_) {
      if (name.get() == "XposedInterface"sv) {
        xposed_interface_cls_ = cls.clone();
        return;
      } else if (ClearModernCallbacks(cls)) {
        modern_cleared_ = true;
        return;
      }
    }

    if (!legacy_cleared_ && name.get() == "XposedBridge"sv) {
      ClearLegacyCallbacks(cls);
      legacy_cleared_ = true;
    }
  }

 private:
  void ClearLegacyCallbacks(ScopedLocalRef<jclass>& cls) { ClearStaticFieldsAssignableTo(cls, collection_cls_, true); }

  auto ClearModernCallbacks(ScopedLocalRef<jclass>& cls) -> bool {
    if (!key_set_view_cls_) return false;

    bool is_xposed = false;

    if (xposed_interface_cls_) {
      if (env_->IsAssignableFrom(cls.get(), xposed_interface_cls_.get())) {
        is_xposed = true;
      }
    } else {
      auto interfaces =
          JNI_Cast<jobjectArray>(JNI_CallNonvirtualObjectMethod(env_, cls, class_cls_, class_getInterfaces_));
      if (!interfaces) return false;

      for (auto& interface : interfaces) {
        if (!interface.get()) continue;

        auto jstr =
            JNI_Cast<jstring>(JNI_CallNonvirtualObjectMethod(env_, interface, class_cls_, class_getSimpleName_));
        if (!jstr) continue;
        auto interface_name = JUTFString{jstr};

        if (interface_name.get() == "XposedInterface"sv) {
          xposed_interface_cls_.reset(reinterpret_cast<jclass>(interface.release()));
          is_xposed = true;
          break;
        }
      }
    }

    if (is_xposed && ClearStaticFieldsAssignableTo(cls, key_set_view_cls_, false)) {
      if (auto framework_name = GetFrameworkName(cls); !framework_name.empty()) {
        framework_name_ = framework_name;
      }
      return true;
    }
    return false;
  }

  auto ClearStaticFieldsAssignableTo(ScopedLocalRef<jclass>& cls,
                                     ScopedLocalRef<jclass>& expected_type,
                                     bool has_wrapper) -> bool {
    if (!cls || !expected_type) return false;

    auto fields =
        JNI_Cast<jobjectArray>(JNI_CallNonvirtualObjectMethod(env_, cls, class_cls_, class_getDeclaredFields_));
    if (!fields) return false;

    auto cleared = false;

    for (auto& field : fields) {
      JNI_CallNonvirtualVoidMethod(env_, field.get(), field_cls_, field_setAccessible_, JNI_TRUE);
      auto modifiers = JNI_CallNonvirtualIntMethod(env_, field.get(), field_cls_, field_getModifiers_);
      if (!(modifiers & kAccStatic)) continue;

      auto collection = JNI_CallNonvirtualObjectMethod(env_, field.get(), field_cls_, field_get_, nullptr);
      if (collection && JNI_IsInstanceOf(env_, collection, expected_type)) {
        cleared = true;
        auto iterator = JNI_CallObjectMethod(env_, collection, iterable_iterator_);
        while (JNI_CallBooleanMethod(env_, iterator, iterator_hasNext_)) {
          auto callback = JNI_CallObjectMethod(env_, iterator, iterator_next_);
          if (has_wrapper) {
            if (auto wrapped_callback = GetFirstNonNullInstanceField(callback)) {
              callback.reset(wrapped_callback);
            }
          }
          InsertUnique(cleared_callbacks_, GetObjectString(callback));
        }
        JNI_CallVoidMethod(env_, collection, collection_clear_);
      }
    }

    return cleared;
  }

  auto GetFrameworkName(ScopedLocalRef<jclass>& cls) -> std::string {
    auto get_framework_name_mid = JNI_GetMethodID(env_, cls, "getFrameworkName", "()Ljava/lang/String;");
    if (!get_framework_name_mid) return {};

    auto get_framework_version_mid = JNI_GetMethodID(env_, cls, "getFrameworkVersion", "()Ljava/lang/String;");
    if (!get_framework_version_mid) return {};

    auto get_framework_version_code_mid = JNI_GetMethodID(env_, cls, "getFrameworkVersionCode", "()J");
    if (!get_framework_version_code_mid) return {};

    auto xposed_module = JNI_AllocObject(env_, cls);
    if (!xposed_module) return {};

    auto name_jstr =
        JNI_Cast<jstring>(JNI_CallNonvirtualObjectMethod(env_, xposed_module, cls, get_framework_name_mid));
    auto version_jstr =
        JNI_Cast<jstring>(JNI_CallNonvirtualObjectMethod(env_, xposed_module, cls, get_framework_version_mid));
    auto version_code = JNI_CallNonvirtualLongMethod(env_, xposed_module, cls, get_framework_version_code_mid);

    if (name_jstr && version_jstr) {
      auto name = JUTFString{name_jstr};
      auto version = JUTFString{version_jstr};
      return FormatString("%s %s (%lld)", name.get(), version.get(), static_cast<long long>(version_code));
    } else if (name_jstr) {
      auto name = JUTFString{name_jstr};
      return name.get();
    } else {
      return {};
    }
  }

  auto GetObjectString(ScopedLocalRef<jobject>& obj) -> std::string {
    auto cls = JNI_GetObjectClass(env_, obj);
    auto class_name_jstr = JNI_Cast<jstring>(JNI_CallNonvirtualObjectMethod(env_, cls, class_cls_, class_getName_));
    auto class_name = JUTFString{class_name_jstr};
    auto hash_code = JNI_CallStaticIntMethod(env_, system_cls_, system_identityHashCode_, obj);
    return FormatString("%s@%x", class_name.get(), hash_code);
  }

  auto GetFirstNonNullInstanceField(ScopedLocalRef<jobject>& obj) -> jobject {
    auto cls = JNI_GetObjectClass(env_, obj);

    auto fields =
        JNI_Cast<jobjectArray>(JNI_CallNonvirtualObjectMethod(env_, cls, class_cls_, class_getDeclaredFields_));
    if (!fields) return nullptr;

    for (auto& field : fields) {
      JNI_CallNonvirtualVoidMethod(env_, field.get(), field_cls_, field_setAccessible_, JNI_TRUE);
      auto modifiers = JNI_CallNonvirtualIntMethod(env_, field.get(), field_cls_, field_getModifiers_);
      if (modifiers & kAccStatic) continue;

      auto instance = JNI_CallNonvirtualObjectMethod(env_, field.get(), field_cls_, field_get_, obj);
      if (instance) {
        return instance.release();
      }
    }

    return nullptr;
  }

  static constexpr jint kAccStatic = 0x0008;

  JNIEnv* env_;

  ScopedLocalRef<jclass> class_cls_;
  ScopedLocalRef<jclass> field_cls_;
  ScopedLocalRef<jclass> collection_cls_;
  ScopedLocalRef<jclass> key_set_view_cls_;
  ScopedLocalRef<jclass> system_cls_;

  ScopedLocalRef<jclass> xposed_interface_cls_;

  jmethodID class_getDeclaredFields_;
  jmethodID class_getName_;
  jmethodID class_getSimpleName_;
  jmethodID class_getInterfaces_;
  jmethodID field_setAccessible_;
  jmethodID field_get_;
  jmethodID field_getModifiers_;
  jmethodID collection_clear_;
  jmethodID iterable_iterator_;
  jmethodID iterator_hasNext_;
  jmethodID iterator_next_;
  jmethodID system_identityHashCode_;

  bool legacy_cleared_{false};
  bool modern_cleared_{false};
};

class Unsafe {
 public:
  explicit Unsafe(JNIEnv* env) : env_{env}, unsafe_{env, nullptr}, object_arr_{env, nullptr} {
    auto unsafe_cls = JNI_FindClass(env, "sun/misc/Unsafe");
    unsafe_ = JNI_AllocObject(env, unsafe_cls);

    object_arr_ = JNI_NewObjectArray(env, 1, JNI_FindClass(env, "java/lang/Object"), nullptr);
    auto array_base_offset_mid = JNI_GetMethodID(env, unsafe_cls, "arrayBaseOffset", "(Ljava/lang/Class;)I");
    auto object_arr_cls = JNI_GetObjectClass(env, object_arr_.get());
    object_arr_base_off_ = JNI_CallNonvirtualIntMethod(env, unsafe_, unsafe_cls, array_base_offset_mid, object_arr_cls);
    get_int_mid_ = JNI_GetMethodID(env, unsafe_cls, "getInt", "(Ljava/lang/Object;J)I");
    put_int_mid_ = JNI_GetMethodID(env, unsafe_cls, "putInt", "(Ljava/lang/Object;JI)V");
  }

  auto GetInt(jobject obj, jlong offset) { return JNI_CallIntMethod(env_, unsafe_, get_int_mid_, obj, offset); }

  void PutInt(jobject obj, jlong offset, jint x) { JNI_CallVoidMethod(env_, unsafe_, put_int_mid_, obj, offset, x); }

  auto GetObjectAddress(jobject obj) {
    object_arr_[0] = obj;
    return static_cast<uint32_t>(GetInt(object_arr_.get(), object_arr_base_off_));
  }

  template <typename T>
  auto NewLocalRef(uint32_t addr) {
    PutInt(object_arr_.get(), object_arr_base_off_, static_cast<jint>(addr));
    return reinterpret_cast<T>(object_arr_[0].release());
  }

 private:
  JNIEnv* env_;
  ScopedLocalRef<jobject> unsafe_;
  ScopedLocalRef<jobjectArray> object_arr_;
  jmethodID get_int_mid_;
  jmethodID put_int_mid_;
  jint object_arr_base_off_;
};

auto GetClassNameList(JNIEnv* env,
                      ScopedLocalRef<jclass>& cls,
                      jfieldID dex_cache_fid,
                      jfieldID dex_file_fid,
                      ScopedLocalRef<jclass>& dex_file_cls,
                      jmethodID get_class_name_list_mid) {
  auto dex_cache = JNI_GetObjectField(env, cls, dex_cache_fid);
  auto native_dex_file = JNI_GetLongField(env, dex_cache, dex_file_fid);
  auto cookie = JNI_NewLongArray(env, 2);
  cookie[1] = native_dex_file;
  cookie.commit();
  return JNI_Cast<jobjectArray>(JNI_CallStaticObjectMethod(env, dex_file_cls, get_class_name_list_mid, cookie));
}

auto GetLSPEntryMethod(void* entry_point) -> void* {
#if defined(__aarch64__)
  auto code = static_cast<uint32_t*>(entry_point);
  for (size_t i = 0; 8 > i; ++i) {
    if (code[i] != 0x58000060) continue;
    if ((code[i + 1] & 0xFFF00FFF) != 0xF8400010) continue;
    if (code[i + 2] != 0xD61F0200) continue;
    return *reinterpret_cast<void**>(&code[i + 3]);
  }
#elif defined(__arm__)
  auto code = static_cast<uint32_t*>(entry_point);
  for (size_t i = 0; 8 > i; ++i) {
    if (code[i] != 0xE59F0000) continue;
    if ((code[i + 1] & 0xFFFFFF00) != 0xE590FF00) continue;
    return *reinterpret_cast<void**>(&code[i + 2]);
  }
#elif defined(__i386__)
  auto code = static_cast<uint8_t*>(entry_point);
  for (size_t i = 0; 32 > i; ++i) {
    if (code[i] != 0xB8) continue;
    if (code[i + 5] != 0xFF) continue;
    if (code[i + 6] != 0x70) continue;
    if (code[i + 8] != 0xC3) continue;
    return *reinterpret_cast<void**>(&code[i + 1]);
  }
#elif defined(__x86_64__)
  auto code = static_cast<uint8_t*>(entry_point);
  for (size_t i = 0; 32 > i; ++i) {
    if (code[i] != 0x48) continue;
    if (code[i + 1] != 0xBF) continue;
    if (code[i + 10] != 0xFF) continue;
    if (code[i + 11] != 0x77) continue;
    if (code[i + 13] != 0xC3) continue;
    return *reinterpret_cast<void**>(&code[i + 2]);
  }
#elif defined(__riscv)
  auto code = static_cast<uint32_t*>(entry_point);
  for (size_t i = 0; 8 > i; ++i) {
    if (code[i] != 0x00000517) continue;
    if (code[i + 1] != 0x01053503) continue;
    if ((code[i + 2] & 0xF00FFFFF) != 0x00053F83) continue;
    if (code[i + 3] != 0x000F8067) continue;
    return *reinterpret_cast<void**>(&code[i + 4]);
  }
#endif
  return nullptr;
}

auto FindFrameworkAPIClassAndClassLoaderByHookedMethod(JNIEnv* env,
                                                       ScopedLocalRef<jclass>& class_cls,
                                                       jmethodID get_class_loader_mid,
                                                       jfieldID art_method_fid,
                                                       size_t art_method_size,
                                                       Unsafe& unsafe)
    -> std::optional<std::pair<ScopedLocalRef<jclass>, ScopedLocalRef<jobject>>> {
  auto loaded_apk_cls = JNI_FindClass(env, "android/app/LoadedApk");
  auto hooked_mid = JNI_GetMethodID(env,
                                    loaded_apk_cls,
                                    "<init>",
                                    "(Landroid/app/ActivityThread;Landroid/content/pm/ApplicationInfo;Landroid/content/"
                                    "res/CompatibilityInfo;Ljava/lang/ClassLoader;ZZZ)V");
  if (!hooked_mid) {
    hooked_mid = JNI_GetMethodID(env, loaded_apk_cls, "createOrUpdateClassLoaderLocked", "(Ljava/util/List;)V");
    if (!hooked_mid) return {};
  }
  auto hooked_method = JNI_ToReflectedMethod(env, loaded_apk_cls, hooked_mid, JNI_FALSE);
  auto art_method = JNI_GetLongField(env, hooked_method, art_method_fid);
  auto entry_point = *reinterpret_cast<void**>(static_cast<uintptr_t>(art_method) + art_method_size - sizeof(void*));
  auto hooker_art_method = GetLSPEntryMethod(entry_point);
  if (!hooker_art_method) return {};
  auto hooker_cls = unsafe.NewLocalRef<jclass>(*static_cast<uint32_t*>(hooker_art_method));
  auto get_declared_fields_mid = JNI_GetMethodID(env, class_cls, "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
  auto fields =
      JNI_Cast<jobjectArray>(JNI_CallNonvirtualObjectMethod(env, hooker_cls, class_cls, get_declared_fields_mid));
  if (!fields || fields.size() != 1) return {};
  auto callback_fid = JNI_FromReflectedField(env, fields[0]);
  if (!callback_fid) return {};
  auto callback = JNI_GetStaticObjectField(env, hooker_cls, callback_fid);
  if (!callback) return {};
  auto callback_cls = JNI_GetObjectClass(env, callback);
  auto class_loader = JNI_CallNonvirtualObjectMethod(env, callback_cls, class_cls, get_class_loader_mid);
  return std::pair{std::move(callback_cls), std::move(class_loader)};
}

auto FindFrameworkAPIClassAndClassLoaderByStackTrace(JNIEnv* env,
                                                     ScopedLocalRef<jclass>& class_cls,
                                                     ScopedLocalRef<jclass>& dex_file_cls,
                                                     jmethodID get_class_loader_mid,
                                                     jfieldID dex_cache_fid,
                                                     jfieldID dex_file_fid,
                                                     jmethodID get_class_name_list_mid)
    -> std::optional<std::pair<ScopedLocalRef<jclass>, ScopedLocalRef<jobject>>> {
  auto finder = [&] -> std::optional<std::pair<ScopedLocalRef<jclass>, ScopedLocalRef<jobject>>> {
    if (!env->ExceptionCheck()) {
      return {};
    }

    auto exception = ScopedLocalRef{env, env->ExceptionOccurred()};
    env->ExceptionClear();

    auto throwable_cls = JNI_FindClass(env, "java/lang/Throwable");
    auto backtrace_fid = JNI_GetFieldID(env, throwable_cls, "backtrace", "Ljava/lang/Object;");
    auto backtrace = JNI_Cast<jobjectArray>(JNI_GetObjectField(env, exception, backtrace_fid));
    auto boot_class_loader = JNI_CallNonvirtualObjectMethod(env, throwable_cls, class_cls, get_class_loader_mid);
    ScopedLocalRef<jclass> framework_api_class{env};
    ScopedLocalRef<jobject> framework_api_class_loader{env};

    for (jsize i = 2, len = static_cast<jsize>(backtrace.size()); i < len; ++i) {
      auto element = JNI_Cast<jclass>(backtrace[i]);
      if (!element) continue;
      auto class_loader = JNI_CallNonvirtualObjectMethod(env, element, class_cls, get_class_loader_mid);
      if (!class_loader) continue;

      if (JNI_IsSameObject(env, class_loader, boot_class_loader)) {
        continue;
      }

      if (!framework_api_class_loader) {
        framework_api_class.reset(element.release());
        framework_api_class_loader.reset(class_loader.release());
        continue;
      }

      if (JNI_IsSameObject(env, class_loader, framework_api_class_loader)) {
        framework_api_class.reset(element.release());
        continue;
      }

      auto class_names =
          GetClassNameList(env, element, dex_cache_fid, dex_file_fid, dex_file_cls, get_class_name_list_mid);
      if (class_names.size() == 1) {
        return std::pair{std::move(framework_api_class), std::move(framework_api_class_loader)};
      }

      framework_api_class.reset(element.release());
      framework_api_class_loader.reset(class_loader.release());
    }

    return {};
  };

  std::array<jvalue, 7> null_args{};

  auto load_dex_mid = JNI_GetStaticMethodID(
      env, dex_file_cls, "loadDex", "(Ljava/lang/String;Ljava/lang/String;I)Ldalvik/system/DexFile;");
  if (load_dex_mid) {
    env->CallStaticObjectMethodA(dex_file_cls.get(), load_dex_mid, null_args.data());
    if (auto result = finder()) return result;
  }

  auto loaded_apk_cls = JNI_FindClass(env, "android/app/LoadedApk");
  auto loaded_apk_init_mid =
      JNI_GetMethodID(env,
                      loaded_apk_cls,
                      "<init>",
                      "(Landroid/app/ActivityThread;Landroid/content/pm/ApplicationInfo;Landroid/content/"
                      "res/CompatibilityInfo;Ljava/lang/ClassLoader;ZZZ)V");
  if (loaded_apk_init_mid) {
    env->NewObjectA(loaded_apk_cls.get(), load_dex_mid, null_args.data());
    if (auto result = finder()) return result;
  }

  auto create_or_update_class_loader_locked_mid =
      JNI_GetMethodID(env, loaded_apk_cls, "createOrUpdateClassLoaderLocked", "(Ljava/util/List;)V");
  if (create_or_update_class_loader_locked_mid) {
    auto instance = JNI_AllocObject(env, loaded_apk_cls);
    env->CallVoidMethodA(instance.get(), create_or_update_class_loader_locked_mid, null_args.data());
    return finder();
  }

  return {};
}

auto CollectIndirectRefTables() {
  static constexpr auto kTargetName = "[anon:dalvik-indirect ref table]"sv;

  std::unordered_map<uintptr_t, uintptr_t> tables;

  for (auto& vma : MapsParser{kVmaRead | kVmaWrite}) {
    if (vma.name != kTargetName) continue;
    tables.emplace(vma.vma_start, vma.vma_end);
  }
  return tables;
}

auto FindGlobalRefTable(JavaVM* vm) -> std::optional<std::span<uint32_t>> {
  auto indirect_ref_tables = CollectIndirectRefTables();
  if (indirect_ref_tables.empty()) {
    return {};
  }

  auto mem = reinterpret_cast<uintptr_t*>(vm + 1);
  for (size_t i = 0; i < 256; ++i) {
    if (mem[i + 1] != 2 /* IndirectRefKind::kGlobal */) continue;
    if (mem[i + 2] > 1'000'000) continue;
    if (mem[i + 3] > 1'000'000) continue;

    if (std::find_if(indirect_ref_tables.begin(), indirect_ref_tables.end(), [addr = mem[i]](const auto& p) {
          return p.first <= addr && addr < p.second;
        }) == indirect_ref_tables.end())
      continue;

    auto global_ref_table = reinterpret_cast<uint32_t*>(mem[i]);
    auto global_ref_count = static_cast<size_t>(mem[i + 2]);

    if (!global_ref_table || !global_ref_count) continue;
    return std::span{global_ref_table, global_ref_count};
  }

  return {};
}

auto xdl_sym(void* handle, const char* symbol) {
  return ::xdl_sym(handle, symbol, nullptr) ?: xdl_dsym(handle, symbol, nullptr);
}

void VisitJNIGlobalReferences(JavaVM* vm, LambdaRootVisitorType visitor) {
  auto vm_library_path = "libart.so";
  if (Dl_info info{}; dladdr(reinterpret_cast<void*>(vm->functions->GetEnv), &info) && info.dli_fname) {
    vm_library_path = info.dli_fname;
  }

  if (auto art = xdl_open(vm_library_path, XDL_DEFAULT)) {
    auto visit_roots = reinterpret_cast<void (*)(JavaVM*, RootVisitor*)>(
        xdl_sym(art, "_ZN3art9JavaVMExt10VisitRootsEPNS_11RootVisitorE"));
    xdl_close(art);
    if (visit_roots) {
      LambdaRootVisitor root_visitor{visitor};
      visit_roots(vm, &root_visitor);
      return;
    }
  }

  if (auto global_ref_table = FindGlobalRefTable(vm)) {
    for (size_t i = 0; i < global_ref_table->size(); ++i) {
      auto ref = global_ref_table->data()[i * 2 + 1];
      if (ref == 0) continue;
      auto object = reinterpret_cast<Object*>(static_cast<uintptr_t>(ref));
      visitor(object, RootInfo{art::kRootJNIGlobal});
    }
  }
}

void RemapExecutableSegmentsForArt(JavaVM* vm) {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(vm->functions->GetEnv), &info)) return;

  auto fd = raw_open(info.dli_fname, O_RDONLY, 0);
  if (fd < 0) return;

  auto size = raw_lseek(fd, 0, SEEK_END);
  auto elf = static_cast<ElfW(Ehdr)*>(raw_mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
  if (reinterpret_cast<uintptr_t>(elf) >= -4095UL) {
    raw_close(fd);
    return;
  }

  for (auto phdr = reinterpret_cast<ElfW(Phdr)*>(reinterpret_cast<uintptr_t>(elf) + elf->e_phoff),
            phdr_limit = phdr + elf->e_phnum;
       phdr < phdr_limit;
       ++phdr) {
    if (phdr->p_type != PT_LOAD) continue;
    if ((phdr->p_flags & PF_X) == 0) continue;
    if ((phdr->p_flags & PF_W) != 0) continue;

    auto segment_addr = __builtin_align_down(static_cast<char*>(info.dli_fbase) + phdr->p_vaddr, phdr->p_align);
    auto segment_size = __builtin_align_up(phdr->p_memsz, static_cast<size_t>(getpagesize()));
    auto segment_offset = __builtin_align_down(static_cast<off_t>(phdr->p_offset), phdr->p_align);

    auto segment_prot = PROT_EXEC;
    if (phdr->p_flags & PF_R) segment_prot |= PROT_READ;

    auto map = raw_mmap(segment_addr, segment_size, segment_prot, MAP_PRIVATE | MAP_FIXED, fd, segment_offset);
    if (reinterpret_cast<uintptr_t>(map) >= -4095UL) {
      continue;
    }
    __builtin___clear_cache(segment_addr, segment_addr + segment_size);
    is_art_restored_ = true;
  }

  raw_munmap(elf, size);
  raw_close(fd);
}

auto ToStringArray(JNIEnv* env, const std::vector<std::string>& vec) {
  auto string_cls = JNI_FindClass(env, "java/lang/String");
  auto arr = JNI_NewObjectArray(env, static_cast<jsize>(vec.size()), string_cls, nullptr);
  jsize i = 0;
  for (const auto& s : vec) {
    auto jstr = JNI_NewStringUTF(env, s);
    arr[i++] = jstr.get();
  }
  return arr.release();
}
}  // namespace

extern "C" {
[[gnu::visibility("default")]]
auto Java_io_github_eirv_disablelsposed_Native_nGetUnhookedMethods([[maybe_unused]] JNIEnv* env, jclass)
    -> jobjectArray {
#ifdef USE_SPANNABLE_STRING_BUILDER
  return nullptr;
#else
  return ToStringArray(env, unhooked_methods_);
#endif
}

[[gnu::visibility("default")]]
auto Java_io_github_eirv_disablelsposed_Native_nGetUnhookedMethodList([[maybe_unused]] JNIEnv* env, jclass) -> jobject {
#ifdef USE_SPANNABLE_STRING_BUILDER
  return unhooked_method_list_;
#else
  return nullptr;
#endif
}

[[gnu::visibility("default")]]
auto Java_io_github_eirv_disablelsposed_Native_nGetFrameworkName(JNIEnv* env, jclass) -> jstring {
  return env->NewStringUTF(framework_name_.c_str());
}

[[gnu::visibility("default")]]
auto Java_io_github_eirv_disablelsposed_Native_nGetClearedCallbacks(JNIEnv* env, jclass) -> jobjectArray {
  return ToStringArray(env, cleared_callbacks_);
}

[[gnu::visibility("default")]]
auto Java_io_github_eirv_disablelsposed_Native_nGetFlags(JNIEnv*, jclass) -> jint {
  jint flags = 0;
  if (is_lsposed_disabled_) flags |= 1 << 0;
  if (is_art_restored_) flags |= 1 << 1;
  return flags;
}
}

auto JNI_OnLoad(JavaVM* vm, void*) -> jint {
  JNIEnv* env;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }

  Unsafe unsafe{env};

  auto method_cls = JNI_FindClass(env, "java/lang/reflect/Method");
  auto class_cls = JNI_GetObjectClass(env, method_cls);
  auto method_get_name_mid = JNI_GetMethodID(env, method_cls, "getName", "()Ljava/lang/String;");
#ifdef USE_SPANNABLE_STRING_BUILDER
  auto method_get_parameter_types_mid = JNI_GetMethodID(env, method_cls, "getParameterTypes", "()[Ljava/lang/Class;");
  auto method_get_return_type_mid = JNI_GetMethodID(env, method_cls, "getReturnType", "()Ljava/lang/Class;");
#else
  auto class_get_name_mid = JNI_GetMethodID(env, class_cls, "getName", "()Ljava/lang/String;");
#endif
  auto executable_cls = JNI_GetSuperclass(env, method_cls);

  auto declaring_class_fid = JNI_GetFieldID(env, executable_cls, "declaringClass", "Ljava/lang/Class;");
  auto declaring_class_field = JNI_ToReflectedField(env, executable_cls, declaring_class_fid, JNI_FALSE);
  auto field_cls = JNI_GetObjectClass(env, declaring_class_field);
  auto offset_fid = JNI_GetFieldID(env, field_cls, "offset", "I");
  auto declaring_class_off = JNI_GetIntField(env, declaring_class_field, offset_fid);

  auto art_method_fid = JNI_GetFieldID(env, executable_cls, "artMethod", "J");
  auto art_method_field = JNI_ToReflectedField(env, executable_cls, art_method_fid, JNI_FALSE);
  auto art_method_off = JNI_GetIntField(env, art_method_field, offset_fid);

  auto methods_fid = JNI_GetFieldID(env, class_cls, "methods", "J");
  auto methods_field = JNI_ToReflectedField(env, class_cls, methods_fid, JNI_FALSE);
  auto methods_off = JNI_GetIntField(env, methods_field, offset_fid);

  auto method_cls_addr = unsafe.GetObjectAddress(method_cls.get());

  size_t art_method_size = 0;
  {
    auto methods = static_cast<uintptr_t>(*reinterpret_cast<uint64_t*>(method_cls_addr + methods_off));
    auto art_method = reinterpret_cast<uint32_t*>(methods + sizeof(size_t));
    for (size_t i = 5; i < 32; ++i) {
      if (art_method[i] != method_cls_addr) continue;
      art_method_size = i * sizeof(uint32_t);
      break;
    }
  }

  auto dex_file_cls = JNI_FindClass(env, "dalvik/system/DexFile");
  auto get_class_loader_mid = JNI_GetMethodID(env, class_cls, "getClassLoader", "()Ljava/lang/ClassLoader;");
  auto dex_cache_cls = JNI_FindClass(env, "java/lang/DexCache");
  auto dex_cache_fid = JNI_GetFieldID(env, class_cls, "dexCache", "Ljava/lang/Object;");
  auto dex_file_fid = JNI_GetFieldID(env, dex_cache_cls, "dexFile", "J");
  auto get_class_name_list_mid =
      JNI_GetStaticMethodID(env, dex_file_cls, "getClassNameList", "(Ljava/lang/Object;)[Ljava/lang/String;");

  auto maybe_framework_api =
      FindFrameworkAPIClassAndClassLoaderByHookedMethod(env, class_cls, get_class_loader_mid, art_method_fid, art_method_size, unsafe) ?:
          FindFrameworkAPIClassAndClassLoaderByStackTrace(
              env, class_cls, dex_file_cls, get_class_loader_mid, dex_cache_fid, dex_file_fid, get_class_name_list_mid);

  if (maybe_framework_api) {
    auto& [framework_api_class, framework_api_class_loader] = *maybe_framework_api;
    XposedCallbackHelper helper{env};

    auto for_name_mid = JNI_GetStaticMethodID(
        env, class_cls, "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");

    auto names =
        GetClassNameList(env, framework_api_class, dex_cache_fid, dex_file_fid, dex_file_cls, get_class_name_list_mid);

    auto native_methods = std::array{JNINativeMethod{
        "hookMethod",
        "(ZLjava/lang/reflect/Executable;Ljava/lang/Class;ILjava/lang/Object;)Z",
        reinterpret_cast<void*>(
            +[](JNIEnv*, jclass, jboolean, jobject, jobject, jint, jobject) -> jboolean { return JNI_TRUE; })}};

    for (auto& name : names) {
      auto current_class = JNI_Cast<jclass>(
          JNI_CallStaticObjectMethod(env, class_cls, for_name_mid, name, JNI_FALSE, framework_api_class_loader));
      if (!current_class) {
        continue;
      }
      helper.ClearXposedCallbacks(current_class);

      if (!is_lsposed_disabled_) {
        if (env->GetStaticMethodID(current_class.get(), native_methods[0].name, native_methods[0].signature) ==
            nullptr) {
          env->ExceptionClear();
          continue;
        }
        if (JNI_RegisterNatives(env, current_class, native_methods.data(), native_methods.size()) != JNI_OK) {
          continue;
        }
        is_lsposed_disabled_ = true;
      }
    }
  }

#ifdef USE_SPANNABLE_STRING_BUILDER
  auto array_list_cls = JNI_FindClass(env, "java/util/ArrayList");
  auto array_list_init_mid = JNI_GetMethodID(env, array_list_cls, "<init>", "()V");
  auto array_list_add_mid = JNI_GetMethodID(env, array_list_cls, "add", "(Ljava/lang/Object;)Z");
  auto array_list = JNI_NewGlobalRef(env, JNI_NewObject(env, array_list_cls, array_list_init_mid));
  unhooked_method_list_ = array_list;
  DescriptorBuilder db{env};
#endif

  auto compiler_cls = JNI_FindClass(env, "java/lang/Compiler");
  auto enable_mid = JNI_GetStaticMethodID(env, compiler_cls, "enable", "()V");
  auto stub_method = JNI_ToReflectedMethod(env, compiler_cls, enable_mid, JNI_TRUE);

  VisitJNIGlobalReferences(vm, [&](auto object, auto) {
    auto ref = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(object));
    if (ref == 0) return;
    auto ref_class_addr = *reinterpret_cast<uint32_t*>(ref);
    if (ref_class_addr != method_cls_addr) return;

    auto art_method = static_cast<uintptr_t>(*reinterpret_cast<uint64_t*>(ref + art_method_off));
    auto target_class_addr = *reinterpret_cast<uint32_t*>(art_method);
    auto declaring_class_addr = *reinterpret_cast<uint32_t*>(ref + declaring_class_off);
    if (target_class_addr == declaring_class_addr) return;

    auto methods = static_cast<uintptr_t>(*reinterpret_cast<uint64_t*>(target_class_addr + methods_off));
    auto method_count = *reinterpret_cast<size_t*>(methods);

    for (size_t j = 0; j < method_count; ++j) {
      auto method = reinterpret_cast<uint32_t*>(j * art_method_size + methods + sizeof(size_t));
      if (method[2] != reinterpret_cast<uint32_t*>(art_method)[2]) continue;

      auto access_flags = method[1];
      if (!GetLSPEntryMethod(*reinterpret_cast<void**>(art_method + art_method_size - sizeof(void*)))) {
        memcpy(method, reinterpret_cast<void*>(art_method), art_method_size);
        method[1] = access_flags;
      } else {
        access_flags |= 0x1000 /* kAccSynthetic */;
      }

      auto target_cls = ScopedLocalRef{env, unsafe.NewLocalRef<jclass>(target_class_addr)};
      JNI_SetLongField(env, stub_method, art_method_fid, reinterpret_cast<jlong>(method));
      auto target_method_name_jstr =
          JNI_Cast<jstring>(JNI_CallNonvirtualObjectMethod(env, stub_method, method_cls, method_get_name_mid));

#ifdef USE_SPANNABLE_STRING_BUILDER
      auto target_parameter_types =
          JNI_CallNonvirtualObjectMethod(env, stub_method, method_cls, method_get_parameter_types_mid);
      auto target_return_type =
          JNI_CallNonvirtualObjectMethod(env, stub_method, method_cls, method_get_return_type_mid);
      auto descriptor = DescriptorBuilder::GetDescriptor(env,
                                                         target_cls.get(),
                                                         target_method_name_jstr.get(),
                                                         reinterpret_cast<jobjectArray>(target_parameter_types.get()),
                                                         reinterpret_cast<jclass>(target_return_type.get()),
                                                         static_cast<jint>(access_flags));
      JNI_CallBooleanMethod(env, array_list, array_list_add_mid, descriptor);
      env->DeleteLocalRef(descriptor);
#else
      auto target_cls_name_jstr =
          JNI_Cast<jstring>(JNI_CallNonvirtualObjectMethod(env, target_cls, class_cls, class_get_name_mid));
      auto target_cls_name = JUTFString{target_cls_name_jstr};
      auto target_method_name = JUTFString{target_method_name_jstr};
      unhooked_methods_.push_back(target_cls_name.get() + "::"s + target_method_name.get());
#endif
      break;
    }
  });

  RemapExecutableSegmentsForArt(vm);

  return JNI_VERSION_1_6;
}
