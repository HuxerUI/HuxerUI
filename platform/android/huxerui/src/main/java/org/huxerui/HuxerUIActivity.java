package org.huxerui;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.Window;
import android.view.WindowInsetsController;
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
        configureEdgeToEdgeWindow();
        contentView = new HuxerUIView(this);
        contentView.setStartupApplicationIntent(getIntent());
        contentView.setSystemBarsController(this::setSystemBarsContentBrightness);
        contentView.setFilePickerLauncher(new HuxerUIView.FilePickerLauncher() {
            @SuppressWarnings("deprecation")
            @Override
            public void launch(Intent intent, int requestCode) {
                startActivityForResult(intent, requestCode);
            }

            @SuppressWarnings("deprecation")
            @Override
            public void cancel(int requestCode) {
                finishActivity(requestCode);
            }
        });
        setContentView(contentView);
        setSystemBarsContentBrightness(HuxerUIView.SYSTEM_BAR_CONTENT_DARK, HuxerUIView.SYSTEM_BAR_CONTENT_DARK);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            backCallback = Api34.registerBackCallback(this);
        } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            backCallback = Api33.registerBackCallback(this);
        }
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        if (contentView != null) {
            contentView.dispatchApplicationIntent(intent);
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

    @SuppressWarnings("deprecation")
    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (contentView != null && contentView.dispatchFilePickerResult(requestCode, resultCode, data)) {
            return;
        }
        super.onActivityResult(requestCode, resultCode, data);
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
        if (contentView != null) {
            contentView.setFilePickerLauncher(null);
            contentView.setSystemBarsController(null);
        }
        contentView = null;
        super.onDestroy();
    }

    private void dispatchBack() {
        if (contentView == null || !contentView.handleBack()) {
            onUnhandledBack();
        }
    }

    @SuppressWarnings("deprecation")
    private void configureEdgeToEdgeWindow() {
        Window window = getWindow();
        window.setStatusBarColor(Color.TRANSPARENT);
        window.setNavigationBarColor(Color.TRANSPARENT);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Api30.configureEdgeToEdge(window);
        } else {
            window.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            Api29.disableContrastEnforcement(window);
        }
    }

    @SuppressWarnings("deprecation")
    private void setSystemBarsContentBrightness(int statusBar, int navigationBar) {
        Window window = getWindow();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Api30.setContentBrightness(window, statusBar, navigationBar);
            return;
        }
        View decorView = window.getDecorView();
        int visibility = decorView.getSystemUiVisibility();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            if (statusBar == HuxerUIView.SYSTEM_BAR_CONTENT_DARK) {
                visibility |= View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR;
            } else {
                visibility &= ~View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR;
            }
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            if (navigationBar == HuxerUIView.SYSTEM_BAR_CONTENT_DARK) {
                visibility |= View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR;
            } else {
                visibility &= ~View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR;
            }
        }
        decorView.setSystemUiVisibility(visibility);
    }

    private static final class Api29 {
        private Api29() {}

        static void disableContrastEnforcement(Window window) {
            window.setStatusBarContrastEnforced(false);
            window.setNavigationBarContrastEnforced(false);
        }
    }

    private static final class Api30 {
        private Api30() {}

        static void configureEdgeToEdge(Window window) {
            window.setDecorFitsSystemWindows(false);
        }

        static void setContentBrightness(Window window, int statusBar, int navigationBar) {
            View decorView = window.peekDecorView();
            if (decorView == null) {
                return;
            }
            WindowInsetsController controller = decorView.getWindowInsetsController();
            if (controller == null) {
                return;
            }
            int appearance = 0;
            if (statusBar == HuxerUIView.SYSTEM_BAR_CONTENT_DARK) {
                appearance |= WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS;
            }
            if (navigationBar == HuxerUIView.SYSTEM_BAR_CONTENT_DARK) {
                appearance |= WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS;
            }
            controller.setSystemBarsAppearance(appearance,
                    WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS
                            | WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS);
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
