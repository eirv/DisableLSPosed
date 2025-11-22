package io.github.eirv.disablelsposed

import android.app.AppComponentFactory

class AppFactory : AppComponentFactory() {
  companion object {
    init {
      Native.getFlags()
    }
  }
}