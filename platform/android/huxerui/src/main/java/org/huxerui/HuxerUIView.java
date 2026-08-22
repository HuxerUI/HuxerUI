package org.huxerui;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.res.Configuration;
import android.graphics.Canvas;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.Debug;
import android.os.SystemClock;
import android.os.Build;
import android.text.Layout;
import android.text.StaticLayout;
import android.text.TextPaint;
import android.text.TextDirectionHeuristics;
import android.util.AttributeSet;
import android.util.LongSparseArray;
import android.util.LruCache;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewParent;
import android.view.ViewTreeObserver;
import android.view.WindowInsets;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Locale;
import java.util.Objects;

public final class HuxerUIView extends ViewGroup {
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

    // These values form the JNI protocol decoded explicitly by the platform adapter.
    private static final int BACK_BEGIN = 0;
    private static final int BACK_UPDATE = 1;
    private static final int BACK_CANCEL = 2;
    private static final int BACK_COMMIT = 3;

    private static final int TEXT_ALIGN_CENTER = 1;
    private static final int TEXT_ALIGN_TRAILING = 2;
    private static final int TEXT_WRAP_WORD = 1;
    private static final int TEXT_DIRECTION_LEFT_TO_RIGHT = 1;
    private static final int TEXT_DIRECTION_RIGHT_TO_LEFT = 2;
    private static final int FONT_FAMILY_MONOSPACE = 1;
    private static final int FONT_FAMILY_NAMED = 2;
    private static final int TEXT_DECORATION_UNDERLINE = 1;
    private static final int TEXT_DECORATION_STRIKE_THROUGH = 2;
    private static final int STROKE_CAP_ROUND = 1;
    private static final int STROKE_CAP_SQUARE = 2;
    private static final int STROKE_JOIN_ROUND = 1;
    private static final int STROKE_JOIN_BEVEL = 2;
    private static final int PATH_FILL_EVEN_ODD = 1;

    private static final int PATH_MOVE_TO = 0;
    private static final int PATH_LINE_TO = 1;
    private static final int PATH_QUADRATIC_TO = 2;
    private static final int PATH_CUBIC_TO = 3;
    private static final int PATH_CLOSE = 4;
    private static final int IMAGE_CACHE_BUDGET = 64 * 1024 * 1024;
    private static final long[] EMPTY_PLATFORM_COMPOSITION = new long[0];

    static final int SYSTEM_BAR_CONTENT_LIGHT = 1;
    static final int SYSTEM_BAR_CONTENT_DARK = 2;

    interface SystemBarsController {
        void setContentBrightness(int statusBar, int navigationBar);
    }

    public interface FilePickerLauncher {
        void launch(Intent intent, int requestCode);

        void cancel(int requestCode);
    }

    private static final float SCROLL_STEP = 48.0F;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);
    private final TextPaint textPaint = new TextPaint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);
    private final Rect textBounds = new Rect();
    private final RectF rect = new RectF();
    private final Path clipPath = new Path();
    private final LruCache<PathKey, Path> pathCache = new LruCache<>(128);
    private final LruCache<FontKey, Typeface> fontCache = new LruCache<>(32);
    private final LruCache<ParagraphKey, StaticLayout> paragraphCache = new LruCache<>(256);
    private final LruCache<Long, Bitmap> imageCache = new LruCache<Long, Bitmap>(IMAGE_CACHE_BUDGET) {
        @Override
        protected int sizeOf(Long identity, Bitmap bitmap) {
            // Cap one entry at the budget so an oversized image remains cached until another image evicts it.
            return Math.min(bitmap.getAllocationByteCount(), IMAGE_CACHE_BUDGET);
        }
    };
    private final Matrix transform = new Matrix();
    private final float[] transformValues = new float[9];
    private final int[] screenLocation = new int[2];
    private final HuxerUIAccessibilityProvider accessibilityProvider;
    private final HuxerUIFilePicker filePicker;
    private final LongSparseArray<PlatformViewContainer> platformViews = new LongSparseArray<>();
    private float density;
    private final ViewTreeObserver.OnPreDrawListener textInputGeometryListener = this::updateTextInputGeometry;
    private final ViewTreeObserver.OnGlobalFocusChangeListener platformViewFocusListener =
            this::synchronizePlatformViewFocus;
    private final Runnable frameCallback = new Runnable() {
        @Override
        public void run() {
            frameScheduled = false;
            frameTime = 0L;
            if (nativeHandle != 0L) {
                byte[] semantics = nativeCommitFrame(nativeHandle);
                boolean platformViewStructureChanged = platformAccessibilityStructureChanged;
                platformAccessibilityStructureChanged = false;
                if (semantics != null) {
                    accessibilityProvider.commitFrame(semantics, platformViewStructureChanged);
                } else if (platformViewStructureChanged) {
                    accessibilityProvider.commitPlatformViews();
                }
            }
        }
    };
    private final Runnable platformTasksCallback = new Runnable() {
        @Override
        public void run() {
            if (nativeHandle != 0L) {
                nativeDrainPlatformTasks(nativeHandle);
            }
        }
    };

    private long nativeHandle;
    private boolean frameScheduled;
    private long frameTime;
    private HuxerUIInputConnection inputConnection;
    private HuxerUIShadowRenderer shadowRenderer;
    private int imeInsetBottom;
    private int safeInsetLeft;
    private int safeInsetTop;
    private int safeInsetRight;
    private int safeInsetBottom;
    private SystemBarsController systemBarsController;
    private FilePickerLauncher filePickerLauncher;
    private long[] platformComposition = EMPTY_PLATFORM_COMPOSITION;
    private boolean nativeDrawing;
    private boolean applyingPlatformViewFocus;
    private boolean platformAccessibilityStructureChanged;
    private long touchPlatformViewIdentity;
    private boolean platformTabKeyHandled;
    private HuxerUIApplicationActivation startupApplicationActivation;
    private final ArrayList<HuxerUIApplicationActivation> pendingApplicationActivations = new ArrayList<>();

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
        accessibilityProvider = new HuxerUIAccessibilityProvider(this);
        filePicker = new HuxerUIFilePicker(this);
        density = getResources().getDisplayMetrics().density;
        setFocusable(true);
        setFocusableInTouchMode(true);
        setClickable(true);
        setWillNotDraw(false);
    }

    void setSystemBarsController(SystemBarsController controller) {
        systemBarsController = controller;
    }

    public void setFilePickerLauncher(FilePickerLauncher launcher) {
        if (filePickerLauncher == launcher) {
            return;
        }
        filePicker.cancelActive();
        filePickerLauncher = launcher;
    }

    public boolean dispatchFilePickerResult(int requestCode, int resultCode, Intent data) {
        return filePicker.dispatchResult(requestCode, resultCode, data);
    }

    /** Sets the Intent normalized as startup input for the next Runtime created by this View. */
    public void setStartupApplicationIntent(Intent intent) {
        if (nativeHandle != 0L) {
            throw new IllegalStateException(
                    "HuxerUI startup application Intent must be set before the View is attached");
        }
        startupApplicationActivation = HuxerUIApplicationActivation.fromIntent(getContext(), intent);
    }

    /** Delivers a supported later Intent to this View's Runtime, retaining it until attachment when necessary. */
    public boolean dispatchApplicationIntent(Intent intent) {
        HuxerUIApplicationActivation activation = HuxerUIApplicationActivation.fromIntent(getContext(), intent);
        if (activation == null) {
            return false;
        }
        if (nativeHandle == 0L) {
            pendingApplicationActivations.add(activation);
        } else {
            handleNativeApplicationActivation(activation);
        }
        return true;
    }

    FilePickerLauncher filePickerLauncher() {
        return filePickerLauncher;
    }

    boolean canOpenFiles() {
        return filePickerLauncher != null;
    }

    boolean canSaveFiles() {
        return filePickerLauncher != null;
    }

    HuxerUIFilePicker.Operation prepareOpenFiles(
            long nativeHandle, String[] extensions, String[] contentTypes, boolean multiple) {
        return filePicker.prepareOpen(nativeHandle, extensions, contentTypes, multiple);
    }

    HuxerUIFilePicker.Operation prepareSaveFile(
            long nativeHandle, String sourcePath, String suggestedName, String[] extensions, String[] contentTypes) {
        return filePicker.prepareSave(nativeHandle, sourcePath, suggestedName, extensions, contentTypes);
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        ViewTreeObserver observer = getViewTreeObserver();
        observer.addOnPreDrawListener(textInputGeometryListener);
        observer.addOnGlobalFocusChangeListener(platformViewFocusListener);
        requestFocus();
        if (nativeHandle == 0L) {
            nativeHandle = createNativeRuntime(startupApplicationActivation);
            startupApplicationActivation = null;
            resizeRuntime(getWidth(), getHeight());
            for (HuxerUIApplicationActivation activation : pendingApplicationActivations) {
                handleNativeApplicationActivation(activation);
            }
            pendingApplicationActivations.clear();
        }
        requestApplyInsets();
    }

    @Override
    protected void onDetachedFromWindow() {
        ViewTreeObserver observer = getViewTreeObserver();
        if (observer.isAlive()) {
            observer.removeOnPreDrawListener(textInputGeometryListener);
            observer.removeOnGlobalFocusChangeListener(platformViewFocusListener);
        }
        removeCallbacks(frameCallback);
        removeCallbacks(platformTasksCallback);
        if (shadowRenderer != null) {
            shadowRenderer.clear();
            shadowRenderer = null;
        }
        pathCache.evictAll();
        paragraphCache.evictAll();
        imageCache.evictAll();
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
        accessibilityProvider.reset();
        super.onDetachedFromWindow();
    }

    @Override
    protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
        super.onSizeChanged(width, height, oldWidth, oldHeight);
        resizeRuntime(width, height);
    }

    @Override
    protected void onConfigurationChanged(Configuration newConfiguration) {
        super.onConfigurationChanged(newConfiguration);
        if (nativeHandle != 0L) {
            float displayScale = resourceScale();
            if (density != displayScale) {
                density = displayScale;
                if (shadowRenderer != null) {
                    shadowRenderer.clear();
                    shadowRenderer = null;
                }
                resizeRuntime(getWidth(), getHeight());
            }
            nativeUpdateResourceConfiguration(nativeHandle, resourceLocale(), displayScale);
        }
    }

    @Override
    protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
        for (int index = 0; index < platformViews.size(); ++index) {
            platformViews.valueAt(index).applyLayout();
        }
        if (changed && inputConnection != null) {
            inputConnection.updateCursorAnchorPosition();
        }
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        super.onMeasure(widthMeasureSpec, heightMeasureSpec);
        for (int index = 0; index < platformViews.size(); ++index) {
            PlatformViewContainer container = platformViews.valueAt(index);
            container.measure(MeasureSpec.makeMeasureSpec(container.containerWidth, MeasureSpec.EXACTLY),
                    MeasureSpec.makeMeasureSpec(container.containerHeight, MeasureSpec.EXACTLY));
        }
    }

    @Override
    public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            android.graphics.Insets systemBars = insets.getInsets(WindowInsets.Type.systemBars());
            android.graphics.Insets displayCutout = insets.getInsets(WindowInsets.Type.displayCutout());
            safeInsetLeft = Math.max(systemBars.left, displayCutout.left);
            safeInsetTop = Math.max(systemBars.top, displayCutout.top);
            safeInsetRight = Math.max(systemBars.right, displayCutout.right);
            safeInsetBottom = Math.max(systemBars.bottom, displayCutout.bottom);
            imeInsetBottom = insets.getInsets(WindowInsets.Type.ime()).bottom;
        } else {
            safeInsetLeft = insets.getStableInsetLeft();
            safeInsetTop = insets.getStableInsetTop();
            safeInsetRight = insets.getStableInsetRight();
            safeInsetBottom = insets.getStableInsetBottom();
            imeInsetBottom = Math.max(0, insets.getSystemWindowInsetBottom() - safeInsetBottom);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P && insets.getDisplayCutout() != null) {
                safeInsetLeft = Math.max(safeInsetLeft, insets.getDisplayCutout().getSafeInsetLeft());
                safeInsetTop = Math.max(safeInsetTop, insets.getDisplayCutout().getSafeInsetTop());
                safeInsetRight = Math.max(safeInsetRight, insets.getDisplayCutout().getSafeInsetRight());
                safeInsetBottom = Math.max(safeInsetBottom, insets.getDisplayCutout().getSafeInsetBottom());
            }
        }
        resizeRuntime(getWidth(), getHeight());
        return super.onApplyWindowInsets(insets);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        canvas.drawColor(0xFFF7F8FA);
        if (nativeHandle == 0L) {
            return;
        }
        nativeBeginDraw(nativeHandle);
        nativeDrawing = true;
        int saveCount = canvas.save();
        canvas.scale(density, density);
        nativeDrawBase(nativeHandle, canvas);
        canvas.restoreToCount(saveCount);
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) {
            touchPlatformViewIdentity = platformViewAt(event.getX(), event.getY());
        }
        boolean handled = super.dispatchTouchEvent(event);
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            touchPlatformViewIdentity = 0L;
        }
        return handled;
    }

    @Override
    public boolean onInterceptTouchEvent(MotionEvent event) {
        return touchPlatformViewIdentity == 0L;
    }

    @Override
    protected void dispatchDraw(Canvas canvas) {
        try {
            if (nativeHandle == 0L) {
                return;
            }
            if (!nativeDrawing) {
                nativeBeginDraw(nativeHandle);
                nativeDrawing = true;
            }
            long drawingTime = getDrawingTime();
            for (int index = 0; index < platformComposition.length; index += 3) {
                long kind = platformComposition[index];
                if (kind == 0L) {
                    int saveCount = canvas.save();
                    canvas.scale(density, density);
                    nativeDrawSlice(
                            nativeHandle, canvas, platformComposition[index + 1], platformComposition[index + 2]);
                    canvas.restoreToCount(saveCount);
                    continue;
                }
                PlatformViewContainer container = platformViews.get(platformComposition[index + 1]);
                if (container != null && container.getVisibility() == VISIBLE) {
                    drawChild(canvas, container, drawingTime);
                }
            }
        } finally {
            if (nativeDrawing && nativeHandle != 0L) {
                nativeDrawing = false;
                nativeEndDraw(nativeHandle);
            }
        }
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
    public AccessibilityNodeProvider getAccessibilityNodeProvider() {
        return accessibilityProvider;
    }

    @Override
    public boolean dispatchHoverEvent(MotionEvent event) {
        boolean accessibilityHandled = accessibilityProvider.dispatchHoverEvent(event);
        boolean viewHandled = super.dispatchHoverEvent(event);
        return accessibilityHandled || viewHandled;
    }

    @Override
    public boolean onInterceptHoverEvent(MotionEvent event) {
        return platformViewAt(event.getX(), event.getY()) == 0L;
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
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (event.getActionMasked() == MotionEvent.ACTION_SCROLL && platformViewAt(event.getX(), event.getY()) == 0L) {
            return onGenericMotionEvent(event);
        }
        return super.dispatchGenericMotionEvent(event);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (nativeHandle != 0L && event.getKeyCode() == KeyEvent.KEYCODE_TAB) {
            long identity = platformViewIdentity(findFocus());
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                if (event.getRepeatCount() == 0) {
                    platformTabKeyHandled = identity != 0L
                            && nativeMoveFocusFromPlatformView(nativeHandle, identity, event.isShiftPressed());
                }
                if (platformTabKeyHandled) {
                    return true;
                }
            } else if (event.getAction() == KeyEvent.ACTION_UP && platformTabKeyHandled) {
                platformTabKeyHandled = false;
                return true;
            }
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            return super.onKeyDown(keyCode, event);
        }
        if (nativeHandle == 0L) {
            return super.onKeyDown(keyCode, event);
        }
        sendKey(event, true);
        return true;
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            return super.onKeyUp(keyCode, event);
        }
        if (nativeHandle == 0L) {
            return super.onKeyUp(keyCode, event);
        }
        sendKey(event, false);
        return true;
    }

    public boolean handleBack() {
        return nativeHandle != 0L && nativeHandleBack(nativeHandle, BACK_COMMIT, 1.0F);
    }

    boolean beginBack() {
        return nativeHandle != 0L && nativeHandleBack(nativeHandle, BACK_BEGIN, 0.0F);
    }

    boolean updateBack(float progress) {
        return nativeHandle != 0L && nativeHandleBack(nativeHandle, BACK_UPDATE, progress);
    }

    boolean cancelBack() {
        return nativeHandle != 0L && nativeHandleBack(nativeHandle, BACK_CANCEL, 0.0F);
    }

    boolean commitBack() {
        return nativeHandle != 0L && nativeHandleBack(nativeHandle, BACK_COMMIT, 1.0F);
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

    private byte[] resourceLocale() {
        Configuration configuration = getResources().getConfiguration();
        Locale locale = Build.VERSION.SDK_INT >= Build.VERSION_CODES.N && !configuration.getLocales().isEmpty()
                ? configuration.getLocales().get(0)
                : configuration.locale;
        if (locale == null) {
            locale = Locale.getDefault();
        }
        return locale.toLanguageTag().getBytes(StandardCharsets.UTF_8);
    }

    private float resourceScale() {
        return getResources().getDisplayMetrics().density;
    }

    private long processPssBytes() {
        return Debug.getPss() * 1024L;
    }

    private byte[] readResource(byte[] encodedPath) {
        String path = new String(encodedPath, StandardCharsets.UTF_8);
        try (InputStream stream = getContext().getAssets().open(path)) {
            return readAllBytes(stream);
        } catch (IOException exception) {
            return null;
        }
    }

    private static byte[] readAllBytes(InputStream stream) throws IOException {
        ByteArrayOutputStream result = new ByteArrayOutputStream();
        byte[] buffer = new byte[8192];
        int length;
        while ((length = stream.read(buffer)) != -1) {
            if (length > 0) {
                result.write(buffer, 0, length);
            }
        }
        return result.toByteArray();
    }

    private void resizeRuntime(int width, int height) {
        if (nativeHandle == 0L) {
            return;
        }
        int usableHeight = Math.max(0, height - imeInsetBottom);
        int bottomInset = imeInsetBottom > 0 ? 0 : safeInsetBottom;
        nativeResize(nativeHandle, width / density, usableHeight / density, safeInsetLeft / density,
                safeInsetTop / density, safeInsetRight / density, bottomInset / density);
    }

    private void setSystemBarsContentBrightness(int statusBar, int navigationBar) {
        if (systemBarsController != null) {
            systemBarsController.setContentBrightness(statusBar, navigationBar);
        }
    }

    private void sendPointer(MotionEvent event, int index, int type) {
        nativePointer(nativeHandle, type, pointerDeviceKind(event, index), event.getPointerId(index),
                event.getX(index) / density, event.getY(index) / density);
    }

    float density() {
        return density;
    }

    void transformToScreen(Matrix result) {
        result.reset();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            transformMatrixToGlobal(result);
            return;
        }

        // Older APIs expose only per-View matrices, so compose the same root-to-leaf order without retaining parents.
        transformToScreenBeforeQ(this, result);
    }

    private void transformToScreenBeforeQ(View view, Matrix result) {
        ViewParent parent = view.getParent();
        if (!(parent instanceof View)) {
            view.getLocationOnScreen(screenLocation);
            result.setTranslate(screenLocation[0], screenLocation[1]);
            return;
        }

        View parentView = (View) parent;
        transformToScreenBeforeQ(parentView, result);
        result.preTranslate(-parentView.getScrollX(), -parentView.getScrollY());
        result.preTranslate(view.getLeft(), view.getTop());
        if (!view.getMatrix().isIdentity()) {
            result.preConcat(view.getMatrix());
        }
    }

    boolean performSemanticAction(int nodeId, int actionKind, String text, long argument0, long argument1,
            double number, float x, float y, long customId) {
        if (nativeHandle == 0L) {
            return false;
        }
        byte[] encodedText = text == null ? null : text.getBytes(StandardCharsets.UTF_8);
        return nativePerformSemanticAction(nativeHandle, nodeId, actionKind, encodedText, argument0, argument1, number,
                x, y, customId);
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

    long platformViewAt(float x, float y) {
        if (nativeHandle == 0L) {
            return 0L;
        }
        long identity = nativeHitTestPlatformView(nativeHandle, x / density, y / density);
        PlatformViewContainer container = identity == 0L ? null : platformViews.get(identity);
        return container != null && container.getVisibility() == VISIBLE ? identity : 0L;
    }

    private long platformViewIdentity(View view) {
        if (view == null) {
            return 0L;
        }
        for (int index = 0; index < platformViews.size(); ++index) {
            PlatformViewContainer container = platformViews.valueAt(index);
            if (container.contains(view)) {
                return platformViews.keyAt(index);
            }
        }
        return 0L;
    }

    private void synchronizePlatformViewFocus(View oldFocus, View newFocus) {
        if (applyingPlatformViewFocus || nativeHandle == 0L) {
            return;
        }
        long oldIdentity = platformViewIdentity(oldFocus);
        long newIdentity = platformViewIdentity(newFocus);
        if (newIdentity != 0L) {
            nativeSynchronizePlatformViewFocus(nativeHandle, newIdentity, !isInTouchMode());
        } else if (oldIdentity != 0L) {
            nativeSynchronizePlatformViewFocus(nativeHandle, 0L, false);
            hidePlatformViewInput();
        }
    }

    private boolean applyPlatformViewFocus(long identity) {
        PlatformViewContainer container = identity == 0L ? null : platformViews.get(identity);
        if (container != null && container.getVisibility() != VISIBLE) {
            container = null;
        }
        applyingPlatformViewFocus = true;
        try {
            if (identity == 0L) {
                if (platformViewIdentity(findFocus()) != 0L) {
                    requestFocus();
                    hidePlatformViewInput();
                }
                return platformViewIdentity(findFocus()) == 0L;
            }
            if (platformViewIdentity(findFocus()) == identity) {
                return true;
            }
            return container != null && container.requestContentFocus()
                    && platformViewIdentity(findFocus()) == identity;
        } finally {
            applyingPlatformViewFocus = false;
        }
    }

    private void hidePlatformViewInput() {
        post(() -> {
            if (inputConnection == null && platformViewIdentity(findFocus()) == 0L && getWindowToken() != null) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && getWindowInsetsController() != null) {
                    getWindowInsetsController().hide(WindowInsets.Type.ime());
                } else {
                    inputMethodManager().hideSoftInputFromWindow(getWindowToken(), 0);
                }
            }
        });
    }

    private int validatePlatformView(Object candidate) {
        if (!(candidate instanceof View)) {
            return 1;
        }
        View content = (View) candidate;
        if (containsSurfaceView(content)) {
            return 2;
        }
        return content.getParent() == null ? 0 : 3;
    }

    private int mountPlatformView(long identity, Object candidate) {
        int validation = validatePlatformView(candidate);
        if (validation != 0) {
            return validation;
        }
        if (platformViews.get(identity) != null) {
            return 3;
        }
        View content = (View) candidate;
        PlatformViewContainer container = new PlatformViewContainer(this, content);
        container.setVisibility(INVISIBLE);
        platformViews.put(identity, container);
        addView(container);
        platformAccessibilityStructureChanged = true;
        return 0;
    }

    private void placePlatformView(long identity, float worldX, float worldY, float worldWidth, float worldHeight,
            float visibleX, float visibleY, float visibleWidth, float visibleHeight, boolean visible) {
        PlatformViewContainer container = platformViews.get(identity);
        if (container == null) {
            throw new IllegalStateException("HuxerUI Android PlatformView placement has no mounted View");
        }
        platformAccessibilityStructureChanged |= container.place(density, worldX, worldY, worldWidth, worldHeight,
                visibleX, visibleY, visibleWidth, visibleHeight, visible);
    }

    private void removePlatformView(long identity) {
        PlatformViewContainer container = platformViews.get(identity);
        if (container == null) {
            return;
        }
        boolean focused = container.contains(findFocus());
        applyingPlatformViewFocus = true;
        try {
            container.clearAccessibilityParent();
            removeView(container);
            platformViews.remove(identity);
            platformAccessibilityStructureChanged = true;
            if (focused) {
                requestFocus();
            }
        } finally {
            applyingPlatformViewFocus = false;
        }
        hidePlatformViewInput();
    }

    void resetPlatformViewAccessibility() {
        for (int index = 0; index < platformViews.size(); ++index) {
            platformViews.valueAt(index).clearAccessibilityParent();
        }
    }

    void configurePlatformViewAccessibility(long identity, int parentId) {
        PlatformViewContainer container = platformViews.get(identity);
        if (container != null && container.getVisibility() == VISIBLE) {
            container.setAccessibilityParent(parentId);
        }
    }

    View platformViewAccessibilityChild(long identity) {
        PlatformViewContainer container = platformViews.get(identity);
        return container != null && container.hasAccessibilityParent() ? container : null;
    }

    void removePlatformViewAccessibilityChildren(AccessibilityNodeInfo info) {
        for (int index = 0; index < platformViews.size(); ++index) {
            info.removeChild(platformViews.valueAt(index));
        }
    }

    private void commitPlatformComposition(long[] composition) {
        if (composition == null || composition.length % 3 != 0) {
            throw new IllegalArgumentException("HuxerUI Android RenderComposition descriptor is invalid");
        }
        platformComposition = composition.length == 0 ? EMPTY_PLATFORM_COMPOSITION : composition;
        for (int index = 0; index < platformViews.size(); ++index) {
            platformViews.valueAt(index).allowContentFocus();
        }
        synchronizePlatformViewOrder();
        invalidate();
    }

    private void synchronizePlatformViewOrder() {
        int childIndex = 0;
        boolean orderChanged = false;
        for (int index = 0; index < platformComposition.length; index += 3) {
            if (platformComposition[index] != 1L) {
                continue;
            }
            PlatformViewContainer container = platformViews.get(platformComposition[index + 1]);
            if (container != null) {
                orderChanged |= childIndex >= getChildCount() || getChildAt(childIndex) != container;
                ++childIndex;
            }
        }
        if (!orderChanged) {
            return;
        }
        for (int index = 0; index < platformComposition.length; index += 3) {
            if (platformComposition[index] == 1L) {
                PlatformViewContainer container = platformViews.get(platformComposition[index + 1]);
                if (container != null) {
                    bringChildToFront(container);
                }
            }
        }
    }

    private static boolean containsSurfaceView(View view) {
        if (view instanceof SurfaceView) {
            return true;
        }
        if (!(view instanceof ViewGroup)) {
            return false;
        }
        ViewGroup group = (ViewGroup) view;
        for (int index = 0; index < group.getChildCount(); ++index) {
            if (containsSurfaceView(group.getChildAt(index))) {
                return true;
            }
        }
        return false;
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

    private void schedulePlatformTasks() {
        post(platformTasksCallback);
    }

    private void invalidateFullFrame() {
        invalidate();
    }

    private static final class PlatformViewContainer extends ViewGroup {
        private final HuxerUIView root;
        private final View content;
        private int accessibilityParentId = AccessibilityNodeProvider.HOST_VIEW_ID;
        private boolean accessibilityParentSet;
        private int containerLeft;
        private int containerTop;
        private int containerWidth;
        private int containerHeight;
        private int contentLeft;
        private int contentTop;
        private int contentWidth;
        private int contentHeight;

        PlatformViewContainer(HuxerUIView root, View content) {
            super(content.getContext());
            this.root = root;
            this.content = content;
            setClipChildren(true);
            setClipToPadding(true);
            setDescendantFocusability(FOCUS_BLOCK_DESCENDANTS);
            setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);
            addView(content);
        }

        void allowContentFocus() {
            if (getDescendantFocusability() == FOCUS_BLOCK_DESCENDANTS) {
                setDescendantFocusability(FOCUS_AFTER_DESCENDANTS);
            }
        }

        boolean place(float density, float worldX, float worldY, float worldWidth, float worldHeight, float visibleX,
                float visibleY, float visibleWidth, float visibleHeight, boolean visible) {
            int nextContainerLeft = Math.round(visibleX * density);
            int nextContainerTop = Math.round(visibleY * density);
            int nextContainerRight = Math.round((visibleX + visibleWidth) * density);
            int nextContainerBottom = Math.round((visibleY + visibleHeight) * density);
            int worldLeft = Math.round(worldX * density);
            int worldTop = Math.round(worldY * density);
            int worldRight = Math.round((worldX + worldWidth) * density);
            int worldBottom = Math.round((worldY + worldHeight) * density);
            int nextContainerWidth = Math.max(0, nextContainerRight - nextContainerLeft);
            int nextContainerHeight = Math.max(0, nextContainerBottom - nextContainerTop);
            int nextContentWidth = Math.max(0, worldRight - worldLeft);
            int nextContentHeight = Math.max(0, worldBottom - worldTop);
            int nextContentLeft = worldLeft - nextContainerLeft;
            int nextContentTop = worldTop - nextContainerTop;
            boolean geometryChanged = containerLeft != nextContainerLeft || containerTop != nextContainerTop
                    || containerWidth != nextContainerWidth || containerHeight != nextContainerHeight
                    || contentLeft != nextContentLeft || contentTop != nextContentTop
                    || contentWidth != nextContentWidth || contentHeight != nextContentHeight;
            containerLeft = nextContainerLeft;
            containerTop = nextContainerTop;
            containerWidth = nextContainerWidth;
            containerHeight = nextContainerHeight;
            contentLeft = nextContentLeft;
            contentTop = nextContentTop;
            contentWidth = nextContentWidth;
            contentHeight = nextContentHeight;
            int nextVisibility = visible ? VISIBLE : INVISIBLE;
            boolean visibilityChanged = getVisibility() != nextVisibility;
            if (visibilityChanged) {
                setVisibility(nextVisibility);
            }
            if (geometryChanged) {
                requestLayout();
            }
            return visibilityChanged;
        }

        void setAccessibilityParent(int parentId) {
            accessibilityParentId = parentId;
            accessibilityParentSet = true;
        }

        void clearAccessibilityParent() {
            accessibilityParentSet = false;
        }

        boolean hasAccessibilityParent() {
            return accessibilityParentSet && getVisibility() == VISIBLE;
        }

        void applyLayout() {
            layout(containerLeft, containerTop, containerLeft + containerWidth, containerTop + containerHeight);
        }

        boolean contains(View view) {
            View current = view;
            while (current != null) {
                if (current == this) {
                    return true;
                }
                ViewParent parent = current.getParent();
                current = parent instanceof View ? (View) parent : null;
            }
            return false;
        }

        boolean requestContentFocus() {
            return content.requestFocus();
        }

        @Override
        public void onInitializeAccessibilityNodeInfo(AccessibilityNodeInfo info) {
            super.onInitializeAccessibilityNodeInfo(info);
            if (accessibilityParentSet) {
                if (accessibilityParentId == AccessibilityNodeProvider.HOST_VIEW_ID) {
                    info.setParent(root);
                } else {
                    info.setParent(root, accessibilityParentId);
                }
            }
            info.setFocusable(false);
            info.setClickable(false);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                info.setScreenReaderFocusable(false);
            }
        }

        @Override
        protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
            setMeasuredDimension(MeasureSpec.getSize(widthMeasureSpec), MeasureSpec.getSize(heightMeasureSpec));
            content.measure(MeasureSpec.makeMeasureSpec(contentWidth, MeasureSpec.EXACTLY),
                    MeasureSpec.makeMeasureSpec(contentHeight, MeasureSpec.EXACTLY));
        }

        @Override
        protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
            content.layout(contentLeft, contentTop, contentLeft + contentWidth, contentTop + contentHeight);
        }
    }

    private float[] fontMetrics(float fontSize, int familyKind, byte[] familyName, int weight, int slant) {
        prepareTextPaint(fontSize, 0xFF000000, familyKind, familyName, weight, slant, 0, null);
        Paint.FontMetrics metrics = textPaint.getFontMetrics();
        float underlineThickness = Math.max(1.0F, fontSize / 16.0F);
        return new float[] {
                -metrics.ascent,
                metrics.descent,
                metrics.leading,
                Math.max(0.0F, metrics.descent * 0.5F),
                underlineThickness,
                -metrics.ascent * 0.5F,
                underlineThickness,
        };
    }

    private float[] measureText(byte[] utf8, float fontSize, float maxWidth, int familyKind, byte[] familyName,
            int weight, int slant, int alignment, int wrap, int direction, byte[] utf8Locale) {
        String text = new String(utf8, StandardCharsets.UTF_8);
        prepareTextPaint(fontSize, 0xFF000000, familyKind, familyName, weight, slant, 0, utf8Locale);
        boolean constrained = !Float.isInfinite(maxWidth) && !Float.isNaN(maxWidth);
        if (constrained && maxWidth <= 0.0F) {
            return new float[5];
        }

        float desiredWidth = StaticLayout.getDesiredWidth(text, textPaint);
        float requestedWidth = constrained && wrap == TEXT_WRAP_WORD ? maxWidth : desiredWidth;
        int layoutWidth = Math.max(1, (int) Math.ceil(requestedWidth));
        StaticLayout layout = createTextLayout(text, layoutWidth, toTextAlignment(alignment), direction);
        float measuredWidth = 0.0F;
        for (int line = 0; line < layout.getLineCount(); ++line) {
            measuredWidth = Math.max(measuredWidth, layout.getLineWidth(line));
        }
        if (constrained) {
            measuredWidth = Math.min(measuredWidth, maxWidth);
        }
        float width = (float) Math.ceil(measuredWidth);
        float height = (float) Math.ceil(layout.getHeight());
        return new float[] {width, height, layout.getLineBaseline(0), layout.getLineBaseline(layout.getLineCount() - 1),
                layout.getLineCount()};
    }

    private float[] measureTextRun(byte[] utf8, float fontSize, int familyKind, byte[] familyName, int weight,
            int slant, int decoration, int direction, byte[] utf8Locale) {
        prepareTextPaint(fontSize, 0xFF000000, familyKind, familyName, weight, slant, decoration, utf8Locale);
        String text = new String(utf8, StandardCharsets.UTF_8);
        boolean rightToLeft = isTextRightToLeft(text, direction);
        float advance = textPaint.getRunAdvance(text, 0, text.length(), 0, text.length(), rightToLeft, text.length());
        Paint.FontMetrics metrics = textPaint.getFontMetrics();
        textBounds.setEmpty();
        textPaint.getTextBounds(text, 0, text.length(), textBounds);
        float left = Math.min(0.0F, textBounds.left);
        float top = Math.min(metrics.ascent, textBounds.top);
        float right = Math.max(advance, textBounds.right);
        float bottom = Math.max(metrics.descent, textBounds.bottom);
        float decorationThickness = Math.max(1.0F, fontSize / 16.0F);
        if ((decoration & TEXT_DECORATION_UNDERLINE) != 0) {
            float center = Math.max(0.0F, metrics.descent * 0.5F);
            top = Math.min(top, center - decorationThickness * 0.5F);
            bottom = Math.max(bottom, center + decorationThickness * 0.5F);
        }
        if ((decoration & TEXT_DECORATION_STRIKE_THROUGH) != 0) {
            float center = metrics.ascent * 0.5F;
            top = Math.min(top, center - decorationThickness * 0.5F);
            bottom = Math.max(bottom, center + decorationThickness * 0.5F);
        }
        return new float[] {
                advance,
                left,
                top,
                right - left,
                bottom - top,
                -metrics.ascent,
                metrics.descent,
                metrics.leading,
                Math.max(0.0F, metrics.descent * 0.5F),
                decorationThickness,
                -metrics.ascent * 0.5F,
                decorationThickness,
        };
    }

    private Object createTextLayout(byte[] utf8, float fontSize, float maxWidth, int familyKind, byte[] familyName,
            int weight, int slant, int alignment, int wrap, int direction, byte[] utf8Locale) {
        prepareTextPaint(fontSize, 0xFF000000, familyKind, familyName, weight, slant, 0, utf8Locale);
        return new HuxerUITextLayout(new String(utf8, StandardCharsets.UTF_8), new TextPaint(textPaint), maxWidth,
                toTextAlignment(alignment), wrap == TEXT_WRAP_WORD, direction);
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
            float fontSize, int familyKind, byte[] familyName, int weight, int slant, int decoration, int alignment,
            int wrap, int direction, byte[] utf8Locale) {
        if (width <= 0.0F || height <= 0.0F) {
            return;
        }
        String text = new String(utf8, StandardCharsets.UTF_8);
        String resolvedFamily = new String(familyName, StandardCharsets.UTF_8);
        String locale = new String(utf8Locale, StandardCharsets.UTF_8);
        prepareTextPaint(fontSize, color, familyKind, resolvedFamily, weight, slant, decoration, locale);
        int saveCount = canvas.save();
        canvas.clipRect(x, y, x + width, y + height);
        boolean wraps = wrap == TEXT_WRAP_WORD;
        float desiredWidth = StaticLayout.getDesiredWidth(text, textPaint);
        int layoutWidth = Math.max(1, (int) Math.ceil(wraps ? width : Math.max(width, desiredWidth)));
        Layout.Alignment layoutAlignment = toTextAlignment(alignment);
        ParagraphKey key = new ParagraphKey(text, fontSize, familyKind, resolvedFamily, weight, slant, decoration,
                alignment, wrap, direction, locale, layoutWidth);
        StaticLayout layout = paragraphCache.get(key);
        if (layout == null) {
            layout = createTextLayout(text, layoutWidth, layoutAlignment, direction, new TextPaint(textPaint));
            paragraphCache.put(key, layout);
        }
        layout.getPaint().setColor(color);
        float horizontalOffset = 0.0F;
        if (!wraps) {
            boolean rightToLeft = isTextRightToLeft(text, direction);
            if (alignment == TEXT_ALIGN_CENTER) {
                horizontalOffset = (width - layoutWidth) * 0.5F;
            } else if ((alignment != TEXT_ALIGN_TRAILING && rightToLeft)
                    || (alignment == TEXT_ALIGN_TRAILING && !rightToLeft)) {
                horizontalOffset = width - layoutWidth;
            }
        }
        float verticalOffset = wraps ? 0.0F : (height - layout.getHeight()) * 0.5F;
        canvas.translate(x + horizontalOffset, y + verticalOffset);
        layout.draw(canvas);
        canvas.restoreToCount(saveCount);
    }

    private void drawTextRuns(Canvas canvas, byte[] textData, int[] textRanges, float[] baselines, int[] colors,
            float[] fontSizes, int[] styles, byte[] metadata, int[] metadataRanges) {
        int count = colors.length;
        if (textRanges.length != count * 2 || baselines.length != count * 2 || fontSizes.length != count
                || styles.length != count * 5 || metadataRanges.length != count * 4) {
            throw new IllegalArgumentException("HuxerUI received malformed text run data");
        }
        for (int index = 0; index < count; ++index) {
            int textIndex = index * 2;
            int styleIndex = index * 5;
            int metadataIndex = index * 4;
            String text =
                    new String(textData, textRanges[textIndex], textRanges[textIndex + 1], StandardCharsets.UTF_8);
            String familyName = new String(
                    metadata, metadataRanges[metadataIndex], metadataRanges[metadataIndex + 1], StandardCharsets.UTF_8);
            String locale = new String(metadata, metadataRanges[metadataIndex + 2], metadataRanges[metadataIndex + 3],
                    StandardCharsets.UTF_8);
            prepareTextPaint(fontSizes[index], colors[index], styles[styleIndex], familyName, styles[styleIndex + 1],
                    styles[styleIndex + 2], styles[styleIndex + 3], locale);
            boolean rightToLeft = isTextRightToLeft(text, styles[styleIndex + 4]);
            canvas.drawTextRun(text, 0, text.length(), 0, text.length(), baselines[textIndex], baselines[textIndex + 1],
                    rightToLeft, textPaint);
        }
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

    private void fillPath(Canvas canvas, float[] elements, int color, int fillRule) {
        Path drawPath = preparePath(elements, fillRule);
        preparePaint(color, Paint.Style.FILL, 0.0F);
        canvas.drawPath(drawPath, paint);
    }

    private void strokePath(
            Canvas canvas, float[] elements, int color, float width, int cap, int join, float miterLimit) {
        if (width <= 0.0F) {
            return;
        }
        Path drawPath = preparePath(elements, 0);
        preparePaint(color, Paint.Style.STROKE, width);
        if (cap == STROKE_CAP_ROUND) {
            paint.setStrokeCap(Paint.Cap.ROUND);
        } else if (cap == STROKE_CAP_SQUARE) {
            paint.setStrokeCap(Paint.Cap.SQUARE);
        }
        if (join == STROKE_JOIN_ROUND) {
            paint.setStrokeJoin(Paint.Join.ROUND);
        } else if (join == STROKE_JOIN_BEVEL) {
            paint.setStrokeJoin(Paint.Join.BEVEL);
        }
        paint.setStrokeMiter(miterLimit);
        canvas.drawPath(drawPath, paint);
    }

    private void drawPathShadow(Canvas canvas, float[] elements, float x, float y, float width, float height, int color,
            float offsetX, float offsetY, float blurRadius, int fillRule) {
        Path drawPath = preparePath(elements, fillRule);
        if (shadowRenderer == null) {
            shadowRenderer = new HuxerUIShadowRenderer(density);
        }
        rect.set(x, y, x + width, y + height);
        shadowRenderer.drawPath(canvas, drawPath, rect, color, offsetX, offsetY, blurRadius);
    }

    private void pushClip(Canvas canvas, float x, float y, float width, float height, float cornerRadius) {
        canvas.save();
        rect.set(x, y, x + width, y + height);
        if (cornerRadius <= 0.0F) {
            canvas.clipRect(rect);
            return;
        }
        float radius = Math.min(cornerRadius, Math.min(width, height) * 0.5F);
        clipPath.reset();
        clipPath.addRoundRect(rect, radius, radius, Path.Direction.CW);
        canvas.clipPath(clipPath);
    }

    private void pushPathClip(Canvas canvas, float[] elements, int fillRule) {
        canvas.save();
        canvas.clipPath(preparePath(elements, fillRule));
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
        paint.setStrokeJoin(Paint.Join.MITER);
        paint.setStrokeMiter(4.0F);
    }

    private Path preparePath(float[] elements, int fillRule) {
        PathKey key = new PathKey(elements, fillRule);
        Path cached = pathCache.get(key);
        if (cached != null) {
            return cached;
        }

        Path result = new Path();
        result.setFillType(fillRule == PATH_FILL_EVEN_ODD ? Path.FillType.EVEN_ODD : Path.FillType.WINDING);
        int index = 0;
        while (index < elements.length) {
            int verb = (int) elements[index++];
            if (verb == PATH_MOVE_TO) {
                result.moveTo(elements[index++], elements[index++]);
            } else if (verb == PATH_LINE_TO) {
                result.lineTo(elements[index++], elements[index++]);
            } else if (verb == PATH_QUADRATIC_TO) {
                result.quadTo(elements[index++], elements[index++], elements[index++], elements[index++]);
            } else if (verb == PATH_CUBIC_TO) {
                result.cubicTo(elements[index++], elements[index++], elements[index++], elements[index++],
                        elements[index++], elements[index++]);
            } else if (verb == PATH_CLOSE) {
                result.close();
            } else {
                throw new IllegalArgumentException("HuxerUI path contains an unsupported verb");
            }
        }
        pathCache.put(key, result);
        return result;
    }

    private static final class PathKey {
        final float[] elements;
        final int fillRule;
        final int hashCode;

        PathKey(float[] elements, int fillRule) {
            this.elements = elements;
            this.fillRule = fillRule;
            hashCode = 31 * Arrays.hashCode(elements) + fillRule;
        }

        @Override
        public boolean equals(Object other) {
            if (this == other) {
                return true;
            }
            if (!(other instanceof PathKey)) {
                return false;
            }
            PathKey key = (PathKey) other;
            return fillRule == key.fillRule && Arrays.equals(elements, key.elements);
        }

        @Override
        public int hashCode() {
            return hashCode;
        }
    }

    private static final class FontKey {
        final int familyKind;
        final String familyName;
        final int weight;
        final int slant;

        FontKey(int familyKind, String familyName, int weight, int slant) {
            this.familyKind = familyKind;
            this.familyName = familyName;
            this.weight = weight;
            this.slant = slant;
        }

        @Override
        public boolean equals(Object other) {
            if (this == other) {
                return true;
            }
            if (!(other instanceof FontKey)) {
                return false;
            }
            FontKey key = (FontKey) other;
            return familyKind == key.familyKind && weight == key.weight && slant == key.slant
                    && Objects.equals(familyName, key.familyName);
        }

        @Override
        public int hashCode() {
            return Objects.hash(familyKind, familyName, weight, slant);
        }
    }

    private void prepareTextPaint(float fontSize, int color, int familyKind, byte[] utf8FamilyName, int weight,
            int slant, int decoration, byte[] utf8Locale) {
        String familyName = utf8FamilyName == null ? "" : new String(utf8FamilyName, StandardCharsets.UTF_8);
        String locale = utf8Locale == null ? "" : new String(utf8Locale, StandardCharsets.UTF_8);
        prepareTextPaint(fontSize, color, familyKind, familyName, weight, slant, decoration, locale);
    }

    private static final class ParagraphKey {
        final String text;
        final float fontSize;
        final int familyKind;
        final String familyName;
        final int weight;
        final int slant;
        final int decoration;
        final int alignment;
        final int wrap;
        final int direction;
        final String locale;
        final int width;

        ParagraphKey(String text, float fontSize, int familyKind, String familyName, int weight, int slant,
                int decoration, int alignment, int wrap, int direction, String locale, int width) {
            this.text = text;
            this.fontSize = fontSize;
            this.familyKind = familyKind;
            this.familyName = familyName;
            this.weight = weight;
            this.slant = slant;
            this.decoration = decoration;
            this.alignment = alignment;
            this.wrap = wrap;
            this.direction = direction;
            this.locale = locale;
            this.width = width;
        }

        @Override
        public boolean equals(Object other) {
            if (this == other) {
                return true;
            }
            if (!(other instanceof ParagraphKey)) {
                return false;
            }
            ParagraphKey key = (ParagraphKey) other;
            return Float.compare(fontSize, key.fontSize) == 0 && familyKind == key.familyKind && weight == key.weight
                    && slant == key.slant && decoration == key.decoration && alignment == key.alignment
                    && wrap == key.wrap && direction == key.direction && width == key.width
                    && Objects.equals(text, key.text) && Objects.equals(familyName, key.familyName)
                    && Objects.equals(locale, key.locale);
        }

        @Override
        public int hashCode() {
            return Objects.hash(text, fontSize, familyKind, familyName, weight, slant, decoration, alignment, wrap,
                    direction, locale, width);
        }
    }

    private void prepareTextPaint(float fontSize, int color, int familyKind, String familyName, int weight, int slant,
            int decoration, String locale) {
        FontKey key = new FontKey(familyKind, familyName, weight, slant);
        Typeface typeface = fontCache.get(key);
        if (typeface == null) {
            Typeface base;
            if (familyKind == FONT_FAMILY_MONOSPACE) {
                base = Typeface.MONOSPACE;
            } else if (familyKind == FONT_FAMILY_NAMED) {
                base = Typeface.create(familyName, Typeface.NORMAL);
            } else {
                base = Typeface.DEFAULT;
            }
            boolean italic = slant != 0;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                typeface = Typeface.create(base, Math.max(1, Math.min(weight, 1000)), italic);
            } else {
                int style = weight >= 600 ? Typeface.BOLD : Typeface.NORMAL;
                if (italic) {
                    style |= Typeface.ITALIC;
                }
                typeface = Typeface.create(base, style);
            }
            fontCache.put(key, typeface);
        }
        textPaint.setTextSize(fontSize);
        textPaint.setColor(color);
        textPaint.setStyle(Paint.Style.FILL);
        textPaint.setTypeface(typeface);
        textPaint.setUnderlineText((decoration & TEXT_DECORATION_UNDERLINE) != 0);
        textPaint.setStrikeThruText((decoration & TEXT_DECORATION_STRIKE_THROUGH) != 0);
        textPaint.setTextAlign(Paint.Align.LEFT);
        if (locale == null || locale.isEmpty()) {
            textPaint.setTextLocale(Locale.getDefault());
        } else {
            textPaint.setTextLocale(Locale.forLanguageTag(locale));
        }
    }

    private Layout.Alignment toTextAlignment(int alignment) {
        if (alignment == TEXT_ALIGN_CENTER) {
            return Layout.Alignment.ALIGN_CENTER;
        }
        if (alignment == TEXT_ALIGN_TRAILING) {
            return Layout.Alignment.ALIGN_OPPOSITE;
        }
        return Layout.Alignment.ALIGN_NORMAL;
    }

    private boolean drawImage(Canvas canvas, long identity, byte[] encoded, float sourceX, float sourceY,
            float sourceWidth, float sourceHeight, float destinationX, float destinationY, float destinationWidth,
            float destinationHeight, float opacity, int sampling) {
        Bitmap bitmap = imageCache.get(identity);
        if (bitmap == null && encoded != null) {
            bitmap = BitmapFactory.decodeByteArray(encoded, 0, encoded.length);
            if (bitmap != null) {
                imageCache.put(identity, bitmap);
            }
        }
        if (bitmap == null) {
            return false;
        }
        drawBitmap(canvas, bitmap, sourceX, sourceY, sourceWidth, sourceHeight, destinationX, destinationY,
                destinationWidth, destinationHeight, opacity, sampling);
        return true;
    }

    private void drawExternalTexture(Canvas canvas, Bitmap bitmap, float sourceX, float sourceY, float sourceWidth,
            float sourceHeight, float destinationX, float destinationY, float destinationWidth,
            float destinationHeight, float opacity, int generation, int sampling) {
        if (bitmap.isRecycled()) {
            throw new IllegalStateException("HuxerUI Android external texture Bitmap was recycled while retained");
        }
        if (bitmap.getGenerationId() != generation) {
            throw new IllegalStateException("HuxerUI Android external texture Bitmap changed after publication");
        }
        drawBitmap(canvas, bitmap, sourceX, sourceY, sourceWidth, sourceHeight, destinationX, destinationY,
                destinationWidth, destinationHeight, opacity, sampling);
    }

    private void drawBitmap(Canvas canvas, Bitmap bitmap, float sourceX, float sourceY, float sourceWidth,
            float sourceHeight, float destinationX, float destinationY, float destinationWidth,
            float destinationHeight, float opacity, int sampling) {
        Rect source = new Rect(Math.round(sourceX), Math.round(sourceY), Math.round(sourceX + sourceWidth),
                Math.round(sourceY + sourceHeight));
        rect.set(destinationX, destinationY, destinationX + destinationWidth, destinationY + destinationHeight);
        paint.reset();
        paint.setAlpha(Math.round(Math.max(0.0F, Math.min(1.0F, opacity)) * 255.0F));
        paint.setFilterBitmap(sampling != 0);
        canvas.drawBitmap(bitmap, source, rect, paint);
    }

    private static boolean isTextRightToLeft(String text, int direction) {
        return direction == TEXT_DIRECTION_RIGHT_TO_LEFT
                || (direction != TEXT_DIRECTION_LEFT_TO_RIGHT
                        && TextDirectionHeuristics.FIRSTSTRONG_LTR.isRtl(text, 0, text.length()));
    }

    private StaticLayout createTextLayout(String text, int width, Layout.Alignment alignment, int direction) {
        return createTextLayout(text, width, alignment, direction, textPaint);
    }

    private StaticLayout createTextLayout(
            String text, int width, Layout.Alignment alignment, int direction, TextPaint layoutPaint) {
        StaticLayout.Builder builder = StaticLayout.Builder.obtain(text, 0, text.length(), layoutPaint, width)
                                               .setAlignment(alignment)
                                               .setIncludePad(true);
        if (direction == TEXT_DIRECTION_LEFT_TO_RIGHT) {
            builder.setTextDirection(TextDirectionHeuristics.LTR);
        } else if (direction == TEXT_DIRECTION_RIGHT_TO_LEFT) {
            builder.setTextDirection(TextDirectionHeuristics.RTL);
        } else {
            builder.setTextDirection(TextDirectionHeuristics.FIRSTSTRONG_LTR);
        }
        return builder.build();
    }

    private long createNativeRuntime(HuxerUIApplicationActivation activation) {
        if (activation == null) {
            return nativeCreate(this, 0, null, null, -1L, null, false);
        }
        return nativeCreate(this, activation.kind, activation.value, activation.name, activation.size,
                activation.contentType, activation.writable);
    }

    private void handleNativeApplicationActivation(HuxerUIApplicationActivation activation) {
        nativeHandleApplicationActivation(nativeHandle, activation.kind, activation.value, activation.name,
                activation.size, activation.contentType, activation.writable);
    }

    private static native long nativeCreate(HuxerUIView view, int activationKind, String activationValue,
            String fileName, long fileSize, String contentType, boolean writable);

    private static native void nativeHandleApplicationActivation(long handle, int activationKind,
            String activationValue, String fileName, long fileSize, String contentType, boolean writable);

    private static native void nativeDestroy(long handle);

    private static native void nativeResize(
            long handle, float width, float height, float safeLeft, float safeTop, float safeRight, float safeBottom);

    private static native void nativeUpdateResourceConfiguration(long handle, byte[] languageTag, float displayScale);

    private static native byte[] nativeCommitFrame(long handle);

    private static native boolean nativePerformSemanticAction(long handle, int nodeId, int actionKind, byte[] text,
            long argument0, long argument1, double number, float x, float y, long customId);

    private static native void nativeBeginDraw(long handle);

    private static native void nativeDrawBase(long handle, Canvas canvas);

    private static native void nativeDrawSlice(long handle, Canvas canvas, long firstCommand, long commandCount);

    private static native void nativeEndDraw(long handle);

    private static native void nativeDrainPlatformTasks(long handle);

    private static native long nativeHitTestPlatformView(long handle, float x, float y);

    private static native void nativeSynchronizePlatformViewFocus(long handle, long identity, boolean focusVisible);

    private static native boolean nativeMoveFocusFromPlatformView(long handle, long identity, boolean reverse);

    private static native void nativePointer(long handle, int type, int deviceKind, long pointerId, float x, float y);

    private static native void nativeScroll(long handle, float x, float y, float deltaX, float deltaY);

    private static native void nativeKey(long handle, boolean down, int keyCode, byte[] text, boolean shift,
            boolean control, boolean alt, boolean meta, boolean repeat);

    private static native boolean nativeHandleBack(long handle, int phase, float progress);
}
