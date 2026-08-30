package org.huxerui;

/** Shared result, event, and cancellation primitives used by Java-backed platform instances. */
public final class HuxerUIPlatformChannel {
    /** Cancels platform work associated with an in-flight invocation. */
    public interface Cancellation {
        /** Requests cancellation. Implementations should be idempotent. */
        void cancel();
    }

    /**
     * Emits named events to the C++ PlatformChannel or PlatformView event bindings that own this instance.
     *
     * <p>PlatformModule notifications may be emitted from any thread, are delivered asynchronously, and return null.
     * PlatformView events must be emitted on the owning UI thread, are delivered synchronously, and may return a
     * decision payload. Off-thread PlatformView emission and calls after {@link #close()} are ignored. Implementations
     * must stop retaining this sink during disposal.</p>
     */
    public static final class Events implements AutoCloseable {
        private long nativeHandle;

        Events(long nativeHandle) {
            if (nativeHandle == 0L) {
                throw new IllegalArgumentException("HuxerUI platform event handle must not be zero");
            }
            this.nativeHandle = nativeHandle;
        }

        /** Emits a fieldless event whose payload is Null and returns an optional synchronous PlatformView result. */
        public synchronized PlatformPayload emit(String event) {
            return emit(event, PlatformPayload.nullValue());
        }

        /** Emits an event with an immutable PlatformPayload value and returns an optional synchronous result. */
        public synchronized PlatformPayload emit(String event, PlatformPayload payload) {
            if (nativeHandle != 0L) {
                return nativeEmit(nativeHandle, event, payload);
            }
            return null;
        }

        /** Detaches this sink from its C++ owner. Later emissions are ignored. */
        @Override
        public synchronized void close() {
            if (nativeHandle != 0L) {
                nativeReleaseEvent(nativeHandle);
                nativeHandle = 0L;
            }
        }

        @Override
        protected void finalize() throws Throwable {
            try {
                close();
            } finally {
                super.finalize();
            }
        }
    }

    /**
     * Completes one C++ PlatformChannel invocation.
     *
     * <p>{@link #complete(PlatformPayload)} and {@link #fail(String, String, PlatformPayload)} are mutually exclusive
     * one-shot operations. Calls after completion, failure, cancellation, or closure are ignored.</p>
     */
    public static final class Result implements AutoCloseable {
        private long nativeHandle;

        Result(long nativeHandle) {
            if (nativeHandle == 0L) {
                throw new IllegalArgumentException("HuxerUI platform result handle must not be zero");
            }
            this.nativeHandle = nativeHandle;
        }

        /** Completes the invocation successfully with one payload. */
        public synchronized void complete(PlatformPayload payload) {
            if (nativeHandle == 0L) {
                return;
            }
            long handle = nativeHandle;
            nativeHandle = 0L;
            nativeComplete(handle, payload);
        }

        /** Completes the invocation with a stable error code, English message, and optional structured details. */
        public synchronized void fail(String code, String message, PlatformPayload details) {
            if (nativeHandle == 0L) {
                return;
            }
            long handle = nativeHandle;
            nativeHandle = 0L;
            nativeFail(handle, code, message, details);
        }

        /** Releases an incomplete result without delivering a value. */
        @Override
        public synchronized void close() {
            if (nativeHandle != 0L) {
                nativeReleaseResult(nativeHandle);
                nativeHandle = 0L;
            }
        }

        @Override
        protected void finalize() throws Throwable {
            try {
                close();
            } finally {
                super.finalize();
            }
        }
    }

    private HuxerUIPlatformChannel() {}

    private static native PlatformPayload nativeEmit(long handle, String event, PlatformPayload payload);

    private static native void nativeReleaseEvent(long handle);

    private static native void nativeComplete(long handle, PlatformPayload payload);

    private static native void nativeFail(long handle, String code, String message, PlatformPayload details);

    private static native void nativeReleaseResult(long handle);
}
