/**********************************************************************
 *  This program is free software; you can redistribute it and/or     *
 *  modify it under the terms of the GNU General Public License       *
 *  as published by the Free Software Foundation; either version 2    *
 *  of the License, or (at your option) any later version.            *
 *                                                                    *
 *  This program is distributed in the hope that it will be useful,   *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of    *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the     *
 *  GNU General Public License for more details.                      *
 **********************************************************************/

// Small bounded producer/consumer queue for the Write and Verify pipelines.
// One decoder thread pushes `Chunk`s (sector-aligned buffer + length + EOF /
// error marker), the main thread pops them and commits to the device (Write)
// or compares against device bytes (Verify). The queue is capped so the
// decoder doesn't race ahead and eat memory on slow SD cards.
//
// Qt-free by design — used by both the GUI (mainwindow.cpp) and the CLI
// (cli_main.cpp). All fields are POD / std.

#ifndef IOPIPELINE_H
#define IOPIPELINE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <malloc.h>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

struct IoChunk {
    char       *data    = nullptr;   // allocated via _aligned_malloc
    std::size_t capacity = 0;
    std::size_t length   = 0;        // valid bytes (may be < capacity at tail)
    bool        eof      = false;
    std::string err;                 // non-empty on producer error

    IoChunk() = default;
    IoChunk(const IoChunk &) = delete;
    IoChunk &operator=(const IoChunk &) = delete;
    ~IoChunk() { if (data) _aligned_free(data); }

    bool allocate(std::size_t cap, std::size_t alignment) {
        data = static_cast<char *>(_aligned_malloc(cap, alignment));
        if (!data) return false;
        capacity = cap;
        return true;
    }
};

class ChunkQueue {
public:
    explicit ChunkQueue(std::size_t maxPending) : m_max(maxPending) {}

    // Producer side. Returns false if the queue was aborted before the push
    // could land; the caller discards the chunk in that case.
    bool push(std::unique_ptr<IoChunk> c) {
        std::unique_lock<std::mutex> lk(m_m);
        m_cv.wait(lk, [&]{ return m_q.size() < m_max || m_abort.load(); });
        if (m_abort.load()) return false;
        m_q.push(std::move(c));
        lk.unlock();
        m_cv.notify_all();
        return true;
    }

    // Consumer side. Returns nullptr iff the queue was aborted with no more
    // chunks pending (the producer will still flush any chunks already sitting
    // in the queue even after abort is requested).
    std::unique_ptr<IoChunk> pop() {
        std::unique_lock<std::mutex> lk(m_m);
        m_cv.wait(lk, [&]{ return !m_q.empty() || m_abort.load(); });
        if (m_q.empty()) return nullptr;
        std::unique_ptr<IoChunk> c = std::move(m_q.front());
        m_q.pop();
        lk.unlock();
        m_cv.notify_all();
        return c;
    }

    // Signal the other side to stop. Producer will return from push() with
    // false; consumer wakes up and sees an empty queue (after draining).
    void requestAbort() {
        m_abort.store(true);
        m_cv.notify_all();
    }

    bool aborted() const { return m_abort.load(); }

private:
    std::size_t                              m_max;
    std::queue<std::unique_ptr<IoChunk>>     m_q;
    std::mutex                               m_m;
    std::condition_variable                  m_cv;
    std::atomic<bool>                        m_abort{false};
};

#endif // IOPIPELINE_H
