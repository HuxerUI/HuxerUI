package org.huxerui.examples.platformview;

import android.content.Context;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.View;
import android.widget.EditText;

import org.huxerui.HuxerUIPlatformChannel;
import org.huxerui.HuxerUIPlatformView;
import org.huxerui.PlatformPayload;

public final class PlatformTextField implements HuxerUIPlatformView.Factory {
    public PlatformTextField() {}

    @Override
    public HuxerUIPlatformView create(
            Context context, PlatformPayload properties, HuxerUIPlatformChannel.Events events) {
        return new Instance(context, properties.requireString(), events);
    }

    private static final class Instance implements HuxerUIPlatformView {
        private final EditText view;
        private final HuxerUIPlatformChannel.Events events;
        private final TextWatcher textWatcher = new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence text, int start, int count, int after) {}

            @Override
            public void onTextChanged(CharSequence text, int start, int before, int count) {}

            @Override
            public void afterTextChanged(Editable text) {
                if (!applyingControlledText) {
                    events.emit("changed", PlatformPayload.string(text.toString()));
                }
            }
        };
        private boolean applyingControlledText;

        Instance(Context context, String text, HuxerUIPlatformChannel.Events events) {
            view = new EditText(context);
            view.setSingleLine(true);
            view.setHint("Edit PlatformView text");
            this.events = events;
            applyControlledText(text);
            view.addTextChangedListener(textWatcher);
        }

        @Override
        public View getView() {
            return view;
        }

        @Override
        public void update(PlatformPayload properties) {
            applyControlledText(properties.requireString());
        }

        @Override
        public void dispose() {
            view.removeTextChangedListener(textWatcher);
        }

        private void applyControlledText(String value) {
            if (value.contentEquals(view.getText())) {
                return;
            }
            applyingControlledText = true;
            view.setText(value);
            view.setSelection(value.length());
            applyingControlledText = false;
        }
    }
}
