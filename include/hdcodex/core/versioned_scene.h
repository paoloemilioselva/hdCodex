#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hdcodex {

struct SceneMesh {
    std::string id;
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    /// Two floats for every triangulated face corner. Keeping UVs in corner
    /// order preserves Hydra face-varying seams without duplicating positions.
    std::vector<float> texcoords;
    /// Three floats for every triangulated face corner. A zero vector requests
    /// the geometric face-normal fallback in the path tracer.
    std::vector<float> normals;
    std::string materialId;
};

struct SceneMaterial {
    std::string id;
    std::array<float, 3> baseColor{0.8F, 0.8F, 0.8F};
    std::array<float, 3> emission{0.0F, 0.0F, 0.0F};
    std::array<float, 3> transmissionColor{1.0F, 1.0F, 1.0F};
    std::array<float, 3> specularColor{1.0F, 1.0F, 1.0F};
    std::array<float, 3> coatColor{1.0F, 1.0F, 1.0F};
    std::array<float, 3> subsurfaceColor{0.8F, 0.8F, 0.8F};
    std::array<float, 3> subsurfaceRadius{1.0F, 0.2F, 0.1F};
    float metalness{0.0F};
    float roughness{0.5F};
    float opacity{1.0F};
    float emissionWeight{0.0F};
    float transmission{0.0F};
    float indexOfRefraction{1.5F};
    float specularWeight{1.0F};
    float coat{0.0F};
    float coatRoughness{0.1F};
    float coatIndexOfRefraction{1.5F};
    float subsurface{0.0F};
    float subsurfaceScale{1.0F};
    bool thinWalled{false};
    std::string baseColorTexture;
    std::string metalnessTexture;
    std::string roughnessTexture;
    std::string emissionTexture;
    std::string opacityTexture;
    std::string normalTexture;
    std::string transmissionTexture;
    std::string specularTexture;
    std::string specularColorTexture;
    std::string coatTexture;
    std::string coatColorTexture;
    std::string coatRoughnessTexture;
    std::string subsurfaceTexture;
    std::string subsurfaceColorTexture;
    std::string subsurfaceRadiusTexture;
};

struct SceneTexture {
    std::string id;
    std::string sourcePath;
    std::uint32_t width{0};
    std::uint32_t height{0};
    bool srgb{false};
    std::vector<std::uint8_t> rgba;
    /// Linear floating-point pixels are used for HDR light textures. Material
    /// textures retain the compact RGBA8 path above.
    std::vector<float> rgbaFloat;
};

struct SceneSnapshot {
    std::uint64_t revision{0};
    std::vector<SceneMesh> meshes;
    std::vector<SceneMaterial> materials;
    std::vector<SceneTexture> textures;
};

/// Tracks staging and published scene revisions without exposing Hydra types to
/// the renderer core. Mutations can arrive concurrently from Hydra Sync calls.
class VersionedScene final {
public:
    using Revision = std::uint64_t;

    [[nodiscard]] Revision MarkDirty() noexcept
    {
        return _staging.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    [[nodiscard]] Revision StagingRevision() const noexcept
    {
        return _staging.load(std::memory_order_acquire);
    }

    [[nodiscard]] Revision PublishedRevision() const noexcept
    {
        return _published.load(std::memory_order_acquire);
    }

    [[nodiscard]] Revision Publish()
    {
        const std::scoped_lock lock(_mutex);
        const Revision revision = StagingRevision();
        auto snapshot = std::make_shared<SceneSnapshot>();
        snapshot->revision = revision;
        snapshot->meshes.reserve(_meshes.size());
        for (const auto& [id, mesh] : _meshes) {
            (void)id;
            snapshot->meshes.push_back(mesh);
        }
        snapshot->materials.reserve(_materials.size());
        for (const auto& [id, material] : _materials) {
            (void)id;
            snapshot->materials.push_back(material);
        }
        snapshot->textures.reserve(_textures.size());
        for (const auto& [id, texture] : _textures) {
            (void)id;
            snapshot->textures.push_back(texture);
        }
        _snapshot = std::move(snapshot);
        _published.store(revision, std::memory_order_release);
        return revision;
    }

    void UpsertMesh(SceneMesh mesh)
    {
        if (mesh.id.empty()) return;
        const std::scoped_lock lock(_mutex);
        _meshes[mesh.id] = std::move(mesh);
        (void)MarkDirty();
    }

    void RemoveMesh(const std::string& id)
    {
        const std::scoped_lock lock(_mutex);
        if (_meshes.erase(id) != 0U) (void)MarkDirty();
    }

    void UpsertMaterial(SceneMaterial material)
    {
        if (material.id.empty()) return;
        const std::scoped_lock lock(_mutex);
        _materials[material.id] = std::move(material);
        (void)MarkDirty();
    }

    void RemoveMaterial(const std::string& id)
    {
        const std::scoped_lock lock(_mutex);
        if (_materials.erase(id) != 0U) (void)MarkDirty();
    }

    [[nodiscard]] bool HasTexture(const std::string& id) const
    {
        const std::scoped_lock lock(_mutex);
        return _textures.contains(id);
    }

    void UpsertTexture(SceneTexture texture)
    {
        const std::size_t expected = static_cast<std::size_t>(texture.width) *
            texture.height * 4U;
        if (texture.id.empty() || texture.width == 0 || texture.height == 0 ||
            (texture.rgba.size() != expected && texture.rgbaFloat.size() != expected)) return;
        const std::scoped_lock lock(_mutex);
        _textures[texture.id] = std::move(texture);
        (void)MarkDirty();
    }

    [[nodiscard]] std::shared_ptr<const SceneSnapshot> Snapshot() const
    {
        const std::scoped_lock lock(_mutex);
        return _snapshot;
    }

private:
    std::atomic<Revision> _staging{0};
    std::atomic<Revision> _published{0};
    mutable std::mutex _mutex;
    std::map<std::string, SceneMesh, std::less<>> _meshes;
    std::map<std::string, SceneMaterial, std::less<>> _materials;
    std::map<std::string, SceneTexture, std::less<>> _textures;
    std::shared_ptr<const SceneSnapshot> _snapshot =
        std::make_shared<const SceneSnapshot>();
};

} // namespace hdcodex
