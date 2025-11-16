#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "file_reader.h"
#include "jni_helper.hpp"
#include "linux_syscall_support.h"

using namespace std::string_literals;
using namespace std::string_view_literals;
using namespace lsplant;

static std::vector<std::string> unhooked_methods_{};
static bool is_lsposed_disabled_{};
static bool is_art_restored_{};

class XposedCallbackHelper {
 public:
  explicit XposedCallbackHelper(JNIEnv* env)
      : env_{env},
        class_cls_{JNI_FindClass(env, "java/lang/Class")},
        field_cls_{JNI_FindClass(env, "java/lang/reflect/Field")},
        collection_cls_{JNI_FindClass(env, "java/util/Collection")},
        key_set_view_cls_{JNI_FindClass(env, "java/util/concurrent/ConcurrentHashMap$KeySetView")},
        xposed_interface_cls_{env} {
    class_getDeclaredFields_ = JNI_GetMethodID(env, class_cls_, "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
    class_getSimpleName_ = JNI_GetMethodID(env, class_cls_, "getSimpleName", "()Ljava/lang/String;");
    class_getInterfaces_ = JNI_GetMethodID(env, class_cls_, "getInterfaces", "()[Ljava/lang/Class;");
    field_setAccessible_ = JNI_GetMethodID(env, field_cls_, "setAccessible", "(Z)V");
    field_get_ = JNI_GetMethodID(env, field_cls_, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
    field_getModifiers_ = JNI_GetMethodID(env, field_cls_, "getModifiers", "()I");
    collection_clear_ = JNI_GetMethodID(env, collection_cls_, "clear", "()V");
  }

  void ClearXposedCallbacks(ScopedLocalRef<jclass>& cls) {
    if (legacy_callbacks_cleared_ && modern_callbacks_cleared_) return;

    auto jstr = JNI_Cast<jstring>(JNI_CallObjectMethod(env_, cls, class_getSimpleName_));
    if (!jstr) return;
    auto simple_name = JUTFString{jstr};

    if (simple_name.get() == "XposedInterface"sv) {
      xposed_interface_cls_ = cls.clone();
      return;
    }

    if (!modern_callbacks_cleared_) {
      ClearModernCallbacks(cls);
      modern_callbacks_cleared_ = true;
      return;
    }

    if (simple_name.get() == "XposedBridge"sv) {
      ClearLegacyCallbacks(cls);
      legacy_callbacks_cleared_ = true;
    }
  }

 private:
  void ClearLegacyCallbacks(ScopedLocalRef<jclass>& cls) { ClearStaticFieldsAssignableTo(cls, collection_cls_); }

  void ClearModernCallbacks(ScopedLocalRef<jclass>& cls) {
    if (!key_set_view_cls_) return;

    auto is_xposed = false;

    if (xposed_interface_cls_) {
      if (env_->IsAssignableFrom(cls.get(), xposed_interface_cls_.get())) {
        is_xposed = true;
      }
    } else {
      auto ifaces = JNI_Cast<jobjectArray>(JNI_CallObjectMethod(env_, cls, class_getInterfaces_));
      if (!ifaces) {
        return;
      }

      for (const auto& element : ifaces) {
        auto iface = JNI_Cast<jclass>(element.clone());

        auto jstr = JNI_Cast<jstring>(JNI_CallObjectMethod(env_, iface, class_getSimpleName_));
        if (!jstr) continue;
        auto iface_simple_name = JUTFString{jstr};

        if (iface_simple_name.get() == "XposedInterface"sv) {
          xposed_interface_cls_.reset(iface.release());
          is_xposed = true;
          break;
        }
      }
    }

    if (is_xposed) {
      ClearStaticFieldsAssignableTo(cls, key_set_view_cls_);
    }
  }

  void ClearStaticFieldsAssignableTo(ScopedLocalRef<jclass>& cls, ScopedLocalRef<jclass>& expected_type) {
    if (!cls || !expected_type) return;

    auto fields = JNI_Cast<jobjectArray>(JNI_CallObjectMethod(env_, cls, class_getDeclaredFields_));
    if (!fields) {
      return;
    }

    for (const auto& element : fields) {
      auto field = element.clone();
      JNI_CallVoidMethod(env_, field, field_setAccessible_, JNI_TRUE);

      auto modifiers = JNI_CallIntMethod(env_, field, field_getModifiers_);

      constexpr jint kAccStatic = 0x0008;
      if (!(modifiers & kAccStatic)) {
        continue;
      }

      auto value = JNI_CallObjectMethod(env_, field, field_get_, nullptr);

      if (value && JNI_IsInstanceOf(env_, value, expected_type)) {
        JNI_CallVoidMethod(env_, value, collection_clear_);
      }
    }
  }

  JNIEnv* env_;
  ScopedLocalRef<jclass> class_cls_;
  ScopedLocalRef<jclass> field_cls_;
  ScopedLocalRef<jclass> collection_cls_;
  ScopedLocalRef<jclass> key_set_view_cls_;
  ScopedLocalRef<jclass> xposed_interface_cls_;

  jmethodID class_getDeclaredFields_;
  jmethodID class_getSimpleName_;
  jmethodID class_getInterfaces_;
  jmethodID field_setAccessible_;
  jmethodID field_get_;
  jmethodID field_getModifiers_;
  jmethodID collection_clear_;

  bool legacy_callbacks_cleared_{};
  bool modern_callbacks_cleared_{};
};

extern "C" {
JNIEXPORT jobjectArray Java_io_github_eirv_disablelsposed_Native_getUnhookedMethods(JNIEnv* env, jclass) {
  auto string_cls = JNI_FindClass(env, "java/lang/String");
  auto arr = JNI_NewObjectArray(env, static_cast<jsize>(unhooked_methods_.size()), string_cls, nullptr);
  jint i = 0;
  for (const auto& str : unhooked_methods_) {
    auto jstr = JNI_NewStringUTF(env, str.c_str());
    arr[i++] = jstr.get();
  }
  return arr.release();
}

JNIEXPORT jint Java_io_github_eirv_disablelsposed_Native_getFlags(JNIEnv*, jclass) {
  jint flags = 0;
  if (is_lsposed_disabled_) {
    flags |= 1 << 0;
  }
  if (is_art_restored_) {
    flags |= 1 << 1;
  }
  return flags;
}
}

static jboolean FakeHookMethod(JNIEnv*, jclass, jboolean, jobject, jobject, jint, jobject) { return JNI_TRUE; }

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

jint JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }

  auto dex_file_cls = JNI_FindClass(env, "dalvik/system/DexFile");
  auto load_dex_mid = JNI_GetStaticMethodID(
      env, dex_file_cls, "loadDex", "(Ljava/lang/String;Ljava/lang/String;I)Ldalvik/system/DexFile;");
  env->CallStaticObjectMethod(dex_file_cls.get(), load_dex_mid, nullptr, nullptr, jint{0});
  auto exception = ScopedLocalRef{env, env->ExceptionOccurred()};
  env->ExceptionClear();
  auto throwable_cls = JNI_FindClass(env, "java/lang/Throwable");
  auto backtrace_fid = JNI_GetFieldID(env, throwable_cls, "backtrace", "Ljava/lang/Object;");
  auto backtrace = JNI_Cast<jobjectArray>(JNI_GetObjectField(env, exception, backtrace_fid));
  auto class_cls = JNI_GetObjectClass(env, dex_file_cls);
  auto get_class_loader_mid = JNI_GetMethodID(env, class_cls, "getClassLoader", "()Ljava/lang/ClassLoader;");
  auto dex_cache_fid = JNI_GetFieldID(env, class_cls, "dexCache", "Ljava/lang/Object;");
  auto dex_cache_cls = JNI_FindClass(env, "java/lang/DexCache");
  auto dex_file_fid = JNI_GetFieldID(env, dex_cache_cls, "dexFile", "J");
  auto boot_class_loader = JNI_CallObjectMethod(env, dex_file_cls, get_class_loader_mid);
  auto get_class_name_list_mid =
      JNI_GetStaticMethodID(env, dex_file_cls, "getClassNameList", "(Ljava/lang/Object;)[Ljava/lang/String;");
  ScopedLocalRef<jclass> previous_class{env};
  ScopedLocalRef<jobject> previous_class_loader{env};
  auto found = false;
  for (jsize i = 2, len = backtrace.size(); len > i; ++i) {
    auto element = JNI_Cast<jclass>(backtrace[i]);
    auto class_loader = JNI_CallObjectMethod(env, element, get_class_loader_mid);
    if (JNI_IsSameObject(env, class_loader, boot_class_loader)) {
      continue;
    }
    if (!previous_class_loader) {
      previous_class.reset(element.release());
      previous_class_loader.reset(class_loader.release());
      continue;
    }
    if (JNI_IsSameObject(env, class_loader, previous_class_loader)) {
      previous_class.reset(element.release());
      continue;
    }
    auto class_names =
        GetClassNameList(env, element, dex_cache_fid, dex_file_fid, dex_file_cls, get_class_name_list_mid);
    if (class_names.size() == 1) {
      found = true;
      break;
    }
    previous_class.reset(element.release());
    previous_class_loader.reset(class_loader.release());
  }
  if (found) {
    JNINativeMethod native_methods[] = {{"hookMethod",
                                         "(ZLjava/lang/reflect/Executable;Ljava/lang/Class;ILjava/lang/Object;)Z",
                                         reinterpret_cast<void*>(FakeHookMethod)}};
    auto for_name_mid = JNI_GetStaticMethodID(
        env, class_cls, "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
    auto class_names =
        GetClassNameList(env, previous_class, dex_cache_fid, dex_file_fid, dex_file_cls, get_class_name_list_mid);
    XposedCallbackHelper helper{env};
    for (jsize len = class_names.size() - 1; len != -1; --len) {
      auto element = JNI_Cast<jstring>(class_names[len]);
      auto current_class = JNI_Cast<jclass>(
          JNI_CallStaticObjectMethod(env, class_cls, for_name_mid, element, JNI_FALSE, previous_class_loader));
      if (!current_class) {
        continue;
      }
      helper.ClearXposedCallbacks(current_class);
      if (!is_lsposed_disabled_) {
        if (JNI_GetStaticMethodID(env, current_class, native_methods[0].name, native_methods[0].signature) == nullptr) {
          continue;
        }
        if (JNI_RegisterNatives(env, current_class, native_methods, 1) != JNI_OK) {
          continue;
        }
        is_lsposed_disabled_ = true;
      }
    }
  }
  std::set<uintptr_t> indirect_ref_tables;
#ifdef __LP64__
  constexpr auto kernel_ptr_size = sizeof(void*);
#else
  auto kernel_ptr_size = raw_access("/system/bin/linker64", X_OK) == 0 ? sizeof(uint64_t) : sizeof(uint32_t);
#endif
  auto maps_name_offset = 25 + kernel_ptr_size * 6;
  for (auto line : io::FileReader{"/proc/self/maps"}) {
    if (line.size() < maps_name_offset) continue;
    auto name = line.substr(maps_name_offset);
    if (name != "[anon:dalvik-indirect ref table]") continue;
    indirect_ref_tables.emplace(strtoul(line.data(), nullptr, 16));
  }
  uint32_t* global_ref_table = nullptr;
  size_t global_ref_count = 0;
  auto mem = reinterpret_cast<uintptr_t*>(vm + 1);
  for (size_t i = 0; 512 > i; ++i) {
    if (indirect_ref_tables.find(mem[i]) == indirect_ref_tables.end()) continue;
    if (mem[i + 1] != 2 /* IndirectRefKind::kGlobal */) continue;
    if (mem[i + 3] > 1'000'000) continue;
    global_ref_table = reinterpret_cast<uint32_t*>(mem[i]);
    global_ref_count = mem[i + 2];
    break;
  }
  if (!global_ref_table) {
    return JNI_ERR;
  }
  auto unsafe_cls = JNI_FindClass(env, "sun/misc/Unsafe");
  auto unsafe = ScopedLocalRef{env, env->AllocObject(unsafe_cls.get())};
  auto method_cls = JNI_FindClass(env, "java/lang/reflect/Method");
  auto method_get_name_mid = JNI_GetMethodID(env, method_cls, "getName", "()Ljava/lang/String;");
  auto class_get_name_mid = JNI_GetMethodID(env, class_cls, "getName", "()Ljava/lang/String;");
  auto executable_cls = ScopedLocalRef{env, env->GetSuperclass(method_cls.get())};
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
  auto object_arr = JNI_NewObjectArray(env, 1, JNI_FindClass(env, "java/lang/Object"), nullptr);
  auto put_int = JNI_GetMethodID(env, unsafe_cls, "putInt", "(Ljava/lang/Object;JI)V");
  auto array_base_offset_mid = JNI_GetMethodID(env, unsafe_cls, "arrayBaseOffset", "(Ljava/lang/Class;)I");
  auto object_arr_cls = JNI_GetObjectClass(env, object_arr.get());
  auto object_arr_base_off = JNI_CallIntMethod(env, unsafe, array_base_offset_mid, object_arr_cls);
  auto put_object_mid = JNI_GetMethodID(env, unsafe_cls, "putObject", "(Ljava/lang/Object;JLjava/lang/Object;)V");
  auto get_int_mid = JNI_GetMethodID(env, unsafe_cls, "getInt", "(Ljava/lang/Object;J)I");
  JNI_CallVoidMethod(env, unsafe, put_object_mid, object_arr, static_cast<jlong>(object_arr_base_off), method_cls);
  auto method_cls_addr = static_cast<uint32_t>(
      JNI_CallIntMethod(env, unsafe, get_int_mid, object_arr, static_cast<jlong>(object_arr_base_off)));
  size_t art_method_size = 0;
  {
    auto methods = static_cast<uintptr_t>(*reinterpret_cast<uint64_t*>(method_cls_addr + methods_off));
    auto art_method = reinterpret_cast<uint32_t*>(methods + sizeof(size_t));
    for (size_t j = 1; 32 > j; ++j) {
      if (art_method[j] != method_cls_addr) continue;
      art_method_size = j * sizeof(uint32_t);
      break;
    }
  }
  auto stub_method = JNI_ToReflectedMethod(env, unsafe_cls, put_int, JNI_FALSE);
  for (size_t i = 0; global_ref_count > i; ++i) {
    auto ref = global_ref_table[i * 2 + 1];
    if (ref == 0) continue;
    auto ref_class_addr = *reinterpret_cast<uint32_t*>(ref);
    if (ref_class_addr != method_cls_addr) continue;
    auto art_method = static_cast<uintptr_t>(*reinterpret_cast<uint64_t*>(ref + art_method_off));
    auto target_class_addr = *reinterpret_cast<uint32_t*>(art_method);
    auto declaring_class_addr = *reinterpret_cast<uint32_t*>(ref + declaring_class_off);
    if (target_class_addr == declaring_class_addr) continue;
    auto methods = static_cast<uintptr_t>(*reinterpret_cast<uint64_t*>(target_class_addr + methods_off));
    auto method_count = *reinterpret_cast<size_t*>(methods);
    for (size_t j = 0; method_count > j; ++j) {
      auto method = reinterpret_cast<uint32_t*>(j * art_method_size + methods + sizeof(size_t));
      if (method[2] != reinterpret_cast<uint32_t*>(art_method)[2]) continue;
      auto access_flags = method[1];
      memcpy(method, reinterpret_cast<void*>(art_method), art_method_size);
      method[1] = access_flags;
      JNI_CallVoidMethod(env,
                         unsafe,
                         put_int,
                         object_arr,
                         static_cast<jlong>(object_arr_base_off),
                         static_cast<jint>(target_class_addr));
      auto target_cls = JNI_Cast<jclass>(object_arr[0]);
      auto target_cls_name_jstr = JNI_Cast<jstring>(JNI_CallObjectMethod(env, target_cls, class_get_name_mid));
      auto target_cls_name = JUTFString{target_cls_name_jstr};
      JNI_SetLongField(env, stub_method, art_method_fid, reinterpret_cast<jlong>(method));
      auto target_method_name_jstr = JNI_Cast<jstring>(JNI_CallObjectMethod(env, stub_method, method_get_name_mid));
      auto target_method_name = JUTFString{target_method_name_jstr};
      unhooked_methods_.push_back(target_cls_name.get() + "::"s + target_method_name.get());
      break;
    }
  }
  if (Dl_info info{}; dladdr(reinterpret_cast<void*>(vm->functions->GetEnv), &info)) {
    auto fd = raw_open(info.dli_fname, O_RDONLY, 0);
    if (fd < 0) return JNI_VERSION_1_6;
    auto size = raw_lseek(fd, 0, SEEK_END);
    auto elf = static_cast<ElfW(Ehdr)*>(raw_mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (reinterpret_cast<uintptr_t>(elf) >= -4095UL) {
      raw_close(fd);
      return JNI_VERSION_1_6;
    }
    for (auto phdr = reinterpret_cast<ElfW(Phdr)*>(reinterpret_cast<uintptr_t>(elf) + elf->e_phoff),
              phdr_limit = phdr + elf->e_phnum;
         phdr < phdr_limit;
         ++phdr) {
      if (phdr->p_type != PT_LOAD) continue;
      if ((phdr->p_flags & PF_X) == 0) continue;
      if ((phdr->p_flags & PF_W) != 0) continue;
      auto segment_addr = __builtin_align_down(static_cast<char*>(info.dli_fbase) + phdr->p_vaddr, phdr->p_align);
      auto segment_size = __builtin_align_up(phdr->p_memsz, getpagesize());
      auto segment_offset = __builtin_align_down(static_cast<off_t>(phdr->p_offset), phdr->p_align);
      auto map =
          raw_mmap(segment_addr, segment_size, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_FIXED, fd, segment_offset);
      if (reinterpret_cast<uintptr_t>(elf) < -4095UL) {
        __builtin___clear_cache(segment_addr, segment_addr + segment_size);
        is_art_restored_ = true;
      }
    }
    raw_munmap(elf, size);
    raw_close(fd);
  }
  return JNI_VERSION_1_6;
}