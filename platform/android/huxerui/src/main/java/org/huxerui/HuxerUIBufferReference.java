package org.huxerui;

import java.nio.ByteBuffer;
import java.util.Objects;

/**
 * Retains address-stable direct memory without copying its bytes.
 *
 * <p>The producer must synchronize writes with readers and must not free or reuse external storage prematurely.
 * Closing a wrapper does not close copies retained by a payload. Retain this wrapper for asynchronous work, never a
 * borrowed ByteBuffer. Native readers may keep storage alive after Java wrappers have been collected.</p>
 */
public final class HuxerUIBufferReference implements AutoCloseable {
    /** Synchronous reader of a borrowed read-only buffer. The buffer must not escape this callback. */
    public interface Reader {
        void read(ByteBuffer buffer);
    }

    private long nativeHandle;
    private final int identityHash;

    HuxerUIBufferReference(long handle) {
        if (handle == 0) {
            throw new IllegalArgumentException("HuxerUI buffer reference handle must not be zero");
        }
        nativeHandle = handle;
        identityHash = hash(handle);
    }

    /**
     * Retains the current position-to-limit range without changing the caller's cursor.
     * @param buffer Direct storage whose address remains valid until all references are released.
     * @return A new retained range. Heap buffers are rejected.
     */
    public static HuxerUIBufferReference wrap(ByteBuffer buffer) {
        return wrap(buffer, null);
    }

    /**
     * Retains direct storage with an optional final-release callback.
     * @param buffer Direct storage; its current position-to-limit range is frozen.
     * @param onRelease Called once after the last native reference releases the backing state, on that thread.
     *                  Must not throw; dispatch to an appropriate thread when the storage owner requires affinity.
     *                  This callback does not run if wrapping fails.
     * @return A reference that keeps the buffer and callback alive, but cannot prevent explicit external deallocation.
     */
    public static HuxerUIBufferReference wrap(ByteBuffer buffer, Runnable onRelease) {
        Objects.requireNonNull(buffer, "buffer");
        if (!buffer.isDirect()) {
            throw new IllegalArgumentException("HuxerUI buffer reference requires a direct ByteBuffer");
        }
        return new HuxerUIBufferReference(create(buffer.slice(), onRelease));
    }

    /**
     * Borrows this range while retaining its storage for the whole callback, even if the wrapper closes inside it.
     * @param reader Synchronous reader; the read-only ByteBuffer must not escape.
     */
    public void withBuffer(Reader reader) {
        Objects.requireNonNull(reader, "reader");
        long retained = retainHandle();
        try {
            reader.read(view(retained).asReadOnlyBuffer());
        } finally {
            release(retained);
        }
    }

    /**
     * Retains a zero-copy subrange.
     * @param offset Byte offset relative to this reference.
     * @param size Number of bytes. Invalid ranges throw IllegalArgumentException.
     * @return A new wrapper sharing this backing state.
     */
    public synchronized HuxerUIBufferReference slice(int offset, int size) {
        requireOpen();
        return new HuxerUIBufferReference(slice(nativeHandle, offset, size));
    }

    synchronized long retainHandle() {
        requireOpen();
        return retain(nativeHandle);
    }

    HuxerUIBufferReference copy() {
        return new HuxerUIBufferReference(retainHandle());
    }

    private void requireOpen() {
        if (nativeHandle == 0) {
            throw new IllegalStateException("HuxerUI buffer reference is closed");
        }
    }

    @Override
    public boolean equals(Object other) {
        if (this == other) {
            return true;
        }
        if (!(other instanceof HuxerUIBufferReference)) {
            return false;
        }
        long left = retainHandle();
        try {
            long right = ((HuxerUIBufferReference) other).retainHandle();
            try {
                return equal(left, right);
            } finally {
                release(right);
            }
        } finally {
            release(left);
        }
    }

    @Override
    public int hashCode() {
        return identityHash;
    }

    /** Releases this wrapper only; copies and active readers remain valid. */
    @Override
    public void close() {
        long handle;
        synchronized (this) {
            handle = nativeHandle;
            nativeHandle = 0;
        }
        release(handle);
    }

    @Override
    @SuppressWarnings("deprecation")
    protected void finalize() throws Throwable {
        try {
            close();
        } finally {
            super.finalize();
        }
    }

    private static native long create(ByteBuffer buffer, Runnable onRelease);
    private static native long retain(long handle);
    private static native void release(long handle);
    private static native ByteBuffer view(long handle);
    private static native long slice(long handle, int offset, int size);
    private static native boolean equal(long left, long right);
    private static native int hash(long handle);
}
