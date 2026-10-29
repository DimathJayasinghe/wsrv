// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "server/loop_shard.hpp"

#include <functional>
#include <future>
#include <memory>
#include <thread>

namespace wsrv {

/// A thread that builds a LoopShard on itself (a loop belongs to the thread that creates it),
/// runs the loop until it is stopped, and then keeps the shard alive until the owner lets go.
/// The wait matters: the owner may still post to the shard's loop while shutting down, and the
/// shard must not disappear under it. Destruction order is therefore: owner stops every loop,
/// then destroys the WorkerThreads (release, join).
///
/// Thread-affinity: start and the destructor on the owner's thread; stop_loop(): any thread.
class WorkerThread {
public:
    /// Start the thread and wait until its shard exists (or failed).
    /// @return the error that kept the shard from being created.
    [[nodiscard]] static Result<std::unique_ptr<WorkerThread>>
    start(const ShardShared& shared, std::function<void()> on_drained);

    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;
    WorkerThread(WorkerThread&&) = delete;
    WorkerThread& operator=(WorkerThread&&) = delete;
    ~WorkerThread();

    /// Valid from start() until the destructor.
    [[nodiscard]] LoopShard& shard() noexcept {
        return *shard_;
    }
    /// End run(). A stop that comes before run() starts is remembered by the loop.
    void stop_loop() noexcept {
        shard_->loop().stop();
    }

    /// Construction goes through start() (passkey).
    class Key {
        friend class WorkerThread;
        Key() = default;
    };
    explicit WorkerThread(Key /*unused*/) {}

private:
    void body(const ShardShared& shared,
              std::function<void()> on_drained,
              std::promise<Result<void>> ready);

    LoopShard* shard_ = nullptr;  // written before `ready` is set, read after it
    std::promise<void> release_;
    std::future<void> released_{release_.get_future()};  // taken here, waited on by the thread
    std::thread thread_;
};

}  // namespace wsrv
