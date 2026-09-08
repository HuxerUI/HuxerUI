package @PROJECT_ID@;

import org.huxerui.HuxerUIActivity;

public final class MainActivity extends HuxerUIActivity {
    static {
        System.loadLibrary(BuildConfig.HUXERUI_APP_LIBRARY);
    }
}
