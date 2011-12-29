/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

#ifndef fooworkqueuehfoo
#define fooworkqueuehfoo

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

#include "util.h"
typedef struct WorkQueue WorkQueue;

typedef void (*WorkFunc) (void *data);
typedef void (*WorkCancelFunc) (void *data);
typedef void (*WorkDoneNotify) (void *data);
typedef void (*WorkQueueDestroyNotify) (void *data);

/* These functions can only be called in main thread */
WorkQueue *work_queue_new(const char *name, int maxsize);
WorkQueue *work_bucket_new(const char *name,
                           int maxsize, int width);
void work_queue_set_destroy_notify(WorkQueue *q,
                                   WorkQueueDestroyNotify destroy_notify,
                                   void *context);

WorkQueue *work_queue_ref(WorkQueue *q);
void work_queue_unref(WorkQueue *q);
int work_queue_flush(WorkQueue *q);
int work_queue_stop(WorkQueue *q);

int work_queue_add(WorkQueue *q, WorkFunc work,
                   WorkDoneNotify notify, WorkCancelFunc cancel,
                   void *data);
int work_queue_add_rewind(WorkQueue *q, WorkFunc work,
                          WorkDoneNotify notify, WorkCancelFunc cancel,
                          void *data);

int work_queue_run_main_queue(usec_t timeout);

void work_queue_dump(WorkQueue *q, FILE *f);
void work_queue_dump_all(FILE *f);

#endif
