package io.github.eirv.disablelsposed;

import java.util.ArrayList;

public class Native {
  static {
    System.loadLibrary("disablelsposed");
  }

  public static native int getFlags();

  public static native String[] getUnhookedMethods();

  public static native ArrayList<CharSequence> getUnhookedMethodList();

  public static native String[] getClearedCallbacks();
}
