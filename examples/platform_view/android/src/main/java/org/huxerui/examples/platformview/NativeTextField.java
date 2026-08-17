package org.huxerui.examples.platformview;

import android.content.Context;
import android.text.Editable;
import android.text.TextWatcher;
import android.widget.EditText;

public final class NativeTextField extends EditText {
    private long nativeEventSink;
    private boolean applyingControlledText;
    private final TextWatcher textWatcher = new TextWatcher() {
        @Override
        public void beforeTextChanged(CharSequence text, int start, int count, int after) {}

        @Override
        public void onTextChanged(CharSequence text, int start, int before, int count) {}

        @Override
        public void afterTextChanged(Editable text) {
            if (!applyingControlledText && nativeEventSink != 0L) {
                nativeChanged(nativeEventSink, text.toString());
            }
        }
    };

    public NativeTextField(Context context) {
        super(context);
        setSingleLine(true);
        setHint("Edit native text");
        addTextChangedListener(textWatcher);
    }

    public void installNativeBridge(long eventSink) {
        nativeEventSink = eventSink;
    }

    public void applyControlledText(String value) {
        if (value.contentEquals(getText())) {
            return;
        }
        applyingControlledText = true;
        setText(value);
        setSelection(value.length());
        applyingControlledText = false;
    }

    public long disposeNativeBridge() {
        removeTextChangedListener(textWatcher);
        long result = nativeEventSink;
        nativeEventSink = 0L;
        return result;
    }

    private static native void nativeChanged(long eventSink, String value);
}
