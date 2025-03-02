package io.github.eirv.disablelsposed;

import android.app.Application;

public class App extends Application {
    static {
        Native.getFlags();
    }
}
