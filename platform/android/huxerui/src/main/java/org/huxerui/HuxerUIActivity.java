package org.huxerui;

import android.app.Activity;
import android.os.Build;
import android.os.Bundle;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

public class HuxerUIActivity extends Activity {
    static {
        System.loadLibrary("huxerui_app");
    }

    private HuxerUIView contentView;
    private Object backCallback;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        contentView = new HuxerUIView(this);
        setContentView(contentView);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            backCallback = Api33.registerBackCallback(this);
        }
    }

    @SuppressWarnings("deprecation")
    @Override
    public void onBackPressed() {
        if (contentView == null || !contentView.handleBack()) {
            super.onBackPressed();
        }
    }

    protected void onUnhandledBack() {
        finishAfterTransition();
    }

    @Override
    protected void onDestroy() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU && backCallback != null) {
            Api33.unregisterBackCallback(this, backCallback);
            backCallback = null;
        }
        contentView = null;
        super.onDestroy();
    }

    private void dispatchBack() {
        if (contentView == null || !contentView.handleBack()) {
            onUnhandledBack();
        }
    }

    private static final class Api33 {
        private Api33() {}

        static Object registerBackCallback(HuxerUIActivity activity) {
            OnBackInvokedCallback callback = activity::dispatchBack;
            activity.getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    OnBackInvokedDispatcher.PRIORITY_DEFAULT, callback);
            return callback;
        }

        static void unregisterBackCallback(HuxerUIActivity activity, Object callback) {
            activity.getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback((OnBackInvokedCallback) callback);
        }
    }
}
