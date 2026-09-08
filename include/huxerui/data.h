#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace huxerui {

/// Owned, contiguous binary storage shared by resource, file, and network APIs.
/// Values may contain embedded nulls or arbitrary non-text bytes; no encoding or terminator is implied.
/// @code{.cpp}
/// Bytes payload{std::byte{0x01}, std::byte{0x00}, std::byte{0xFF}};
/// payload.push_back(std::byte{0x02});
/// @endcode
using Bytes = std::vector<std::byte>;

/// A retained, read-only view of address-stable memory, not an immutable byte snapshot.
/// The owner must keep storage alive without resizing, unmapping, or closing it while any reference survives.
/// Producers must synchronize writes with readers; retaining a reference does not prevent concurrent mutation.
/// Copies retain the same backing state. Equality compares that state and the selected range, never byte contents.
/// Independently wrapping the same address creates distinct references. An empty reference is valid.
/// Nonempty construction requires an owner; invalid storage throws std::invalid_argument.
/// @code{.cpp}
/// auto storage = std::make_shared<Bytes>(4096);
/// BufferReference frame(*storage, storage);
/// FillFrame(*storage);
/// Analyze(frame.AsBytes());
/// // Reuse storage only after all readers finish; do not resize it.
/// @endcode
class BufferReference final {
public:
  BufferReference() noexcept = default;
  BufferReference(std::span<const std::byte> bytes, std::shared_ptr<const void> owner);

  /// Borrows this range without copying. The span does not retain its owner.
  /// @return Read-only bytes, valid while a reference to the backing state remains alive.
  [[nodiscard]] std::span<const std::byte> AsBytes() const noexcept;

  /// Retains a subrange with the same backing identity and lifetime.
  /// @param offset Byte offset relative to this range, including its end for an empty slice.
  /// @param size Number of bytes to include.
  /// @return A zero-copy reference to the selected range.
  /// @throws std::invalid_argument If the range exceeds this reference.
  [[nodiscard]] BufferReference Slice(std::size_t offset, std::size_t size) const;

  bool operator==(const BufferReference& other) const noexcept;

private:
  struct Data;
  std::shared_ptr<const Data> data_;
  std::size_t offset_ = 0;
  std::size_t size_ = 0;
};

/// An owned success value or domain-specific error, without implicit boolean or value conversion.
/// @tparam T Owned success type; use void when success carries no payload.
/// @tparam ErrorT Owned, non-void error type chosen by the operation's domain.
///
/// Check Succeeded() before accessing Value() or Error(); accessing an unavailable branch throws std::logic_error.
/// Success() and Failure() select a branch explicitly, including when T and ErrorT are the same type.
/// References returned by accessors borrow this result and must not outlive its current stored alternative.
/// Exceptions from the stored types are not converted into ErrorT.
///
/// @code{.cpp}
/// Result<std::string, std::string> RequireName(std::string name) {
///   if (name.empty()) {
///     return Result<std::string, std::string>::Failure("Name is required");
///   }
///   return Result<std::string, std::string>::Success(std::move(name));
/// }
/// @endcode
template <class T, class ErrorT> class [[nodiscard]] Result final {
public:
  template <class ValueT = T>
    requires(std::is_same_v<ValueT, T> && !std::is_same_v<T, ErrorT>)
  explicit Result(T value) : value_(std::in_place_index<0>, std::move(value)) {}

  explicit Result(ErrorT error) requires(!std::is_same_v<T, ErrorT>)
      : value_(std::in_place_index<1>, std::move(error)) {}

  /// Selects success explicitly, including when T and ErrorT are identical.
  /// @param value Owned payload to store in the success alternative.
  /// @return A result with Succeeded() equal to true.
  [[nodiscard]] static Result Success(T value) {
    return Result(std::in_place_index<0>, std::move(value));
  }

  /// Selects failure explicitly.
  /// @param error Owned domain error to store, even if its own value is empty.
  /// @return A result with Succeeded() equal to false.
  [[nodiscard]] static Result Failure(ErrorT error) {
    return Result(std::in_place_index<1>, std::move(error));
  }

  /// Tests whether the result represents success without inspecting the payload.
  /// @return True for success, including empty or false payloads; false otherwise.
  [[nodiscard]] bool Succeeded() const noexcept {
    return value_.index() == 0;
  }

  /// Borrows the success payload for inspection or modification.
  /// @return A mutable reference to the value stored in this result.
  /// @throws std::logic_error If the success alternative is unavailable.
  [[nodiscard]] T& Value() & {
    if (auto* value = std::get_if<0>(&value_)) {
      return *value;
    }
    throw std::logic_error("HuxerUI result does not contain a value");
  }

  /// Borrows the success payload without allowing modification.
  /// @return A const reference to the value stored in this result.
  /// @throws std::logic_error If the success alternative is unavailable.
  [[nodiscard]] const T& Value() const& {
    if (const auto* value = std::get_if<0>(&value_)) {
      return *value;
    }
    throw std::logic_error("HuxerUI result does not contain a value");
  }

  /// Allows the caller to extract the success payload with std::move(result).Value().
  /// @return An rvalue reference to the stored value, not a new owned value.
  /// Moving from that reference leaves this result in the success branch with a moved-from payload.
  /// @throws std::logic_error If the success alternative is unavailable.
  [[nodiscard]] T&& Value() && {
    return std::move(static_cast<Result&>(*this).Value());
  }

  /// Borrows the success payload of a const rvalue result.
  /// @return A const rvalue reference; it does not extend this result's lifetime.
  /// @throws std::logic_error If the success alternative is unavailable.
  [[nodiscard]] const T&& Value() const&& {
    return std::move(static_cast<const Result&>(*this).Value());
  }

  /// Borrows the failure payload for inspection or modification.
  /// @return A mutable reference to the error stored in this result.
  /// @throws std::logic_error If the error alternative is unavailable.
  [[nodiscard]] ErrorT& Error() & {
    if (auto* error = std::get_if<1>(&value_)) {
      return *error;
    }
    throw std::logic_error("HuxerUI result does not contain an error");
  }

  /// Borrows the failure payload without allowing modification.
  /// @return A const reference to the error stored in this result.
  /// @throws std::logic_error If the error alternative is unavailable.
  [[nodiscard]] const ErrorT& Error() const& {
    if (const auto* error = std::get_if<1>(&value_)) {
      return *error;
    }
    throw std::logic_error("HuxerUI result does not contain an error");
  }

  /// Allows the caller to extract the error with std::move(result).Error().
  /// @return An rvalue reference to the stored error, not a new owned value.
  /// Moving from that reference leaves this result in the failure branch with a moved-from error.
  /// @throws std::logic_error If the error alternative is unavailable.
  [[nodiscard]] ErrorT&& Error() && {
    return std::move(static_cast<Result&>(*this).Error());
  }

  /// Borrows the failure payload of a const rvalue result.
  /// @return A const rvalue reference; it does not extend this result's lifetime.
  /// @throws std::logic_error If the error alternative is unavailable.
  [[nodiscard]] const ErrorT&& Error() const&& {
    return std::move(static_cast<const Result&>(*this).Error());
  }

private:
  template <std::size_t Index, class ValueT>
  Result(std::in_place_index_t<Index> branch, ValueT&& value) : value_(branch, std::forward<ValueT>(value)) {}

  std::variant<T, ErrorT> value_;
};

/// A result whose successful branch carries no value.
/// @tparam ErrorT Owned, non-void error type chosen by the operation's domain.
/// Success() is explicit; Value() validates success without returning a payload.
/// Error accessors have the same ownership and branch-checking rules as Result<T, ErrorT>.
/// @code{.cpp}
/// Result<void, std::string> CheckReady(bool ready) {
///   if (!ready) {
///     return Result<void, std::string>::Failure("Not ready");
///   }
///   return Result<void, std::string>::Success();
/// }
/// @endcode
template <class ErrorT> class [[nodiscard]] Result<void, ErrorT> final {
public:
  explicit Result(ErrorT error) : error_(std::move(error)) {}

  /// Selects success without a payload.
  /// @return A result with Succeeded() equal to true.
  [[nodiscard]] static Result Success() {
    return Result();
  }

  /// Selects failure explicitly.
  /// @param error Owned domain error to store, even if its own value is empty.
  /// @return A result with Succeeded() equal to false.
  [[nodiscard]] static Result Failure(ErrorT error) {
    return Result(std::move(error));
  }

  /// Tests whether the result represents success without inspecting the payload.
  /// @return True for success, including empty or false payloads; false otherwise.
  [[nodiscard]] bool Succeeded() const noexcept {
    return !error_.has_value();
  }

  /// Validates that this result represents success without returning a payload.
  /// @throws std::logic_error If this result contains an error.
  void Value() const {
    if (error_) {
      throw std::logic_error("HuxerUI result does not contain a value");
    }
  }

  /// Borrows the failure payload for inspection or modification.
  /// @return A mutable reference to the error stored in this result.
  /// @throws std::logic_error If the error alternative is unavailable.
  [[nodiscard]] ErrorT& Error() & {
    if (error_) {
      return *error_;
    }
    throw std::logic_error("HuxerUI result does not contain an error");
  }

  /// Borrows the failure payload without allowing modification.
  /// @return A const reference to the error stored in this result.
  /// @throws std::logic_error If the error alternative is unavailable.
  [[nodiscard]] const ErrorT& Error() const& {
    if (error_) {
      return *error_;
    }
    throw std::logic_error("HuxerUI result does not contain an error");
  }

  /// Allows the caller to extract the error with std::move(result).Error().
  /// @return An rvalue reference to the stored error, not a new owned value.
  /// Moving from that reference leaves this result in the failure branch with a moved-from error.
  /// @throws std::logic_error If the error alternative is unavailable.
  [[nodiscard]] ErrorT&& Error() && {
    return std::move(static_cast<Result&>(*this).Error());
  }

  /// Borrows the failure payload of a const rvalue result.
  /// @return A const rvalue reference; it does not extend this result's lifetime.
  /// @throws std::logic_error If the error alternative is unavailable.
  [[nodiscard]] const ErrorT&& Error() const&& {
    return std::move(static_cast<const Result&>(*this).Error());
  }

private:
  Result() = default;

  std::optional<ErrorT> error_;
};

/// An owned absolute URI with lexical component access and no I/O.
/// Generic ASCII URI syntax and percent escapes are validated; scheme-specific semantics remain the caller's job.
/// Relative references and raw Unicode IRIs are unsupported. Scheme case, escapes, and dot segments are preserved.
/// Component views and ToString() borrow this object; use them only while its storage remains unchanged and alive.
/// @code{.cpp}
/// auto uri = Uri::Parse("https://example.test/a%20b?download#details");
/// if (uri) {
///   std::string path(uri->Path()); // "/a%20b", still percent-encoded.
///   std::string query(uri->Query().value_or(""));
/// }
/// @endcode
class Uri final {
public:
  explicit Uri(std::string value);

  Uri(const Uri&) = default;
  Uri(Uri&&) noexcept = default;
  Uri& operator=(const Uri&) = default;
  Uri& operator=(Uri&&) noexcept = default;

  /// Validates and copies an external URI without throwing for malformed syntax.
  /// @param value Absolute ASCII serialization, including a scheme; the input need only live for this call.
  /// @return An owned URI, or std::nullopt for invalid generic URI syntax.
  /// Allocation failures can still propagate; successful parsing does not grant access to the referenced resource.
  [[nodiscard]] static std::optional<Uri> Parse(std::string_view value);

  /// Returns the stored scheme without the trailing colon.
  /// @return A borrowed view preserving the input's spelling and case.
  [[nodiscard]] std::string_view Scheme() const noexcept;
  /// Returns the unsplit authority without its leading "//".
  /// @return A borrowed view of any user-info, host, and port, or std::nullopt if no authority delimiter was present.
  /// A present empty authority remains distinct from an absent authority.
  [[nodiscard]] std::optional<std::string_view> Authority() const noexcept;
  /// Returns the stored path without decoding percent escapes or removing dot segments.
  /// @return A borrowed, possibly empty view, excluding the query and fragment delimiters.
  [[nodiscard]] std::string_view Path() const noexcept;
  /// Returns the raw query without splitting fields, decoding escapes, or interpreting '+'.
  /// @return A borrowed view excluding '?', or std::nullopt if '?' was absent.
  /// A URI ending in '?' has a present empty query.
  [[nodiscard]] std::optional<std::string_view> Query() const noexcept;
  /// Returns the raw fragment without interpreting its application-specific meaning.
  /// @return A borrowed view excluding '#', or std::nullopt if '#' was absent.
  /// A URI ending in '#' has a present empty fragment.
  [[nodiscard]] std::optional<std::string_view> Fragment() const noexcept;

  /// Returns the original validated serialization without normalization.
  /// @return A reference to this URI's owned string, not a reconstructed or decoded value.
  [[nodiscard]] const std::string& ToString() const noexcept;
  [[nodiscard]] bool operator==(const Uri& other) const noexcept;

private:
  struct Range {
    std::size_t offset = 0;
    std::size_t length = 0;
  };

  void ParseValue();
  [[nodiscard]] std::string_view View(Range range) const noexcept;

  std::string value_;
  Range scheme_;
  std::optional<Range> authority_;
  Range path_;
  std::optional<Range> query_;
  std::optional<Range> fragment_;
};

} // namespace huxerui
