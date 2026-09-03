package org.huxerui;

import android.annotation.SuppressLint;
import android.content.Context;
import android.graphics.SurfaceTexture;
import android.view.Surface;
import android.view.TextureView;
import android.view.View;

@SuppressLint("ViewConstructor")
final class HuxerUITextureLayer extends TextureView implements TextureView.SurfaceTextureListener {
    private final HuxerUIView root;
    private final long identity;
    private Surface surface;
    private boolean surfaceRegistered;
    private int layerWidth = 1;
    private int layerHeight = 1;

    HuxerUITextureLayer(Context context, HuxerUIView root, long identity) {
        super(context);
        this.root = root;
        this.identity = identity;
        setSurfaceTextureListener(this);
        setOpaque(false);
        setClickable(false);
        setFocusable(false);
        setFocusableInTouchMode(false);
        setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
    }

    void resize(int width, int height) {
        int nextWidth = Math.max(1, width);
        int nextHeight = Math.max(1, height);
        if (layerWidth == nextWidth && layerHeight == nextHeight) {
            return;
        }
        layerWidth = nextWidth;
        layerHeight = nextHeight;
        requestLayout();
    }

    void applyLayout() {
        layout(0, 0, layerWidth, layerHeight);
    }

    int layerWidth() {
        return layerWidth;
    }

    int layerHeight() {
        return layerHeight;
    }

    void detachLayer() {
        if (surfaceRegistered) {
            surfaceRegistered = false;
            root.textureLayerSurfaceDestroyed(identity);
        }
        if (surface != null) {
            surface.release();
            surface = null;
        }
    }

    @Override
    public void onSurfaceTextureAvailable(SurfaceTexture surfaceTexture, int width, int height) {
        if (surface != null) {
            surface.release();
        }
        surface = new Surface(surfaceTexture);
        surfaceRegistered = true;
        root.textureLayerSurfaceAvailable(identity, surface, width, height);
    }

    @Override
    public void onSurfaceTextureSizeChanged(SurfaceTexture surfaceTexture, int width, int height) {
        if (surface != null) {
            root.textureLayerSurfaceAvailable(identity, surface, width, height);
        }
    }

    @Override
    public boolean onSurfaceTextureDestroyed(SurfaceTexture surfaceTexture) {
        detachLayer();
        return true;
    }

    @Override
    public void onSurfaceTextureUpdated(SurfaceTexture surfaceTexture) {}
}
