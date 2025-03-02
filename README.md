# DisableLSPosed

让 LSPosed 失效并恢复所有被 LSPlant hook 的方法

代码我乱写的,仅供娱乐,如需使用,以下几点需要你来完成:
 - 处理 JNI 产生的异常
 - [等待 gc 并暂停其他线程](https://github.com/canyie/pine/blob/fff37b80774a091d0e1e5c6d5c3ecabcb4082815/core/src/main/cpp/android.h#L152)
