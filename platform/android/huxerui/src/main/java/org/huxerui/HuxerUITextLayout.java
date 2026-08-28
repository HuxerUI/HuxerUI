package org.huxerui;

import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.text.Layout;
import android.text.StaticLayout;
import android.text.TextPaint;
import android.text.TextDirectionHeuristics;

import java.util.Arrays;

final class HuxerUITextLayout {
    private final String text;
    private final TextPaint paint;
    private final StaticLayout layout;
    private final float horizontalOffset;

    HuxerUITextLayout(
            String text, TextPaint paint, float maxWidth, Layout.Alignment alignment, boolean wrap, int direction) {
        this.text = text;
        this.paint = paint;
        float desiredWidth = StaticLayout.getDesiredWidth(text, paint);
        float requestedWidth = wrap && Float.isFinite(maxWidth) ? maxWidth : desiredWidth;
        int width = Math.max(1, (int) Math.ceil(requestedWidth));
        StaticLayout.Builder builder = StaticLayout.Builder.obtain(text, 0, text.length(), paint, width)
                                               .setAlignment(alignment)
                                               .setIncludePad(true);
        if (direction == 1) {
            builder.setTextDirection(TextDirectionHeuristics.LTR);
        } else if (direction == 2) {
            builder.setTextDirection(TextDirectionHeuristics.RTL);
        } else {
            builder.setTextDirection(TextDirectionHeuristics.FIRSTSTRONG_LTR);
        }
        layout = builder.build();
        horizontalOffset = wrap ? 0.0F : resolveHorizontalOffset(text, maxWidth, width, alignment, direction);
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
        int offset = layout.getOffsetForHorizontal(line, x - horizontalOffset);
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
        float x = (upstream ? layout.getSecondaryHorizontal(offset) : layout.getPrimaryHorizontal(offset))
                + horizontalOffset;
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
            result[output++] = bounds.left + horizontalOffset;
            result[output++] = bounds.top;
            result[output++] = bounds.width();
            result[output++] = bounds.height();
        }
        return output == result.length ? result : Arrays.copyOf(result, output);
    }

    static float resolveHorizontalOffset(String text, float availableWidth, float layoutWidth,
            Layout.Alignment alignment, int direction) {
        if (!Float.isFinite(availableWidth)) {
            return 0.0F;
        }
        if (alignment == Layout.Alignment.ALIGN_CENTER) {
            return (availableWidth - layoutWidth) * 0.5F;
        }
        boolean rightToLeft = isRightToLeft(text, direction);
        if ((alignment == Layout.Alignment.ALIGN_NORMAL && rightToLeft)
                || (alignment == Layout.Alignment.ALIGN_OPPOSITE && !rightToLeft)) {
            return availableWidth - layoutWidth;
        }
        return 0.0F;
    }

    static boolean isRightToLeft(String text, int direction) {
        return direction == 2
                || (direction != 1 && TextDirectionHeuristics.FIRSTSTRONG_LTR.isRtl(text, 0, text.length()));
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
