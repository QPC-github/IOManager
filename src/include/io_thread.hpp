/**
 * Copyright eBay Corporation 2018
 */

#pragma once

#include <sds_logging/logging.h>
#include <pthread.h>
#include <iostream>
#include <folly/MPMCQueue.h>
#include "io_interface.hpp"
#include "iomgr_msg.hpp"
#include "iomgr_timer.hpp"
#include <metrics/metrics.hpp>
#include <chrono>
#include <variant>
#include <boost/heap/binomial_heap.hpp>

SDS_LOGGING_DECL(iomgr);

namespace iomgr {

constexpr size_t MAX_PRI = 10;
using ev_callback = std::function< void(int fd, void* cookie, uint32_t events) >;
typedef std::function< void(const iomgr_msg&) > io_thread_msg_handler;
typedef std::function< void(void) > run_method_t;

struct fd_info;

class IOInterface;
class ioMgrThreadMetrics : public sisl::MetricsGroupWrapper {
public:
    explicit ioMgrThreadMetrics(uint32_t thread_num) :
            sisl::MetricsGroupWrapper("ioMgrThreadMetrics", std::to_string(thread_num)) {
        LOGINFO("Registring metrics group name = ioMgrThreadMetrics, thread_num = {}, inst name = {}", thread_num,
                std::to_string(thread_num));

        REGISTER_GAUGE(iomgr_thread_io_count, "IO Manager per thread IO count");
        REGISTER_GAUGE(iomgr_thread_total_msg_recvd, "Total message received for this thread");
        REGISTER_GAUGE(iomgr_thread_rescheduled_in, "Count of times IOs rescheduled into this thread");
        REGISTER_GAUGE(iomgr_thread_rescheduled_out, "Count of times IOs rescheduled out of this thread");

        register_me_to_farm();

        attach_gather_cb(std::bind(&ioMgrThreadMetrics::on_gather, this));
    }

    ~ioMgrThreadMetrics() { deregister_me_from_farm(); }

    void on_gather() {
        GAUGE_UPDATE(*this, iomgr_thread_io_count, io_count);
        GAUGE_UPDATE(*this, iomgr_thread_total_msg_recvd, msg_recvd_count);
        GAUGE_UPDATE(*this, iomgr_thread_rescheduled_in, rescheduled_in);
        GAUGE_UPDATE(*this, iomgr_thread_rescheduled_out, rescheduled_out);
    }

    uint64_t io_count = 0;
    uint64_t msg_recvd_count = 0;
    uint64_t rescheduled_in = 0;
    uint64_t rescheduled_out = 0;
};

typedef std::function< bool(std::shared_ptr< fd_info >) > fd_selector_t;

class ioMgrThreadContext {
    friend class IOManager;

public:
    ioMgrThreadContext();
    ~ioMgrThreadContext();
    void run(bool is_iomgr_thread = false, const fd_selector_t& fd_selector = nullptr,
             const io_thread_msg_handler& this_thread_msg_handler = nullptr);
    void listen();
    int add_fd_to_thread(std::shared_ptr< fd_info > info);
    int remove_fd_from_thread(std::shared_ptr< fd_info > info);
    bool is_io_thread() const;

    /***
     * Put the message to the message q for this thread.
     * @param msg
     */
    // void put_msg(iomgr_msg&& msg);
    void put_msg(const iomgr_msg& msg);
    void put_msg(iomgr_msg_type type, fd_info* info, int event, void* buf = nullptr, uint32_t size = 0);

private:
    void iothread_init(bool wait_till_ready);
    void iothread_stop();
    void on_msg_fd_notification();
    void on_user_fd_notification(fd_info* info, uint32_t event);
    bool is_fd_addable(std::shared_ptr< fd_info > info);
    void notify_thread_state(bool is_started);
    io_thread_msg_handler& msg_handler();

private:
    int m_epollfd = -1;                       // Parent epoll context for this thread
    int m_thread_num;                         // Thread num
    std::shared_ptr< fd_info > m_msg_fd_info; // fd_info for the message fd
    uint64_t m_count = 0;                     // Count of operations this thread is handling.
    uint64_t m_time_spent_ns = 0;
    bool m_is_io_thread = false;
    bool m_is_iomgr_thread = false; // Is this thread created by iomanager itself
    bool m_keep_running = true;
    fd_selector_t m_fd_selector = nullptr;

    folly::MPMCQueue< iomgr_msg, std::atomic, true > m_msg_q; // Q of message for this thread
    std::unique_ptr< ioMgrThreadMetrics > m_metrics;

    std::unique_ptr< timer > m_thread_timer;
    io_thread_msg_handler m_this_thread_msg_handler;
};
} // namespace iomgr
