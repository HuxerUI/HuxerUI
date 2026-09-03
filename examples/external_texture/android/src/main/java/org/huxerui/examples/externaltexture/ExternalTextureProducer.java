package org.huxerui.examples.externaltexture;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.util.Log;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.Objects;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

final class ExternalTextureProducer {
    private static final int WIDTH = 320;
    private static final int HEIGHT = 180;

    private final ScheduledExecutorService executor;
    private final Surface producerSurface;
    private final int[] bitmapPixels = new int[WIDTH * HEIGHT];
    private final byte[] glPixels = new byte[WIDTH * HEIGHT * 4];
    private final ByteBuffer glBuffer = ByteBuffer.allocateDirect(WIDTH * HEIGHT * 4);
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private long jniBridge;
    private ScheduledFuture<?> task;
    private EGLDisplay eglDisplay = EGL14.EGL_NO_DISPLAY;
    private EGLContext eglContext = EGL14.EGL_NO_CONTEXT;
    private EGLSurface eglSurface = EGL14.EGL_NO_SURFACE;
    private int textureName;
    private int phase;
    private boolean glPublishingEnabled = true;

    ExternalTextureProducer(Context context, long jniBridge, Surface producerSurface) {
        String threadName = "HuxerUI-" + Objects.requireNonNull(context).getPackageName() + "-ExternalTexture";
        executor = Executors.newSingleThreadScheduledExecutor(runnable -> {
            Thread thread = new Thread(runnable, threadName);
            thread.setDaemon(true);
            return thread;
        });
        this.jniBridge = jniBridge;
        this.producerSurface = Objects.requireNonNull(producerSurface);
    }

    synchronized void setRunning(boolean running) {
        if (jniBridge == 0L) {
            return;
        }
        if (!running) {
            if (task != null) {
                task.cancel(false);
                task = null;
            }
            return;
        }
        if (task == null) {
            task = executor.scheduleAtFixedRate(this::publish, 0L, 50L, TimeUnit.MILLISECONDS);
        }
    }

    synchronized void disposePlatformBridge() {
        jniBridge = 0L;
        if (task != null) {
            task.cancel(false);
            task = null;
        }
        try {
            executor.submit(this::releaseGl).get();
        } catch (ExecutionException ignored) {
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }
        executor.shutdownNow();
    }

    private synchronized void publish() {
        if (jniBridge == 0L) {
            return;
        }
        try {
            publishBitmap();
            publishGl();
            publishSurface();
            ++phase;
        } catch (RuntimeException error) {
            Log.e("HuxerUIExample", "External texture producer frame failed", error);
        }
    }

    private void publishBitmap() {
        for (int y = 0; y < HEIGHT; ++y) {
            for (int x = 0; x < WIDTH; ++x) {
                bitmapPixels[y * WIDTH + x] = frameColor(x, y);
            }
        }
        Bitmap bitmap = Bitmap.createBitmap(bitmapPixels, WIDTH, HEIGHT, Bitmap.Config.ARGB_8888);
        nativePublishBitmap(jniBridge, bitmap);
    }

    private void publishGl() {
        if (!glPublishingEnabled || !ensureGl()) {
            return;
        }
        int offset = 0;
        for (int y = 0; y < HEIGHT; ++y) {
            for (int x = 0; x < WIDTH; ++x) {
                int color = frameColor(x, y);
                glPixels[offset++] = (byte) Color.red(color);
                glPixels[offset++] = (byte) Color.green(color);
                glPixels[offset++] = (byte) Color.blue(color);
                glPixels[offset++] = (byte) 0xFF;
            }
        }
        glBuffer.clear();
        glBuffer.put(glPixels);
        glBuffer.flip();
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, textureName);
        GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA, WIDTH, HEIGHT, 0, GLES20.GL_RGBA,
                GLES20.GL_UNSIGNED_BYTE, glBuffer);
        glPublishingEnabled = nativePublishGl(jniBridge, textureName, WIDTH, HEIGHT);
    }

    private void publishSurface() {
        Canvas canvas = null;
        try {
            canvas = producerSurface.lockCanvas(null);
            canvas.drawColor(frameColor(0, 0));
            paint.setColor(Color.argb(110, 255, 255, 255));
            float left = phase % (WIDTH + 48) - 48;
            canvas.drawRect(left, 0.0F, left + 48.0F, HEIGHT, paint);
            paint.setColor(Color.WHITE);
            paint.setTextSize(24.0F);
            canvas.drawText("Surface", 16.0F, 36.0F, paint);
        } catch (RuntimeException ignored) {
        } finally {
            if (canvas != null) {
                producerSurface.unlockCanvasAndPost(canvas);
            }
        }
    }

    private boolean ensureGl() {
        if (textureName != 0) {
            return EGL14.eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext);
        }
        eglDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        int[] version = new int[2];
        if (eglDisplay == EGL14.EGL_NO_DISPLAY || !EGL14.eglInitialize(eglDisplay, version, 0, version, 1)) {
            return false;
        }
        int[] configAttributes = {
                EGL14.EGL_RENDERABLE_TYPE,
                EGL14.EGL_OPENGL_ES2_BIT,
                EGL14.EGL_SURFACE_TYPE,
                EGL14.EGL_PBUFFER_BIT,
                EGL14.EGL_RED_SIZE,
                8,
                EGL14.EGL_GREEN_SIZE,
                8,
                EGL14.EGL_BLUE_SIZE,
                8,
                EGL14.EGL_ALPHA_SIZE,
                8,
                EGL14.EGL_NONE,
        };
        EGLConfig[] configs = new EGLConfig[1];
        int[] configCount = new int[1];
        if (!EGL14.eglChooseConfig(eglDisplay, configAttributes, 0, configs, 0, configs.length, configCount, 0)
                || configCount[0] == 0) {
            return false;
        }
        int[] contextAttributes = {
                EGL14.EGL_CONTEXT_CLIENT_VERSION,
                2,
                EGL14.EGL_NONE,
        };
        eglContext = EGL14.eglCreateContext(eglDisplay, configs[0], EGL14.EGL_NO_CONTEXT, contextAttributes, 0);
        int[] surfaceAttributes = {
                EGL14.EGL_WIDTH,
                1,
                EGL14.EGL_HEIGHT,
                1,
                EGL14.EGL_NONE,
        };
        eglSurface = EGL14.eglCreatePbufferSurface(eglDisplay, configs[0], surfaceAttributes, 0);
        if (eglContext == EGL14.EGL_NO_CONTEXT || eglSurface == EGL14.EGL_NO_SURFACE
                || !EGL14.eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) {
            releaseGl();
            return false;
        }
        int[] textures = new int[1];
        GLES20.glGenTextures(1, textures, 0);
        textureName = textures[0];
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, textureName);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);
        return textureName != 0;
    }

    private void releaseGl() {
        if (eglDisplay == EGL14.EGL_NO_DISPLAY) {
            return;
        }
        if (eglContext != EGL14.EGL_NO_CONTEXT && eglSurface != EGL14.EGL_NO_SURFACE) {
            EGL14.eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext);
        }
        if (textureName != 0) {
            int[] textures = {textureName};
            GLES20.glDeleteTextures(1, textures, 0);
            textureName = 0;
        }
        EGL14.eglMakeCurrent(eglDisplay, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
        if (eglSurface != EGL14.EGL_NO_SURFACE) {
            EGL14.eglDestroySurface(eglDisplay, eglSurface);
            eglSurface = EGL14.EGL_NO_SURFACE;
        }
        if (eglContext != EGL14.EGL_NO_CONTEXT) {
            EGL14.eglDestroyContext(eglDisplay, eglContext);
            eglContext = EGL14.EGL_NO_CONTEXT;
        }
        EGL14.eglTerminate(eglDisplay);
        eglDisplay = EGL14.EGL_NO_DISPLAY;
    }

    private int frameColor(int x, int y) {
        int red = (255 + phase * 2 - x / 2) & 0xFF;
        int green = y * 255 / (HEIGHT - 1);
        int blue = (x + phase * 3) & 0xFF;
        return Color.rgb(red, green, blue);
    }

    private static native void nativePublishBitmap(long bridge, Bitmap bitmap);

    private static native boolean nativePublishGl(long bridge, int textureName, int pixelWidth, int pixelHeight);
}
