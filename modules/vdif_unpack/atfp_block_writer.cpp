#include "rdma_dada/modules/vdif_unpack/atfp_block_writer.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace rdma_dada {
namespace modules {
namespace vdif_unpack {
namespace {

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

bool CheckedMultiply(std::uint64_t left, std::uint64_t right,
                     std::uint64_t* result) {
    if (!result ||
        (left != 0U &&
         right > std::numeric_limits<std::uint64_t>::max() / left)) {
        return false;
    }
    *result = left * right;
    return true;
}

}  // namespace

AtfpBlockWriter::AtfpBlockWriter()
    : block_capacity_(0), sink_(NULL), configured_(false), finished_(false),
      failed_(false), statistics_() {}

bool AtfpBlockWriter::Configure(std::uint64_t block_capacity,
                                pipeline::WritableBlockSink* sink,
                                std::string* error) {
    if (configured_ && !finished_)
        return Fail("cannot reconfigure an active ATFP block writer", error);
    if (!sink) return Fail("ATFP block sink pointer is null", error);
    if (block_capacity == 0U ||
        block_capacity > std::numeric_limits<std::size_t>::max()) {
        return Fail("ATFP block capacity must fit positive host size_t", error);
    }
    block_capacity_ = block_capacity;
    sink_ = sink;
    configured_ = true;
    finished_ = false;
    failed_ = false;
    statistics_ = AtfpBlockWriterStatistics();
    return true;
}

bool AtfpBlockWriter::Write(const AtfpBlockView& view,
                            std::string* error) {
    if (!configured_) return Fail("ATFP block writer is not configured", error);
    if (finished_) return Fail("cannot write after ATFP writer Finish", error);
    if (failed_) return Fail("ATFP block writer is in a failed transfer", error);
    if (!view.window_data)
        return Fail("ATFP window data pointer is null", error);
    if (view.window_capacity_groups == 0U || view.group_count == 0U ||
        view.nant == 0U || view.packet_payload_bytes == 0U) {
        return Fail("ATFP block view extents must be positive", error);
    }
    if (view.first_slot >= view.window_capacity_groups ||
        view.group_count > view.window_capacity_groups) {
        return Fail("ATFP block view exceeds circular window", error);
    }

    std::uint64_t bytes_per_antenna = 0;
    std::uint64_t output_bytes = 0;
    std::uint64_t window_plane_bytes = 0;
    if (!CheckedMultiply(view.group_count, view.packet_payload_bytes,
                         &bytes_per_antenna) ||
        !CheckedMultiply(bytes_per_antenna, view.nant, &output_bytes) ||
        !CheckedMultiply(view.window_capacity_groups,
                         view.packet_payload_bytes, &window_plane_bytes) ||
        output_bytes > block_capacity_) {
        return Fail("ATFP block view byte geometry exceeds capacity", error);
    }

    const std::uint64_t first_groups = std::min(
        view.group_count, view.window_capacity_groups - view.first_slot);
    const std::uint64_t second_groups = view.group_count - first_groups;
    std::uint64_t first_bytes = 0;
    std::uint64_t second_bytes = 0;
    if (!CheckedMultiply(first_groups, view.packet_payload_bytes,
                         &first_bytes) ||
        !CheckedMultiply(second_groups, view.packet_payload_bytes,
                         &second_bytes)) {
        return Fail("ATFP circular segment geometry overflows", error);
    }

    std::uint8_t* block = NULL;
    std::uint64_t acquired_capacity = 0;
    const std::chrono::steady_clock::time_point acquire_begin =
        std::chrono::steady_clock::now();
    ++statistics_.acquire_calls;
    const bool acquired = sink_->Acquire(&block, &acquired_capacity, error);
    const std::chrono::steady_clock::time_point acquire_end =
        std::chrono::steady_clock::now();
    statistics_.acquire_wait_ns += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            acquire_end - acquire_begin).count());
    if (!acquired) {
        failed_ = true;
        return false;
    }
    if (!block || acquired_capacity != block_capacity_) {
        failed_ = true;
        return Fail("acquired ATFP block capacity does not match configuration",
                    error);
    }

    for (std::uint32_t antenna = 0; antenna < view.nant; ++antenna) {
        const std::uint8_t* source_plane =
            view.window_data +
            static_cast<std::size_t>(antenna * window_plane_bytes);
        std::uint8_t* destination_plane =
            block + static_cast<std::size_t>(antenna * bytes_per_antenna);
        std::memcpy(destination_plane,
                    source_plane + static_cast<std::size_t>(
                        view.first_slot * view.packet_payload_bytes),
                    static_cast<std::size_t>(first_bytes));
        if (second_bytes != 0U) {
            std::memcpy(destination_plane + static_cast<std::size_t>(first_bytes),
                        source_plane,
                        static_cast<std::size_t>(second_bytes));
        }
    }

    ++statistics_.commit_calls;
    if (!sink_->Commit(output_bytes, error)) {
        failed_ = true;
        return false;
    }
    ++statistics_.committed_blocks;
    statistics_.committed_bytes += output_bytes;
    return true;
}

bool AtfpBlockWriter::Finish(std::string* error) {
    if (!configured_) return Fail("ATFP block writer is not configured", error);
    if (finished_) return true;
    if (failed_) return Fail("ATFP block writer is in a failed transfer", error);
    finished_ = true;
    return true;
}

const AtfpBlockWriterStatistics& AtfpBlockWriter::statistics() const {
    return statistics_;
}

struct AsyncAtfpBlockWriter::Impl {
    std::uint64_t queue_capacity;
    int writer_cpu;
    AtfpBlockWriter writer;
    std::function<bool(std::uint64_t, std::string*)> release;
    std::vector<AtfpBlockView> queue;
    std::size_t queue_head;
    std::size_t queue_tail;
    std::size_t queue_size;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable not_empty;
    std::condition_variable not_full;
    bool configured;
    bool thread_ready;
    bool stopping;
    bool finished;
    bool failed;
    std::string error;
    AsyncAtfpBlockWriterStatistics statistics;

    Impl()
        : queue_capacity(0), writer_cpu(-1), queue_head(0U), queue_tail(0U),
          queue_size(0U), configured(false), thread_ready(false),
          stopping(false), finished(false), failed(false), statistics() {}

    void Run() {
#if defined(__linux__)
        if (writer_cpu >= 0) {
            cpu_set_t mask;
            CPU_ZERO(&mask);
            CPU_SET(writer_cpu, &mask);
            if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) !=
                0) {
                std::lock_guard<std::mutex> lock(mutex);
                failed = true;
                error = "cannot bind ATFP writer thread CPU";
                stopping = true;
                thread_ready = true;
                not_empty.notify_all();
                not_full.notify_all();
                return;
            }
        }
#endif
        {
            std::lock_guard<std::mutex> lock(mutex);
            thread_ready = true;
            not_empty.notify_all();
        }
        for (;;) {
            AtfpBlockView view = {};
            {
                std::unique_lock<std::mutex> lock(mutex);
                not_empty.wait(lock, [this] {
                    return stopping || queue_size != 0U;
                });
                if (queue_size == 0U) {
                    if (stopping) break;
                    continue;
                }
                view = queue[queue_head];
                queue_head = (queue_head + 1U) % queue.size();
                --queue_size;
                not_full.notify_one();
            }
            std::string write_error;
            bool ok = !failed && writer.Write(view, &write_error);
            std::string release_error;
            if (!release(view.lease_id, &release_error)) {
                ok = false;
                if (write_error.empty()) write_error = release_error;
            }
            if (!ok) {
                std::lock_guard<std::mutex> lock(mutex);
                if (!failed) error = write_error;
                failed = true;
            }
        }
    }
};

AsyncAtfpBlockWriter::AsyncAtfpBlockWriter() : impl_(new Impl) {}
AsyncAtfpBlockWriter::~AsyncAtfpBlockWriter() { Abort(); }

bool AsyncAtfpBlockWriter::Configure(
    std::uint64_t block_capacity, std::uint64_t queue_capacity,
    int writer_cpu, pipeline::WritableBlockSink* sink,
    const std::function<bool(std::uint64_t, std::string*)>& release,
    std::string* error) {
    if (impl_->configured && !impl_->finished)
        return Fail("cannot reconfigure an active async ATFP writer", error);
    if (queue_capacity == 0U || !release)
        return Fail("async ATFP queue and release callback are required", error);
    if (!impl_->writer.Configure(block_capacity, sink, error)) return false;
    impl_->queue_capacity = queue_capacity;
    impl_->writer_cpu = writer_cpu;
    impl_->release = release;
    impl_->queue.assign(static_cast<std::size_t>(queue_capacity),
                        AtfpBlockView());
    impl_->queue_head = 0U;
    impl_->queue_tail = 0U;
    impl_->queue_size = 0U;
    impl_->stopping = false;
    impl_->thread_ready = false;
    impl_->finished = false;
    impl_->failed = false;
    impl_->error.clear();
    impl_->statistics = AsyncAtfpBlockWriterStatistics();
    impl_->configured = true;
    impl_->thread = std::thread(&Impl::Run, impl_.get());
    {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        impl_->not_empty.wait(lock, [this] { return impl_->thread_ready; });
        if (impl_->failed) {
            const std::string failure = impl_->error;
            lock.unlock();
            if (impl_->thread.joinable()) impl_->thread.join();
            impl_->finished = true;
            return Fail(failure, error);
        }
    }
    return true;
}

bool AsyncAtfpBlockWriter::Enqueue(const AtfpBlockView& view,
                                   std::string* error) {
    if (!impl_->configured || impl_->finished)
        return Fail("async ATFP writer is not active", error);
    if (view.lease_id == 0U)
        return Fail("async ATFP block requires a non-zero lease", error);
    const std::chrono::steady_clock::time_point begin =
        std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->not_full.wait(lock, [this] {
        return impl_->failed || impl_->queue_size < impl_->queue_capacity;
    });
    const std::chrono::steady_clock::time_point end =
        std::chrono::steady_clock::now();
    impl_->statistics.enqueue_wait_ns += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
    if (impl_->failed) return Fail(impl_->error, error);
    impl_->queue[impl_->queue_tail] = view;
    impl_->queue_tail = (impl_->queue_tail + 1U) % impl_->queue.size();
    ++impl_->queue_size;
    ++impl_->statistics.enqueued_blocks;
    impl_->statistics.queue_high_watermark = std::max<std::uint64_t>(
        impl_->statistics.queue_high_watermark, impl_->queue_size);
    lock.unlock();
    impl_->not_empty.notify_one();
    return true;
}

bool AsyncAtfpBlockWriter::Finish(std::string* error) {
    if (!impl_->configured)
        return Fail("async ATFP writer is not configured", error);
    if (impl_->finished) return !impl_->failed;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->not_empty.notify_all();
    impl_->not_full.notify_all();
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->finished = true;
    if (impl_->failed) return Fail(impl_->error, error);
    return impl_->writer.Finish(error);
}

void AsyncAtfpBlockWriter::Abort() {
    if (!impl_->configured || impl_->finished) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->not_empty.notify_all();
    impl_->not_full.notify_all();
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->finished = true;
}

const AtfpBlockWriterStatistics&
AsyncAtfpBlockWriter::writer_statistics() const {
    return impl_->writer.statistics();
}

const AsyncAtfpBlockWriterStatistics&
AsyncAtfpBlockWriter::statistics() const {
    return impl_->statistics;
}

}  // namespace vdif_unpack
}  // namespace modules
}  // namespace rdma_dada
