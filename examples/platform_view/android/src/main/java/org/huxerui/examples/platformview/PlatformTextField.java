package org.huxerui.examples.platformview;

import android.content.Context;
import android.text.Editable;
import android.text.TextWatcher;
import android.widget.EditText;

public final class PlatformTextField extends EditText {
    private long platformEventSink;
    private boolean applyingControlledText;
    private final TextWatcher textWatcher = new TextWatcher() {
        @Override
        public void beforeTextChanged(CharSequence text, int start, int count, int after) {}

        @Override
        public void onTextChanged(CharSequence text, int start, int before, int count) {}

        @Override
        public void afterTextChanged(Editable text) {
            if (!applyingControlledText && platformEventSink != 0L) {
                nativeChanged(platformEventSink, text.toString());
            }
        }
    };

    public PlatformTextField(Context context) {
        super(context);
        setSingleLine(true);
        setHint("Edit PlatformView text");
        addTextChangedListener(textWatcher);
    }

    public void installPlatformBridge(long eventSink) {
        platformEventSink = eventSink;
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

    public long disposePlatformBridge() {
        removeTextChangedListener(textWatcher);
        long result = platformEventSink;
        platformEventSink = 0L;
        return result;
    }

    private static native void nativeChanged(long eventSink, String value);
}
