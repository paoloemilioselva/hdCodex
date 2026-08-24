#include "hdcodex/core/shader_cache.h"

#include "hdcodex/core/hash.h"

#include <array>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace hdcodex {
namespace {

constexpr std::string_view Magic = "HDCODEX-SPIRV-CACHE-1";

void AppendFramed(std::string& output, std::string_view value)
{
    output.append(std::to_string(value.size()));
    output.push_back(':');
    output.append(value);
    output.push_back(';');
}

[[nodiscard]] bool IsLowerHex(std::string_view key)
{
    if (key.size() != 64U) {
        return false;
    }
    for (const char value : key) {
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string MakeShaderCacheKey(const ShaderCacheKeyInput& input)
{
    std::string framed;
    framed.reserve(input.source.size() + 256U);
    AppendFramed(framed, input.source);
    AppendFramed(framed, input.generatorVersion);
    AppendFramed(framed, input.compilerVersion);
    AppendFramed(framed, input.targetEnvironment);
    AppendFramed(framed, input.materialAbi);
    AppendFramed(framed, std::to_string(input.options.size()));
    for (const std::string& option : input.options) {
        AppendFramed(framed, option);
    }
    return Sha256(framed);
}

ShaderCache::ShaderCache(std::filesystem::path root)
    : _root(std::move(root))
{
    if (_root.empty()) {
        throw std::invalid_argument("shader cache root must not be empty");
    }
}

const std::filesystem::path& ShaderCache::Root() const noexcept
{
    return _root;
}

std::optional<std::vector<std::byte>> ShaderCache::Load(std::string_view key) const
{
    ValidateKey(key);
    const std::scoped_lock lock(_mutex);
    std::ifstream input(EntryPath(key), std::ios::binary);
    if (!input) {
        return std::nullopt;
    }

    std::string magic;
    std::string storedKey;
    std::string sizeLine;
    if (!std::getline(input, magic) || !std::getline(input, storedKey)
        || !std::getline(input, sizeLine) || magic != Magic || storedKey != key) {
        return std::nullopt;
    }

    std::size_t payloadSize = 0;
    try {
        std::size_t consumed = 0;
        payloadSize = std::stoull(sizeLine, &consumed);
        if (consumed != sizeLine.size()) {
            return std::nullopt;
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }

    std::vector<std::byte> payload(payloadSize);
    input.read(reinterpret_cast<char*>(payload.data()),
               static_cast<std::streamsize>(payload.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(payload.size())) {
        return std::nullopt;
    }
    return payload;
}

void ShaderCache::Store(std::string_view key, std::span<const std::byte> payload)
{
    ValidateKey(key);
    const std::scoped_lock lock(_mutex);

    std::error_code error;
    std::filesystem::create_directories(_root, error);
    if (error) {
        throw std::filesystem::filesystem_error(
            "failed to create shader cache", _root, error);
    }

    const std::filesystem::path destination = EntryPath(key);
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path temporary = destination;
    temporary += ".tmp-" + std::to_string(nonce);

    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("failed to create shader cache entry");
        }
        output << Magic << '\n' << key << '\n' << payload.size() << '\n';
        output.write(reinterpret_cast<const char*>(payload.data()),
                     static_cast<std::streamsize>(payload.size()));
        output.flush();
        if (!output) {
            std::filesystem::remove(temporary, error);
            throw std::runtime_error("failed to write shader cache entry");
        }
    }

    std::filesystem::remove(destination, error);
    error.clear();
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary);
        throw std::filesystem::filesystem_error(
            "failed to publish shader cache entry", destination, error);
    }
}

bool ShaderCache::Remove(std::string_view key)
{
    ValidateKey(key);
    const std::scoped_lock lock(_mutex);
    std::error_code error;
    const bool removed = std::filesystem::remove(EntryPath(key), error);
    if (error) {
        throw std::filesystem::filesystem_error(
            "failed to remove shader cache entry", EntryPath(key), error);
    }
    return removed;
}

std::filesystem::path ShaderCache::EntryPath(std::string_view key) const
{
    return _root / (std::string(key) + ".spvbin");
}

void ShaderCache::ValidateKey(std::string_view key)
{
    if (!IsLowerHex(key)) {
        throw std::invalid_argument("shader cache key must be a lowercase SHA-256 digest");
    }
}

} // namespace hdcodex

