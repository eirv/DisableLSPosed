package io.github.eirv.disablelsposed

object Native {
  init {
    System.loadLibrary("disablelsposed")
  }

  @JvmStatic
  external fun getFlags(): Int

  @JvmStatic
  external fun getUnhookedMethods(): Array<String>

  @JvmStatic
  external fun getUnhookedMethodList(): ArrayList<CharSequence>?

  @JvmStatic
  external fun getClearedCallbacks(): Array<String>

  @JvmStatic
  external fun getFrameworkName(): String
}
