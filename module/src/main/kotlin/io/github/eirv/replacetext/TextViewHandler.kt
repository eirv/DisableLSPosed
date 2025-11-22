package io.github.eirv.replacetext

object TextViewHandler {
  fun handleSetText(args: Array<Any>) {
    args.getOrNull(0)?.let { firstArg ->
      if (firstArg is String && !firstArg.endsWith("[Hooked]")) {
        args[0] = "$firstArg[Hooked]"
      }
    }
  }
}
