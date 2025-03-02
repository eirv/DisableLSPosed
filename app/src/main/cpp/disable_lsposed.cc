#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace std::string_literals;

static std::vector<std::string> unhooked_methods_;
static bool is_lsposed_disabled_ = false;
static bool is_art_restored_ = false;

extern "C" {
JNIEXPORT jobjectArray Java_io_github_eirv_disablelsposed_Native_getUnhookedMethods(JNIEnv* env, jclass) {
  auto string_cls = env->FindClass("java/lang/String");
  auto arr = env->NewObjectArray(static_cast<jsize>(unhooked_methods_.size()), string_cls, nullptr);
  env->DeleteLocalRef(string_cls);
  jint i = 0;
  for (const auto& str : unhooked_methods_) {
    auto jstr = env->NewStringUTF(str.c_str());
    env->SetObjectArrayElement(arr, i++, jstr);
    env->DeleteLocalRef(jstr);
  }
  return arr;
}

JNIEXPORT jint Java_io_github_eirv_disablelsposed_Native_getFlags(JNIEnv* env, jclass) {
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

static jboolean FakeHookMethod(JNIEnv* env, jclass, jboolean, jobject, jobject, jint, jobject) {
  return JNI_TRUE;
}

jobjectArray GetClassNameList(JNIEnv* env, jclass cls, jfieldID dex_cache_fid, jfieldID dex_file_fid, jclass dex_file_cls, jmethodID get_class_name_list_mid) {
  auto dex_cache = env->GetObjectField(cls, dex_cache_fid);
  auto native_dex_file = env->GetLongField(dex_cache, dex_file_fid);
  env->DeleteLocalRef(dex_cache);
  auto cookie = env->NewLongArray(2);
  env->SetLongArrayRegion(cookie, 1, 1, &native_dex_file);
  auto class_names = reinterpret_cast<jobjectArray>(env->CallStaticObjectMethod(dex_file_cls, get_class_name_list_mid, cookie));
  env->DeleteLocalRef(cookie);
  return class_names;
}

jint JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }

  auto dex_file_cls = env->FindClass("dalvik/system/DexFile");
  auto load_dex_mid = env->GetStaticMethodID(dex_file_cls, "loadDex", "(Ljava/lang/String;Ljava/lang/String;I)Ldalvik/system/DexFile;");
  env->CallStaticObjectMethod(dex_file_cls, load_dex_mid, nullptr, nullptr, jint{0});
  auto exception = env->ExceptionOccurred();
  env->ExceptionClear();
  auto throwable_cls = env->FindClass("java/lang/Throwable");
  auto backtrace_fid = env->GetFieldID(throwable_cls, "backtrace", "Ljava/lang/Object;");
  env->DeleteLocalRef(throwable_cls);
  auto backtrace = reinterpret_cast<jobjectArray>(env->GetObjectField(exception, backtrace_fid));
  env->DeleteLocalRef(exception);
  auto class_cls = env->GetObjectClass(dex_file_cls);
  auto get_class_loader_mid = env->GetMethodID(class_cls, "getClassLoader", "()Ljava/lang/ClassLoader;");
  auto dex_cache_fid = env->GetFieldID(class_cls, "dexCache", "Ljava/lang/Object;");
  auto dex_cache_cls = env->FindClass("java/lang/DexCache");
  auto dex_file_fid = env->GetFieldID(dex_cache_cls, "dexFile", "J");
  env->DeleteLocalRef(dex_cache_cls);
  auto boot_class_loader = env->CallObjectMethod(dex_file_cls, get_class_loader_mid);
  auto get_class_name_list_mid = env->GetStaticMethodID(dex_file_cls, "getClassNameList", "(Ljava/lang/Object;)[Ljava/lang/String;");
  jclass previous_class = nullptr;
  jobject previous_class_loader = nullptr;
  auto found = false;
  for (jsize i = 2, len = env->GetArrayLength(backtrace); len > i; ++i) {
    auto element = reinterpret_cast<jclass>(env->GetObjectArrayElement(backtrace, i));
    auto class_loader = env->CallObjectMethod(element, get_class_loader_mid);
    if (env->IsSameObject(class_loader, boot_class_loader)) {
      env->DeleteLocalRef(element);
      env->DeleteLocalRef(class_loader);
      continue;
    }
    if (!previous_class_loader) {
      previous_class = element;
      previous_class_loader = class_loader;
      continue;
    }
    if (env->IsSameObject(class_loader, previous_class_loader)) {
      env->DeleteLocalRef(previous_class);
      previous_class = element;
      env->DeleteLocalRef(class_loader);
      continue;
    }
    auto class_names = GetClassNameList(env, element, dex_cache_fid, dex_file_fid, dex_file_cls, get_class_name_list_mid);
    auto class_count = env->GetArrayLength(class_names);
    env->DeleteLocalRef(class_names);
    if (class_count == 1) {
      found = true;
      env->DeleteLocalRef(element);
      env->DeleteLocalRef(class_loader);
      break;
    }
    env->DeleteLocalRef(previous_class);
    env->DeleteLocalRef(previous_class_loader);
    previous_class = element;
    previous_class_loader = class_loader;
  }
  env->DeleteLocalRef(backtrace);
  env->DeleteLocalRef(boot_class_loader);
  if (found) {
    JNINativeMethod native_methods[] = {{"hookMethod", "(ZLjava/lang/reflect/Executable;Ljava/lang/Class;ILjava/lang/Object;)Z", reinterpret_cast<void*>(FakeHookMethod)}};
    auto for_name_mid = env->GetStaticMethodID(class_cls, "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
    auto class_names = GetClassNameList(env, previous_class, dex_cache_fid, dex_file_fid, dex_file_cls, get_class_name_list_mid);
    for (auto len = env->GetArrayLength(class_names) - 1; len != -1; --len) {
      auto element = reinterpret_cast<jstring>(env->GetObjectArrayElement(class_names, len));
      auto current_class = reinterpret_cast<jclass>(env->CallStaticObjectMethod(class_cls, for_name_mid, element, JNI_FALSE, previous_class_loader));
      env->DeleteLocalRef(element);
      if (!current_class) {
        env->ExceptionClear();
        continue;
      }
      if (env->GetStaticMethodID(current_class, native_methods[0].name, native_methods[0].signature) == nullptr) {
        env->ExceptionClear();
        env->DeleteLocalRef(current_class);
        continue;
      }
      if (env->RegisterNatives(current_class, native_methods, 1) != JNI_OK) {
        env->ExceptionClear();
        env->DeleteLocalRef(current_class);
        continue;
      }
      env->DeleteLocalRef(current_class);
      is_lsposed_disabled_ = true;
      break;
    }
    env->DeleteLocalRef(class_names);
  }
  env->DeleteLocalRef(dex_file_cls);
  env->DeleteLocalRef(previous_class_loader);
  env->DeleteLocalRef(previous_class);
  std::vector<uintptr_t> indirect_ref_tables;
  auto maps = fopen("/proc/self/maps", "r");
  char buf[256];
  while (fgets(buf, sizeof(buf), maps)) {
    if (strstr(buf, "[anon:dalvik-indirect ref table]")) {
      indirect_ref_tables.push_back(strtoul(buf, nullptr, 16));
    }
  }
  fclose(maps);
  uint32_t* global_ref_table = nullptr;
  size_t global_ref_count = 0;
  auto mem = reinterpret_cast<uintptr_t*>(vm + 1);
  for (size_t i = 0; 512 > i; ++i) {
    if (std::find(indirect_ref_tables.begin(), indirect_ref_tables.end(), mem[i]) == indirect_ref_tables.end()) continue;
    if (mem[i + 1] != 2 /* IndirectRefKind::kGlobal */) continue;
    if (mem[i + 3] > 1000000) continue;
    global_ref_table = reinterpret_cast<uint32_t*>(mem[i]);
    global_ref_count = mem[i + 2];
    break;
  }
  if (!global_ref_table) {
    return JNI_ERR;
  }
  auto unsafe_cls = env->FindClass("sun/misc/Unsafe");
  auto unsafe = env->AllocObject(unsafe_cls);
  auto method_cls = env->FindClass("java/lang/reflect/Method");
  auto method_get_name_mid = env->GetMethodID(method_cls, "getName", "()Ljava/lang/String;");
  auto class_get_name_mid = env->GetMethodID(class_cls, "getName", "()Ljava/lang/String;");
  auto executable_cls = env->GetSuperclass(method_cls);
  auto declaring_class_fid = env->GetFieldID(executable_cls, "declaringClass", "Ljava/lang/Class;");
  auto declaring_class_field = env->ToReflectedField(executable_cls, declaring_class_fid, JNI_FALSE);
  auto field_cls = env->GetObjectClass(declaring_class_field);
  auto offset_fid = env->GetFieldID(field_cls, "offset", "I");
  env->DeleteLocalRef(field_cls);
  auto declaring_class_off = env->GetIntField(declaring_class_field, offset_fid);
  env->DeleteLocalRef(declaring_class_field);
  auto art_method_fid = env->GetFieldID(executable_cls, "artMethod", "J");
  auto art_method_field = env->ToReflectedField(executable_cls, art_method_fid, JNI_FALSE);
  auto art_method_off = env->GetIntField(art_method_field, offset_fid);
  auto methods_fid = env->GetFieldID(class_cls, "methods", "J");
  auto methods_field = env->ToReflectedField(class_cls, methods_fid, JNI_FALSE);
  env->DeleteLocalRef(class_cls);
  auto methods_off = env->GetIntField(methods_field, offset_fid);
  env->DeleteLocalRef(methods_field);
  env->DeleteLocalRef(art_method_field);
  env->DeleteLocalRef(executable_cls);
  auto object_cls = env->FindClass("java/lang/Object");
  auto object_arr = env->NewObjectArray(1, object_cls, nullptr);
  env->DeleteLocalRef(object_cls);
  auto put_int = env->GetMethodID(unsafe_cls, "putInt", "(Ljava/lang/Object;JI)V");
  auto array_base_offset_mid = env->GetMethodID(unsafe_cls, "arrayBaseOffset", "(Ljava/lang/Class;)I");
  auto object_arr_cls = env->GetObjectClass(object_arr);
  auto object_arr_base_off = env->CallIntMethod(unsafe, array_base_offset_mid, object_arr_cls);
  env->DeleteLocalRef(object_arr_cls);
  auto put_object_mid = env->GetMethodID(unsafe_cls, "putObject", "(Ljava/lang/Object;JLjava/lang/Object;)V");
  auto get_int_mid = env->GetMethodID(unsafe_cls, "getInt", "(Ljava/lang/Object;J)I");
  env->CallVoidMethod(unsafe, put_object_mid, object_arr, static_cast<jlong>(object_arr_base_off), method_cls);
  auto method_cls_addr = static_cast<uint32_t>(env->CallIntMethod(unsafe, get_int_mid, object_arr, static_cast<jlong>(object_arr_base_off)));
  env->DeleteLocalRef(method_cls);
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
  auto stub_method = env->ToReflectedMethod(unsafe_cls, put_int, JNI_FALSE);
  env->DeleteLocalRef(unsafe_cls);
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
      env->CallVoidMethod(unsafe, put_int, object_arr, static_cast<jlong>(object_arr_base_off), static_cast<jint>(target_class_addr));
      auto target_cls = reinterpret_cast<jclass>(env->GetObjectArrayElement(object_arr, 0));
      auto target_cls_name_jstr = reinterpret_cast<jstring>(env->CallObjectMethod(target_cls, class_get_name_mid));
      auto target_cls_name = env->GetStringUTFChars(target_cls_name_jstr, nullptr);
      env->SetLongField(stub_method, art_method_fid, reinterpret_cast<jlong>(method));
      auto target_method_name_jstr = reinterpret_cast<jstring>(env->CallObjectMethod(stub_method, method_get_name_mid));
      auto target_method_name = env->GetStringUTFChars(target_method_name_jstr, nullptr);
      unhooked_methods_.push_back(target_cls_name + "::"s + target_method_name);
      env->ReleaseStringUTFChars(target_cls_name_jstr, target_cls_name);
      env->ReleaseStringUTFChars(target_method_name_jstr, target_method_name);
      env->DeleteLocalRef(target_method_name_jstr);
      env->DeleteLocalRef(target_cls_name_jstr);
      env->DeleteLocalRef(target_cls);
      break;
    }
  }
  env->DeleteLocalRef(stub_method);
  env->DeleteLocalRef(object_arr);
  env->DeleteLocalRef(unsafe);
  if (Dl_info info{}; dladdr(reinterpret_cast<void*>(vm->functions->GetEnv), &info)) {
    auto fd = open(info.dli_fname, O_RDONLY, 0);
    if (fd < 0) return JNI_VERSION_1_6;
    auto size = lseek(fd, 0, SEEK_END);
    auto elf = reinterpret_cast<ElfW(Ehdr)*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (elf == MAP_FAILED) {
      close(fd);
      return JNI_VERSION_1_6;
    }
    for (auto phdr = reinterpret_cast<ElfW(Phdr)*>(reinterpret_cast<uintptr_t>(elf) + elf->e_phoff), phdr_limit = phdr + elf->e_phnum; phdr < phdr_limit; ++phdr) {
      if ((phdr->p_flags & PF_X) == 0) continue;
      auto sg_addr = __builtin_align_down(reinterpret_cast<char*>(info.dli_fbase) + phdr->p_vaddr, phdr->p_align);
#pragma clang diagnostic push
#pragma ide diagnostic ignored "ArgumentSelectionDefects"
      auto sg_size = __builtin_align_up(phdr->p_memsz, getpagesize());
#pragma clang diagnostic pop
      auto page = mmap(sg_addr, sg_size, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_FIXED, fd, __builtin_align_down(static_cast<off_t>(phdr->p_offset), phdr->p_align));
      if (page != MAP_FAILED) {
        __builtin___clear_cache(sg_addr, sg_addr + sg_size);
        is_art_restored_ = true;
      }
      break;
    }
    close(fd);
    munmap(elf, size);
  }
  return JNI_VERSION_1_6;
}
