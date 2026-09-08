package org.huxerui.examples.bufferreference;

import android.content.Context;

import java.nio.ByteBuffer;
import java.util.LinkedHashMap;
import java.util.Map;

import org.huxerui.HuxerUIBufferReference;
import org.huxerui.HuxerUIPlatformChannel;
import org.huxerui.HuxerUIPlatformModule;
import org.huxerui.PlatformPayload;

public final class PlatformFrameSource implements HuxerUIPlatformModule.Factory {
    @Override
    public HuxerUIPlatformModule create(
            Context context, PlatformPayload options, HuxerUIPlatformChannel.Events events) {
        options.requireNull();
        return new Instance();
    }

    private static final class Instance implements HuxerUIPlatformModule {
        private static final int WIDTH = 320;
        private static final int HEIGHT = 180;
        private static final int STRIDE = 336;
        private final ByteBuffer storage = ByteBuffer.allocateDirect(STRIDE * HEIGHT);
        private final HuxerUIBufferReference reference = HuxerUIBufferReference.wrap(storage);
        private long sequence;
        private boolean disposed;

        @Override
        public HuxerUIPlatformChannel.Cancellation invoke(
                String method, PlatformPayload arguments, HuxerUIPlatformChannel.Result result) {
            if (!"next".equals(method) || !arguments.isNull() || disposed || sequence == Long.MAX_VALUE) {
                result.fail("example/invalid-request", "HuxerUI frame source cannot fulfill this request",
                        PlatformPayload.nullValue());
                return null;
            }
            ++sequence;
            // There is no producer timer: the next request acknowledges completion of the preceding C++ read.
            for (int y = 0; y < HEIGHT; ++y) {
                for (int x = 0; x < STRIDE; ++x) {
                    int value = x < WIDTH ? (int) ((x + y + sequence % 128) % 128 + (sequence % 2) * 128) : 255;
                    storage.put(y * STRIDE + x, (byte) value);
                }
            }
            Map<String, PlatformPayload> fields = new LinkedHashMap<>();
            fields.put("pixels", PlatformPayload.bufferReference(reference));
            fields.put("width", PlatformPayload.int64(WIDTH));
            fields.put("height", PlatformPayload.int64(HEIGHT));
            fields.put("stride", PlatformPayload.int64(STRIDE));
            fields.put("sequence", PlatformPayload.int64(sequence));
            result.complete(PlatformPayload.object(fields));
            return null;
        }

        @Override
        public void dispose() {
            disposed = true;
            reference.close();
        }
    }
}
