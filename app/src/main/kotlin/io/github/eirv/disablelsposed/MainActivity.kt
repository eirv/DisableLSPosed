package io.github.eirv.disablelsposed

import android.app.Activity
import android.graphics.Typeface
import android.os.Build
import android.os.Bundle
import android.os.Process
import android.text.Html
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.TypefaceSpan
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.widget.HorizontalScrollView
import android.widget.ScrollView
import android.widget.TextView

class MainActivity : Activity() {

  object Keys {
    private val NAME = MainActivity::class.java.name
    val METHOD_LIST = "$NAME::methodList"
    val FLAGS = "$NAME::flags"
    val CLEARED_CALLBACKS = "$NAME::clearedCallbacks"
    val FRAMEWORK_NAME = "$NAME::frameworkName"
  }

  private var methodList: ArrayList<CharSequence>? = null
  private var flags: Int = 0
  private lateinit var clearedCallbacks: Array<String>
  private lateinit var frameworkName: String

  companion object {
    init {
      Native.getFlags()
    }
  }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)

    actionBar?.subtitle = "Pid: ${Process.myPid()}"

    if (savedInstanceState != null) {
      restoreState(savedInstanceState)
    } else {
      loadNativeInfo()
    }

    val text = buildDisplayText()

    val scrollView = ScrollView(this).apply {
      isFillViewport = true
    }
    val horizontalScroll = HorizontalScrollView(this).apply {
      isFillViewport = true
    }

    val textView = TextView(this).apply {
      setTextIsSelectable(true)
      this.text = text
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.VANILLA_ICE_CREAM) {
        fitsSystemWindows = true
      }
    }

    horizontalScroll.addView(textView, MATCH_PARENT, MATCH_PARENT)
    scrollView.addView(horizontalScroll, MATCH_PARENT, MATCH_PARENT)

    setContentView(scrollView)
  }

  private fun restoreState(state: Bundle) {
    methodList = state.getCharSequenceArrayList(Keys.METHOD_LIST)
    flags = state.getInt(Keys.FLAGS)
    clearedCallbacks = state.getStringArray(Keys.CLEARED_CALLBACKS) ?: emptyArray()
    frameworkName = state.getString(Keys.FRAMEWORK_NAME) ?: ""
  }

  private fun loadNativeInfo() {
    val ml = Native.getUnhookedMethodList()
    methodList = when {
      ml != null -> ml
      else -> ArrayList(Native.getUnhookedMethods().toList())
    }

    flags = Native.getFlags()
    clearedCallbacks = Native.getClearedCallbacks()
    frameworkName = Native.getFrameworkName()
  }

  private fun buildDisplayText(): SpannableStringBuilder {
    val success = getString(R.string.success)
    val failure = getString(R.string.failure)

    val clearedText = buildString {
      clearedCallbacks.forEach {
        append("\n")
        append(it)
      }
      if (isNotEmpty()) append("\n")
    }

    val header = Html.fromHtml(
      getString(
        R.string.message,
        frameworkName,
        if (flags and 1 != 0) success else failure,
        if (flags and (1 shl 1) != 0) success else failure,
        clearedCallbacks.size,
        clearedText,
        methodList?.size ?: 0
      ).replace("\n", "<br/>"), Html.FROM_HTML_MODE_LEGACY
    )

    return SpannableStringBuilder().apply {
      append(header)

      val pos = length

      methodList?.forEach {
        append("\n")
        append(it)
      }

      setSpan(TypefaceSpan(Typeface.MONOSPACE), pos, length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
    }
  }

  override fun onSaveInstanceState(outState: Bundle) {
    super.onSaveInstanceState(outState)
    outState.putCharSequenceArrayList(Keys.METHOD_LIST, methodList)
    outState.putInt(Keys.FLAGS, flags)
    outState.putStringArray(Keys.CLEARED_CALLBACKS, clearedCallbacks)
    outState.putString(Keys.FRAMEWORK_NAME, frameworkName)
  }
}
