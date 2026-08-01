package org.huxerui;

import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.text.StaticLayout;
import android.text.TextPaint;

import java.util.Arrays;

final class HuxerUITextLayout {
    private final String text;
    private final TextPaint paint;
    private final StaticLayout layout;

    HuxerUITextLayout(String text, TextPaint paint, float maxWidth) {
        this.text = text;
        this.paint = paint;
        float desiredWidth = StaticLayout.getDesiredWidth(text, paint);
        int width = Math.max(
                1, (int) Math.ceil(Float.isFinite(maxWidth) ? Math.min(maxWidth, desiredWidth) : desiredWidth));
        layout = StaticLayout.Builder.obtain(text, 0, text.length(), paint, width).setIncludePad(true).build();
    }

    private float[] measure() {
        float width = 0.0F;
        for (int line = 0; line < layout.getLineCount(); ++line) {
            width = Math.max(width, layout.getLineWidth(line));
        }
        return new float[] {
                (float) Math.ceil(width),
                (float) Math.ceil(layout.getHeight()),
        };
    }

    private long hitTest(float x, float y) {
        int line = layout.getLineForVertical((int) Math.max(0.0F, y));
        int offset = layout.getOffsetForHorizontal(line, x);
        boolean upstream = line + 1 < layout.getLineCount() && offset == layout.getLineEnd(line)
                && layout.getLineStart(line + 1) == offset;
        return upstream ? -(long) offset - 1L : offset;
    }

    private float[] caret(long requestedOffset, boolean upstream) {
        int offset = (int) Math.max(0L, Math.min(requestedOffset, text.length()));
        int line = layout.getLineForOffset(offset);
        if (upstream && line > 0 && layout.getLineStart(line) == offset && layout.getLineEnd(line - 1) == offset) {
            --line;
        }
        float x = upstream ? layout.getSecondaryHorizontal(offset) : layout.getPrimaryHorizontal(offset);
        return new float[] {
                x,
                layout.getLineTop(line),
                1.0F,
                layout.getLineBottom(line) - layout.getLineTop(line),
        };
    }

    private float[] range(long requestedStart, long requestedEnd) {
        int start = (int) Math.max(0L, Math.min(requestedStart, text.length()));
        int end = (int) Math.max(start, Math.min(requestedEnd, text.length()));
        if (start == end) {
            return new float[0];
        }
        int firstLine = layout.getLineForOffset(start);
        int lastLine = layout.getLineForOffset(end);
        float[] result = new float[(lastLine - firstLine + 1) * 4];
        int output = 0;
        for (int line = firstLine; line <= lastLine; ++line) {
            int lineStart = Math.max(start, layout.getLineStart(line));
            int lineEnd = Math.min(end, layout.getLineEnd(line));
            if (lineStart >= lineEnd) {
                continue;
            }
            Path selection = new Path();
            layout.getSelectionPath(lineStart, lineEnd, selection);
            RectF bounds = new RectF();
            selection.computeBounds(bounds, true);
            result[output++] = bounds.left;
            result[output++] = bounds.top;
            result[output++] = bounds.width();
            result[output++] = bounds.height();
        }
        return output == result.length ? result : Arrays.copyOf(result, output);
    }

    private long previous(long requestedOffset) {
        int offset = (int) Math.max(0L, Math.min(requestedOffset, text.length()));
        if (offset == 0) {
            return 0L;
        }
        int result = paint.getTextRunCursor(text, 0, text.length(), false, offset, Paint.CURSOR_BEFORE);
        return result < 0 ? 0L : result;
    }

    private long next(long requestedOffset) {
        int offset = (int) Math.max(0L, Math.min(requestedOffset, text.length()));
        if (offset == text.length()) {
            return offset;
        }
        int result = paint.getTextRunCursor(text, 0, text.length(), false, offset, Paint.CURSOR_AFTER);
        return result < 0 ? text.length() : result;
    }
}
