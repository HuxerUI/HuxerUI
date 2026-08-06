package org.huxerui;

import android.app.Activity;
import android.os.Build;
import android.os.Bundle;
import android.window.BackEvent;
import android.window.OnBackAnimationCallback;
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
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            backCallback = Api34.registerBackCallback(this);
        } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
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
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                Api34.unregisterBackCallback(this, backCallback);
            } else {
                Api33.unregisterBackCallback(this, backCallback);
            }
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

    private static final class Api34 {
        private Api34() {}

        static Object registerBackCallback(HuxerUIActivity activity) {
            OnBackAnimationCallback callback = new OnBackAnimationCallback() {
                private boolean handled;

                @Override
                public void onBackStarted(BackEvent event) {
                    handled = activity.contentView != null && activity.contentView.beginBack();
                }

                @Override
                public void onBackProgressed(BackEvent event) {
                    if (handled && activity.contentView != null) {
                        activity.contentView.updateBack(event.getProgress());
                    }
                }

                @Override
                public void onBackCancelled() {
                    if (handled && activity.contentView != null) {
                        activity.contentView.cancelBack();
                    }
                    handled = false;
                }

                @Override
                public void onBackInvoked() {
                    if (!handled) {
                        activity.dispatchBack();
                        return;
                    }
                    boolean committed = activity.contentView != null && activity.contentView.commitBack();
                    handled = false;
                    if (!committed) {
                        activity.onUnhandledBack();
                    }
                }
            };
            activity.getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    OnBackInvokedDispatcher.PRIORITY_DEFAULT, callback);
            return callback;
        }

        static void unregisterBackCallback(HuxerUIActivity activity, Object callback) {
            activity.getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback((OnBackAnimationCallback) callback);
        }
    }
}
