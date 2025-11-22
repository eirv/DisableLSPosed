package io.github.eirv.disablelsposed

import android.util.Log

object Native {
  private val loaded: Boolean

  init {
    var ok = false
    try {
      System.loadLibrary("disablelsposed")
      ok = true
    } catch (e: UnsatisfiedLinkError) {
      Log.e("Native", "Native library failed to load", e)
    }
    loaded = ok
  }

  @JvmStatic
  fun getFlags(): Int {
    if (!loaded) return 0
    return nGetFlags()
  }

  @JvmStatic
  fun getUnhookedMethods(): Array<String> {
    if (!loaded) return emptyArray()
    return nGetUnhookedMethods()
  }

  @JvmStatic
  fun getUnhookedMethodList(): ArrayList<CharSequence> {
    if (!loaded) return arrayListOf()
    return nGetUnhookedMethodList()
  }

  @JvmStatic
  fun getClearedCallbacks(): Array<String> {
    if (!loaded) return emptyArray()
    return nGetClearedCallbacks()
  }

  @JvmStatic
  fun getFrameworkName(): String {
    if (!loaded) return "LSPosed"
    return nGetFrameworkName()
  }

  @JvmStatic
  private external fun nGetFlags(): Int

  @JvmStatic
  private external fun nGetUnhookedMethods(): Array<String>

  @JvmStatic
  private external fun nGetUnhookedMethodList(): ArrayList<CharSequence>

  @JvmStatic
  private external fun nGetClearedCallbacks(): Array<String>

  @JvmStatic
  private external fun nGetFrameworkName(): String
}
