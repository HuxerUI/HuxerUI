package org.huxerui;

import android.content.Context;
import android.graphics.Matrix;
import android.text.InputType;
import android.text.TextUtils;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.CursorAnchorInfo;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.ExtractedText;
import android.view.inputmethod.ExtractedTextRequest;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

import java.nio.charset.StandardCharsets;

final class HuxerUIInputConnection extends BaseInputConnection {
    private static final int TEXT_INPUT_COMMIT_TEXT = 0;
    private static final int TEXT_INPUT_SET_COMPOSING_TEXT = 1;
    private static final int TEXT_INPUT_FINISH_COMPOSING = 2;
    private static final int TEXT_INPUT_SET_SELECTION = 3;
    private static final int TEXT_INPUT_DELETE_SURROUNDING = 4;
    private static final int TEXT_INPUT_DELETE_SURROUNDING_CODE_POINTS = 5;
    private static final int TEXT_INPUT_SET_COMPOSING_REGION = 6;

    private static final int TEXT_INPUT_RESULT_OK = 0;
    private static final int TEXT_INPUT_CONTEXT_METADATA_SIZE = 8;
    private static final int TEXT_EDITING_ACTION_CUT = 0;
    private static final int TEXT_EDITING_ACTION_COPY = 1;
    private static final int TEXT_EDITING_ACTION_PASTE = 2;
    private static final int TEXT_EDITING_ACTION_SELECT_ALL = 3;

    private final HuxerUIView view;
    private final long nativeHandle;
    private final long sessionId;
    private final float density;
    private final int textInputType;
    private final int textCapitalization;
    private final int textInputAction;
    private final boolean textInputMultiline;
    private final boolean textInputSecure;
    private final boolean textInputAutocorrect;
    private final Matrix cursorAnchorMatrix = new Matrix();
    private final Matrix candidateCursorAnchorMatrix = new Matrix();

    private boolean active = true;
    private long selectionAnchor;
    private long selectionActive;
    private long compositionStart;
    private long compositionEnd;
    private int batchDepth;
    private boolean stateNotificationPending;
    private int cursorUpdateMode;
    private boolean hasCaretGeometry;
    private float caretX;
    private float caretY;
    private float caretWidth;
    private float caretHeight;
    private boolean hasCursorAnchorMatrix;

    HuxerUIInputConnection(HuxerUIView view, long nativeHandle, long sessionId, int type, int capitalization,
            int action, boolean multiline, boolean secure, boolean autocorrect, long anchor, long active,
            long composingStart, long composingEnd) {
        super(view, false);
        this.view = view;
        this.nativeHandle = nativeHandle;
        this.sessionId = sessionId;
        density = view.getResources().getDisplayMetrics().density;
        textInputType = type;
        textCapitalization = capitalization;
        textInputAction = action;
        textInputMultiline = multiline;
        textInputSecure = secure;
        textInputAutocorrect = autocorrect;
        updateState(anchor, active, composingStart, composingEnd);
    }

    long sessionId() {
        return sessionId;
    }

    boolean isActive() {
        return active && sessionId != 0L && nativeHandle != 0L;
    }

    void deactivate() {
        active = false;
        batchDepth = 0;
        stateNotificationPending = false;
        cursorUpdateMode = 0;
        hasCursorAnchorMatrix = false;
    }

    void configureEditorInfo(EditorInfo outAttrs) {
        outAttrs.inputType = androidInputType();
        outAttrs.imeOptions = androidImeOptions();
        outAttrs.initialSelStart = androidOffset(selectionAnchor);
        outAttrs.initialSelEnd = androidOffset(selectionActive);
        outAttrs.initialCapsMode = getCursorCapsMode(outAttrs.inputType);
    }

    void updateState(long anchor, long active, long composingStart, long composingEnd) {
        selectionAnchor = anchor;
        selectionActive = active;
        compositionStart = composingStart;
        compositionEnd = composingEnd;
    }

    void updateCaretGeometry(int result, float x, float y, float width, float height) {
        setCaretGeometry(result == TEXT_INPUT_RESULT_OK, x, y, width, height);
    }

    private void setCaretGeometry(boolean valid, float x, float y, float width, float height) {
        hasCaretGeometry = valid;
        caretX = x;
        caretY = y;
        caretWidth = width;
        caretHeight = height;
    }

    void notifyStateChanged() {
        if (!isActive()) {
            return;
        }
        if (batchDepth > 0) {
            stateNotificationPending = true;
        } else {
            publishState();
        }
    }

    @Override
    public boolean beginBatchEdit() {
        if (!isActive()) {
            return false;
        }
        ++batchDepth;
        return true;
    }

    @Override
    public boolean endBatchEdit() {
        if (batchDepth <= 0) {
            return false;
        }
        --batchDepth;
        if (batchDepth == 0 && stateNotificationPending) {
            stateNotificationPending = false;
            publishState();
        }
        return true;
    }

    @Override
    public boolean commitText(CharSequence text, int newCursorPosition) {
        return applyTextCommand(TEXT_INPUT_COMMIT_TEXT, text, newCursorPosition, 0L, 0L);
    }

    @Override
    public boolean setComposingText(CharSequence text, int newCursorPosition) {
        return applyTextCommand(TEXT_INPUT_SET_COMPOSING_TEXT, text, newCursorPosition, 0L, 0L);
    }

    @Override
    public boolean setComposingRegion(int start, int end) {
        return applyTextCommand(TEXT_INPUT_SET_COMPOSING_REGION, null, start, end, 0L);
    }

    @Override
    public boolean finishComposingText() {
        return applyTextCommand(TEXT_INPUT_FINISH_COMPOSING, null, 0L, 0L, 0L);
    }

    @Override
    public boolean setSelection(int start, int end) {
        return applyTextCommand(TEXT_INPUT_SET_SELECTION, null, start, end, 0L);
    }

    @Override
    public boolean deleteSurroundingText(int beforeLength, int afterLength) {
        return applyTextCommand(TEXT_INPUT_DELETE_SURROUNDING, null, beforeLength, afterLength, 0L);
    }

    @Override
    public boolean deleteSurroundingTextInCodePoints(int beforeLength, int afterLength) {
        return applyTextCommand(TEXT_INPUT_DELETE_SURROUNDING_CODE_POINTS, null, beforeLength, afterLength, 0L);
    }

    @Override
    public CharSequence getTextBeforeCursor(int length, int flags) {
        if (!isActive() || textInputSecure || length < 0) {
            return null;
        }
        long end = Math.min(selectionAnchor, selectionActive);
        long start = Math.max(0L, end - length);
        return queryText(start, end);
    }

    @Override
    public CharSequence getTextAfterCursor(int length, int flags) {
        if (!isActive() || textInputSecure || length < 0) {
            return null;
        }
        long start = Math.max(selectionAnchor, selectionActive);
        long end = start > Integer.MAX_VALUE - length ? Integer.MAX_VALUE : start + length;
        return queryText(start, end);
    }

    @Override
    public CharSequence getSelectedText(int flags) {
        if (!isActive() || textInputSecure) {
            return null;
        }
        long start = Math.min(selectionAnchor, selectionActive);
        long end = Math.max(selectionAnchor, selectionActive);
        return start == end ? null : queryText(start, end);
    }

    @Override
    public int getCursorCapsMode(int reqModes) {
        if (!isActive() || textInputSecure) {
            return 0;
        }
        long cursor = Math.min(selectionAnchor, selectionActive);
        CharSequence before = queryText(0L, cursor);
        return before == null ? 0 : TextUtils.getCapsMode(before, before.length(), reqModes);
    }

    @Override
    public ExtractedText getExtractedText(ExtractedTextRequest request, int flags) {
        if (!isActive() || textInputSecure) {
            return null;
        }
        TextInputContextData context = queryContext(0L, Integer.MAX_VALUE);
        if (context == null) {
            return null;
        }
        ExtractedText result = new ExtractedText();
        result.text = context.text;
        result.startOffset = androidOffset(context.sliceStart);
        result.partialStartOffset = -1;
        result.partialEndOffset = -1;
        result.selectionStart = androidOffset(context.selectionAnchor - context.sliceStart);
        result.selectionEnd = androidOffset(context.selectionActive - context.sliceStart);
        result.flags = textInputMultiline ? 0 : ExtractedText.FLAG_SINGLE_LINE;
        return result;
    }

    @Override
    public boolean performEditorAction(int editorAction) {
        if (!isActive() || !nativePerformTextInputAction(nativeHandle, sessionId, editorAction)) {
            return false;
        }
        if (editorAction != EditorInfo.IME_ACTION_NEXT && editorAction != EditorInfo.IME_ACTION_NONE) {
            inputMethodManager().hideSoftInputFromWindow(view.getWindowToken(), 0);
        }
        return true;
    }

    @Override
    public boolean performContextMenuAction(int id) {
        int action = editingActionForMenuItem(id);
        if (!isActive() || action < 0 || !nativePerformTextEditingAction(nativeHandle, sessionId, action)) {
            return false;
        }
        notifyStateChanged();
        view.invalidate();
        return true;
    }

    @Override
    public boolean requestCursorUpdates(int cursorUpdateMode) {
        if (!isActive()) {
            return false;
        }
        this.cursorUpdateMode = cursorUpdateMode;
        if ((cursorUpdateMode & InputConnection.CURSOR_UPDATE_IMMEDIATE) != 0) {
            publishCursorAnchorInfo();
            this.cursorUpdateMode &= ~InputConnection.CURSOR_UPDATE_IMMEDIATE;
        }
        return true;
    }

    private boolean applyTextCommand(
            int operation, CharSequence text, long argument0, long argument1, long argument2) {
        if (!isActive()) {
            return false;
        }
        byte[] utf8 = text == null ? new byte[0] : text.toString().getBytes(StandardCharsets.UTF_8);
        return nativeApplyTextInputCommand(
                nativeHandle, sessionId, operation, utf8, argument0, argument1, argument2);
    }

    private CharSequence queryText(long start, long end) {
        TextInputContextData context = queryContext(start, Math.max(0L, end - start));
        if (context == null) {
            return null;
        }
        long relativeStart = start - context.sliceStart;
        long relativeEnd = end - context.sliceStart;
        if (relativeStart < 0L || relativeEnd < relativeStart || relativeEnd > context.text.length()) {
            return null;
        }
        return context.text.substring((int) relativeStart, (int) relativeEnd);
    }

    private TextInputContextData queryContext(long start, long length) {
        if (!isActive()) {
            return null;
        }
        long[] metadata = new long[TEXT_INPUT_CONTEXT_METADATA_SIZE];
        byte[] utf8 = nativeQueryTextInputContext(nativeHandle, sessionId, start, length, metadata);
        if (utf8 == null || metadata[0] != TEXT_INPUT_RESULT_OK) {
            return null;
        }
        return new TextInputContextData(new String(utf8, StandardCharsets.UTF_8), metadata);
    }

    private void publishState() {
        inputMethodManager().updateSelection(view, androidOffset(selectionAnchor), androidOffset(selectionActive),
                compositionStart < 0L ? -1 : androidOffset(compositionStart),
                compositionEnd < 0L ? -1 : androidOffset(compositionEnd));
        if ((cursorUpdateMode & InputConnection.CURSOR_UPDATE_MONITOR) != 0) {
            publishCursorAnchorInfo();
        }
    }

    void updateCursorAnchorPosition() {
        if (isActive() && (cursorUpdateMode & InputConnection.CURSOR_UPDATE_MONITOR) != 0
                && updateCursorAnchorMatrix()) {
            publishCursorAnchorInfo();
        }
    }

    private void publishCursorAnchorInfo() {
        if (!isActive()) {
            return;
        }
        if (!hasCaretGeometry) {
            float[] geometry = new float[4];
            if (nativeQueryTextInputGeometry(nativeHandle, sessionId, selectionActive, selectionActive, geometry)) {
                setCaretGeometry(true, geometry[0], geometry[1], geometry[2], geometry[3]);
            }
        }
        if (!hasCaretGeometry) {
            return;
        }
        updateCursorAnchorMatrix();
        float left = caretX * density;
        float top = caretY * density;
        float bottom = top + Math.max(1.0F, caretHeight * density);
        CursorAnchorInfo info =
                new CursorAnchorInfo.Builder()
                        .setMatrix(cursorAnchorMatrix)
                        .setSelectionRange(androidOffset(selectionAnchor), androidOffset(selectionActive))
                        .setInsertionMarkerLocation(
                                left, top, bottom, bottom, CursorAnchorInfo.FLAG_HAS_VISIBLE_REGION)
                        .build();
        inputMethodManager().updateCursorAnchorInfo(view, info);
    }

    private boolean updateCursorAnchorMatrix() {
        view.transformToScreen(candidateCursorAnchorMatrix);
        if (hasCursorAnchorMatrix && candidateCursorAnchorMatrix.equals(cursorAnchorMatrix)) {
            return false;
        }
        cursorAnchorMatrix.set(candidateCursorAnchorMatrix);
        hasCursorAnchorMatrix = true;
        return true;
    }

    private InputMethodManager inputMethodManager() {
        return (InputMethodManager) view.getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
    }

    private int androidInputType() {
        int result;
        switch (textInputType) {
            case 1:
                result = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS;
                break;
            case 2:
                result = InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_SIGNED;
                break;
            case 3:
                result = InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_SIGNED
                        | InputType.TYPE_NUMBER_FLAG_DECIMAL;
                break;
            case 4:
                result = InputType.TYPE_CLASS_PHONE;
                break;
            case 5:
                result = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI;
                break;
            default:
                result = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_NORMAL;
                break;
        }
        if ((result & InputType.TYPE_MASK_CLASS) == InputType.TYPE_CLASS_TEXT) {
            if (textInputSecure) {
                result = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD;
                result |= InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
            } else {
                if (textInputAutocorrect) {
                    result |= InputType.TYPE_TEXT_FLAG_AUTO_CORRECT;
                } else {
                    result |= InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
                }
                if (textInputMultiline) {
                    result |= InputType.TYPE_TEXT_FLAG_MULTI_LINE;
                }
                if (textCapitalization == 1) {
                    result |= InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS;
                } else if (textCapitalization == 2) {
                    result |= InputType.TYPE_TEXT_FLAG_CAP_WORDS;
                } else if (textCapitalization == 3) {
                    result |= InputType.TYPE_TEXT_FLAG_CAP_SENTENCES;
                }
            }
        }
        return result;
    }

    private int androidImeOptions() {
        int result;
        switch (textInputAction) {
            case 1:
                result = EditorInfo.IME_ACTION_DONE;
                break;
            case 2:
                result = EditorInfo.IME_ACTION_GO;
                break;
            case 3:
                result = EditorInfo.IME_ACTION_NEXT;
                break;
            case 4:
                result = EditorInfo.IME_ACTION_SEARCH;
                break;
            case 5:
                result = EditorInfo.IME_ACTION_SEND;
                break;
            case 6:
                result = EditorInfo.IME_ACTION_NONE;
                break;
            default:
                result = textInputMultiline ? EditorInfo.IME_ACTION_NONE : EditorInfo.IME_ACTION_DONE;
                break;
        }
        return result | EditorInfo.IME_FLAG_NO_EXTRACT_UI;
    }

    private static int editingActionForMenuItem(int itemId) {
        if (itemId == android.R.id.cut) {
            return TEXT_EDITING_ACTION_CUT;
        }
        if (itemId == android.R.id.copy) {
            return TEXT_EDITING_ACTION_COPY;
        }
        if (itemId == android.R.id.paste) {
            return TEXT_EDITING_ACTION_PASTE;
        }
        if (itemId == android.R.id.selectAll) {
            return TEXT_EDITING_ACTION_SELECT_ALL;
        }
        return -1;
    }

    private static int androidOffset(long value) {
        return (int) Math.max(0L, Math.min(value, Integer.MAX_VALUE));
    }

    private static final class TextInputContextData {
        private final String text;
        private final long sliceStart;
        private final long selectionAnchor;
        private final long selectionActive;

        TextInputContextData(String text, long[] metadata) {
            this.text = text;
            sliceStart = metadata[1];
            selectionAnchor = metadata[3];
            selectionActive = metadata[4];
        }
    }

    private static native boolean nativeApplyTextInputCommand(
            long handle, long sessionId, int operation, byte[] text, long argument0, long argument1, long argument2);

    private static native byte[] nativeQueryTextInputContext(
            long handle, long sessionId, long start, long length, long[] metadata);

    private static native boolean nativeQueryTextInputGeometry(
            long handle, long sessionId, long start, long end, float[] geometry);

    private static native boolean nativePerformTextInputAction(long handle, long sessionId, int editorAction);

    private static native boolean nativePerformTextEditingAction(long handle, long sessionId, int action);
}
