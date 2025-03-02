package io.github.eirv.disablelsposed;

public class Native {
    static {
        System.loadLibrary("disablelsposed");
    }

    public static native int getFlags();

    public static native String[] getUnhookedMethods();
}
