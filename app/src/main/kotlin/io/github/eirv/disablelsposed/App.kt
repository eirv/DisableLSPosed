package io.github.eirv.disablelsposed

import android.app.Application
import android.graphics.Typeface
import java.util.Map

class App : Application() {
  companion object {
    init {
      Native.getFlags()
    }
  }

  override fun onCreate() {
    super.onCreate()
    try {
      val firaCode = Typeface.createFromAsset(getAssets(), "FiraCode-Medium.ttf")

      val monospaceField = Typeface::class.java.getDeclaredField("MONOSPACE")
      monospaceField.setAccessible(true)
      monospaceField.set(null, firaCode)

      val sSystemFontMapField = Typeface::class.java.getDeclaredField("sSystemFontMap")
      sSystemFontMapField.setAccessible(true)
      @Suppress("UNCHECKED_CAST")
      val sSystemFontMap = sSystemFontMapField.get(null) as Map<String, Typeface>
      sSystemFontMap.put("monospace", firaCode)
    } catch (e: ReflectiveOperationException) {}
  }
}