package org.huxerui;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.os.SystemClock;
import android.os.Build;
import android.text.Layout;
import android.text.StaticLayout;
import android.text.TextPaint;
import android.util.AttributeSet;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewTreeObserver;
import android.view.WindowInsets;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

import java.nio.charset.StandardCharsets;

public final class HuxerUIView extends View {
    static {
        System.loadLibrary("huxerui");
    }

    private static final int POINTER_DOWN = 0;
    private static final int POINTER_UP = 1;
    private static final int POINTER_MOVE = 2;
    private static final int POINTER_CANCEL = 3;

    private static final int POINTER_DEVICE_MOUSE = 0;
    private static final int POINTER_DEVICE_TOUCH = 1;
    private static final int POINTER_DEVICE_PEN = 2;

    private static final int TEXT_ALIGN_CENTER = 1;
    private static final int STROKE_CAP_ROUND = 1;
    private static final int STROKE_CAP_SQUARE = 2;

    private static final float SCROLL_STEP = 48.0F;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);
    private final TextPaint textPaint = new TextPaint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);
    private final RectF rect = new RectF();
    private final Path path = new Path();
    private final Matrix transform = new Matrix();
    private final float[] transformValues = new float[9];
    private final float density;
    private final ViewTreeObserver.OnPreDrawListener textInputGeometryListener = this::updateTextInputGeometry;
    private final Runnable frameCallback = new Runnable() {
        @Override
        public void run() {
            frameScheduled = false;
            frameTime = 0L;
            if (nativeHandle != 0L) {
                nativeCommitFrame(nativeHandle);
            }
        }
    };

    private long nativeHandle;
    private boolean frameScheduled;
    private long frameTime;
    private HuxerUIInputConnection inputConnection;
    private HuxerUIShadowRenderer shadowRenderer;
    private int imeInsetBottom;

    private boolean updateTextInputGeometry() {
        if (inputConnection != null) {
            inputConnection.updateCursorAnchorPosition();
        }
        return true;
    }

    public HuxerUIView(Context context) {
        this(context, null);
    }

    public HuxerUIView(Context context, AttributeSet attributes) {
        this(context, attributes, 0);
    }

    public HuxerUIView(Context context, AttributeSet attributes, int defaultStyleAttribute) {
        super(context, attributes, defaultStyleAttribute);
        density = getResources().getDisplayMetrics().density;
        setFocusable(true);
        setFocusableInTouchMode(true);
        setClickable(true);
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        getViewTreeObserver().addOnPreDrawListener(textInputGeometryListener);
        if (nativeHandle == 0L) {
            nativeHandle = nativeCreate(this);
            resizeNativeState(getWidth(), getHeight());
        }
    }

    @Override
    protected void onDetachedFromWindow() {
        ViewTreeObserver observer = getViewTreeObserver();
        if (observer.isAlive()) {
            observer.removeOnPreDrawListener(textInputGeometryListener);
        }
        removeCallbacks(frameCallback);
        if (shadowRenderer != null) {
            shadowRenderer.clear();
            shadowRenderer = null;
        }
        frameScheduled = false;
        frameTime = 0L;
        if (inputConnection != null) {
            inputConnection.deactivate();
            inputConnection = null;
        }
        if (nativeHandle != 0L) {
            nativeDestroy(nativeHandle);
            nativeHandle = 0L;
        }
        super.onDetachedFromWindow();
    }

    @Override
    protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
        super.onSizeChanged(width, height, oldWidth, oldHeight);
        resizeNativeState(width, height);
    }

    @Override
    protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
        super.onLayout(changed, left, top, right, bottom);
        if (changed && inputConnection != null) {
            inputConnection.updateCursorAnchorPosition();
        }
    }

    @Override
    public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            imeInsetBottom = insets.getInsets(WindowInsets.Type.ime()).bottom;
            resizeNativeState(getWidth(), getHeight());
        }
        return super.onApplyWindowInsets(insets);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        canvas.drawColor(0xFFF7F8FA);
        if (nativeHandle == 0L) {
            return;
        }
        int saveCount = canvas.save();
        canvas.scale(density, density);
        nativeDraw(nativeHandle, canvas);
        canvas.restoreToCount(saveCount);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (nativeHandle == 0L) {
            return false;
        }
        int action = event.getActionMasked();
        int actionIndex = event.getActionIndex();
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            requestFocus();
            sendPointer(event, actionIndex, POINTER_DOWN);
        } else if (action == MotionEvent.ACTION_MOVE) {
            for (int index = 0; index < event.getPointerCount(); ++index) {
                sendPointer(event, index, POINTER_MOVE);
            }
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            sendPointer(event, actionIndex, POINTER_UP);
        } else if (action == MotionEvent.ACTION_CANCEL) {
            for (int index = 0; index < event.getPointerCount(); ++index) {
                sendPointer(event, index, POINTER_CANCEL);
            }
        }
        return true;
    }

    @Override
    public boolean onHoverEvent(MotionEvent event) {
        if (nativeHandle == 0L) {
            return false;
        }
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_HOVER_ENTER || action == MotionEvent.ACTION_HOVER_MOVE) {
            sendPointer(event, 0, POINTER_MOVE);
            return true;
        }
        if (action == MotionEvent.ACTION_HOVER_EXIT) {
            sendPointer(event, 0, POINTER_CANCEL);
            return true;
        }
        return super.onHoverEvent(event);
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        if (nativeHandle != 0L && event.getActionMasked() == MotionEvent.ACTION_SCROLL) {
            float horizontal = event.getAxisValue(MotionEvent.AXIS_HSCROLL);
            float vertical = event.getAxisValue(MotionEvent.AXIS_VSCROLL);
            nativeScroll(nativeHandle, event.getX() / density, event.getY() / density, -horizontal * SCROLL_STEP,
                    -vertical * SCROLL_STEP);
            return true;
        }
        return super.onGenericMotionEvent(event);
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (nativeHandle == 0L) {
            return super.onKeyDown(keyCode, event);
        }
        sendKey(event, true);
        return true;
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (nativeHandle == 0L) {
            return super.onKeyUp(keyCode, event);
        }
        sendKey(event, false);
        return true;
    }

    @Override
    public boolean onCheckIsTextEditor() {
        return inputConnection != null && inputConnection.isActive();
    }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
        if (inputConnection == null || !inputConnection.isActive()) {
            return null;
        }
        inputConnection.configureEditorInfo(outAttrs);
        return inputConnection;
    }

    private void startTextInput(long sessionId, int type, int capitalization, int action, boolean multiline,
            boolean secure, boolean autocorrect, long revision, long anchor, long active, int affinity,
            long composingStart, long composingEnd, int geometryResult, float caretX, float caretY, float caretWidth,
            float caretHeight) {
        replaceInputConnection(sessionId, type, capitalization, action, multiline, secure, autocorrect, anchor, active,
                composingStart, composingEnd);
        inputConnection.updateCaretGeometry(geometryResult, caretX, caretY, caretWidth, caretHeight);
        post(() -> {
            if (!hasTextInputSession(sessionId)) {
                return;
            }
            requestFocus();
            InputMethodManager manager = inputMethodManager();
            manager.restartInput(this);
            manager.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT);
        });
    }

    private void updateTextInput(long sessionId, long revision, long anchor, long active, int affinity,
            long composingStart, long composingEnd, int geometryResult, float caretX, float caretY, float caretWidth,
            float caretHeight) {
        if (!hasTextInputSession(sessionId)) {
            return;
        }
        inputConnection.updateState(anchor, active, composingStart, composingEnd);
        inputConnection.updateCaretGeometry(geometryResult, caretX, caretY, caretWidth, caretHeight);
        inputConnection.notifyStateChanged();
    }

    private void restartTextInput(long sessionId, int type, int capitalization, int action, boolean multiline,
            boolean secure, boolean autocorrect, long revision, long anchor, long active, int affinity,
            long composingStart, long composingEnd, int geometryResult, float caretX, float caretY, float caretWidth,
            float caretHeight) {
        if (!hasTextInputSession(sessionId)) {
            return;
        }
        replaceInputConnection(sessionId, type, capitalization, action, multiline, secure, autocorrect, anchor, active,
                composingStart, composingEnd);
        inputConnection.updateCaretGeometry(geometryResult, caretX, caretY, caretWidth, caretHeight);
        post(() -> {
            if (hasTextInputSession(sessionId)) {
                inputMethodManager().restartInput(this);
            }
        });
    }

    private void stopTextInput(long sessionId) {
        if (!hasTextInputSession(sessionId)) {
            return;
        }
        inputConnection.deactivate();
        inputConnection = null;
        post(() -> {
            if (inputConnection == null) {
                inputMethodManager().hideSoftInputFromWindow(getWindowToken(), 0);
            }
        });
    }

    private void requestShowTextInput(long sessionId) {
        if (!hasTextInputSession(sessionId)) {
            return;
        }
        post(() -> {
            if (!hasTextInputSession(sessionId)) {
                return;
            }
            requestFocus();
            inputMethodManager().showSoftInput(this, InputMethodManager.SHOW_IMPLICIT);
        });
    }

    private void replaceInputConnection(long sessionId, int type, int capitalization, int action, boolean multiline,
            boolean secure, boolean autocorrect, long anchor, long active, long composingStart, long composingEnd) {
        if (inputConnection != null) {
            inputConnection.deactivate();
        }
        inputConnection = new HuxerUIInputConnection(this, nativeHandle, sessionId, type, capitalization, action,
                multiline, secure, autocorrect, anchor, active, composingStart, composingEnd);
    }

    private boolean hasTextInputSession(long sessionId) {
        return inputConnection != null && inputConnection.isActive() && inputConnection.sessionId() == sessionId;
    }

    private InputMethodManager inputMethodManager() {
        return (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
    }

    private byte[] readClipboardText() {
        ClipboardManager clipboard = (ClipboardManager) getContext().getSystemService(Context.CLIPBOARD_SERVICE);
        if (clipboard == null || !clipboard.hasPrimaryClip()) {
            return null;
        }
        ClipData clip = clipboard.getPrimaryClip();
        if (clip == null || clip.getItemCount() == 0) {
            return null;
        }
        CharSequence text = clip.getItemAt(0).coerceToText(getContext());
        return text == null ? null : text.toString().getBytes(StandardCharsets.UTF_8);
    }

    private boolean writeClipboardText(byte[] utf8) {
        ClipboardManager clipboard = (ClipboardManager) getContext().getSystemService(Context.CLIPBOARD_SERVICE);
        if (clipboard == null) {
            return false;
        }
        clipboard.setPrimaryClip(ClipData.newPlainText("HuxerUI", new String(utf8, StandardCharsets.UTF_8)));
        return true;
    }

    private void resizeNativeState(int width, int height) {
        if (nativeHandle == 0L) {
            return;
        }
        int usableHeight = Math.max(0, height - imeInsetBottom);
        nativeResize(nativeHandle, width / density, usableHeight / density);
    }

    private void sendPointer(MotionEvent event, int index, int type) {
        nativePointer(nativeHandle, type, pointerDeviceKind(event, index), event.getPointerId(index),
                event.getX(index) / density, event.getY(index) / density);
    }

    private int pointerDeviceKind(MotionEvent event, int index) {
        int toolType = event.getToolType(index);
        if (toolType == MotionEvent.TOOL_TYPE_MOUSE) {
            return POINTER_DEVICE_MOUSE;
        }
        if (toolType == MotionEvent.TOOL_TYPE_STYLUS || toolType == MotionEvent.TOOL_TYPE_ERASER) {
            return POINTER_DEVICE_PEN;
        }
        return POINTER_DEVICE_TOUCH;
    }

    private void sendKey(KeyEvent event, boolean down) {
        int unicode = down ? event.getUnicodeChar() : 0;
        byte[] text =
                unicode == 0 ? new byte[0] : new String(Character.toChars(unicode)).getBytes(StandardCharsets.UTF_8);
        nativeKey(nativeHandle, down, event.getKeyCode(), text, event.isShiftPressed(), event.isCtrlPressed(),
                event.isAltPressed(), event.isMetaPressed(), event.getRepeatCount() > 0);
    }

    private void scheduleFrame(long delayMilliseconds) {
        long now = SystemClock.uptimeMillis();
        long targetTime = now + Math.max(0L, delayMilliseconds);
        if (frameScheduled && targetTime >= frameTime) {
            return;
        }
        if (frameScheduled) {
            removeCallbacks(frameCallback);
        }
        frameScheduled = true;
        frameTime = targetTime;
        if (delayMilliseconds <= 0L) {
            postOnAnimation(frameCallback);
        } else {
            postDelayed(frameCallback, delayMilliseconds);
        }
    }

    private void invalidateFullFrame() {
        invalidate();
    }

    private float[] measureText(byte[] utf8, float fontSize, float maxWidth) {
        String text = new String(utf8, StandardCharsets.UTF_8);
        prepareTextPaint(fontSize, 0xFF000000);
        boolean constrained = !Float.isInfinite(maxWidth) && !Float.isNaN(maxWidth);
        if (constrained && maxWidth <= 0.0F) {
            return new float[] {0.0F, 0.0F};
        }

        float desiredWidth = StaticLayout.getDesiredWidth(text, textPaint);
        int layoutWidth = Math.max(1, (int) Math.ceil(constrained ? maxWidth : desiredWidth));
        StaticLayout layout = createTextLayout(text, layoutWidth, Layout.Alignment.ALIGN_NORMAL);
        float measuredWidth = 0.0F;
        for (int line = 0; line < layout.getLineCount(); ++line) {
            measuredWidth = Math.max(measuredWidth, layout.getLineWidth(line));
        }
        if (constrained) {
            measuredWidth = Math.min(measuredWidth, maxWidth);
        }
        return new float[] {
                (float) Math.ceil(measuredWidth),
                (float) Math.ceil(layout.getHeight()),
        };
    }

    private Object createTextLayout(byte[] utf8, float fontSize, float maxWidth) {
        prepareTextPaint(fontSize, 0xFF000000);
        return new HuxerUITextLayout(new String(utf8, StandardCharsets.UTF_8), new TextPaint(textPaint), maxWidth);
    }

    private void drawRect(Canvas canvas, float x, float y, float width, float height, int color, float cornerRadius) {
        if (width <= 0.0F || height <= 0.0F) {
            return;
        }
        preparePaint(color, Paint.Style.FILL, 0.0F);
        rect.set(x, y, x + width, y + height);
        canvas.drawRoundRect(rect, Math.max(0.0F, cornerRadius), Math.max(0.0F, cornerRadius), paint);
    }

    private void drawText(Canvas canvas, byte[] utf8, float x, float y, float width, float height, int color,
            float fontSize, int alignment) {
        if (width <= 0.0F || height <= 0.0F) {
            return;
        }
        String text = new String(utf8, StandardCharsets.UTF_8);
        prepareTextPaint(fontSize, color);
        int saveCount = canvas.save();
        canvas.clipRect(x, y, x + width, y + height);
        if (alignment == TEXT_ALIGN_CENTER) {
            Paint.FontMetrics metrics = textPaint.getFontMetrics();
            float textWidth = textPaint.measureText(text);
            float baseline = y + (height - metrics.descent + metrics.ascent) * 0.5F - metrics.ascent;
            canvas.drawText(text, x + Math.max(0.0F, (width - textWidth) * 0.5F), baseline, textPaint);
        } else {
            StaticLayout layout =
                    createTextLayout(text, Math.max(1, (int) Math.ceil(width)), Layout.Alignment.ALIGN_NORMAL);
            canvas.translate(x, y);
            layout.draw(canvas);
        }
        canvas.restoreToCount(saveCount);
    }

    private void drawCircle(Canvas canvas, float centerX, float centerY, float radius, int color) {
        if (radius <= 0.0F) {
            return;
        }
        preparePaint(color, Paint.Style.FILL, 0.0F);
        canvas.drawCircle(centerX, centerY, radius, paint);
    }

    private void drawArc(Canvas canvas, float centerX, float centerY, float radius, float startAngle, float sweepAngle,
            int color, float width, int cap) {
        if (radius <= 0.0F || width <= 0.0F) {
            return;
        }
        preparePaint(color, Paint.Style.STROKE, width);
        if (cap == STROKE_CAP_ROUND) {
            paint.setStrokeCap(Paint.Cap.ROUND);
        } else if (cap == STROKE_CAP_SQUARE) {
            paint.setStrokeCap(Paint.Cap.SQUARE);
        } else {
            paint.setStrokeCap(Paint.Cap.BUTT);
        }
        rect.set(centerX - radius, centerY - radius, centerX + radius, centerY + radius);
        canvas.drawArc(rect, startAngle, sweepAngle, false, paint);
    }

    private void drawBorder(Canvas canvas, float x, float y, float width, float height, int color, float strokeWidth,
            float cornerRadius) {
        if (width <= 0.0F || height <= 0.0F || strokeWidth <= 0.0F) {
            return;
        }
        preparePaint(color, Paint.Style.STROKE, strokeWidth);
        float inset = strokeWidth * 0.5F;
        rect.set(x + inset, y + inset, x + Math.max(inset, width - inset), y + Math.max(inset, height - inset));
        float radius = Math.max(0.0F, cornerRadius - inset);
        canvas.drawRoundRect(rect, radius, radius, paint);
    }

    private void drawShadow(Canvas canvas, float x, float y, float width, float height, int color, float blurRadius,
            float cornerRadius) {
        if (shadowRenderer == null) {
            shadowRenderer = new HuxerUIShadowRenderer(density);
        }
        shadowRenderer.draw(canvas, x, y, width, height, color, blurRadius, cornerRadius);
    }

    private void pushClip(Canvas canvas, float x, float y, float width, float height, float cornerRadius) {
        canvas.save();
        rect.set(x, y, x + width, y + height);
        if (cornerRadius <= 0.0F) {
            canvas.clipRect(rect);
            return;
        }
        float radius = Math.min(cornerRadius, Math.min(width, height) * 0.5F);
        path.reset();
        path.addRoundRect(rect, radius, radius, Path.Direction.CW);
        canvas.clipPath(path);
    }

    private void popClip(Canvas canvas) {
        canvas.restore();
    }

    private void pushOpacity(Canvas canvas, float opacity) {
        int alpha = Math.round(Math.max(0.0F, Math.min(opacity, 1.0F)) * 255.0F);
        canvas.saveLayerAlpha(null, alpha);
    }

    private void popOpacity(Canvas canvas) {
        canvas.restore();
    }

    private void pushTransform(
            Canvas canvas, float m11, float m12, float m21, float m22, float translateX, float translateY) {
        canvas.save();
        transformValues[Matrix.MSCALE_X] = m11;
        transformValues[Matrix.MSKEW_X] = m21;
        transformValues[Matrix.MTRANS_X] = translateX;
        transformValues[Matrix.MSKEW_Y] = m12;
        transformValues[Matrix.MSCALE_Y] = m22;
        transformValues[Matrix.MTRANS_Y] = translateY;
        transformValues[Matrix.MPERSP_0] = 0.0F;
        transformValues[Matrix.MPERSP_1] = 0.0F;
        transformValues[Matrix.MPERSP_2] = 1.0F;
        transform.setValues(transformValues);
        canvas.concat(transform);
    }

    private void popTransform(Canvas canvas) {
        canvas.restore();
    }

    private void preparePaint(int color, Paint.Style style, float strokeWidth) {
        paint.setColor(color);
        paint.setStyle(style);
        paint.setStrokeWidth(strokeWidth);
        paint.setStrokeCap(Paint.Cap.BUTT);
    }

    private void prepareTextPaint(float fontSize, int color) {
        textPaint.setTextSize(fontSize);
        textPaint.setColor(color);
        textPaint.setStyle(Paint.Style.FILL);
    }

    private StaticLayout createTextLayout(String text, int width, Layout.Alignment alignment) {
        return StaticLayout.Builder.obtain(text, 0, text.length(), textPaint, width)
                .setAlignment(alignment)
                .setIncludePad(true)
                .build();
    }

    private static native long nativeCreate(HuxerUIView view);

    private static native void nativeDestroy(long handle);

    private static native void nativeResize(long handle, float width, float height);

    private static native void nativeCommitFrame(long handle);

    private static native void nativeDraw(long handle, Canvas canvas);

    private static native void nativePointer(long handle, int type, int deviceKind, long pointerId, float x, float y);

    private static native void nativeScroll(long handle, float x, float y, float deltaX, float deltaY);

    private static native void nativeKey(long handle, boolean down, int keyCode, byte[] text, boolean shift,
            boolean control, boolean alt, boolean meta, boolean repeat);
}
