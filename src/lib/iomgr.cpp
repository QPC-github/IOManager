//
// Created by Rishabh Mittal on 04/20/2018
//

#include "iomgr.hpp"

extern "C" {
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/types.h>
}

#include <cerrno>
#include <chrono>
#include <ctime>
#include <functional>
#include <vector>
#include <thread>

#include <sds_logging/logging.h>

#include "include/drive_interface.hpp"
#include "include/iomgr.hpp"
#include "include/io_thread.hpp"
#include <utility/thread_factory.hpp>
#include <fds/obj_allocator.hpp>

namespace iomgr {

IOManager::IOManager() { m_iface_list.wlock()->reserve(inbuilt_interface_count + 5); }

IOManager::~IOManager() = default;

void IOManager::start(size_t const expected_custom_ifaces, size_t const num_threads,
                      const io_thread_msg_handler& handler) {
    LOGINFO("Starting IOManager");
    m_expected_ifaces += expected_custom_ifaces;
    m_yet_to_start_nthreads.set(num_threads);
    m_common_thread_msg_handler = handler;

    set_state(iomgr_state::waiting_for_interfaces);

    /* Create all in-built interfaces here.
     * TODO: Can we create aio_drive_end_point by default itself
     * */
    m_default_general_iface = std::make_shared< DefaultIOInterface >();
    add_interface(m_default_general_iface);
}

void IOManager::stop() {
    LOGINFO("Stopping IOManager");
    set_state(iomgr_state::stopping);

    // Increment stopping threads by 1 and then decrement after sending message to prevent case where there are no
    // IO threads, which hangs the iomanager stop
    m_yet_to_stop_nthreads.increment();

    // Send all the threads to reliquish its io thread status
    iomgr_msg msg(iomgr_msg_type::RELINQUISH_IO_THREAD);
    send_msg(-1, std::move(msg));

    // Free up and unregister fds for global timer
    m_global_timer = nullptr;

    // Now decrement and check if all io threads have already reliquished the io thread status.
    if (m_yet_to_stop_nthreads.decrement_testz()) {
        set_state(iomgr_state::stopped);
    } else {
        // Few threads are still in process of coming out io loop, wait for them.
        wait_to_be_stopped();
    }

    LOGINFO("All IO threads have stopped and hence IOManager is moved to stopped state, joining any iomanager threads");
    // Join all the iomanager threads
    for (auto& t : m_iomgr_threads) {
        t.join();
    }

    m_iomgr_threads.clear();
    m_yet_to_start_nthreads.set(0);
    m_expected_ifaces = inbuilt_interface_count;
    m_drive_ifaces.wlock()->clear();
    m_iface_list.wlock()->clear();
    assert(get_state() == iomgr_state::stopped);
}

void IOManager::add_drive_interface(std::shared_ptr< DriveInterface > iface, bool default_iface) {
    add_interface(std::dynamic_pointer_cast< IOInterface >(iface));
    m_drive_ifaces.wlock()->push_back(iface);
    if (default_iface) m_default_drive_iface = iface;
}

void IOManager::add_interface(std::shared_ptr< IOInterface > iface) {
    m_iface_list.wlock()->push_back(iface);

    auto iface_count = m_iface_list.rlock()->size();
    if (iface_count == m_expected_ifaces) {
        LOGINFO("Registered expected {} interfaces, marking iomanager waiting for threads", iface_count);

        auto nthreads = m_yet_to_start_nthreads.get();
        if (nthreads) {
            set_state_and_notify(iomgr_state::waiting_for_threads);
            LOGINFO("IOManager is asked to start {} number of threads, starting them", nthreads);
            for (auto i = 0; i < nthreads; i++) {
                m_iomgr_threads.push_back(std::move(
                    sisl::thread_factory("io_thread", &IOManager::run_io_loop, this, true, nullptr, nullptr)));
                LOGTRACEMOD(iomgr, "Created iomanager thread...", i);
                // t.detach();
            }
        } else {
            set_state_and_notify(iomgr_state::running);
        }
    } else if (iface_count < m_expected_ifaces) {
        LOGINFO("Only added {} interfaces, need to wait till we get {} interfaces registered", iface_count,
                m_expected_ifaces);
    }
}

void IOManager::run_io_loop(bool is_iomgr_thread, const fd_selector_t& fd_selector,
                            const io_thread_msg_handler& override_msg_handler) {
    m_thread_ctx->run(is_iomgr_thread, fd_selector, override_msg_handler);
}

void IOManager::stop_io_loop() { m_thread_ctx->iothread_stop(); }

void IOManager::io_thread_started(bool is_iomgr_thread) {
    m_yet_to_stop_nthreads.increment();
    if (is_iomgr_thread && m_yet_to_start_nthreads.decrement_testz()) { set_state_and_notify(iomgr_state::running); }
}

void IOManager::io_thread_stopped() {
    if (m_yet_to_stop_nthreads.decrement_testz()) { set_state_and_notify(iomgr_state::stopped); }
}

std::shared_ptr< fd_info > IOManager::_add_fd(IOInterface* iface, int fd, ev_callback cb, int iomgr_ev, int pri,
                                              void* cookie, bool is_per_thread_fd) {
    // We can add per thread fd even when iomanager is not ready. However, global fds need IOManager
    // to be initialized, since it has to maintain global map
    if (!is_per_thread_fd && (get_state() != iomgr_state::running)) {
        LOGINFO("IOManager is not ready to add fd {}, will wait for it to be ready", fd);
        wait_to_be_ready();
        LOGINFO("IOManager is ready now, proceed to add fd to the list");
    }

    LOGTRACEMOD(iomgr, "fd {} is requested to add to IOManager, will add it to {} thread(s)", fd,
                (is_per_thread_fd ? "this" : "all"));

    auto finfo = create_fd_info(iface, fd, cb, iomgr_ev, pri, cookie);
    finfo->is_global = !is_per_thread_fd;

    if (is_per_thread_fd) {
        if (m_thread_ctx->is_fd_addable(finfo)) { m_thread_ctx->add_fd_to_thread(finfo); }
    } else {
        m_thread_ctx.access_all_threads([finfo](ioMgrThreadContext* ctx) {
            if (ctx->is_io_thread() && ctx->is_fd_addable(finfo)) { ctx->add_fd_to_thread(finfo); }
        });
        m_fd_info_map.wlock()->insert(std::pair< int, std::shared_ptr< fd_info > >(fd, finfo));
    }
    return finfo;
}

void IOManager::remove_fd(IOInterface* iface, std::shared_ptr< fd_info > info, ioMgrThreadContext* iomgr_ctx) {
    (void)iface;
    auto state = get_state();
    if ((state != iomgr_state::running) && (state != iomgr_state::stopping)) {
        LOGDFATAL("Expected IOManager to be running or stopping state before we receive _remove_fd");
        return;
    }

    if (info->is_global) {
        m_thread_ctx.access_all_threads([info](ioMgrThreadContext* ctx) {
            if (ctx->is_io_thread()) { ctx->remove_fd_from_thread(info); }
        });
        m_fd_info_map.wlock()->erase(info->fd);
    } else {
        iomgr_ctx ? iomgr_ctx->remove_fd_from_thread(info) : m_thread_ctx->remove_fd_from_thread(info);
    }
}

void IOManager::fd_reschedule(int fd, uint32_t event) { fd_reschedule(fd_to_info(fd), event); }

void IOManager::fd_reschedule(fd_info* info, uint32_t event) {
    iomgr_msg msg(iomgr_msg_type::RESCHEDULE, info, event);
    send_to_least_busy_thread(msg);
}

void IOManager::run_in_io_thread(const run_method_t& fn) {
    auto run_method = sisl::ObjectAllocator< run_method_t >::make_object();
    *run_method = fn;

    iomgr_msg msg(iomgr_msg_type::RUN_METHOD, nullptr, -1, (void*)run_method, sizeof(run_method_t));
    send_to_least_busy_thread(msg);
}

#if 0
void IOManager::create_io_thread_and_run(const run_method_t& fn) {
    std::mutex start_mutex;
    std::condition_variable cv;
    bool started = false;
    // auto t = sisl::thread_factory("on_demand_io_thread", [&]() {
    auto t = std::thread([&]() {
        pthread_setname_np(pthread_self(), "on_demand_io_thread");
        {
            std::unique_lock< std::mutex > lk(start_mutex);
            started = true;
        }
        cv.notify_all();
        m_thread_ctx->run(false, nullptr, [](iomgr_msg& msg) {

        });
    });
    t.detach();

    {
        std::unique_lock< std::mutex > lk(start_mutex);
        if (!started) {
            cv.wait(lk, [&] { return started; });
        }
    }
    auto run_method = sisl::ObjectAllocator< run_method_t >::make_object();
    *run_method = fn;
    iomgr_msg msg(iomgr_msg_type::RUN_METHOD, nullptr, -1, (void*)run_method, sizeof(run_method_t));
    send_msg();
}
#endif

void IOManager::send_to_least_busy_thread(const iomgr_msg& msg) {
    bool sent = false;
    do {
        auto min_id = find_least_busy_thread_id();

        // Try to send msg to the thread. send_msg could fail if thread is not alive (i,e between access_all_threads)
        // and next method, thread exits.
        sent = (send_msg(min_id, msg) == 1);
    } while (!sent);
}

int IOManager::find_least_busy_thread_id() {
    uint64_t min_cnt = UINTMAX_MAX;
    int min_id = 0;
    m_thread_ctx.access_all_threads([&min_id, &min_cnt](ioMgrThreadContext* ctx) {
        if (!ctx->is_io_thread()) { return; }
        if (ctx->m_count < min_cnt) {
            min_id = ctx->m_thread_num;
            min_cnt = ctx->m_count;
        }
    });
    return min_id;
}

uint32_t IOManager::send_msg(int thread_num, const iomgr_msg& msg) {
    uint32_t msg_sent_count = 0;
    if (thread_num == -1) {
        m_thread_ctx.access_all_threads([msg, &msg_sent_count](ioMgrThreadContext* ctx) {
            if (!ctx->m_msg_fd_info || !ctx->m_is_io_thread) return;

            LOGTRACEMOD(iomgr, "Sending msg of type {} to local thread msg fd = {}, ptr = {}", msg.m_type,
                        ctx->m_msg_fd_info->fd, (void*)ctx->m_msg_fd_info.get());
            ctx->put_msg(std::move(msg));
            uint64_t temp = 1;
            while (0 > write(ctx->m_msg_fd_info->fd, &temp, sizeof(uint64_t)) && errno == EAGAIN)
                ;
            msg_sent_count++;
        });
    } else {
        m_thread_ctx.access_specific_thread(thread_num, [msg, &msg_sent_count](ioMgrThreadContext* ctx) {
            if (!ctx->m_msg_fd_info || !ctx->m_is_io_thread) return;

            ctx->put_msg(std::move(msg));
            uint64_t temp = 1;
            while (0 > write(ctx->m_msg_fd_info->fd, &temp, sizeof(uint64_t)) && errno == EAGAIN)
                ;
            msg_sent_count++;
        });
    }
    return msg_sent_count;
}

std::shared_ptr< fd_info > IOManager::create_fd_info(IOInterface* iface, int fd, const iomgr::ev_callback& cb, int ev,
                                                     int pri, void* cookie) {
    auto info = std::make_shared< fd_info >();

    info->cb = cb;
    info->is_processing[fd_info::READ] = 0;
    info->is_processing[fd_info::WRITE] = 0;
    info->fd = fd;
    info->ev = ev;
    info->is_global = false;
    info->pri = pri;
    info->cookie = cookie;
    info->io_interface = iface;
    return info;
}

fd_info* IOManager::fd_to_info(int fd) {
    auto it = m_fd_info_map.rlock()->find(fd);
    assert(it->first == fd);
    auto finfo = it->second;

    return finfo.get();
}

void IOManager::foreach_fd_info(std::function< void(std::shared_ptr< fd_info >) > fd_cb) {
    m_fd_info_map.withRLock([&](auto& fd_infos) {
        for (auto& fdi : fd_infos) {
            fd_cb(fdi.second);
        }
    });
}

void IOManager::foreach_interface(std::function< void(IOInterface*) > iface_cb) {
    m_iface_list.withRLock([&](auto& iface_list) {
        for (auto iface : iface_list) {
            iface_cb(iface.get());
        }
    });
}
} // namespace iomgr
