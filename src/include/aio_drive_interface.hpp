//
// Created by Rishabh Mittal 04/20/2018
//
#pragma once

#include <unistd.h>
#include <string>
#include <stack>
#include <atomic>
#include <mutex>
#include "drive_interface.hpp"
#include <metrics/metrics.hpp>

#ifdef linux
#include <fcntl.h>
#include <libaio.h>
#include <sys/eventfd.h>
#include <stdio.h>
#endif

using namespace std;
using Clock = std::chrono::steady_clock;

namespace iomgr {
#define MAX_OUTSTANDING_IO 200               // if max outstanding IO is more than 200 then io_submit will fail.
#define MAX_COMPLETIONS (MAX_OUTSTANDING_IO) // how many completions to process in one shot

#ifdef linux
struct iocb_info : public iocb {
    bool              is_read;
    uint32_t          size;
    uint64_t          offset;
    Clock::time_point start_time;
    int               fd;

    std::string to_string() const {
        std::stringstream ss;
        ss << "is_read = " << is_read << ", size = " << size << ", offset = " << offset << ", fd = " << fd;
        return ss.str();
    }
};

template < typename T, typename Container = std::deque< T > >
class iterable_stack : public std::stack< T, Container > {
    using std::stack< T, Container >::c;

public:
    // expose just the iterators of the underlying container
    auto begin() { return std::begin(c); }
    auto end() { return std::end(c); }

    auto begin() const { return std::begin(c); }
    auto end() const { return std::end(c); }
};

struct fd_info;
class ioMgrThreadContext;
struct aio_thread_context {
    struct io_event                 events[MAX_COMPLETIONS] = {{}};
    int                             ev_fd = 0;
    io_context_t                    ioctx = 0;
    std::stack< struct iocb_info* > iocb_list;
    fd_info*                        ev_fd_info = nullptr; // fd info after registering with IOManager

    ~aio_thread_context() {
        if (ev_fd) { close(ev_fd); }
        io_destroy(ioctx);

        while (!iocb_list.empty()) {
            auto info = iocb_list.top();
            free(info);
            iocb_list.pop();
        }
    }
};

class AioDriveInterfaceMetrics : public sisl::MetricsGroupWrapper {
public:
    explicit AioDriveInterfaceMetrics(const char* inst_name = "AioDriveInterface") :
            sisl::MetricsGroupWrapper("AioDriveInterface", inst_name) {
        REGISTER_COUNTER(spurious_events, "Spurious events count");
        REGISTER_COUNTER(completion_errors, "Aio Completion errors");
        REGISTER_COUNTER(write_io_submission_errors, "Aio write submission errors", "io_submission_errors",
                         {"io_direction", "write"});
        REGISTER_COUNTER(read_io_submission_errors, "Aio read submission errors", "io_submission_errors",
                         {"io_direction", "read"});
        REGISTER_COUNTER(force_sync_io_empty_iocb, "Forced sync io because of empty iocb");
        REGISTER_COUNTER(force_sync_io_eagain_error, "Forced sync io because of EAGAIN error");
        REGISTER_COUNTER(async_write_count, "Aio Drive async write count", "io_count", {"io_direction", "write"});
        REGISTER_COUNTER(async_read_count, "Aio Drive async read count", "io_count", {"io_direction", "read"});
        REGISTER_COUNTER(sync_write_count, "Aio Drive sync write count", "io_count", {"io_direction", "write"});
        REGISTER_COUNTER(sync_read_count, "Aio Drive sync read count", "io_count", {"io_direction", "read"});

        REGISTER_HISTOGRAM(write_io_sizes, "Write IO Sizes", "io_sizes", {"io_direction", "write"},
                           HistogramBucketsType(ExponentialOfTwoBuckets));
        REGISTER_HISTOGRAM(read_io_sizes, "Read IO Sizes", "io_sizes", {"io_direction", "read"},
                           HistogramBucketsType(ExponentialOfTwoBuckets));
        register_me_to_farm();
    }
};

class AioDriveInterface : public DriveInterface {
public:
    AioDriveInterface(const io_interface_comp_cb_t& cb = nullptr);
    drive_interface_type interface_type() const override { return drive_interface_type::aio; }

    void attach_completion_cb(const io_interface_comp_cb_t& cb) override { m_comp_cb = cb; }
    int  open_dev(std::string devname, int oflags) override;
    void add_fd(int fd, int priority = 9) override;
    void sync_write(int data_fd, const char* data, uint32_t size, uint64_t offset) override;
    void sync_writev(int data_fd, const iovec* iov, int iovcnt, uint32_t size, uint64_t offset) override;
    void sync_read(int data_fd, char* data, uint32_t size, uint64_t offset) override;
    void sync_readv(int data_fd, const iovec* iov, int iovcnt, uint32_t size, uint64_t offset) override;
    void async_write(int data_fd, const char* data, uint32_t size, uint64_t offset, uint8_t* cookie) override;
    void async_writev(int data_fd, const iovec* iov, int iovcnt, uint32_t size, uint64_t offset,
                      uint8_t* cookie) override;
    void async_read(int data_fd, char* data, uint32_t size, uint64_t offset, uint8_t* cookie) override;
    void async_readv(int data_fd, const iovec* iov, int iovcnt, uint32_t size, uint64_t offset,
                     uint8_t* cookie) override;
    void process_completions(int fd, void* cookie, int event);
    void on_io_thread_start(ioMgrThreadContext* iomgr_ctx) override;
    void on_io_thread_stopped(ioMgrThreadContext* iomgr_ctx) override;

private:
    static thread_local aio_thread_context* _aio_ctx;
    AioDriveInterfaceMetrics                m_metrics;
    io_interface_comp_cb_t                  m_comp_cb;
};
#else
class AioDriveInterface : public DriveInterface {
public:
    AioDriveInterface(const io_interface_comp_cb_t& cb = nullptr) {}
    void on_io_thread_start(ioMgrThreadContext* iomgr_ctx) override {}
    void on_io_thread_stopped(ioMgrThreadContext* iomgr_ctx) override {}
};
#endif
} // namespace iomgr
