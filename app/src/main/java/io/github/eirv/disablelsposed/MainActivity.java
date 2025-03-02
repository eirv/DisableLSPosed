package io.github.eirv.disablelsposed;

import android.app.ActionBar;
import android.app.Activity;
import android.os.Bundle;
import android.widget.ScrollView;
import android.widget.TextView;

public class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        ActionBar actionBar = getActionBar();
        if (actionBar != null) {
            actionBar.setSubtitle("pid=" + android.os.Process.myPid());
        }

        String[] methods = Native.getUnhookedMethods();
        int flags = Native.getFlags();

        StringBuilder sb = new StringBuilder();
        sb.append("尝试禁用 LSPosed：");
        sb.append((flags & 1) != 0 ? "成功" : "失败");

        sb.append("\n\n尝试恢复对 libart.so 的 inline hook：");
        sb.append((flags & (1 << 1)) != 0 ? "成功" : "失败");

        sb.append("\n\n恢复了 ");
        sb.append(methods.length);
        sb.append(" 个被 LSPlant hook 的方法：\n");
        for (String method : methods) {
            if (method == null) break;
            sb.append("\n");
            sb.append(method);
        }

        ScrollView scrollView = new ScrollView(this);
        scrollView.setFillViewport(true);
        TextView textView = new TextView(this);
        textView.setTextIsSelectable(true);
        textView.setText(sb.toString());
        scrollView.addView(textView, ScrollView.LayoutParams.MATCH_PARENT, ScrollView.LayoutParams.MATCH_PARENT);
        setContentView(scrollView);
    }
}
