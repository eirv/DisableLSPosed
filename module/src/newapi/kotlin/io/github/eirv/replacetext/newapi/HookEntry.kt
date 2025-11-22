package io.github.eirv.replacetext.newapi

import android.os.Build
import android.widget.TextView
import io.github.eirv.replacetext.TextViewHandler
import io.github.libxposed.api.XposedInterface
import io.github.libxposed.api.XposedModule
import io.github.libxposed.api.XposedModuleInterface.ModuleLoadedParam
import io.github.libxposed.api.XposedModuleInterface.PackageLoadedParam

class HookEntry(base: XposedInterface, param: ModuleLoadedParam) : XposedModule(base, param) {
  init {
    log("HookEntry::HookEntry(base=${base}, param.isSystemServer=${param.isSystemServer}, param.processName=${param.processName})")
    for (method in TextView::class.java.getDeclaredMethods()) {
      if (method.name != "setText") continue
      if (method.parameterCount == 0) continue
      if (method.getParameterTypes()[0] != CharSequence::class.java) continue
      hook(method, TextViewHook::class.java)
    }
  }

  override fun onPackageLoaded(param: PackageLoadedParam) {
    val sb = StringBuilder()
    sb.append("HookEntry::onPackageLoaded(packageName=")
    sb.append(param.packageName)
    sb.append(", applicationInfo=")
    sb.append(param.applicationInfo)
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
      sb.append(", defaultClassLoader=")
      sb.append(param.defaultClassLoader)
    }
    sb.append(", classLoader=")
    sb.append(param.classLoader)
    sb.append(", isFirstPackage=")
    sb.append(param.isFirstPackage)
    sb.append(")")
    log(sb.toString())
  }

  class TextViewHook : XposedInterface.Hooker {
    companion object {
      @Suppress("unused")
      @JvmStatic
      fun before(callback: XposedInterface.BeforeHookCallback) {
        TextViewHandler.handleSetText(callback.args)
      }
    }
  }
}