package io.github.eirv.disablelsposed;

import android.app.AppComponentFactory;

public class AppFactory extends AppComponentFactory {
  static {
    Native.getFlags();
  }
}
