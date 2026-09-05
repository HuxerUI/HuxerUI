package org.huxerui;

import android.app.Activity;
import android.app.Instrumentation;
import android.os.Bundle;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.text.Layout;
import android.text.TextPaint;

import java.lang.reflect.Method;

/** Android-runtime regression checks; no native HuxerUI library is required. */
public final class HuxerUITextLayoutTest extends Instrumentation {
    @Override
    public void onCreate(Bundle arguments) {
        super.onCreate(arguments);
        start();
    }

    @Override
    public void onStart() {
        Bundle status = new Bundle();
        status.putString("class", getClass().getName());
        status.putString("test", "paragraphGeometry");
        status.putInt("numtests", 1);
        status.putInt("current", 1);
        sendStatus(1, status);
        Bundle result = new Bundle();
        try {
            verifyGeometry();
            sendStatus(0, status);
            result.putString("stream", "HuxerUI Android text layout: " + assertions + " assertions passed\n");
        } catch (Throwable error) {
            String stack = android.util.Log.getStackTraceString(error);
            status.putString("stack", stack);
            sendStatus(-2, status);
            result.putString("stream", stack);
        }
        finish(Activity.RESULT_OK, result);
    }

    private static int assertions;

    private static void check(boolean value, String message) {
        ++assertions;
        if (!value) {
            throw new AssertionError(message);
        }
    }

    private static HuxerUITextLayout layout(String text, float width, Layout.Alignment alignment, boolean wrap,
            int direction) {
        Typeface typeface = Typeface.MONOSPACE;
        TextPaint paint = new TextPaint(Paint.ANTI_ALIAS_FLAG);
        paint.setTypeface(typeface);
        paint.setTextSize(20.25F);
        return new HuxerUITextLayout(text, paint, width, alignment, wrap, direction);
    }

    private static float[] caret(HuxerUITextLayout layout, int offset, boolean upstream) throws Exception {
        Method method = HuxerUITextLayout.class.getDeclaredMethod("caret", long.class, boolean.class);
        method.setAccessible(true);
        return (float[]) method.invoke(layout, (long) offset, upstream);
    }

    private static long hit(HuxerUITextLayout layout, float[] caret) throws Exception {
        Method method = HuxerUITextLayout.class.getDeclaredMethod("hitTest", float.class, float.class);
        method.setAccessible(true);
        return (long) method.invoke(layout, caret[0], caret[1] + caret[3] * 0.5F);
    }

    private static float[] range(HuxerUITextLayout layout, int start, int end) throws Exception {
        Method method = HuxerUITextLayout.class.getDeclaredMethod("range", long.class, long.class);
        method.setAccessible(true);
        return (float[]) method.invoke(layout, (long) start, (long) end);
    }

    private static void verifyGeometry() throws Exception {
        assertions = 0;
        for (int direction : new int[] {1, 2}) {
            String text = direction == 1 ? "abcdefghijklm" : "אבגדהוזחטיכלמ";
            for (Layout.Alignment alignment : new Layout.Alignment[] {
                    Layout.Alignment.ALIGN_NORMAL, Layout.Alignment.ALIGN_CENTER, Layout.Alignment.ALIGN_OPPOSITE}) {
                HuxerUITextLayout layout = layout(text, 62.0F, alignment, true, direction);
                int boundary = 1;
                while (boundary < text.length() && caret(layout, boundary, false)[1] == caret(layout, 0, false)[1]) {
                    ++boundary;
                }
                check(boundary < text.length(), "Expected automatic wrapping");
                float[] before = caret(layout, boundary, true);
                float[] after = caret(layout, boundary, false);
                check(before[1] < after[1], "Upstream must stay on the preceding visual line");
                long encoded = hit(layout, before);
                check(encoded == -(long) boundary - 1L, "Soft-wrap hit must preserve upstream affinity: "
                        + direction + " / " + boundary + " / " + encoded + " / " + before[0]);
            }
        }
        for (String separator : new String[] {"\n", "\r\n"}) {
            HuxerUITextLayout layout = layout("abc" + separator + "xyz", 200.0F,
                    Layout.Alignment.ALIGN_NORMAL, true, 1);
            int offset = 3 + separator.length();
            float[] before = caret(layout, offset, true);
            float[] after = caret(layout, offset, false);
            check(before[0] == after[0] && before[1] == after[1], "Hard breaks must not move the caret backwards");
        }
        String bidi = "abc אבג xyz";
        for (Layout.Alignment alignment : new Layout.Alignment[] {
                Layout.Alignment.ALIGN_NORMAL, Layout.Alignment.ALIGN_CENTER, Layout.Alignment.ALIGN_OPPOSITE}) {
            HuxerUITextLayout layout = layout(bidi, 300.0F, alignment, false, 1);
            for (int offset = 0; offset <= bidi.length(); ++offset) {
                for (boolean upstream : new boolean[] {false, true}) {
                    float[] position = caret(layout, offset, upstream);
                    long encoded = hit(layout, position);
                    float[] resolved = caret(layout, (int) (encoded < 0 ? -encoded - 1 : encoded), encoded < 0);
                    check(Math.abs(position[0] - resolved[0]) < 0.1F && position[1] == resolved[1],
                            "Bidi caret geometry must round-trip at offset " + offset + " / " + alignment);
                }
            }
            check(range(layout, 2, 5).length >= 8, "Disjoint bidi selection fragments must remain separate");
        }
        HuxerUITextLayout emoji = layout("a\u0301👩‍💻b", 300.0F, Layout.Alignment.ALIGN_NORMAL, false, 1);
        float[] selected = range(emoji, 2, 7);
        check(selected.length >= 4 && selected[2] > 0.0F, "Emoji selection must retain nonempty geometry");
        HuxerUITextLayout fractional = layout("abc", 300.0F, Layout.Alignment.ALIGN_NORMAL, false, 1);
        float[] fragment = range(fractional, 0, 1);
        float[] leading = caret(fractional, 0, false);
        float[] trailing = caret(fractional, 1, true);
        check(fragment.length == 4 && Math.abs(fragment[0] - leading[0]) <= 1.0F / 64.0F
                && Math.abs(fragment[0] + fragment[2] - trailing[0]) <= 1.0F / 64.0F,
                "Region conversion must preserve subpixel selection edges");
        StringBuilder longText = new StringBuilder();
        for (int line = 0; line < 1000; ++line) {
            longText.append("abc\n");
        }
        HuxerUITextLayout longLayout = layout(longText.toString(), 300.0F, Layout.Alignment.ALIGN_NORMAL, false, 1);
        float[] tail = range(longLayout, longText.length() - 4, longText.length() - 1);
        check(tail.length >= 4 && tail[1] > 10000.0F, "Selection must preserve the origin of distant lines");
    }
}
