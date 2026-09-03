package org.huxerui;

import android.graphics.SurfaceTexture;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.Surface;

final class HuxerUISurfaceStream implements SurfaceTexture.OnFrameAvailableListener {
    private final long callbackToken;
    private final HandlerThread callbackThread;
    private final SurfaceTexture surfaceTexture;
    private final Surface producerSurface;
    private volatile boolean released;

    HuxerUISurfaceStream(int textureName, long callbackToken, int pixelWidth, int pixelHeight) {
        this.callbackToken = callbackToken;
        callbackThread = new HandlerThread("HuxerUI SurfaceTexture");
        callbackThread.start();
        surfaceTexture = new SurfaceTexture(textureName);
        surfaceTexture.setDefaultBufferSize(pixelWidth, pixelHeight);
        surfaceTexture.setOnFrameAvailableListener(this, new Handler(callbackThread.getLooper()));
        producerSurface = new Surface(surfaceTexture);
    }

    Surface producerSurface() {
        return producerSurface;
    }

    void updateTexture(float[] transform) {
        if (released) {
            return;
        }
        surfaceTexture.updateTexImage();
        surfaceTexture.getTransformMatrix(transform);
    }

    void setDefaultBufferSize(int pixelWidth, int pixelHeight) {
        if (!released) {
            surfaceTexture.setDefaultBufferSize(pixelWidth, pixelHeight);
        }
    }

    void release() {
        if (released) {
            return;
        }
        released = true;
        surfaceTexture.setOnFrameAvailableListener(null);
        producerSurface.release();
        surfaceTexture.release();
        callbackThread.quitSafely();
    }

    @Override
    public void onFrameAvailable(SurfaceTexture ignored) {
        if (!released) {
            nativeFrameAvailable(callbackToken);
        }
    }

    private static native void nativeFrameAvailable(long callbackToken);
}
