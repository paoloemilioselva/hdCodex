#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace hdcodex {

/// Returns the lowercase hexadecimal SHA-256 digest of the supplied bytes.
[[nodiscard]] std::string Sha256(std::span<const std::byte> bytes);

/// Convenience overload for UTF-8/source strings.
[[nodiscard]] std::string Sha256(std::string_view text);

} // namespace hdcodex

