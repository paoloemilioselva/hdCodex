#pragma once

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hdcodex {

struct ShaderCacheKeyInput {
    std::string_view source;
    std::string_view generatorVersion;
    std::string_view compilerVersion;
    std::string_view targetEnvironment;
    std::string_view materialAbi;
    std::span<const std::string> options;
};

/// Builds a deterministic SHA-256 key with length-framed fields.
[[nodiscard]] std::string MakeShaderCacheKey(const ShaderCacheKeyInput& input);

/// Small, process-thread-safe persistent cache for compiled shader blobs.
///
/// The on-disk header contains a magic value, format version, key, and payload
/// length. Writes use a temporary sibling followed by rename, so interrupted
/// compilation never leaves a valid-looking partial entry.
class ShaderCache final {
public:
    explicit ShaderCache(std::filesystem::path root);

    [[nodiscard]] const std::filesystem::path& Root() const noexcept;
    [[nodiscard]] std::optional<std::vector<std::byte>> Load(
        std::string_view key) const;
    void Store(std::string_view key, std::span<const std::byte> payload);
    [[nodiscard]] bool Remove(std::string_view key);

private:
    [[nodiscard]] std::filesystem::path EntryPath(std::string_view key) const;
    static void ValidateKey(std::string_view key);

    std::filesystem::path _root;
    mutable std::mutex _mutex;
};

} // namespace hdcodex

