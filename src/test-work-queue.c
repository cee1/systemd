/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Chen Jie

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

#include <errno.h>
#include <string.h>
#include <stdio.h>

#include <sys/select.h>
#include <unistd.h>
#include "work-queue.h"
#include "log.h"

#define TEST_QUEUE_MAXSIZE 10
#define TEST_N_ITEMS_ADD 30
#define THREAD_START_TIME 4000
#define WORK_USLEEP_TIME (THREAD_START_TIME * TEST_N_ITEMS_ADD)
#define MAIN_QUEUE_ONE_ITERATION_TIMEOUT (WORK_USLEEP_TIME * 2)

#define CHECK_IT(expr, ...) do {              \
        if (!(expr)) {                        \
                log_error(__VA_ARGS__);       \
                assert_se(!(#expr));          \
        }                                     \
} while(0)

#define CHECK_Q(expr, q, ...) do {            \
        if (!(expr)) {                        \
                log_error(__VA_ARGS__);       \
                work_queue_dump((q), stderr); \
                assert_se(!(#expr));          \
        }                                     \
} while(0)

#define ON_FAIL(expr, stmts) do {             \
        if (!(expr)) {                        \
                stmts;                        \
        }                                     \
} while(0)

static void my_sleep(usec_t us) {
        usec_t ts1, ts2, left = us;
        int r;
        struct timeval tv;

        do {
                tv.tv_sec = left / USEC_PER_SEC;
                tv.tv_usec = left % USEC_PER_SEC;

                ts1 = now(CLOCK_MONOTONIC);
                r = select(0, NULL, NULL, NULL, &tv);
                ts2 = now(CLOCK_MONOTONIC);

                if (r && left > ts2 - ts1) {
                        assert(r == EINTR);

                        left -= (ts2 - ts1);
                        continue;
                }
        } while (0);
}

static void queue_destroyed(void *ref) {
        WorkQueue **q = (WorkQueue **)ref;

        log_info("Queue or bucket '%p' destroyed!", *q);
        *q = NULL;
}

static void done_notify(void *data) {
        char *msg = (char *)data;

        log_info("Done: %s", msg ? : "finished");
        free(msg);
}

static void work(void *data) {
        my_sleep(WORK_USLEEP_TIME);
}

static void done_notify_add_back(void *data) {
        int r;
        char *msg;
        WorkQueue *q = (WorkQueue *)data;

        asprintf(&msg, "item be added back(to queue %p)", q);
        r = work_queue_add(q, work, done_notify, free, msg);

        CHECK_Q(r == 0 || r == -EAGAIN, q,
                "*** Expect work_queue_add() returns 0 or -EAGAIN,"
                "but it reports -'%s'", strerror(-r));

        ON_FAIL(r == 0,
                log_info("%s(): Failed to add back: queue='%p' is full",
                         __func__, q);
                free(msg));
        work_queue_unref(q);
}

static int test_add(WorkQueue *q) {
        int i, r;
        char *msg;

        for (i = 0; i < TEST_N_ITEMS_ADD; i++) {
                if (i == TEST_QUEUE_MAXSIZE) {
                        /* The queue should possible be full,
                         * sleep to make sure the threads running */
                        my_sleep(THREAD_START_TIME);
                }

                asprintf(&msg, "%p count=%d", q, i);
                if ((r = work_queue_add(q, work, done_notify, free, msg))) {
                        CHECK_Q(r == -EAGAIN, q,
                                "*** Expect work_queue_add() returns -EAGAIN,"
                                "but it reports -'%s'", strerror(-r));

                        log_info("%s(): Failed to Add %d items to queue or bucket '%p', it's full",
                                 __func__, i, q);
                        free(msg);

                        break;
                }
        }

        return i;
}

static int test_add2(WorkQueue *q) {
        int i, r;

        for (i = 0; i < TEST_N_ITEMS_ADD; i++) {
                if (i == TEST_QUEUE_MAXSIZE) {
                        /* The queue should possible be full,
                         * sleep to make sure the threads running */
                        my_sleep(THREAD_START_TIME);
                }

                work_queue_ref(q);
                if ((r = work_queue_add(q, work, done_notify_add_back,
                                        (WorkCancelFunc)work_queue_unref, q))) {
                        CHECK_Q(r == -EAGAIN, q,
                                "*** Expect work_queue_add() returns -EAGAIN,"
                                "but it reports -'%s'", strerror(-r));

                        log_info("%s(): Failed to Add %d items to queue or bucket '%p', it's full",
                                 __func__, i, q);
                        work_queue_unref(q);
                        break;
                }
        }

        return i;
}

static int test_add_ring(WorkQueue *q) {
        int i, r;
        char *msg;

        for (i = 0; i < TEST_N_ITEMS_ADD; i++) {
                if (i == TEST_QUEUE_MAXSIZE) {
                        /* The queue should possible be full,
                         * sleep to make sure the threads running */
                        my_sleep(THREAD_START_TIME);
                }

                asprintf(&msg, "%p count=%d", q, i);
                r = work_queue_add_rewind(q, work, done_notify, free, msg);
                CHECK_Q(r >= 0, q,
                        "*** Expect work_queue_add_rewind() successful,"
                        "but it reports -'%s'", strerror(-r));
        }

        return i;
}

static void check_nr_iterations(int expected_value) {
        int i, r;

        for (i = 0; ; i++) {
                r = work_queue_run_main_queue(MAIN_QUEUE_ONE_ITERATION_TIMEOUT);
                if (r)
                        break;
        }

        CHECK_Q(r == -EAGAIN, (WorkQueue *)1,
                 "*** Expect work_queue_run_main_queue returns -EAGAIN,"
                 "but it reports -'%s'", strerror(-r));
        CHECK_Q(expected_value == i, (WorkQueue *)1,
                 "*** Unexpected number of iterations of work_queue_run_main_queue()"
                 " expect '%d' but got '%d'", expected_value, i);
}

static WorkQueue *sample_queue;
static WorkQueue *sample_buckets[3];
static const char *q_or_b_names[] = {
        "sample-queue",
        "sample-bucket[0]",
        "sample-bucket[1]",
        "sample-bucket[2]",
};

static int samples_init(void) {
        WorkQueue *q;
        int n_buckets;
        int i;

        ON_FAIL(q = work_queue_new(q_or_b_names[0], TEST_QUEUE_MAXSIZE),
                log_warning("Failed to create queue '%s'", q_or_b_names[0]);
                goto fail);
        work_queue_set_destroy_notify(q, queue_destroyed, &sample_queue);
        sample_queue = q;

        n_buckets = sizeof(sample_buckets)/sizeof(WorkQueue *);
        for (i = 0; i < n_buckets; i++) {
                ON_FAIL(q = work_bucket_new(q_or_b_names[1+i], TEST_QUEUE_MAXSIZE, i+1),
                        log_warning("Failed to create bucket '%s'", q_or_b_names[1+i]);
                        goto fail);
                work_queue_set_destroy_notify(q, queue_destroyed, sample_buckets+i);
                sample_buckets[i] = q;
        }

        return 0;

fail:
        work_queue_unref(sample_queue);
        sample_queue = NULL;

        for (i = 0; i < n_buckets; i++) {
                work_queue_unref(sample_buckets[i]);
                sample_buckets[i] = NULL;
        }

        return -ENOMEM;
}

static void sample_add_test(void) {
        /* assume: main queue is empty */

        WorkQueue *q;
        int i, r, n, tmp;
        int n_buckets;

        log_info("Test adding to queue...");
        q = sample_queue;

        tmp = TEST_QUEUE_MAXSIZE + 1;
        CHECK_Q((r = test_add(q)) == tmp /* maxsize + width */, q,
                "*** Unexpected %d items be added in test_add(queue), expect %d",
                r, tmp);
        n = tmp;

        log_info("Test adding to buckets...");
        n_buckets = sizeof(sample_buckets)/sizeof(WorkQueue *);

        for (i = 0; i < n_buckets; i++) {
                q = sample_buckets[i];
                tmp = TEST_QUEUE_MAXSIZE + i + 1;

                CHECK_Q((r = test_add(q)) == tmp /* maxsize + width */, q,
                        "*** Unexpected %d items be added in test_add(bucket), expect %d",
                        r, tmp);
                n += tmp;
        }

        log_info("Check main queue...");
        check_nr_iterations(n);
}

static void sample_add_ring_test(void) {
        /* assume: main queue is empty */

        WorkQueue *q;
        int r;

        q = sample_queue;
        log_info("Test adding in ring mode...");
        CHECK_Q((r = test_add_ring(q)) == TEST_N_ITEMS_ADD, q,
                "*** Unexpected %d items be added in test_add_ring(queue), expect %d",
                r, TEST_N_ITEMS_ADD);
        check_nr_iterations(TEST_QUEUE_MAXSIZE + 1);
}

static void sample_flush_stop_test(void) {
        WorkQueue *q;
        int i, r, tmp;
        int n_buckets;

        log_info("Test flush/stop -> fill queue with items...");
        q = sample_queue;
        tmp = TEST_QUEUE_MAXSIZE + 1;
        CHECK_Q((r = test_add2(q)) == tmp /* maxsize + width */, q,
                "*** Unexpected %d items be added in test_add2(queue), expect %d",
                r, tmp);

        log_info("Test flush/stop -> fill buckets with items...");
        n_buckets = sizeof(sample_buckets)/sizeof(WorkQueue *);

        for (i = 0; i < n_buckets; i++) {
                q = sample_buckets[i];
                tmp = TEST_QUEUE_MAXSIZE + i + 1;
                CHECK_Q((r = test_add2(q)) == tmp /* maxsize + width */, q,
                        "*** Unexpected %d items be added in test_add2(bucket), expect %d",
                        r, tmp);
        }

        log_info("Test flush/stop -> flush(queue)...");
        q = sample_queue;
        CHECK_Q((r = work_queue_flush(q)) == 0, q,
                "*** Unexpected error '%s' for work_queue_flush(queue)",
                strerror(-r));

        log_info("Test flush/stop -> stop(buckets)...");
        for (i = 0; i < n_buckets; i++) {
                q = sample_buckets[i];
                CHECK_Q((r = work_queue_stop(q)) == 0, q,
                        "*** Unexpected error '%s' for work_queue_stop(bucket)",
                        strerror(-r));

        }
}

static void sample_free_test(void) {
        WorkQueue *q;
        int i, n_buckets;

        log_info("Free queue and buckets -> free queue...");
        q = sample_queue;
        work_queue_unref(q);
        while (sample_queue)
                work_queue_run_main_queue(MAIN_QUEUE_ONE_ITERATION_TIMEOUT);

        log_info("Free queue and buckets -> free buckets...");
        n_buckets = sizeof(sample_buckets)/sizeof(WorkQueue *);
        for (i = 0; i < n_buckets; i++) {
                q = sample_buckets[i];
                work_queue_unref(q);
        }
        for (i = 0; i < n_buckets; i++) {
                while (sample_buckets[i])
                        work_queue_run_main_queue(MAIN_QUEUE_ONE_ITERATION_TIMEOUT);
        }
}

int main(int argc, char* argv[]) {
        int r;

        if (samples_init() < 0)
                return 255;

        work_queue_dump_all(stderr);

        sample_add_test();
        sample_add_ring_test();
        sample_flush_stop_test();
        sample_free_test();

        CHECK_IT((r = work_queue_stop(NULL)) == 0,
                 "*** Unexpected error '%s' for work_queue_stop(concurrent_queue)",
                 strerror(-r));

        work_queue_dump_all(stderr);
        log_info("Finished!");

        return 0;
}
