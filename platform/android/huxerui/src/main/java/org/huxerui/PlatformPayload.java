package org.huxerui;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.TreeMap;

/**
 * An immutable kind-preserving value exchanged with C++ at a Java platform boundary.
 *
 * <p>PlatformPayload is not JSON: signed 64-bit integers, doubles, UTF-8 strings, bytes, collections, and external
 * textures remain distinct. Factory methods defensively copy mutable input, collection accessors return immutable
 * views, and byte accessors return copies.</p>
 *
 * <p>Structured decoders should use {@link #requireField(String)}, the {@code requireXxx} accessors, and
 * {@link #rejectUnknownFields(Set)}. Validation errors include a path such as {@code $.session.timeout}.</p>
 *
 * <pre>{@code
 * PlatformPayload options = PlatformPayload.object(Map.of(
 *         "enabled", PlatformPayload.booleanValue(true),
 *         "attempt", PlatformPayload.int64(3)));
 * boolean enabled = options.requireField("enabled").requireBoolean();
 * options.rejectUnknownFields(Set.of("enabled", "attempt"));
 * }</pre>
 */
public final class PlatformPayload {
    /** Identifies the exact kind stored by a PlatformPayload. */
    public enum Kind {
        NULL,
        BOOLEAN,
        INT64,
        DOUBLE,
        STRING,
        BYTES,
        LIST,
        OBJECT,
        EXTERNAL_TEXTURE,
    }

    private static final int MAX_ENVELOPE_BYTES = 64 * 1024 * 1024;
    private static final int MAX_SCALAR_BYTES = 16 * 1024 * 1024;
    private static final int MAX_CONTAINER_ENTRIES = 1024 * 1024;
    private static final int MAX_CAPABILITY_SLOTS = 1024 * 1024;
    private static final int MAX_NESTING_DEPTH = 64;
    private static final byte EXTERNAL_TEXTURE_CAPABILITY = 1;
    private static final PlatformPayload NULL = new PlatformPayload(Kind.NULL, null, "$");

    private final Kind kind;
    private final Object value;
    private final String path;

    private PlatformPayload(Kind kind, Object value, String path) {
        this.kind = kind;
        this.value = value;
        this.path = path;
    }

    /** Returns the shared Null payload. */
    public static PlatformPayload nullValue() {
        return NULL;
    }

    /** Creates a Boolean payload. */
    public static PlatformPayload booleanValue(boolean value) {
        return new PlatformPayload(Kind.BOOLEAN, value, "$");
    }

    /** Creates a signed 64-bit Integer payload. */
    public static PlatformPayload int64(long value) {
        return new PlatformPayload(Kind.INT64, value, "$");
    }

    /** Creates a finite Double payload. */
    public static PlatformPayload doubleValue(double value) {
        if (!Double.isFinite(value)) {
            throw new IllegalArgumentException("HuxerUI PlatformPayload double must be finite");
        }
        return new PlatformPayload(Kind.DOUBLE, value == 0.0 ? 0.0 : value, "$");
    }

    /** Creates a String payload after validating that the value can be represented as UTF-8. */
    public static PlatformPayload string(String value) {
        requireValidString(Objects.requireNonNull(value, "value"), "string");
        return new PlatformPayload(Kind.STRING, value, "$");
    }

    /** Creates a Bytes payload containing a defensive copy. */
    public static PlatformPayload bytes(byte[] value) {
        Objects.requireNonNull(value, "value");
        if (value.length > MAX_SCALAR_BYTES) {
            throw new IllegalArgumentException("HuxerUI PlatformPayload byte value is too large");
        }
        return new PlatformPayload(Kind.BYTES, value.clone(), "$");
    }

    /** Creates an immutable List payload after validating its size and nesting depth. */
    public static PlatformPayload list(List<PlatformPayload> values) {
        Objects.requireNonNull(values, "values");
        if (values.size() > MAX_CONTAINER_ENTRIES) {
            throw new IllegalArgumentException("HuxerUI PlatformPayload list is too large");
        }
        ArrayList<PlatformPayload> copy = new ArrayList<>(values.size());
        for (PlatformPayload value : values) {
            PlatformPayload child = Objects.requireNonNull(value, "list value");
            validateNesting(child, 1);
            copy.add(child);
        }
        return new PlatformPayload(Kind.LIST, Collections.unmodifiableList(copy), "$");
    }

    /** Creates an immutable Object payload with unique UTF-8 keys in deterministic byte order. */
    public static PlatformPayload object(Map<String, PlatformPayload> fields) {
        Objects.requireNonNull(fields, "fields");
        if (fields.size() > MAX_CONTAINER_ENTRIES) {
            throw new IllegalArgumentException("HuxerUI PlatformPayload object is too large");
        }
        TreeMap<String, PlatformPayload> sorted = new TreeMap<>(PlatformPayload::compareUtf8);
        for (Map.Entry<String, PlatformPayload> field : fields.entrySet()) {
            String name = Objects.requireNonNull(field.getKey(), "field name");
            PlatformPayload value = Objects.requireNonNull(field.getValue(), "field value");
            requireValidString(name, "object key");
            validateNesting(value, 1);
            sorted.put(name, value);
        }
        return new PlatformPayload(Kind.OBJECT, Collections.unmodifiableMap(sorted), "$");
    }

    /** Creates an opaque ExternalTexture capability payload. */
    public static PlatformPayload externalTexture(HuxerUIExternalTexture value) {
        return new PlatformPayload(Kind.EXTERNAL_TEXTURE, Objects.requireNonNull(value, "value"), "$");
    }

    /** Returns the exact stored kind. */
    public Kind kind() {
        return kind;
    }

    /** Returns true when this payload stores Null. */
    public boolean isNull() {
        return kind == Kind.NULL;
    }

    /** Requires Null or throws an IllegalArgumentException containing this value's structured path. */
    public void requireNull() {
        requireKind(Kind.NULL);
    }

    /** Returns the Boolean value or throws when the kind does not match. */
    public boolean requireBoolean() {
        requireKind(Kind.BOOLEAN);
        return (boolean) value;
    }

    /** Returns the signed 64-bit Integer value or throws when the kind does not match. */
    public long requireInt64() {
        requireKind(Kind.INT64);
        return (long) value;
    }

    /** Returns the Double value or throws when the kind does not match. */
    public double requireDouble() {
        requireKind(Kind.DOUBLE);
        return (double) value;
    }

    /** Returns the immutable String value or throws when the kind does not match. */
    public String requireString() {
        requireKind(Kind.STRING);
        return (String) value;
    }

    /** Returns a copy of the Bytes value or throws when the kind does not match. */
    public byte[] requireBytes() {
        requireKind(Kind.BYTES);
        return ((byte[]) value).clone();
    }

    /** Returns the ExternalTexture capability or throws when the kind does not match. */
    public HuxerUIExternalTexture requireExternalTexture() {
        requireKind(Kind.EXTERNAL_TEXTURE);
        return (HuxerUIExternalTexture) value;
    }

    /** Returns a required Object field with its full validation path, or throws when it is missing. */
    public PlatformPayload requireField(String name) {
        PlatformPayload field = field(name);
        if (field == null) {
            throw new IllegalArgumentException("HuxerUI PlatformPayload is missing " + childPath(name));
        }
        return field;
    }

    /** Returns an optional Object field with its full validation path, or null when it is absent. */
    public PlatformPayload field(String name) {
        requireKind(Kind.OBJECT);
        String fieldName = Objects.requireNonNull(name, "name");
        PlatformPayload field = objectValue().get(fieldName);
        return field == null ? null : field.atPath(childPath(fieldName));
    }

    /** Returns an immutable view of all Object fields with their full validation paths. */
    public Map<String, PlatformPayload> fields() {
        requireKind(Kind.OBJECT);
        TreeMap<String, PlatformPayload> result = new TreeMap<>(PlatformPayload::compareUtf8);
        for (Map.Entry<String, PlatformPayload> field : objectValue().entrySet()) {
            result.put(field.getKey(), field.getValue().atPath(childPath(field.getKey())));
        }
        return Collections.unmodifiableMap(result);
    }

    /** Throws when the Object contains a field outside the supplied accepted-name set. */
    public void rejectUnknownFields(Set<String> names) {
        requireKind(Kind.OBJECT);
        Objects.requireNonNull(names, "names");
        for (String name : objectValue().keySet()) {
            if (!names.contains(name)) {
                throw new IllegalArgumentException("HuxerUI PlatformPayload contains unknown field " + childPath(name));
            }
        }
    }

    /** Returns an immutable view of all List elements with index-aware validation paths. */
    public List<PlatformPayload> elements() {
        requireKind(Kind.LIST);
        ArrayList<PlatformPayload> result = new ArrayList<>(listValue().size());
        for (int index = 0; index < listValue().size(); ++index) {
            result.add(listValue().get(index).atPath(path + "[" + index + "]"));
        }
        return Collections.unmodifiableList(result);
    }

    /** Returns one List element with its index-aware validation path. */
    public PlatformPayload element(int index) {
        requireKind(Kind.LIST);
        if (index < 0 || index >= listValue().size()) {
            throw new IndexOutOfBoundsException("HuxerUI PlatformPayload element is outside " + path);
        }
        return listValue().get(index).atPath(path + "[" + index + "]");
    }

    @Override
    public boolean equals(Object other) {
        if (this == other) {
            return true;
        }
        if (!(other instanceof PlatformPayload)) {
            return false;
        }
        PlatformPayload payload = (PlatformPayload) other;
        if (kind != payload.kind) {
            return false;
        }
        if (kind == Kind.BYTES) {
            return Arrays.equals((byte[]) value, (byte[]) payload.value);
        }
        return Objects.equals(value, payload.value);
    }

    @Override
    public int hashCode() {
        return kind == Kind.BYTES ? 31 * kind.hashCode() + Arrays.hashCode((byte[]) value)
                                  : 31 * kind.hashCode() + Objects.hashCode(value);
    }

    private PlatformPayload atPath(String nextPath) {
        return new PlatformPayload(kind, value, nextPath);
    }

    private void requireKind(Kind expected) {
        if (kind != expected) {
            throw new IllegalArgumentException(
                    "HuxerUI PlatformPayload at " + path + " is " + kind + ", expected " + expected);
        }
    }

    private String childPath(String name) {
        return path + "." + name;
    }

    @SuppressWarnings("unchecked")
    private List<PlatformPayload> listValue() {
        return (List<PlatformPayload>) value;
    }

    @SuppressWarnings("unchecked")
    private Map<String, PlatformPayload> objectValue() {
        return (Map<String, PlatformPayload>) value;
    }

    private static int compareUtf8(String left, String right) {
        byte[] leftBytes = left.getBytes(StandardCharsets.UTF_8);
        byte[] rightBytes = right.getBytes(StandardCharsets.UTF_8);
        int count = Math.min(leftBytes.length, rightBytes.length);
        for (int index = 0; index < count; ++index) {
            int difference = Byte.toUnsignedInt(leftBytes[index]) - Byte.toUnsignedInt(rightBytes[index]);
            if (difference != 0) {
                return difference;
            }
        }
        return leftBytes.length - rightBytes.length;
    }

    private static void requireValidString(String value, String description) {
        try {
            StandardCharsets.UTF_8.newEncoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .encode(java.nio.CharBuffer.wrap(value));
        } catch (CharacterCodingException exception) {
            throw new IllegalArgumentException("HuxerUI PlatformPayload " + description + " is not valid Unicode");
        }
    }

    private static void validateNesting(PlatformPayload payload, int depth) {
        if (depth > MAX_NESTING_DEPTH) {
            throw new IllegalArgumentException("HuxerUI PlatformPayload exceeds the maximum nesting depth");
        }
        if (payload.kind == Kind.LIST) {
            for (PlatformPayload child : payload.listValue()) {
                validateNesting(child, depth + 1);
            }
        } else if (payload.kind == Kind.OBJECT) {
            for (PlatformPayload child : payload.objectValue().values()) {
                validateNesting(child, depth + 1);
            }
        }
    }

    static Envelope encodeEnvelope(PlatformPayload payload) {
        return new Writer().write(Objects.requireNonNull(payload, "payload"));
    }

    static PlatformPayload decodeEnvelope(byte[] bytes, List<HuxerUIExternalTexture> externalTextures) {
        return new Reader(bytes, externalTextures).read();
    }

    static final class Envelope {
        final byte[] bytes;
        final List<HuxerUIExternalTexture> externalTextures;

        Envelope(byte[] bytes, List<HuxerUIExternalTexture> externalTextures) {
            this.bytes = bytes;
            this.externalTextures = externalTextures;
        }
    }

    private static final class Writer {
        private final ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        private final ArrayList<HuxerUIExternalTexture> externalTextures = new ArrayList<>();

        Envelope write(PlatformPayload payload) {
            writeByte('H');
            writeByte('U');
            writeByte('X');
            writeByte('P');
            writeInt16(1);
            writeInt16(0);
            writeValue(payload, 0);
            return new Envelope(bytes.toByteArray(), Collections.unmodifiableList(externalTextures));
        }

        private void writeValue(PlatformPayload payload, int depth) {
            if (depth > MAX_NESTING_DEPTH) {
                throw new IllegalArgumentException(
                        "HuxerUI PlatformPayload exceeds the maximum envelope nesting depth");
            }
            writeByte(payload.kind.ordinal());
            switch (payload.kind) {
                case NULL:
                    return;
                case BOOLEAN:
                    writeByte(payload.requireBoolean() ? 1 : 0);
                    return;
                case INT64:
                    writeInt64(payload.requireInt64());
                    return;
                case DOUBLE:
                    writeInt64(Double.doubleToRawLongBits(payload.requireDouble()));
                    return;
                case STRING:
                    writeString(payload.requireString());
                    return;
                case BYTES:
                    writeBytes(payload.requireBytes(), MAX_SCALAR_BYTES, "byte value");
                    return;
                case LIST:
                    writeLength(payload.listValue().size(), MAX_CONTAINER_ENTRIES, "list");
                    for (PlatformPayload element : payload.listValue()) {
                        writeValue(element, depth + 1);
                    }
                    return;
                case OBJECT:
                    writeLength(payload.objectValue().size(), MAX_CONTAINER_ENTRIES, "object");
                    for (Map.Entry<String, PlatformPayload> field : payload.objectValue().entrySet()) {
                        writeString(field.getKey());
                        writeValue(field.getValue(), depth + 1);
                    }
                    return;
                case EXTERNAL_TEXTURE:
                    writeByte(EXTERNAL_TEXTURE_CAPABILITY);
                    HuxerUIExternalTexture texture = payload.requireExternalTexture();
                    int slot = externalTextures.indexOf(texture);
                    if (slot < 0) {
                        if (externalTextures.size() >= MAX_CAPABILITY_SLOTS) {
                            throw new IllegalArgumentException(
                                    "HuxerUI PlatformPayload contains too many external textures");
                        }
                        slot = externalTextures.size();
                        externalTextures.add(texture);
                    }
                    writeInt32(slot);
            }
        }

        private void writeString(String value) {
            writeBytes(value.getBytes(StandardCharsets.UTF_8), MAX_SCALAR_BYTES, "string");
        }

        private void writeBytes(byte[] value, int maximum, String description) {
            writeLength(value.length, maximum, description);
            ensureAvailable(value.length);
            bytes.write(value, 0, value.length);
        }

        private void writeLength(int value, int maximum, String description) {
            if (value < 0 || value > maximum) {
                throw new IllegalArgumentException("HuxerUI PlatformPayload " + description + " is too large");
            }
            writeInt32(value);
        }

        private void writeByte(int value) {
            ensureAvailable(1);
            bytes.write(value);
        }

        private void writeInt16(int value) {
            writeByte(value);
            writeByte(value >>> 8);
        }

        private void writeInt32(int value) {
            for (int offset = 0; offset < 32; offset += 8) {
                writeByte(value >>> offset);
            }
        }

        private void writeInt64(long value) {
            for (int offset = 0; offset < 64; offset += 8) {
                writeByte((int) (value >>> offset));
            }
        }

        private void ensureAvailable(int additional) {
            if (additional < 0 || bytes.size() > MAX_ENVELOPE_BYTES - additional) {
                throw new IllegalArgumentException("HuxerUI PlatformPayload envelope is too large");
            }
        }
    }

    private static final class Reader {
        private final ByteBuffer bytes;
        private final List<HuxerUIExternalTexture> externalTextures;

        Reader(byte[] bytes, List<HuxerUIExternalTexture> externalTextures) {
            Objects.requireNonNull(bytes, "bytes");
            Objects.requireNonNull(externalTextures, "externalTextures");
            if (bytes.length > MAX_ENVELOPE_BYTES || externalTextures.size() > MAX_CAPABILITY_SLOTS) {
                throw new IllegalArgumentException("HuxerUI PlatformPayload envelope is too large");
            }
            if (new HashSet<>(externalTextures).size() != externalTextures.size() || externalTextures.contains(null)) {
                throw new IllegalArgumentException("HuxerUI PlatformPayload capability table is invalid");
            }
            this.bytes = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
            this.externalTextures = externalTextures;
        }

        PlatformPayload read() {
            if (readByte() != 'H' || readByte() != 'U' || readByte() != 'X' || readByte() != 'P') {
                throw malformed("invalid header");
            }
            if (readInt16() != 1) {
                throw malformed("unsupported version");
            }
            if (readInt16() != 0) {
                throw malformed("unsupported flags");
            }
            PlatformPayload payload = readValue(0, "$");
            if (bytes.hasRemaining()) {
                throw malformed("trailing bytes");
            }
            return payload;
        }

        private PlatformPayload readValue(int depth, String path) {
            if (depth > MAX_NESTING_DEPTH) {
                throw malformed("excessive nesting");
            }
            int tag = readByte();
            Kind[] kinds = Kind.values();
            if (tag < 0 || tag >= kinds.length) {
                throw malformed("unknown value tag");
            }
            switch (kinds[tag]) {
                case NULL:
                    return new PlatformPayload(Kind.NULL, null, path);
                case BOOLEAN:
                    int booleanValue = readByte();
                    if (booleanValue > 1) {
                        throw malformed("invalid boolean");
                    }
                    return new PlatformPayload(Kind.BOOLEAN, booleanValue != 0, path);
                case INT64:
                    return new PlatformPayload(Kind.INT64, readInt64(), path);
                case DOUBLE:
                    double doubleValue = Double.longBitsToDouble(readInt64());
                    if (!Double.isFinite(doubleValue)) {
                        throw malformed("non-finite double");
                    }
                    return new PlatformPayload(Kind.DOUBLE, doubleValue == 0.0 ? 0.0 : doubleValue, path);
                case STRING:
                    return new PlatformPayload(Kind.STRING, readString(), path);
                case BYTES:
                    return new PlatformPayload(Kind.BYTES, readBytes(readLength(MAX_SCALAR_BYTES)), path);
                case LIST:
                    int listSize = readLength(MAX_CONTAINER_ENTRIES);
                    ArrayList<PlatformPayload> elements = new ArrayList<>(listSize);
                    for (int index = 0; index < listSize; ++index) {
                        elements.add(readValue(depth + 1, path + "[" + index + "]"));
                    }
                    return new PlatformPayload(Kind.LIST, Collections.unmodifiableList(elements), path);
                case OBJECT:
                    int fieldCount = readLength(MAX_CONTAINER_ENTRIES);
                    TreeMap<String, PlatformPayload> fields = new TreeMap<>(PlatformPayload::compareUtf8);
                    for (int index = 0; index < fieldCount; ++index) {
                        String name = readString();
                        if (fields.put(name, readValue(depth + 1, path + "." + name)) != null) {
                            throw malformed("duplicate object key");
                        }
                    }
                    return new PlatformPayload(Kind.OBJECT, Collections.unmodifiableMap(fields), path);
                case EXTERNAL_TEXTURE:
                    if (readByte() != EXTERNAL_TEXTURE_CAPABILITY) {
                        throw malformed("unknown capability kind");
                    }
                    int slot = readInt32();
                    if (slot < 0 || slot >= externalTextures.size()) {
                        throw malformed("missing external texture");
                    }
                    return new PlatformPayload(Kind.EXTERNAL_TEXTURE, externalTextures.get(slot), path);
                default:
                    throw malformed("unknown value tag");
            }
        }

        private String readString() {
            byte[] value = readBytes(readLength(MAX_SCALAR_BYTES));
            try {
                return StandardCharsets.UTF_8.newDecoder()
                        .onMalformedInput(CodingErrorAction.REPORT)
                        .onUnmappableCharacter(CodingErrorAction.REPORT)
                        .decode(ByteBuffer.wrap(value))
                        .toString();
            } catch (CharacterCodingException exception) {
                throw malformed("invalid UTF-8 string");
            }
        }

        private byte[] readBytes(int length) {
            require(length);
            byte[] result = new byte[length];
            bytes.get(result);
            return result;
        }

        private int readLength(int maximum) {
            long value = Integer.toUnsignedLong(readInt32());
            if (value > maximum) {
                throw malformed("excessive length");
            }
            return (int) value;
        }

        private int readByte() {
            require(1);
            return Byte.toUnsignedInt(bytes.get());
        }

        private int readInt16() {
            require(2);
            return Short.toUnsignedInt(bytes.getShort());
        }

        private int readInt32() {
            require(4);
            return bytes.getInt();
        }

        private long readInt64() {
            require(8);
            return bytes.getLong();
        }

        private void require(int length) {
            if (length < 0 || bytes.remaining() < length) {
                throw malformed("truncated input");
            }
        }

        private IllegalArgumentException malformed(String reason) {
            return new IllegalArgumentException("HuxerUI PlatformPayload envelope has " + reason);
        }
    }
}
