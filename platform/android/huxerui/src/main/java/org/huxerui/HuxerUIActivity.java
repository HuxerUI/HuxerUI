package org.huxerui;

import android.app.Activity;
import android.os.Bundle;

public class HuxerUIActivity extends Activity {
    static {
        System.loadLibrary("huxerui_app");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(new HuxerUIView(this));
    }
}
