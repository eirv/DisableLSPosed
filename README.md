# DisableLSPosed

[**中文**](README_zh.md)

Disable [LSPosed](https://github.com/LSPosed/LSPosed)/[LSPatch](https://github.com/LSPosed/LSPatch) and restore all methods hooked by [LSPlant](https://github.com/LSPosed/LSPlant).

* Prevent `LSPosed` from hooking any method
* Block calls to the `IXposedHookLoadPackage::handleLoadPackage` callback
* Restore methods previously hooked by `LSPosed (LSPlant)`
* Restore inline hooks to `libart.so` in memory

The code was written casually and is for entertainment purposes only. If you intend to use it, you will need to handle the following yourself:

* [Handle exceptions thrown by JNI](https://chatgpt.com)
* [Handle GC](https://github.com/canyie/pine/blob/fff37b80774a091d0e1e5c6d5c3ecabcb4082815/core/src/main/cpp/android.h#L152)

<img alt="Screenshot" src="./imgs/screenshot_en.jpg" width="480" />
