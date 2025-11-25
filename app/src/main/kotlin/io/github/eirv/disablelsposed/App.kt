package io.github.eirv.disablelsposed

import android.app.Application

class App : Application() {
  companion object {
    init {
      Native.getFlags()
    }
  }
}