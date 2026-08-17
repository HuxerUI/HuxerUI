package org.huxerui.examples.platformmodule;

import android.content.Context;
import android.graphics.Bitmap;

import java.util.Objects;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

final class NativeColorStream {
    private static final int WIDTH = 320;
    private static final int HEIGHT = 180;

    private final ScheduledExecutorService executor;
    private final int[] pixels = new int[WIDTH * HEIGHT];
    private long nativeBridge;
    private ScheduledFuture<?> task;
    private int phase;

    NativeColorStream(Context context, long nativeBridge) {
        String threadName = "HuxerUI-" + Objects.requireNonNull(context).getPackageName() + "-ColorStream";
        executor = Executors.newSingleThreadScheduledExecutor(runnable -> {
            Thread thread = new Thread(runnable, threadName);
            thread.setDaemon(true);
            return thread;
        });
        this.nativeBridge = nativeBridge;
    }

    synchronized void start() {
        if (task != null) {
            return;
        }
        task = executor.scheduleAtFixedRate(this::publish, 0L, 33L, TimeUnit.MILLISECONDS);
    }

    synchronized void disposeNativeBridge() {
        nativeBridge = 0L;
        if (task != null) {
            task.cancel(false);
            task = null;
        }
        executor.shutdownNow();
    }

    private synchronized void publish() {
        if (nativeBridge == 0L) {
            return;
        }
        for (int y = 0; y < HEIGHT; ++y) {
            for (int x = 0; x < WIDTH; ++x) {
                int red = (255 + phase * 2 - x / 2) & 0xFF;
                int green = y * 255 / (HEIGHT - 1);
                int blue = (x + phase * 3) & 0xFF;
                pixels[y * WIDTH + x] = 0xFF000000 | red << 16 | green << 8 | blue;
            }
        }
        Bitmap bitmap = Bitmap.createBitmap(pixels, WIDTH, HEIGHT, Bitmap.Config.ARGB_8888);
        nativePublish(nativeBridge, bitmap);
        ++phase;
    }

    private static native void nativePublish(long bridge, Bitmap bitmap);
}
