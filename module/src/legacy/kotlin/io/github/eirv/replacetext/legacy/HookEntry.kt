package io.github.eirv.replacetext.legacy

import android.widget.TextView
import de.robv.android.xposed.IXposedHookLoadPackage
import de.robv.android.xposed.IXposedHookZygoteInit
import de.robv.android.xposed.XC_MethodHook
import de.robv.android.xposed.XposedBridge
import de.robv.android.xposed.callbacks.XC_LoadPackage
import io.github.eirv.replacetext.TextViewHandler

class HookEntry : XC_MethodHook(), IXposedHookZygoteInit, IXposedHookLoadPackage {
  override fun initZygote(startupParam: IXposedHookZygoteInit.StartupParam) {
    XposedBridge.log("HookEntry::initZygote(modulePath=${startupParam.modulePath}, startsSystemServer=${startupParam.startsSystemServer})")
    XposedBridge.hookAllMethods(TextView::class.java, "setText", this)
  }

  override fun handleLoadPackage(lpparam: XC_LoadPackage.LoadPackageParam) {
    XposedBridge.log("HookEntry::handleLoadPackage(packageName=${lpparam.packageName}, processName=${lpparam.processName}, classLoader=${lpparam.classLoader}, appInfo=${lpparam.appInfo}, isFirstApplication=${lpparam.isFirstApplication})")
  }

  override fun beforeHookedMethod(param: MethodHookParam) {
    TextViewHandler.handleSetText(param.args)
  }
}
