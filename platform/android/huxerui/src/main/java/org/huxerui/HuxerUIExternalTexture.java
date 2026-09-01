package org.huxerui;

/**
 * An opaque shared reference to an ExternalTexture capability.
 *
 * <p>Platform libraries receive this value through {@link PlatformPayload}. It may be retained in another payload while
 * open, but it does not expose renderer internals or frame publication. Close the value when the Java owner no
 * longer needs it; repeated calls to {@link #close()} are safe.</p>
 */
public final class HuxerUIExternalTexture implements AutoCloseable {
    private long handle;

    HuxerUIExternalTexture(long handle) {
        if (handle == 0L) {
            throw new IllegalArgumentException("HuxerUI external texture handle must not be zero");
        }
        this.handle = handle;
    }

    synchronized long handle() {
        if (handle == 0L) {
            throw new IllegalStateException("HuxerUI external texture is closed");
        }
        return handle;
    }

    synchronized long retainHandle() {
        if (handle == 0L) {
            throw new IllegalStateException("HuxerUI external texture is closed");
        }
        return retain(handle);
    }

    /** Releases this Java reference to the shared texture capability. */
    @Override
    public synchronized void close() {
        if (handle != 0L) {
            release(handle);
            handle = 0L;
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

    private static native void release(long handle);

    private static native long retain(long handle);
}
