package io.github.eirv.disablelsposed

import android.app.Activity
import android.os.Build
import android.os.Bundle
import android.os.Process
import android.text.Html
import android.text.SpannableStringBuilder
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.widget.HorizontalScrollView
import android.widget.ScrollView
import android.widget.TextView

class MainActivity : Activity() {
  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)

    actionBar?.subtitle = "Pid: ${Process.myPid()}"

    var methodList: ArrayList<CharSequence>? = Native.getUnhookedMethodList()
    if (methodList == null) {
      val methods = Native.getUnhookedMethods()
      methodList = ArrayList(methods.toList())
    }

    val flags = Native.getFlags()
    val clearedCallbacks = Native.getClearedCallbacks()

    val clearedCallbacksHtml = StringBuilder()
    if (clearedCallbacks.isNotEmpty()) {
      for (callback in clearedCallbacks) {
        clearedCallbacksHtml.append('\n')
        clearedCallbacksHtml.append(callback)
      }
      clearedCallbacksHtml.append('\n')
    }

    val success = getString(R.string.success)
    val failure = getString(R.string.failure)

    val message = getString(
      R.string.message,
      Native.getFrameworkName(),
      if (flags and 1 != 0) success else failure,
      if (flags and (1 shl 1) != 0) success else failure,
      clearedCallbacks.size,
      clearedCallbacksHtml.toString(),
      methodList.size
    ).replace("\n", "<br/>")

    val text = SpannableStringBuilder()
    text.append(Html.fromHtml(message, Html.FROM_HTML_MODE_LEGACY))

    for (method in methodList) {
      text.append("\n")
      text.append(method)
    }

    val scrollView = ScrollView(this).apply {
      isFillViewport = true
    }

    val horizontalScrollView = HorizontalScrollView(this).apply {
      isFillViewport = true
    }

    val textView = TextView(this).apply {
      setTextIsSelectable(true)
      this.text = text
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.VANILLA_ICE_CREAM) {
        fitsSystemWindows = true
      }
    }

    horizontalScrollView.addView(
      textView, MATCH_PARENT, MATCH_PARENT
    )

    scrollView.addView(
      horizontalScrollView, MATCH_PARENT, MATCH_PARENT
    )

    setContentView(scrollView)
  }
}
