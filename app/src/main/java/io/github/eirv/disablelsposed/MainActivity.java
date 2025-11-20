package io.github.eirv.disablelsposed;

import android.app.Activity;
import android.os.Build;
import android.os.Bundle;
import android.text.Html;
import android.text.SpannableStringBuilder;
import android.widget.HorizontalScrollView;
import android.widget.ScrollView;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.Arrays;

public class MainActivity extends Activity {
  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);

    var actionBar = getActionBar();
    if (actionBar != null) {
      actionBar.setSubtitle("Pid: " + android.os.Process.myPid());
    }

    var methodList = Native.getUnhookedMethodList();
    if (methodList == null) {
      var methods = Native.getUnhookedMethods();
      methodList = new ArrayList<>(methods.length);
      methodList.addAll(Arrays.asList(methods));
    }
    var flags = Native.getFlags();

    var success = getString(R.string.success);
    var failure = getString(R.string.failure);
    var message =
        getString(
                R.string.message,
                (flags & 1) != 0 ? success : failure,
                (flags & (1 << 1)) != 0 ? success : failure,
                methodList.size())
            .replace("\n", "<br/>");

    var sb = new SpannableStringBuilder();
    sb.append(Html.fromHtml(message, Html.FROM_HTML_MODE_LEGACY));
    for (var method : methodList) {
      if (method == null) break;
      sb.append("\n");
      sb.append(method);
    }

    var scrollView = new ScrollView(this);
    scrollView.setFillViewport(true);

    var horizontalScrollView = new HorizontalScrollView(this);
    horizontalScrollView.setFillViewport(true);

    var textView = new TextView(this);
    textView.setTextIsSelectable(true);
    textView.setText(sb);
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.VANILLA_ICE_CREAM) {
      textView.setFitsSystemWindows(true);
    }

    horizontalScrollView.addView(
        textView, ScrollView.LayoutParams.MATCH_PARENT, ScrollView.LayoutParams.MATCH_PARENT);
    scrollView.addView(
        horizontalScrollView,
        ScrollView.LayoutParams.MATCH_PARENT,
        ScrollView.LayoutParams.MATCH_PARENT);

    setContentView(scrollView);
  }
}
