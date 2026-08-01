package org.huxerui;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffXfermode;
import android.graphics.RectF;
import android.os.Build;
import android.util.LruCache;

final class HuxerUIShadowRenderer {
    private static final int MINIMUM_CACHE_BYTES = 2 * 1024 * 1024;
    private static final int MAXIMUM_CACHE_BYTES = 8 * 1024 * 1024;

    private final Paint shadowPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint erasePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint bitmapPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final PorterDuffXfermode eraseMode = new PorterDuffXfermode(PorterDuff.Mode.DST_OUT);
    private final RectF destination = new RectF();
    private final RectF caster = new RectF();
    private final RectF bitmapCaster = new RectF();
    private final float density;
    private final int cacheBytes;
    private final LruCache<Object, Bitmap> cache;

    HuxerUIShadowRenderer(float density) {
        this.density = density;
        // The software fallback is per host view, so keep its lazily populated bitmap cache proportional to the app heap.
        long adaptiveBytes = Runtime.getRuntime().maxMemory() / 64L;
        cacheBytes = (int) Math.max(MINIMUM_CACHE_BYTES, Math.min(MAXIMUM_CACHE_BYTES, adaptiveBytes));
        cache = new LruCache<Object, Bitmap>(cacheBytes) {
            @Override
            protected int sizeOf(Object key, Bitmap bitmap) {
                return bitmap.getAllocationByteCount();
            }
        };
    }

    void clear() {
        cache.evictAll();
    }

    void draw(Canvas canvas, float x, float y, float width, float height, int color, float blurRadius,
            float cornerRadius) {
        if (width <= 0.0F || height <= 0.0F || (color >>> 24) == 0) {
            return;
        }
        caster.set(x, y, x + width, y + height);
        if (blurRadius <= 0.0F) {
            drawCaster(canvas, color, cornerRadius);
            return;
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            drawHardwareShadow(canvas, color, blurRadius, cornerRadius);
        } else {
            drawCachedShadow(canvas, x, y, width, height, color, blurRadius, cornerRadius);
        }
    }

    void drawPath(Canvas canvas, Path path, RectF bounds, int color, float offsetX, float offsetY, float blurRadius) {
        if (bounds.width() <= 0.0F || bounds.height() <= 0.0F || (color >>> 24) == 0) {
            return;
        }
        if (blurRadius <= 0.0F) {
            int saveCount = canvas.save();
            canvas.translate(offsetX, offsetY);
            shadowPaint.setStyle(Paint.Style.FILL);
            shadowPaint.setColor(color);
            canvas.drawPath(path, shadowPaint);
            canvas.restoreToCount(saveCount);
            return;
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            drawHardwarePathShadow(canvas, path, bounds, color, offsetX, offsetY, blurRadius);
        } else {
            drawSoftwarePathShadow(canvas, path, bounds, color, offsetX, offsetY, blurRadius);
        }
    }

    private void drawCachedShadow(Canvas canvas, float x, float y, float width, float height, int color,
            float blurRadius, float cornerRadius) {
        int blurExtent = Math.max(1, (int) Math.ceil(blurRadius * density));
        int casterWidth = Math.max(1, (int) Math.ceil(width * density));
        int casterHeight = Math.max(1, (int) Math.ceil(height * density));
        int bitmapWidth = casterWidth + blurExtent * 2;
        int bitmapHeight = casterHeight + blurExtent * 2;
        long pixelCount = (long) bitmapWidth * bitmapHeight;
        if (pixelCount > cacheBytes) {
            return;
        }

        int cornerRadiusPixels = Math.max(0, Math.round(cornerRadius * density));
        int standardDeviation = Math.max(1, Math.round(blurRadius * density / 3.0F));
        ShadowKey key = new ShadowKey(casterWidth, casterHeight, cornerRadiusPixels, blurExtent, standardDeviation);
        Bitmap bitmap = cache.get(key);
        if (bitmap == null) {
            try {
                bitmap = createShadowBitmap(key);
            } catch (OutOfMemoryError ignored) {
                return;
            }
            cache.put(key, bitmap);
        }

        destination.set(x - blurRadius, y - blurRadius, x + width + blurRadius, y + height + blurRadius);
        bitmapPaint.setColor(color);
        canvas.drawBitmap(bitmap, null, destination, bitmapPaint);
    }

    private void drawHardwareShadow(Canvas canvas, int color, float blurRadius, float cornerRadius) {
        destination.set(caster.left - blurRadius, caster.top - blurRadius, caster.right + blurRadius,
                caster.bottom + blurRadius);
        int saveCount = canvas.saveLayer(destination, null);

        shadowPaint.setStyle(Paint.Style.FILL);
        shadowPaint.setColor(0xFFFFFFFF);
        shadowPaint.setShadowLayer(blurRadius / 3.0F, 0.0F, 0.0F, color);
        canvas.drawRoundRect(caster, cornerRadius, cornerRadius, shadowPaint);
        shadowPaint.clearShadowLayer();

        erasePaint.setStyle(Paint.Style.FILL);
        erasePaint.setColor(0xFFFFFFFF);
        erasePaint.setXfermode(eraseMode);
        canvas.drawRoundRect(caster, cornerRadius, cornerRadius, erasePaint);
        erasePaint.setXfermode(null);
        canvas.restoreToCount(saveCount);
    }

    private void drawHardwarePathShadow(
            Canvas canvas, Path path, RectF bounds, int color, float offsetX, float offsetY, float blurRadius) {
        destination.set(bounds.left + offsetX - blurRadius, bounds.top + offsetY - blurRadius,
                bounds.right + offsetX + blurRadius, bounds.bottom + offsetY + blurRadius);
        int layerSaveCount = canvas.saveLayer(destination, null);
        canvas.translate(offsetX, offsetY);

        shadowPaint.setStyle(Paint.Style.FILL);
        shadowPaint.setColor(0xFFFFFFFF);
        shadowPaint.setShadowLayer(blurRadius / 3.0F, 0.0F, 0.0F, color);
        canvas.drawPath(path, shadowPaint);
        shadowPaint.clearShadowLayer();

        erasePaint.setStyle(Paint.Style.FILL);
        erasePaint.setColor(0xFFFFFFFF);
        erasePaint.setXfermode(eraseMode);
        canvas.drawPath(path, erasePaint);
        erasePaint.setXfermode(null);
        canvas.restoreToCount(layerSaveCount);
    }

    private void drawSoftwarePathShadow(
            Canvas canvas, Path path, RectF bounds, int color, float offsetX, float offsetY, float blurRadius) {
        int blurExtent = Math.max(1, (int) Math.ceil(blurRadius * density));
        long bitmapWidth = Math.max(1, (int) Math.ceil(bounds.width() * density)) + (long) blurExtent * 2L;
        long bitmapHeight = Math.max(1, (int) Math.ceil(bounds.height() * density)) + (long) blurExtent * 2L;
        if (bitmapWidth > Integer.MAX_VALUE || bitmapHeight > Integer.MAX_VALUE
                || bitmapWidth * bitmapHeight > cacheBytes) {
            return;
        }

        PathShadowKey key = new PathShadowKey(
                path, (int) bitmapWidth, (int) bitmapHeight, blurExtent, Float.floatToIntBits(blurRadius));
        Bitmap bitmap = cache.get(key);
        if (bitmap == null) {
            try {
                bitmap = createPathShadowBitmap(path, bounds, key);
            } catch (OutOfMemoryError ignored) {
                return;
            }
            cache.put(key, bitmap);
        }

        destination.set(bounds.left + offsetX - blurRadius, bounds.top + offsetY - blurRadius,
                bounds.right + offsetX + blurRadius, bounds.bottom + offsetY + blurRadius);
        bitmapPaint.setColor(color);
        canvas.drawBitmap(bitmap, null, destination, bitmapPaint);
    }

    private void drawCaster(Canvas canvas, int color, float cornerRadius) {
        shadowPaint.setStyle(Paint.Style.FILL);
        shadowPaint.setColor(color);
        canvas.drawRoundRect(caster, cornerRadius, cornerRadius, shadowPaint);
    }

    private Bitmap createShadowBitmap(ShadowKey key) {
        int bitmapWidth = key.casterWidth + key.blurExtent * 2;
        int bitmapHeight = key.casterHeight + key.blurExtent * 2;
        Bitmap bitmap = Bitmap.createBitmap(bitmapWidth, bitmapHeight, Bitmap.Config.ALPHA_8);
        Canvas canvas = new Canvas(bitmap);
        bitmapCaster.set(
                key.blurExtent, key.blurExtent, key.blurExtent + key.casterWidth, key.blurExtent + key.casterHeight);

        shadowPaint.setStyle(Paint.Style.FILL);
        shadowPaint.setColor(0xFFFFFFFF);
        shadowPaint.setShadowLayer(key.standardDeviation, 0.0F, 0.0F, 0xFFFFFFFF);
        canvas.drawRoundRect(bitmapCaster, key.cornerRadius, key.cornerRadius, shadowPaint);
        shadowPaint.clearShadowLayer();

        erasePaint.setStyle(Paint.Style.FILL);
        erasePaint.setColor(0xFFFFFFFF);
        erasePaint.setXfermode(eraseMode);
        canvas.drawRoundRect(bitmapCaster, key.cornerRadius, key.cornerRadius, erasePaint);
        erasePaint.setXfermode(null);
        return bitmap;
    }

    private Bitmap createPathShadowBitmap(Path path, RectF bounds, PathShadowKey key) {
        float blurRadius = Float.intBitsToFloat(key.blurRadiusBits);
        Bitmap bitmap = Bitmap.createBitmap(key.bitmapWidth, key.bitmapHeight, Bitmap.Config.ALPHA_8);
        Canvas canvas = new Canvas(bitmap);
        canvas.scale(density, density);
        canvas.translate(-bounds.left + blurRadius, -bounds.top + blurRadius);

        shadowPaint.setStyle(Paint.Style.FILL);
        shadowPaint.setColor(0xFFFFFFFF);
        shadowPaint.setShadowLayer(blurRadius * density / 3.0F, 0.0F, 0.0F, 0xFFFFFFFF);
        canvas.drawPath(path, shadowPaint);
        shadowPaint.clearShadowLayer();

        erasePaint.setStyle(Paint.Style.FILL);
        erasePaint.setColor(0xFFFFFFFF);
        erasePaint.setXfermode(eraseMode);
        canvas.drawPath(path, erasePaint);
        erasePaint.setXfermode(null);
        return bitmap;
    }

    private static final class ShadowKey {
        final int casterWidth;
        final int casterHeight;
        final int cornerRadius;
        final int blurExtent;
        final int standardDeviation;

        ShadowKey(int casterWidth, int casterHeight, int cornerRadius, int blurExtent, int standardDeviation) {
            this.casterWidth = casterWidth;
            this.casterHeight = casterHeight;
            this.cornerRadius = cornerRadius;
            this.blurExtent = blurExtent;
            this.standardDeviation = standardDeviation;
        }

        @Override
        public boolean equals(Object other) {
            if (this == other) {
                return true;
            }
            if (!(other instanceof ShadowKey)) {
                return false;
            }
            ShadowKey key = (ShadowKey) other;
            return casterWidth == key.casterWidth && casterHeight == key.casterHeight
                    && cornerRadius == key.cornerRadius && blurExtent == key.blurExtent
                    && standardDeviation == key.standardDeviation;
        }

        @Override
        public int hashCode() {
            int result = casterWidth;
            result = 31 * result + casterHeight;
            result = 31 * result + cornerRadius;
            result = 31 * result + blurExtent;
            return 31 * result + standardDeviation;
        }
    }

    private static final class PathShadowKey {
        final Path path;
        final int bitmapWidth;
        final int bitmapHeight;
        final int blurExtent;
        final int blurRadiusBits;

        PathShadowKey(Path path, int bitmapWidth, int bitmapHeight, int blurExtent, int blurRadiusBits) {
            this.path = path;
            this.bitmapWidth = bitmapWidth;
            this.bitmapHeight = bitmapHeight;
            this.blurExtent = blurExtent;
            this.blurRadiusBits = blurRadiusBits;
        }

        @Override
        public boolean equals(Object other) {
            if (this == other) {
                return true;
            }
            if (!(other instanceof PathShadowKey)) {
                return false;
            }
            PathShadowKey key = (PathShadowKey) other;
            return path == key.path && bitmapWidth == key.bitmapWidth && bitmapHeight == key.bitmapHeight
                    && blurExtent == key.blurExtent && blurRadiusBits == key.blurRadiusBits;
        }

        @Override
        public int hashCode() {
            int result = System.identityHashCode(path);
            result = 31 * result + bitmapWidth;
            result = 31 * result + bitmapHeight;
            result = 31 * result + blurExtent;
            return 31 * result + blurRadiusBits;
        }
    }
}
