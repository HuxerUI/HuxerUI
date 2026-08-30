# URI and Local File URI Design

This document defines the immutable `Uri` value, generic URI parsing, retained serialization, local `File` conversion, and the boundaries with application activation, HTTP, and platform-granted files.

## Public model

`<huxerui/data.h>` declares the shared owned-data values `Bytes` and `Uri`:

```cpp
class Uri final {
public:
  explicit Uri(std::string value);

  [[nodiscard]] static std::optional<Uri> Parse(std::string_view value);

  [[nodiscard]] std::string_view Scheme() const noexcept;
  [[nodiscard]] std::optional<std::string_view> Authority() const noexcept;
  [[nodiscard]] std::string_view Path() const noexcept;
  [[nodiscard]] std::optional<std::string_view> Query() const noexcept;
  [[nodiscard]] std::optional<std::string_view> Fragment() const noexcept;

  [[nodiscard]] const std::string& ToString() const noexcept;

  [[nodiscard]] bool operator==(const Uri& other) const noexcept;
};
```

`Uri` has no empty or default state.
The direct constructor throws `std::invalid_argument` for invalid caller input, while `Parse()` returns `std::nullopt` for invalid external text.
Allocation failures and other non-validation failures are not converted into parse failure.

## Syntax and components

`Uri` accepts the absolute generic URI syntax defined by RFC 3986: a non-empty ASCII scheme followed by `:`, a hierarchical part, and optional query and fragment components.
The parser validates generic component delimiters, allowed characters, and every percent escape.
It does not validate scheme-specific semantics, resolve host names, test ports, access the network, or determine whether a value can be dereferenced.

The retained serialization is ASCII URI syntax.
Raw non-ASCII text belongs to the distinct IRI model and is rejected; producers encode textual data as UTF-8 octets followed by URI percent encoding.
`Uri` does not add an IRI conversion or normalization policy.

`Scheme()` and `Path()` always return defined components, although the path may be empty.
`Authority()`, `Query()`, and `Fragment()` return `std::optional<std::string_view>` because an absent delimiter is structurally different from a present empty component.
The views refer to the immutable retained serialization and remain valid while that `Uri` value is not moved from or destroyed.

The generic parser validates authority delimiters and permitted characters but does not expose separate host, user-info, or port values.
Scheme owners remain responsible for the semantics of those subcomponents.

## Serialization and equality

`Uri` preserves the validated input byte for byte.
It does not sort query parameters, remove duplicate keys, decode and re-encode components, collapse path segments, rewrite case, remove default ports, or normalize percent escapes.

Equality compares only the retained serialization.
Lexically different valid values remain different even if a scheme-specific processor could treat them as equivalent.
This preserves signed URLs and application-defined identifiers without introducing an implicit canonicalization policy.

## Local File conversion

`<huxerui/file.h>` adds explicit conversion in both directions:

```cpp
class File final {
public:
  explicit File(const Uri& uri);
  [[nodiscard]] Uri ToUri() const;
};
```

`File` already resolves relative path input to a normalized absolute path at construction.
`ToUri()` encodes that stable stored path and never resolves it again against a later process working directory.
Neither conversion accesses the filesystem, requires the target to exist, or changes permissions.

File path text is encoded as UTF-8 and then percent-encoded by URI path rules.
Path separators remain structural `/` characters, while `%`, spaces, non-ASCII bytes, `?`, `#`, and other non-path data are encoded.
Decoding rejects encoded separators, encoded control characters, malformed UTF-8, and values that would change the platform path structure.

On POSIX-like hosts, including Apple, Android, Linux, and Web virtual filesystems, local conversion accepts an absent authority, an empty authority, or ASCII-case-insensitive `localhost` and rejects every remote authority.
The decoded path must be absolute.

On Windows, an absent or empty authority requires an absolute drive path such as `file:///C:/folder/item.txt`.
An ASCII-case-insensitive `localhost` authority maps a drive-shaped path to that drive and otherwise identifies the local UNC server.
Any other supported non-empty authority maps to a UNC server and requires a non-empty share path.
`ToUri()` emits the authority form for UNC paths.
Device paths, drive-relative paths, legacy vertical-line drive forms, raw backslash URI forms, and UNC paths embedded in an empty-authority URI path are unsupported.

Every host rejects file URIs containing user-info, a port, a query, or a fragment.
Unsupported authorities and non-file schemes throw `std::invalid_argument` rather than being reinterpreted as local paths.

## Application activation and HTTP

`UrlActivation::url` is a `Uri` value.
Platform application shells validate native URL input before Runtime delivery.
Invalid cold-start input becomes `LaunchActivation`, while invalid later input is ignored at the platform boundary.
Equal serialized values delivered at different times remain distinct FIFO activations.

Android `content:` values and platform file-open inputs continue becoming `FileReference` capabilities when they carry platform access.
They are not projected into `UrlActivation` or local `File` values merely because their serialized syntax can be represented by `Uri`.

`HttpRequest::url` and `HttpResponse::url` remain `std::string`.
Platform HTTP transports remain authoritative for redirects, proxy behavior, TLS, signed request values, and platform URL compatibility.
HuxerUI does not reconstruct an HTTP URL through `Uri` before transport.

## Ownership and non-goals

`Uri` owns only generic syntax, retained serialization, and component offsets.
`File` owns file-scheme conversion semantics.
Platform application shells own native URL conversion and invalid-input policy.
HTTP transports own network URL behavior, and `FileReference` owns external file capabilities.

The design does not add a parser service, builder, normalizer, query map, host value, scheme registry, route matcher, URI template system, or Runtime state.
Relative URI references remain strings in the existing Web navigation codec.

## Validation

Focused unit tests cover valid hierarchical and opaque URIs, empty and absent components, malformed schemes and escapes, invalid characters, raw Unicode rejection, retained equality, and non-throwing parse failure.
File tests cover spaces, percent characters, UTF-8 path text, roots, Windows drives, UNC paths, POSIX paths, local authorities, and rejection of unsupported structure without filesystem access.
Application and platform tests cover validated cold and subsequent URL activations without changing FIFO delivery or file-capability behavior.
The owning public header compiles independently and is exported through `<huxerui/huxerui.h>` and SDK packaging.
