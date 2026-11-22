package org.huxerui;

import android.app.Activity;
import android.app.Instrumentation;
import android.os.Bundle;
import android.os.Build;
import android.graphics.Insets;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.text.Layout;
import android.text.TextPaint;
import android.view.WindowInsets;
import android.view.WindowInsetsAnimation;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.Collections;

/** Android-runtime regression checks; host checks require the matching native HuxerUI library. */
public final class HuxerUIRuntimeTest extends Instrumentation {
    static {
        System.loadLibrary("huxerui");
    }

    @Override
    public void onCreate(Bundle arguments) {
        super.onCreate(arguments);
        start();
    }

    @Override
    public void onStart() {
        assertions = 0;
        Bundle status = new Bundle();
        status.putString("class", getClass().getName());
        status.putString("test", "androidRuntime");
        status.putInt("numtests", 1);
        status.putInt("current", 1);
        sendStatus(1, status);
        Bundle result = new Bundle();
        try {
            verifyHostFocusHighlight();
            verifyImeInsetsAnimation();
            verifyGeometry();
            int notificationAssertions = HuxerUILocalNotificationTest.verify(getTargetContext());
            int systemTaskAssertions = HuxerUISystemTasksTest.verify(this);
            sendStatus(0, status);
            result.putString("stream",
                    "HuxerUI Android runtime: " + (assertions + notificationAssertions + systemTaskAssertions)
                            + " assertions passed\n");
        } catch (Throwable error) {
            String stack = android.util.Log.getStackTraceString(error);
            status.putString("stack", stack);
            sendStatus(-2, status);
            result.putString("stream", stack);
        }
        finish(Activity.RESULT_OK, result);
    }

    private static int assertions;

    private void verifyHostFocusHighlight() {
        final Throwable[] failure = new Throwable[1];
        runOnMainSync(() -> {
            try {
                HuxerUIView host = new HuxerUIView(getTargetContext());
                check(host.isFocusable(), "The host must remain focusable for keyboard input");
                check(host.isFocusableInTouchMode(), "The host must remain focusable in touch mode for IME input");
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    check(!host.getDefaultFocusHighlightEnabled(),
                            "Android default focus highlighting must not cover the HuxerUI host");
                }
            } catch (Throwable error) {
                // Report main-thread setup and assertion failures through the instrumentation result.
                failure[0] = error;
            }
        });
        if (failure[0] != null) {
            throw new AssertionError("HuxerUI host focus regression failed", failure[0]);
        }
    }

    private void verifyImeInsetsAnimation() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return;
        final Throwable[] failure = new Throwable[1];
        runOnMainSync(() -> {
            try {
                HuxerUIView host = new HuxerUIView(getTargetContext());
                Field bottom = HuxerUIView.class.getDeclaredField("imeInsetBottom");
                bottom.setAccessible(true);
                Method viewportBottom = HuxerUIView.class.getDeclaredMethod("viewportBottomSafeInset");
                viewportBottom.setAccessible(true);
                int type = WindowInsets.Type.ime();
                int navigation = WindowInsets.Type.navigationBars();
                WindowInsets hidden = new WindowInsets.Builder()
                        .setInsets(navigation, Insets.of(0, 0, 0, 48)).build();
                WindowInsets shown = new WindowInsets.Builder(hidden)
                        .setInsets(type, Insets.of(0, 0, 0, 300)).build();
                WindowInsets entering = new WindowInsets.Builder(hidden)
                        .setInsets(type, Insets.of(0, 0, 0, 12))
                        .setInsets(navigation, Insets.of(0, 0, 0, 54)).build();
                WindowInsets midway = new WindowInsets.Builder(hidden)
                        .setInsets(type, Insets.of(0, 0, 0, 120)).build();
                WindowInsetsAnimation show = new WindowInsetsAnimation(type, null, 200);
                WindowInsetsAnimation hide = new WindowInsetsAnimation(type, null, 200);

                host.dispatchApplyWindowInsets(hidden);
                check((int) viewportBottom.invoke(host) == 48, "The viewport must reserve the three-button bar");
                host.dispatchWindowInsetsAnimationPrepare(show);
                host.dispatchApplyWindowInsets(shown);
                check(bottom.getInt(host) == 0, "The destination must not shrink the host before the IME moves");
                host.dispatchWindowInsetsAnimationProgress(entering, Collections.singletonList(show));
                check(bottom.getInt(host) == 12, "The keyboard initially covers only 12 pixels");
                check((int) viewportBottom.invoke(host) == 42,
                        "Only the part of the current navigation bar above the IME stays in the safe area");
                host.dispatchWindowInsetsAnimationProgress(midway, Collections.singletonList(show));
                check(bottom.getInt(host) == 120, "The current IME inset must follow animation progress");
                check((int) viewportBottom.invoke(host) == 0, "Overlapping bottom insets must not be reserved twice");
                host.dispatchWindowInsetsAnimationEnd(show);
                check(bottom.getInt(host) == 300,
                        "Completing the animation must apply the final IME inset, got " + bottom.getInt(host)
                                + ", target " + shown.getInsets(type).bottom);

                host.dispatchWindowInsetsAnimationPrepare(hide);
                host.dispatchApplyWindowInsets(hidden);
                check(bottom.getInt(host) == 0, "The host can expand behind the dismissing keyboard");
                host.dispatchWindowInsetsAnimationProgress(midway, Collections.singletonList(hide));
                check(bottom.getInt(host) == 0, "Dismissal must not shrink the expanded host again");
                host.dispatchWindowInsetsAnimationPrepare(show);
                host.dispatchApplyWindowInsets(shown);
                host.dispatchWindowInsetsAnimationProgress(midway, Collections.singletonList(show));
                host.dispatchWindowInsetsAnimationEnd(hide);
                check(bottom.getInt(host) == 120, "Retiring a superseded animation must preserve the current inset");
                host.dispatchWindowInsetsAnimationEnd(show);
                host.dispatchApplyWindowInsets(hidden);
                check(bottom.getInt(host) == 0, "Insets dispatched without animation must still apply immediately");
                check((int) viewportBottom.invoke(host) == 48, "Dismissing the keyboard restores the navigation bar");
                host.dispatchApplyWindowInsets(shown);
                check(bottom.getInt(host) == 300 && (int) viewportBottom.invoke(host) == 0,
                        "A keyboard that opens without animation must apply its final geometry immediately");
            } catch (Throwable error) {
                failure[0] = error;
            }
        });
        if (failure[0] != null) {
            throw new AssertionError("HuxerUI IME inset animation regression failed", failure[0]);
        }
    }

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
        for (int direction : new int[] {1, 2}) {
            String text = direction == 1 ? "abcdefghijklm" : "אבגדהוזחטיכלמ";
            for (Layout.Alignment alignment :
                 new Layout.Alignment[] {Layout.Alignment.ALIGN_NORMAL, Layout.Alignment.ALIGN_CENTER,
                                         Layout.Alignment.ALIGN_OPPOSITE}) {
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
                check(encoded == -(long) boundary - 1L,
                      "Soft-wrap hit must preserve upstream affinity: " + direction + " / " + boundary + " / " + encoded
                              + " / " + before[0]);
            }
        }
        for (String separator : new String[] {"\n", "\r\n"}) {
            HuxerUITextLayout layout =
                    layout("abc" + separator + "xyz", 200.0F, Layout.Alignment.ALIGN_NORMAL, true, 1);
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
