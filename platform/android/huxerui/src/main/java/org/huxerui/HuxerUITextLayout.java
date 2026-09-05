package org.huxerui;

import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Matrix;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Region;
import android.graphics.RegionIterator;
import android.text.Layout;
import android.text.StaticLayout;
import android.text.TextPaint;
import android.text.TextDirectionHeuristics;

import java.util.Arrays;

final class HuxerUITextLayout {
    // Region uses integer coordinates; this grid retains selection edges to 1/64 of a logical pixel.
    private static final float SELECTION_COORDINATE_SCALE = 64.0F;
    private final CharSequence text;
    private final TextPaint paint;
    private final StaticLayout layout;
    private final float horizontalOffset;

    HuxerUITextLayout(CharSequence text, TextPaint paint, float maxWidth, Layout.Alignment alignment, boolean wrap,
            int direction) {
        this.text = text;
        this.paint = paint;
        // Wrapped layouts already have a width constraint; NoWrap keeps natural width and aligns by translation.
        float requestedWidth = wrap && Float.isFinite(maxWidth)
                ? maxWidth : StaticLayout.getDesiredWidth(text, paint);
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

    float[] measure() {
        float width = 0.0F;
        for (int line = 0; line < layout.getLineCount(); ++line) {
            width = Math.max(width, layout.getLineWidth(line));
        }
        return new float[] {
                (float) Math.ceil(width),
                (float) Math.ceil(layout.getHeight()),
                layout.getLineBaseline(0),
                layout.getLineBaseline(layout.getLineCount() - 1),
                layout.getLineCount(),
        };
    }

    private long hitTest(float x, float y) {
        int line = layout.getLineForVertical((int) Math.max(0.0F, y));
        int offset = layout.getOffsetForHorizontal(line, x - horizontalOffset);
        if (offset > layout.getLineStart(line) && offset == layout.getLineEnd(line) && isHardBreak(offset - 1)) {
            offset = (int) previous(offset);
        }
        // Affinity chooses a visual position, so compare candidates on the actual hit line rather than only offsets.
        float[] before = caret(offset, true);
        float[] after = caret(offset, false);
        float beforeDistance = before[1] == layout.getLineTop(line) ? Math.abs(x - before[0]) : Float.POSITIVE_INFINITY;
        float afterDistance = after[1] == layout.getLineTop(line) ? Math.abs(x - after[0]) : Float.POSITIVE_INFINITY;
        // Android may return the last character at a soft line end; include the boundary after that character.
        int lineEnd = layout.getLineEnd(line);
        if (line + 1 < layout.getLineCount() && lineEnd > layout.getLineStart(line) && !isHardBreak(lineEnd - 1)) {
            float[] boundary = caret(lineEnd, true);
            if (Math.abs(x - boundary[0]) <= Math.min(beforeDistance, afterDistance)) {
                return -(long) lineEnd - 1L;
            }
        }
        return beforeDistance < afterDistance ? -(long) offset - 1L : offset;
    }

    private float[] caret(long requestedOffset, boolean upstream) {
        int offset = (int) Math.max(0L, Math.min(requestedOffset, text.length()));
        // Hard breaks consume source characters; only an automatic wrap shares one offset between adjacent lines.
        upstream = upstream && offset > 0 && !isHardBreak(offset - 1);
        int line = layout.getLineForOffset(offset);
        if (upstream && line > 0 && layout.getLineStart(line) == offset) {
            --line;
        }
        float x = horizontal(offset, upstream, line) + horizontalOffset;
        return new float[] {
                x,
                layout.getLineTop(line),
                1.0F,
                layout.getLineBottom(line) - layout.getLineTop(line),
        };
    }

    private boolean isHardBreak(int offset) {
        return offset >= 0 && offset < text.length() && (text.charAt(offset) == '\n' || text.charAt(offset) == '\r');
    }

    private float horizontal(int offset, boolean upstream, int line) {
        int start = upstream ? (int) previous(offset) : offset;
        int end = upstream ? offset : (int) next(offset);
        if (start == end || isHardBreak(start)) {
            return layout.getPrimaryHorizontal(offset);
        }
        // Use the adjacent cluster's Layout-reported edge; primary/secondary horizontals do not select a wrap side.
        Path selection = new Path();
        layout.getSelectionPath(start, end, selection);
        Path clip = new Path();
        clip.addRect(layout.getLineLeft(line), layout.getLineTop(line), layout.getLineRight(line),
                layout.getLineBottom(line), Path.Direction.CW);
        if (!selection.op(clip, Path.Op.INTERSECT)) {
            throw new IllegalStateException("HuxerUI could not resolve the text caret path");
        }
        RectF bounds = new RectF();
        selection.computeBounds(bounds, true);
        boolean rtl = layout.isRtlCharAt(start);
        if (bounds.isEmpty()) {
            return rtl == upstream ? layout.getLineLeft(line) : layout.getLineRight(line);
        }
        return rtl == upstream ? bounds.left : bounds.right;
    }

    int height() {
        return layout.getHeight();
    }

    float horizontalOffset() {
        return horizontalOffset;
    }

    void draw(Canvas canvas, int color) {
        // Default foreground is late-bound for cache reuse; explicit span colors override it during drawing.
        layout.getPaint().setColor(color);
        layout.draw(canvas);
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
        Matrix scale = new Matrix();
        scale.setScale(SELECTION_COORDINATE_SCALE, SELECTION_COORDINATE_SCALE);
        Path selection = new Path();
        RectF bounds = new RectF();
        Region region = new Region();
        Region clip = new Region();
        Rect rect = new Rect();
        for (int line = firstLine; line <= lastLine; ++line) {
            int lineStart = Math.max(start, layout.getLineStart(line));
            int lineEnd = Math.min(end, layout.getLineEnd(line));
            if (lineStart >= lineEnd) {
                continue;
            }
            layout.getSelectionPath(lineStart, lineEnd, selection);
            selection.computeBounds(bounds, true);
            // Scale local coordinates, not a distant line's absolute position, to preserve Region's integer range.
            float originX = bounds.left;
            float originY = layout.getLineTop(line);
            selection.offset(-originX, -originY);
            selection.transform(scale);
            selection.computeBounds(bounds, true);
            double right = Math.ceil(bounds.right);
            double bottom = Math.ceil((layout.getLineBottom(line) - originY) * SELECTION_COORDINATE_SCALE);
            if (!Double.isFinite(right) || !Double.isFinite(bottom)
                    || right >= Integer.MAX_VALUE || bottom >= Integer.MAX_VALUE) {
                throw new IllegalArgumentException("HuxerUI text selection exceeds the supported coordinate range");
            }
            // Preserve disjoint bidi fragments without including the next line's continuation path.
            clip.set(0, 0, (int) right, (int) bottom);
            region.setPath(selection, clip);
            RegionIterator fragments = new RegionIterator(region);
            while (fragments.next(rect)) {
                if (output + 4 > result.length) {
                    result = Arrays.copyOf(result, Math.max(output + 4, result.length * 2));
                }
                result[output++] = rect.left / SELECTION_COORDINATE_SCALE + originX + horizontalOffset;
                result[output++] = rect.top / SELECTION_COORDINATE_SCALE + originY;
                result[output++] = rect.width() / SELECTION_COORDINATE_SCALE;
                result[output++] = rect.height() / SELECTION_COORDINATE_SCALE;
            }
        }
        return output == result.length ? result : Arrays.copyOf(result, output);
    }

    static float resolveHorizontalOffset(
            CharSequence text, float availableWidth, float layoutWidth, Layout.Alignment alignment, int direction) {
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

    static boolean isRightToLeft(CharSequence text, int direction) {
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
