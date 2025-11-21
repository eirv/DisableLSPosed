package io.github.eirv.disablelsposed

import android.app.AppComponentFactory

class AppFactory : AppComponentFactory() {
  init {
    Native.getFlags()
  }
}
