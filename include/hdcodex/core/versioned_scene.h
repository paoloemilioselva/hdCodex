#pragma once

#include <atomic>
#include <cstdint>

namespace hdcodex {

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

    [[nodiscard]] Revision Publish() noexcept
    {
        const Revision revision = StagingRevision();
        _published.store(revision, std::memory_order_release);
        return revision;
    }

private:
    std::atomic<Revision> _staging{0};
    std::atomic<Revision> _published{0};
};

} // namespace hdcodex

