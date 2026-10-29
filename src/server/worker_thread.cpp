// SPDX-License-Identifier: Apache-2.0
#include "server/worker_thread.hpp"

#include <new>
#include <system_error>
#include <utility>

namespace wsrv {

Result<std::unique_ptr<WorkerThread>> WorkerThread::start(const ShardShared& shared,
                                                          std::function<void()> on_drained) {
    std::unique_ptr<WorkerThread> worker;
    std::promise<Result<void>> ready;
    std::future<Result<void>> outcome = ready.get_future();
    try {
        worker = std::make_unique<WorkerThread>(Key{});
        WorkerThread& self = *worker;
        self.thread_ = std::thread(
            [&self, &shared, ready = std::move(ready), drained = std::move(on_drained)]() mutable {
                self.body(shared, std::move(drained), std::move(ready));
            });
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "server: out of memory starting a loop thread");
    } catch (const std::system_error&) {
        return fail(ErrorCode::ResourceExhausted, "server: cannot start a loop thread");
    }
    const Result<void> created = outcome.get();
    if (!created) {
        worker->release_.set_value();
        worker->thread_.join();
        return std::unexpected{created.error()};
    }
    return worker;
}

WorkerThread::~WorkerThread() {
    if (thread_.joinable()) {
        release_.set_value();
        thread_.join();
    }
}

void WorkerThread::body(const ShardShared& shared,
                        std::function<void()> on_drained,
                        std::promise<Result<void>> ready) {
    Result<std::unique_ptr<LoopShard>> created = LoopShard::create(shared, std::move(on_drained));
    if (!created) {
        ready.set_value(std::unexpected{created.error()});
        return;
    }
    const std::unique_ptr<LoopShard> shard = std::move(*created);
    shard_ = shard.get();
    ready.set_value({});
    if (const Result<void> ran = shard->loop().run(); !ran) {
        shared.logger.log(LogLevel::Error, "server: a loop stopped with a poller failure");
    }
    released_.wait();  // the owner may still post to this loop until it releases us
}  // the shard is destroyed here, on the thread that created it

}  // namespace wsrv
