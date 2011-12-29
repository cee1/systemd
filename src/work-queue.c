/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2011 Chen Jie

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "macro.h"
#include "list.h"
#include "log.h"
#include "work-queue.h"

#define DEFAULT_WORK_QUEUE_WIDTH   32
#define DEFAULT_WORK_QUEUE_MAXSIZE 65535

typedef struct WorkItem WorkItem;
struct WorkItem {
        WorkFunc work;
        WorkDoneNotify notify;
        WorkCancelFunc cancel;
        void *data;
        WorkItem *next;
};

struct WorkQueueVTable {
        int (*prepare_executive) (void *);
        int (*signal_executive) (void *);
};

struct WorkQueue {
        const char *name;
        int xref_cnt; /* External reference count */
        const struct WorkQueueVTable *vtable;

        pthread_mutex_t mutex;
        int maxsize;
        int n_items;
        int n_running;
        int width;

        WorkItem *head;
        WorkItem *tail;

        union {
                struct {
                        pthread_cond_t enqueue;
                        int n_threads;
                } q;
        } data;

        /* item used for barrier/stop */
        WorkItem item;
        WorkQueueDestroyNotify destroy_notify;
        void *context;
        LIST_FIELDS(WorkQueue, q_or_b);
};

static struct {
        WorkQueue concurrent_queue;
        WorkQueue main_queue;
        LIST_HEAD(WorkQueue, private_queues);
        LIST_HEAD(WorkQueue, buckets);

        bool initialized;
        pthread_condattr_t cond_attr;
        pthread_attr_t thread_attr;
} g;

static int _work_queue_add_unlocked(WorkQueue *q, WorkItem *item,
                                    bool do_rewind, bool impose_maxsize);
static int work_queue_run_unlocked(WorkQueue *q, usec_t timeout);
static void *work_queue_thread(void *data);

inline static WorkItem *_work_queue_drain_one_unlocked(WorkQueue *q) {
        WorkItem *item;

        if (!(item = q->head))
                return NULL;

        if (!(q->head = q->head->next)) {
                q->tail = NULL;
                assert(q->n_items == 1);
        }
        q->n_items--;

        item->next = NULL;

        return item;
}

inline static void _work_queue_add_tail_unlocked(WorkQueue *q, WorkItem *item) {
        item->next = NULL;

        if (q->tail)
                q->tail->next = item;
        else
                q->head = item;

        q->tail = item;
        q->n_items++;
}

inline static WorkItem *
_work_queue_unlink_one_unlocked(WorkQueue *q, WorkItem **link_start) {
        WorkItem *item = *link_start;

        if (item) {
                *link_start = item->next;
                item->next = NULL;

                q->n_items--;

                /* There is only one reference to an item in the queue,
                 * except for the tail item, in which case,
                 * both the link (the next field of previous item or q->head)
                 * and q->tail reference it*/
                if (q->tail == item)
                        q->tail = *link_start;
        }

        return item;
}

inline static int _work_queue_add(WorkQueue *q, WorkItem *item,
                                  bool do_rewind, bool impose_maxsize) {
        int r;
        assert(pthread_mutex_lock(&q->mutex) == 0);
        r = _work_queue_add_unlocked(q, item, do_rewind, impose_maxsize);
        assert(pthread_mutex_unlock(&q->mutex) == 0);

        return r;
}

inline static int _more_thread_unlocked(WorkQueue *q) {
        bool more_thread;
        int n_threads;

        n_threads = q->data.q.n_threads;
        more_thread = n_threads < q->width &&
                      /* n idle threads < n items to process */
                      n_threads - q->n_running < q->n_items+1;

        if (n_threads == 0 || more_thread) {
                pthread_t t;
                int r;

                if ((r = -pthread_create(&t, &g.thread_attr,
                                         work_queue_thread, (void *)q)) < 0) {
                        if (n_threads == 0)
                                return r;
                        return 0;
                }
                q->data.q.n_threads++;
        }

        return 1;
}

static void __wq_stop(void *data) {}
static void __wq_barrier(void *data) {}
static void __wq_run_bucket(void *data);

inline static bool work_item_is_static(const WorkItem *item) {
        return item->work == __wq_stop ||
               item->work == __wq_barrier;
}

inline static bool work_item_is_stop(const WorkItem *item) {
        return item->work == __wq_stop;
}

inline static bool work_item_is_barrier(const WorkItem *item) {
        return item->work == __wq_barrier;
}

static int prepare_executive_queue(void *data) {
        WorkQueue *q = (WorkQueue *)data;
        return _more_thread_unlocked(q);
}

static int signal_executive_queue(void *data) {
        WorkQueue *q = (WorkQueue *)data;
        return pthread_cond_signal(&q->data.q.enqueue);
}

static const struct WorkQueueVTable queue_vtable = {
        .prepare_executive = prepare_executive_queue,
        .signal_executive = signal_executive_queue
};

static int prepare_executive_bucket(void *data) {
        WorkQueue *b = (WorkQueue *)data;
        WorkItem *exc_item;
        int r;

        if (b->n_running >= b->width)
                return 0;

        if (!(exc_item = new0(WorkItem, 1)))
                return -ENOMEM;

        exc_item->work = __wq_run_bucket;
        exc_item->data = (void *)b;
        if ((r = _work_queue_add(&g.concurrent_queue, exc_item,
                                 false, false)) < 0) {
                free(exc_item);
                return r;
        }

        b->n_running++;
        return 0;
}

static const struct WorkQueueVTable bucket_vtable = {
        .prepare_executive = prepare_executive_bucket,
};

static int _work_queue_init(WorkQueue *q, const struct WorkQueueVTable *vtable) {
        int r = 0;

        zero(*q);
        if ((r = pthread_mutex_init(&q->mutex, NULL)))
                return -r;

        if (vtable == &queue_vtable) {
                if ((r = pthread_cond_init(&q->data.q.enqueue,
                                           &g.cond_attr))) {
                        pthread_mutex_destroy(&q->mutex);
                        return -r;
                }
        }

        q->vtable = vtable;
        q->xref_cnt = 1;
        LIST_INIT(WorkQueue, q_or_b, q);

        return 0;
}

static void _work_queue_uninit(WorkQueue *q) {
        if (q->vtable == &queue_vtable)
                pthread_cond_destroy(&q->data.q.enqueue);
        pthread_mutex_destroy(&q->mutex);
        zero(*q);
}

static int init_global_once(void) {
        int r;

        if (g.initialized)
                return 0;

        assert(pthread_attr_init(&g.thread_attr) == 0); /* Always success on Linux */
        assert(pthread_attr_setdetachstate(&g.thread_attr, PTHREAD_CREATE_DETACHED) == 0);

        assert(pthread_condattr_init(&g.cond_attr) == 0);
        assert(pthread_condattr_setclock(&g.cond_attr, CLOCK_MONOTONIC) == 0);

        if ((r = _work_queue_init(&g.concurrent_queue, &queue_vtable)) < 0)
                goto fail;

        if ((r = _work_queue_init(&g.main_queue, &queue_vtable)) < 0) {
                _work_queue_uninit(&g.concurrent_queue);
                goto fail;
        }

        g.concurrent_queue.name = "concurrent-queue";
        g.concurrent_queue.maxsize = DEFAULT_WORK_QUEUE_MAXSIZE;
        g.concurrent_queue.width = DEFAULT_WORK_QUEUE_WIDTH;

        g.main_queue.name = "main-queue";
        g.main_queue.maxsize = DEFAULT_WORK_QUEUE_MAXSIZE;
        g.main_queue.width = 1;
        g.main_queue.data.q.n_threads = 1;

        LIST_HEAD_INIT(WorkQueue, g.private_queues);
        LIST_HEAD_INIT(WorkQueue, g.buckets);
        g.initialized =true;

        return 0;

fail:
        pthread_attr_destroy(&g.thread_attr);
        pthread_condattr_destroy(&g.cond_attr);

        return r;
}

WorkQueue *work_bucket_new(const char *name,
                           int maxsize, int width) {
        WorkQueue *b = NULL;
        int r = 0;

        assert(maxsize > 0);
        assert(width > 0);

        if (!(b = new(WorkQueue, 1))) {
                r = -ENOMEM;
                goto fail;
        }

        if ((r = init_global_once()) < 0)
                goto fail;

        if ((r = _work_queue_init(b, &bucket_vtable)) < 0)
                goto fail;

        b->name = name;
        b->maxsize = maxsize;
        b->width = width;

        LIST_PREPEND(WorkQueue, q_or_b, g.buckets, b);

        errno = 0;
        return b;

fail:
        free(b);
        errno = -r;
        return NULL;

}

WorkQueue *work_queue_new(const char *name, int maxsize) {
        WorkQueue *q = NULL;
        int r = 0;

        assert(maxsize > 0);

        if (!(q = new(WorkQueue, 1))) {
                r = -ENOMEM;
                goto fail;
        }

        if ((r = init_global_once()) < 0)
                goto fail;

        if ((r = _work_queue_init(q, &queue_vtable)) < 0)
                goto fail;

        q->name = name;
        q->maxsize = maxsize;
        q->width = 1;

        LIST_PREPEND(WorkQueue, q_or_b, g.private_queues, q);

        errno = 0;
        return q;

fail:
        free(q);
        errno = -r;
        return NULL;
}

void work_queue_set_destroy_notify(WorkQueue *q,
                                   WorkQueueDestroyNotify destroy_notify,
                                   void *context) {
        work_queue_ref(q);
        q->destroy_notify = destroy_notify;
        q->context = context;
        work_queue_unref(q);
}

static void _work_queue_free(WorkQueue *q) {
        WorkQueueDestroyNotify destroy_notify = q->destroy_notify;
        void *context = q->context;

        assert(q != &g.concurrent_queue && q != &g.main_queue);
        assert(q->xref_cnt == 0);

        assert(pthread_mutex_lock(&q->mutex) == 0);
        /* Here: the q or b was stopped
         * For queue: stop means the last thread of it exited
         * For bucket: stop means threads of concurrent queue no longer touch it */
        if (q->vtable == &queue_vtable)
                assert(q->data.q.n_threads == 0);
        else if (q->vtable == &bucket_vtable)
                assert(q->n_running == 0);
        assert(pthread_mutex_unlock(&q->mutex) == 0);

        if (q->vtable == &queue_vtable)
                LIST_REMOVE(WorkQueue, q_or_b, g.private_queues, q);
        else if (q->vtable == &bucket_vtable)
                LIST_REMOVE(WorkQueue, q_or_b, g.buckets, q);

        _work_queue_uninit(q);
        free(q);

        if (destroy_notify)
                destroy_notify(context);
}

WorkQueue *work_queue_ref(WorkQueue *q) {
        if (!q || q == &g.concurrent_queue || q == &g.main_queue)
                return q;

        assert(q->xref_cnt++ > 0);
        return q;
}

void work_queue_unref(WorkQueue *q) {
        if (!q || q == &g.concurrent_queue || q == &g.main_queue)
                return;

        assert(q->xref_cnt-- > 0);

        if (q->xref_cnt == 0) {
                /* xref_cnt is zero, it should have no barrier/stop in processing */
                assert(q->item.work == NULL);

                /* The external reference count reaches zero, that means
                 * it won't add new items to this q in notifiers of running WorkItems
                 *
                 * let quit all work threads, and do free in main queue */
                q->item.work = __wq_stop;
                q->item.data = q;
                q->item.notify = (WorkDoneNotify)_work_queue_free;
                q->item.next = NULL;

                _work_queue_add(q, &q->item, false, false);
        }
}

static void set_bool(bool *val) {
        *val = true;
}

int work_queue_flush(WorkQueue *q) {
        bool barrier_reached;
        int r = 0;

        if (q == NULL)
                q = &g.concurrent_queue;

        /*
         * Flush main_queue makes no sense
         * And can lead to deadlock if was in stack of work_queue_run_main_queue
         */
        assert(q != &g.main_queue);

        work_queue_ref(q);

        /* assert: no barrier/stop in processing */
        assert(q->item.work == NULL);
        q->item.work = __wq_barrier;
        q->item.data = &barrier_reached;
        q->item.notify = (WorkDoneNotify)set_bool;

        while (true) {
                assert(pthread_mutex_lock(&q->mutex) == 0);
                if (q->n_running == 0) {
                        /* no running items in this moment, the queue is empty */
                        r = 0;
                        break;
                }
                if ((r = _work_queue_add_unlocked(q, &q->item, false, false)) < 0)
                        break;
                assert(pthread_mutex_unlock(&q->mutex) == 0);

                for (barrier_reached = false; !barrier_reached;) {
                        work_queue_run_main_queue(-1);
                }
        }
        assert(pthread_mutex_unlock(&q->mutex) == 0);
        zero(q->item);

        work_queue_unref(q);

        return r;
}

int work_queue_stop(WorkQueue *q) {
        bool stopped;
        int r = 0;

        if (q == NULL)
                q = &g.concurrent_queue;

        /* stop main_queue makes no sense */
        assert(q != &g.main_queue);

        work_queue_ref(q);
        if ((r = work_queue_flush(q)) < 0)
                goto out;

        if (q->vtable == &queue_vtable) {
                /* assert: no barrier/stop in processing */
                assert(q->item.work == NULL);
                q->item.work = __wq_stop;
                q->item.data = &stopped;
                q->item.notify = (WorkDoneNotify)set_bool;

                assert(pthread_mutex_lock(&q->mutex) == 0);
                if (q->data.q.n_threads == 0) { /* Already stopped ? */
                        assert(pthread_mutex_unlock(&q->mutex) == 0);
                        goto out;
                }

                if ((r = _work_queue_add_unlocked(q, &q->item, false, false)) < 0) {
                        assert(pthread_mutex_unlock(&q->mutex) == 0);
                        goto out;
                }
                assert(pthread_mutex_unlock(&q->mutex) == 0);

                for (stopped = false; !stopped;) {
                        work_queue_run_main_queue(-1);
                }
                zero(q->item);
        }

out:
        work_queue_unref(q);

        return r;
}

int work_queue_add(WorkQueue *q, WorkFunc work,
                   WorkDoneNotify notify, WorkCancelFunc cancel,
                   void *data) {
        WorkItem *item;
        int r = 0;

        assert(work);
        if (!q) {
                if ((r = init_global_once()) < 0)
                        return r;
                q = &g.concurrent_queue;
        }

        work_queue_ref(q);

        if (!(item = new0(WorkItem, 1)))
                r = -ENOMEM;
        else {
                item->work = work;
                item->cancel = cancel;
                item->notify = notify;
                item->data = data;

                if ((r = _work_queue_add(q, item, false, true)) < 0)
                        free(item);
        }

        work_queue_unref(q);
        return r;
}

int work_queue_add_rewind(WorkQueue *q, WorkFunc work,
                          WorkDoneNotify notify, WorkCancelFunc cancel,
                          void *data) {
        WorkItem *item;
        int r = 0;

        assert(work);
        assert(q != NULL && q != &g.concurrent_queue && q != &g.main_queue);

        work_queue_ref(q);

        if (!(item = new0(WorkItem, 1)))
                r = -ENOMEM;
        else {
                item->work = work;
                item->notify = notify;
                item->cancel = cancel;
                item->data = data;

                if ((r = _work_queue_add(q, item, true, true)) < 0)
                        free(item);
        }

        work_queue_unref(q);
        return r;
}

int work_queue_run_main_queue(usec_t timeout) {
        int r;

        assert(pthread_mutex_lock(&g.main_queue.mutex) == 0);
        r = work_queue_run_unlocked(&g.main_queue, timeout);
        assert(pthread_mutex_unlock(&g.main_queue.mutex) == 0);

        return r;
}

static void *work_queue_thread(void *data) {
        WorkQueue *q = (WorkQueue *)data;
        int r = 0;

        assert(q->vtable == &queue_vtable);
        assert(pthread_mutex_lock(&q->mutex) == 0);

        do {
                r = work_queue_run_unlocked(q, 65 * USEC_PER_SEC);
        } while (r != -ECANCELED && r != -EAGAIN);

        q->data.q.n_threads--;
        assert(pthread_mutex_unlock(&q->mutex) == 0);

        return NULL;
}

static int _work_queue_add_unlocked(WorkQueue *q, WorkItem *item,
                                    bool do_rewind, bool impose_maxsize) {
        int r;
        bool full;

        full = impose_maxsize && q->n_items >= q->maxsize;

        if (full && !do_rewind)
                return -EAGAIN;

        if (q->vtable->prepare_executive &&
            (r = q->vtable->prepare_executive((void *)q)) < 0)
                return r;

        r = 0;
        if (full) {
                WorkItem **l;
                WorkItem *tmp;

                /* try to remove a normal item */
                for (l = &q->head;
                     *l && work_item_is_static(*l);
                     l = &(*l)->next) {}

                tmp = _work_queue_unlink_one_unlocked(q, l);
                if (tmp->cancel)
                        tmp->cancel(tmp->data);
                free(tmp);
                r = tmp ? 1 : 0;
        }

        _work_queue_add_tail_unlocked(q, item);

        if (q->vtable->signal_executive)
                assert(q->vtable->signal_executive((void *)q) == 0);

        return r;
}

static void work_item_execute(WorkItem *item, bool do_notify) {
        bool need_free = !work_item_is_static(item);

        if (do_notify) {
                item->notify(item->data);
        } else {
                item->work(item->data);

                if (item->notify) {
                        /* Send to main queue with the item untouched.
                         * Always successful for main queue */
                        assert(_work_queue_add(&g.main_queue, item, false, false) == 0);
                        return;
                }
        }

        if (need_free)
                free(item);
}

/*
 * Process an item in the queue.
 * It may block if timeout specified and the queue is empty or received a barrier.
 * Returns with q locked:
 *           0             Process one or more items
 *          -EAGAIN        No items in the queue
 *          -EBUSY         A barrier was received, stops us draining items after barrier
 *          -ECANCELED     A stop was received
 */
static int work_queue_run_unlocked(WorkQueue *q, usec_t timeout) {
        int r;
        WorkItem *item;
        struct timespec ts;
        bool is_main_queue = (q == &g.main_queue);

        while (q->head == NULL ||
               (work_item_is_barrier(q->head) &&
                q->n_running > 0)) {

                if (timeout == (usec_t) -1) /* wait cond forever */
                        assert(pthread_cond_wait(&q->data.q.enqueue, &q->mutex) == 0);
                else if (timeout != 0) { /* do timed wait */
                        timespec_store(&ts, now(CLOCK_MONOTONIC) + timeout);
                        r = pthread_cond_timedwait(&q->data.q.enqueue, &q->mutex, &ts);
                        assert(r == 0 || r == ETIMEDOUT);
                }

                if (q->head == NULL)
                        return -EAGAIN;
                if (work_item_is_barrier(q->head) && q->n_running > 0)
                        return -EBUSY;
        }

        /* Here: handle stop/barrier workitem without leaving lock protected area
         * to avoid some race conditions with _work_queue_free/work_queue_flush */
        if (work_item_is_stop(q->head)) {
                if (q->data.q.n_threads > 1)
                        pthread_cond_signal(&q->data.q.enqueue);
                else
                        work_item_execute(
                          _work_queue_drain_one_unlocked(q),
                          is_main_queue);

                return -ECANCELED;
        }

        if (work_item_is_barrier(q->head)) {
                work_item_execute(
                  _work_queue_drain_one_unlocked(q),
                  is_main_queue);
                return 0;
        }

        item = _work_queue_drain_one_unlocked(q);

        q->n_running++;
        assert(pthread_mutex_unlock(&q->mutex) == 0);

        work_item_execute(item, is_main_queue);

        /* Return to the locked state */
        assert(pthread_mutex_lock(&q->mutex) == 0);
        q->n_running--;

        return 0;
}

static void __wq_run_bucket(void *data) {
        WorkQueue *b = (WorkQueue *)data;
        WorkItem *item = NULL;
        bool stop = false;

        assert(pthread_mutex_lock(&b->mutex) == 0);
        while (b->head && !stop) {
                /* Here: handle stop/barrier workitem
                 * without leaving lock protected area
                 * to avoid some race conditions
                 * with _work_queue_free/work_queue_flush */
                if (work_item_is_stop(b->head) ||
                    work_item_is_barrier(b->head)) {
                        if (b->n_running == 1)
                                work_item_execute(
                                  _work_queue_drain_one_unlocked(b),
                                  false);
                        break;
                }

                item = _work_queue_drain_one_unlocked(b);
                assert(pthread_mutex_unlock(&b->mutex) == 0);

                stop = work_item_is_stop(item);
                work_item_execute(item, false);
                assert(pthread_mutex_lock(&b->mutex) == 0);
        }

        b->n_running--;
        assert(pthread_mutex_unlock(&b->mutex) == 0);
}

void work_queue_dump(WorkQueue *q, FILE *f) {
        char buf[2048];
        char span[100];
        int r;
        usec_t ts1, ts2;

        if ((r = init_global_once()) < 0)
                return;

        if (q == NULL)
                q = &g.concurrent_queue;
        else if (q == (WorkQueue *)1)
                q = &g.main_queue;

        ts1 = now(CLOCK_MONOTONIC);
        assert(pthread_mutex_lock(&q->mutex) == 0);

        if (q->vtable == &queue_vtable)
                snprintf(buf, sizeof(buf), "Queue '%s'<%p>\n"
                         ".xref_cnt\t=\t%d\n"
                         ".maxsize\t=\t%d\n"
                         ".n_items\t=\t%d\n"
                         ".n_running\t=\t%d\n"
                         ".width\t=\t%d\n"
                         ".n_threads\t=\t%d\n"
                         ".head\t=\t%p\n"
                         ".tail\t=\t%p\n"
                         ".context\t=\t%p\n"
                         ".destroy_notify\t=\t%p\n"
                         ".item\t=\t%s",
                         q->name ? : "No Name", q,
                         q->xref_cnt, q->maxsize, q->n_items, q->n_running,
                         q->width, q->data.q.n_threads, q->head, q->tail,
                         q->context, q->destroy_notify,
                         q->item.work == NULL ? "<none>" :
                         q->item.work == __wq_barrier ? "<flush>" :
                         q->item.work == __wq_stop ?
                           q->item.notify == (WorkDoneNotify) _work_queue_free ?
                             "<free>" : "<stop>" :
                         "<corruption>");
        else if (q->vtable == &bucket_vtable)
                snprintf(buf, sizeof(buf), "Bucket '%s'<%p>\n"
                         ".xref_cnt\t=\t%d\n"
                         ".maxsize\t=\t%d\n"
                         ".n_items\t=\t%d\n"
                         ".n_running\t=\t%d\n"
                         ".width\t=\t%d\n"
                         ".head\t=\t%p\n"
                         ".tail\t=\t%p\n"
                         ".context\t=\t%p\n"
                         ".destroy_notify\t=\t%p\n"
                         ".item\t=\t%s",
                         q->name ? : "No Name", q,
                         q->xref_cnt, q->maxsize, q->n_items, q->n_running,
                         q->width, q->head, q->tail,
                         q->context, q->destroy_notify,
                         q->item.work == NULL ? "<none>" :
                         q->item.work == __wq_barrier ? "<flush>" :
                         q->item.work == __wq_stop ?
                           q->item.notify == (WorkDoneNotify) _work_queue_free ?
                             "<free>" : "<stop>" :
                         "<corruption>");
        else
                snprintf(buf, sizeof(buf), "Corrupted Queue Object '%p'", q);

        assert(pthread_mutex_unlock(&q->mutex) == 0);
        ts2 = now(CLOCK_MONOTONIC);

        fprintf(f, "%s\nTotal spend %s.\n\n", buf, format_timespan(span, sizeof(span), ts2-ts1));
}

void work_queue_dump_all(FILE *f) {
        int r;
        WorkQueue *q;

        if ((r = init_global_once()) < 0)
                return;

        fputs("### Dump global queues:\n", f);
        work_queue_dump(&g.concurrent_queue, f);
        work_queue_dump(&g.main_queue, f);

        fputs("### Dump private queues:\n", f);
        LIST_FOREACH(q_or_b, q, g.private_queues)
                work_queue_dump(q, f);

        fputs("### Dump buckets:\n", f);
        LIST_FOREACH(q_or_b, q, g.buckets)
                work_queue_dump(q, f);
}
