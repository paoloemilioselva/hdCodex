#pragma once

#include <atomic>
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
    std::string materialId;
};

struct SceneSnapshot {
    std::uint64_t revision{0};
    std::vector<SceneMesh> meshes;
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
    std::shared_ptr<const SceneSnapshot> _snapshot =
        std::make_shared<const SceneSnapshot>();
};

} // namespace hdcodex
