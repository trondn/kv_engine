/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Thread management for memcached.
 */
#include "config.h"
#include "memcached.h"
#include "connections.h"

#include <atomic>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <fcntl.h>
#include <platform/cb_malloc.h>
#include <platform/platform.h>
#include <platform/strerror.h>
#include <queue>
#include <memory>

#define ITEMS_PER_ALLOC 64

static char devnull[8192];
extern std::atomic<bool> memcached_shutdown;

/* An item in the connection queue. */
struct ConnectionQueueItem {
    ConnectionQueueItem(SOCKET sock, in_port_t port)
        : sfd(sock),
          parent_port(port) {
        // empty
    }

    SOCKET sfd;
    in_port_t parent_port;
};

class ConnectionQueue {
public:
    ~ConnectionQueue() {
        while (!connections.empty()) {
            safe_close(connections.front()->sfd);
            connections.pop();
        }
    }

    std::unique_ptr<ConnectionQueueItem> pop() {
        std::lock_guard<std::mutex> guard(mutex);
        if (connections.empty()) {
            return nullptr;
        }
        std::unique_ptr<ConnectionQueueItem> ret(std::move(connections.front()));
        connections.pop();
        return ret;
    }

    void push(std::unique_ptr<ConnectionQueueItem> &item) {
        std::lock_guard<std::mutex> guard(mutex);
        connections.push(std::move(item));
    }

private:
    std::mutex mutex;
    std::queue< std::unique_ptr<ConnectionQueueItem> > connections;
};


/* Connection lock around accepting new connections */
cb_mutex_t conn_lock;

static LIBEVENT_THREAD dispatcher_thread;

/*
 * Each libevent instance has a wakeup pipe, which other threads
 * can use to signal that they've put a new connection on its queue.
 */
static int nthreads;
static LIBEVENT_THREAD *threads;
static cb_thread_t *thread_ids;
std::vector<TimingHistogram> scheduler_info;

/*
 * Number of worker threads that have finished setting themselves up.
 */
static int init_count = 0;
static cb_mutex_t init_lock;
static cb_cond_t init_cond;

static void thread_libevent_process(evutil_socket_t fd, short which, void *arg);

/*
 * Creates a worker thread.
 */
static void create_worker(void (*func)(void *), void *arg, cb_thread_t *id,
                          const char* name) {
    int ret;

    if ((ret = cb_create_named_thread(id, func, arg, 0, name)) != 0) {
        FATAL_ERROR(EXIT_FAILURE, "Can't create thread %s: %s",
                    name, cb_strerror(GetLastError()).c_str());
    }
}

/****************************** LIBEVENT THREADS *****************************/

bool create_notification_pipe(LIBEVENT_THREAD *me)
{
    int j;

#ifdef WIN32
#define DATATYPE intptr_t
#else
#define DATATYPE int
#endif

    if (evutil_socketpair(SOCKETPAIR_AF, SOCK_STREAM, 0,
                          reinterpret_cast< DATATYPE *>(me->notify)) == SOCKET_ERROR) {
        log_socket_error(EXTENSION_LOG_WARNING, NULL,
                         "Can't create notify pipe: %s");
        return false;
    }

    for (j = 0; j < 2; ++j) {
        int flags = 1;
#if defined(WIN32)
        char* flag_ptr = reinterpret_cast<char*>(&flags);
#else
        void* flag_ptr = reinterpret_cast<void*>(&flags);
#endif
        setsockopt(me->notify[j], IPPROTO_TCP,
                   TCP_NODELAY, flag_ptr, sizeof(flags));
        setsockopt(me->notify[j], SOL_SOCKET,
                   SO_REUSEADDR, flag_ptr, sizeof(flags));


        if (evutil_make_socket_nonblocking(me->notify[j]) == -1) {
            log_socket_error(EXTENSION_LOG_WARNING, NULL,
                             "Failed to enable non-blocking: %s");
            return false;
        }
    }
    return true;
}

static void setup_dispatcher(struct event_base *main_base,
                             void (*dispatcher_callback)(evutil_socket_t, short, void *))
{
    memset(&dispatcher_thread, 0, sizeof(dispatcher_thread));
    dispatcher_thread.type = ThreadType::DISPATCHER;
    dispatcher_thread.base = main_base;
	dispatcher_thread.thread_id = cb_thread_self();
    if (!create_notification_pipe(&dispatcher_thread)) {
        FATAL_ERROR(EXIT_FAILURE, "Unable to create notification pipe");
    }

    /* Listen for notifications from other threads */
    if ((event_assign(&dispatcher_thread.notify_event,
                      dispatcher_thread.base,
                      dispatcher_thread.notify[0],
                      EV_READ | EV_PERSIST,
                      dispatcher_callback,
                      nullptr) == -1) ||
        (event_add(&dispatcher_thread.notify_event, 0) == -1)) {
        FATAL_ERROR(EXIT_FAILURE, "Can't monitor libevent notify pipe");
    }
}

/*
 * Set up a thread's information.
 */
static void setup_thread(LIBEVENT_THREAD *me) {
    me->type = ThreadType::GENERAL;

    if (settings.isStdinListen()) {
        // can't use epoll for stdin listening
        struct event_config *cfg = event_config_new();
        event_config_avoid_method(cfg, "epoll");
        me->base = event_base_new_with_config(cfg);
        event_config_free(cfg);
    } else {
        me->base = event_base_new();
    }

    if (! me->base) {
        FATAL_ERROR(EXIT_FAILURE, "Can't allocate event base");
    }

    /* Listen for notifications from other threads */
    if ((event_assign(&me->notify_event, me->base, me->notify[0],
                      EV_READ | EV_PERSIST,
                      thread_libevent_process,
                      me) == -1) ||
        (event_add(&me->notify_event, 0) == -1)) {
        FATAL_ERROR(EXIT_FAILURE, "Can't monitor libevent notify pipe");
    }

    try {
        me->new_conn_queue = new ConnectionQueue;
    } catch (std::bad_alloc&) {
        FATAL_ERROR(EXIT_FAILURE, "Failed to allocate memory for connection queue");
    }

    cb_mutex_initialize(&me->mutex);

    // Initialize threads' sub-document parser / handler
    me->subdoc_op = subdoc_op_alloc();

    try {
        me->validator = new JSON_checker::Validator();
    } catch (const std::bad_alloc&) {
        FATAL_ERROR(EXIT_FAILURE, "Failed to allocate memory for JSON validator");
    }
}

/*
 * Worker thread: main event loop
 */
static void worker_libevent(void *arg) {
    LIBEVENT_THREAD *me = reinterpret_cast<LIBEVENT_THREAD *>(arg);

    /* Any per-thread setup can happen here; thread_init() will block until
     * all threads have finished initializing.
     */

    cb_mutex_enter(&init_lock);
    init_count++;
    cb_cond_signal(&init_cond);
    cb_mutex_exit(&init_lock);

    event_base_loop(me->base, 0);

    // Event loop exited; cleanup before thread exits.
    ERR_remove_state(0);
}

static int number_of_pending(Connection *c, Connection *list) {
    int rv = 0;
    for (; list; list = list->getNext()) {
        if (list == c) {
            rv ++;
        }
    }
    return rv;
}

static void drain_notification_channel(evutil_socket_t fd)
{
    int nread;
    while ((nread = recv(fd, devnull, sizeof(devnull), 0)) == (int)sizeof(devnull)) {
        /* empty */
    }

    if (nread == -1) {
        log_socket_error(EXTENSION_LOG_WARNING, NULL,
                         "Can't read from libevent pipe: %s");
    }
}

void dispatch_new_connections(LIBEVENT_THREAD* me) {
    std::unique_ptr<ConnectionQueueItem> item;
    while ((item = me->new_conn_queue->pop()) != nullptr) {
        Connection* c = nullptr;
        if (item->sfd == fileno(stdin)) {
            c = conn_pipe_new(item->sfd, me->base, me);
        } else {
            c = conn_new(item->sfd, item->parent_port, me->base, me);
        }
        if (c == nullptr) {
            LOG_WARNING(nullptr, "Failed to dispatch event for socket %ld",
                        long(item->sfd));
            safe_close(item->sfd);
        }
    }
}

/*
 * Processes an incoming "handle a new connection" item. This is called when
 * input arrives on the libevent wakeup pipe.
 */
static void thread_libevent_process(evutil_socket_t fd, short which, void *arg) {
    LIBEVENT_THREAD* me = reinterpret_cast<LIBEVENT_THREAD*>(arg);

    cb_assert(me->type == ThreadType::GENERAL);
    // Start by draining the notification channel before doing any work.
    // By doing so we know that we'll be notified again if someone
    // tries to notify us while we're doing the work below (so we don't have
    // to care about race conditions for stuff people try to notify us
    // about.
    drain_notification_channel(fd);

    if (memcached_shutdown) {
        // Someone requested memcached to shut down. The listen thread should
        // be stopped immediately.
        if (is_listen_thread()) {
            LOG_NOTICE(NULL, "Stopping listen thread (thread.cc)");
            event_base_loopbreak(me->base);
            return;
        }

        if (signal_idle_clients(me, -1, false) == 0) {
            LOG_NOTICE(NULL, "Stopping worker thread %u", me->index);
            event_base_loopbreak(me->base);
            return;
        }
    }

    dispatch_new_connections(me);

    LOCK_THREAD(me);
    Connection* pending = me->pending_io;
    me->pending_io = NULL;
    while (pending != NULL) {
        Connection *c = pending;
        cb_assert(me == c->getThread());
        pending = pending->getNext();
        c->setNext(nullptr);

        auto *mcbp = dynamic_cast<McbpConnection*>(c);
        if (mcbp != nullptr) {
            if (c->getSocketDescriptor() != INVALID_SOCKET &&
                !mcbp->isRegisteredInLibevent()) {
                /* The socket may have been shut down while we're looping */
                /* in delayed shutdown */
                mcbp->registerEvent();
            }

            /*
             * We don't want the thread to keep on serving all of the data
             * from the context of the notification pipe, so just let it
             * run one time to set up the correct mask in libevent
             */
            mcbp->setNumEvents(1);
        }
        run_event_loop(c, EV_READ|EV_WRITE);
    }

    /*
     * I could look at all of the connection objects bound to dying buckets
     */
    if (me->deleting_buckets) {
        notify_thread_bucket_deletion(me);
    }

    if (memcached_shutdown) {
        // Someone requested memcached to shut down. If we don't have
        // any connections bound to this thread we can just shut down
        int connected = signal_idle_clients(me, -1, true);
        if (connected == 0) {
            LOG_NOTICE(NULL, "Stopping worker thread %u", me->index);
            event_base_loopbreak(me->base);
        } else {
            // @todo Change loglevel once MB-16255 is resolved
            LOG_NOTICE(NULL,
                       "Waiting for %d connected clients on worker thread %u",
                       connected, me->index);
        }
    }

    UNLOCK_THREAD(me);
}

extern volatile rel_time_t current_time;

static bool has_cycle(Connection *c) {
    Connection *slowNode, *fastNode1, *fastNode2;

    if (!c) {
        return false;
    }

    slowNode = fastNode1 = fastNode2 = c;
    while (slowNode && (fastNode1 = fastNode2->getNext()) && (fastNode2 = fastNode1->getNext())) {
        if (slowNode == fastNode1 || slowNode == fastNode2) {
            return true;
        }
        slowNode = slowNode->getNext();
    }
    return false;
}

bool list_contains(Connection *haystack, Connection *needle) {
    for (; haystack; haystack = haystack->getNext()) {
        if (needle == haystack) {
            return true;
        }
    }
    return false;
}

Connection * list_remove(Connection *haystack, Connection *needle) {
    if (!haystack) {
        return NULL;
    }

    if (haystack == needle) {
        Connection *rv = needle->getNext();
        needle->setNext(nullptr);
        return rv;
    }

    haystack->setNext(list_remove(haystack->getNext(), needle));

    return haystack;
}

static void enlist_conn(Connection *c, Connection **list) {
    LIBEVENT_THREAD *thr = c->getThread();
    cb_assert(list == &thr->pending_io);
    cb_assert(!list_contains(thr->pending_io, c));
    cb_assert(c->getNext() == nullptr);
    c->setNext(*list);
    *list = c;
    cb_assert(list_contains(*list, c));
    cb_assert(!has_cycle(*list));
}

void notify_io_complete(const void *void_cookie, ENGINE_ERROR_CODE status)
{
    if (void_cookie == nullptr) {
        throw std::logic_error(
            "notify_io_complete: can't be called without cookie");
    }

    auto* cookie = reinterpret_cast<const Cookie*>(void_cookie);
    cookie->validate();

    Connection* connection = cookie->connection;
    if (connection == nullptr) {
        throw std::logic_error(
            "notify_io_complete: can't be called with "
                "connection set to null");
    }

    LIBEVENT_THREAD* thr = connection->getThread();
    if (thr == nullptr) {
        throw std::runtime_error(
            "notify_io_complete: connection should be bound to a thread");
    }

    int notify;

    LOG_DEBUG(NULL, "Got notify from %u, status 0x%x",
              connection->getId(), status);

    LOCK_THREAD(thr);
    reinterpret_cast<McbpConnection*>(connection)->setAiostat(status);
    notify = add_conn_to_pending_io_list(connection);
    UNLOCK_THREAD(thr);

    /* kick the thread in the butt */
    if (notify) {
        notify_thread(thr);
    }
}

/* Which thread we assigned a connection to most recently. */
static int last_thread = -1;

/*
 * Dispatches a new connection to another thread. This is only ever called
 * from the main thread, or because of an incoming connection.
 */
void dispatch_conn_new(SOCKET sfd, int parent_port) {
    int tid = (last_thread + 1) % settings.getNumWorkerThreads();
    LIBEVENT_THREAD* thread = threads + tid;
    last_thread = tid;

    try {
        std::unique_ptr<ConnectionQueueItem> item(
            new ConnectionQueueItem(sfd, parent_port));
        thread->new_conn_queue->push(item);
    } catch (std::bad_alloc& e) {
        LOG_WARNING(nullptr,
                    "dispatch_conn_new: Failed to dispatch new connection: %s",
                    e.what());
        safe_close(sfd);
        return ;
    }

    MEMCACHED_CONN_DISPATCH(sfd, (uintptr_t)thread->thread_id);
    notify_thread(thread);
}

/*
 * Returns true if this is the thread that listens for new TCP connections.
 */
int is_listen_thread() {
    return dispatcher_thread.thread_id == cb_thread_self();
}

void notify_dispatcher(void) {
    notify_thread(&dispatcher_thread);
}

/******************************* GLOBAL STATS ******************************/

void threadlocal_stats_reset(struct thread_stats *thread_stats) {
    for (int ii = 0; ii < settings.getNumWorkerThreads(); ++ii) {
        thread_stats[ii].reset();
    }
}

/*
 * Initializes the thread subsystem, creating various worker threads.
 *
 * nthreads  Number of worker event handler threads to spawn
 * main_base Event base for main thread
 */
void thread_init(int nthr, struct event_base *main_base,
                 void (*dispatcher_callback)(evutil_socket_t, short, void *)) {
    int i;
    nthreads = nthr;

    cb_mutex_initialize(&conn_lock);
    cb_mutex_initialize(&init_lock);
    cb_cond_initialize(&init_cond);

    scheduler_info.resize(nthreads);
    threads = reinterpret_cast<LIBEVENT_THREAD*>(cb_calloc(nthreads,
                                                        sizeof(LIBEVENT_THREAD)));
    if (threads == nullptr) {
        FATAL_ERROR(EXIT_FAILURE, "Can't allocate thread descriptors");
    }
    thread_ids = reinterpret_cast<cb_thread_t*>(cb_calloc(nthreads, sizeof(cb_thread_t)));
    if (thread_ids == nullptr) {
        FATAL_ERROR(EXIT_FAILURE, "Can't allocate thread descriptors");
    }

    setup_dispatcher(main_base, dispatcher_callback);

    for (i = 0; i < nthreads; i++) {
        if (!create_notification_pipe(&threads[i])) {
            FATAL_ERROR(EXIT_FAILURE, "Cannot create notification pipe");
        }
        threads[i].index = i;

        setup_thread(&threads[i]);
    }

    /* Create threads after we've done all the libevent setup. */
    for (i = 0; i < nthreads; i++) {
        std::string name = "mc:worker_" + std::to_string(i);
        create_worker(worker_libevent, &threads[i], &thread_ids[i],
                      name.c_str());
        threads[i].thread_id = thread_ids[i];
    }

    /* Wait for all the threads to set themselves up before returning. */
    cb_mutex_enter(&init_lock);
    while (init_count < nthreads) {
        cb_cond_wait(&init_cond, &init_lock);
    }
    cb_mutex_exit(&init_lock);
}

void threads_shutdown(void)
{
    int ii;
    for (ii = 0; ii < nthreads; ++ii) {
        notify_thread(&threads[ii]);
        cb_join_thread(thread_ids[ii]);
    }
}

void threads_cleanup(void)
{
    int ii;
    for (ii = 0; ii < nthreads; ++ii) {
        safe_close(threads[ii].notify[0]);
        safe_close(threads[ii].notify[1]);
        event_base_free(threads[ii].base);

        cb_free(threads[ii].read.buf);
        threads[ii].write.reset();
        subdoc_op_free(threads[ii].subdoc_op);
        delete threads[ii].validator;
        delete threads[ii].new_conn_queue;
    }

    cb_free(thread_ids);
    cb_free(threads);
}

void threads_notify_bucket_deletion(void)
{
    for (int ii = 0; ii < nthreads; ++ii) {
        LIBEVENT_THREAD *thr = threads + ii;
        notify_thread(thr);
    }
}

void threads_complete_bucket_deletion(void)
{
    for (int ii = 0; ii < nthreads; ++ii) {
        LIBEVENT_THREAD *thr = threads + ii;
        LOCK_THREAD(thr);
        threads[ii].deleting_buckets--;
        UNLOCK_THREAD(thr);
    }
}

void threads_initiate_bucket_deletion(void)
{
    for (int ii = 0; ii < nthreads; ++ii) {
        LIBEVENT_THREAD *thr = threads + ii;
        LOCK_THREAD(thr);
        threads[ii].deleting_buckets++;
        UNLOCK_THREAD(thr);
    }
}

void notify_thread(LIBEVENT_THREAD *thread) {
    if (send(thread->notify[1], "", 1, 0) != 1 &&
            !is_blocking(GetLastNetworkError())) {
        log_socket_error(EXTENSION_LOG_WARNING, NULL,
                         "Failed to notify thread: %s");
    }
}

int add_conn_to_pending_io_list(Connection *c) {
    int notify = 0;
    auto thread = c->getThread();
    if (number_of_pending(c, thread->pending_io) == 0) {
        if (thread->pending_io == NULL) {
            notify = 1;
        }
        enlist_conn(c, &thread->pending_io);
    }

    return notify;
}
