/*
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */

#define _POSIX_C_SOURCE 1

#include <proton/driver.h>
#include "../util.h"
#include "../driver_impl.h"

#include <poll.h>
#include <unistd.h>

typedef struct pn_driver_impl_t {
  size_t capacity;
  struct pollfd *fds;
  size_t nfds;
} pn_driver_impl_t;

typedef struct pn_listener_impl_t {
    int idx;
} pn_listener_impl_t;

typedef struct pn_connector_impl_t {
    int idx;
} pn_connector_impl_t;


int pn_driver_impl_init( struct pn_driver_t *d )
{
    d->impl = calloc(1, sizeof(pn_driver_impl_t));
    if (!d->impl) {
        perror("Unable to allocate select() driver_impl:");
        return -1;
    }
    return 0;
}

void pn_driver_impl_destroy( struct pn_driver_t *d )
{
    if (d->impl) {
        if (d->impl->fds) free(d->impl->fds);
        free(d->impl);
    }
    d->impl = NULL;
}


int pn_listener_impl_init( struct pn_listener_t *l )
{
    l->impl = calloc(1, sizeof(pn_listener_impl_t));
    if (!l->impl) {
        perror("Unable to allocate poll() listener_impl:");
        return -1;
    }
    return 0;
}

void pn_listener_impl_destroy( struct pn_listener_t *l )
{
    if (l->impl) free(l->impl);
    l->impl = NULL;
}


int pn_connector_impl_init( struct pn_connector_t *c )
{
    c->impl = calloc(1, sizeof(pn_connector_impl_t));
    if (!c->impl) {
        perror("Unable to allocate poll() connector_impl:");
        return -1;
    }
    return 0;
}

void pn_connector_impl_destroy( struct pn_connector_t *c )
{
    if (c->impl) free(c->impl);
    c->impl = NULL;
}


void pn_driver_impl_wait(pn_driver_t *d, int timeout)
{
  pn_driver_impl_t *impl = d->impl;
  size_t size = d->listener_count + d->connector_count;
  while (impl->capacity < size + 1) {
    impl->capacity = impl->capacity ? 2*impl->capacity : 16;
    impl->fds = realloc(impl->fds, impl->capacity*sizeof(struct pollfd));
  }

  impl->nfds = 0;

  impl->fds[impl->nfds].fd = d->ctrl[0];
  impl->fds[impl->nfds].events = POLLIN;
  impl->fds[impl->nfds].revents = 0;
  impl->nfds++;

  pn_listener_t *l = d->listener_head;
  for (int i = 0; i < d->listener_count; i++) {
    impl->fds[impl->nfds].fd = l->fd;
    impl->fds[impl->nfds].events = POLLIN;
    impl->fds[impl->nfds].revents = 0;
    l->impl->idx = impl->nfds;
    impl->nfds++;
    l = l->listener_next;
  }

  pn_connector_t *c = d->connector_head;
  for (int i = 0; i < d->connector_count; i++)
  {
    if (!c->closed) {
      impl->fds[impl->nfds].fd = c->fd;
      impl->fds[impl->nfds].events = (c->status & PN_SEL_RD ? POLLIN : 0) |
        (c->status & PN_SEL_WR ? POLLOUT : 0);
      impl->fds[impl->nfds].revents = 0;
      c->impl->idx = impl->nfds;
      impl->nfds++;
    }
    c = c->connector_next;
  }

  DIE_IFE(poll(impl->fds, impl->nfds, d->closed_count > 0 ? 0 : timeout));

  if (impl->fds[0].revents & POLLIN) {
    //clear the pipe
    char buffer[512];
    while (read(d->ctrl[0], buffer, 512) == 512);
  }

  l = d->listener_head;
  while (l) {
    int idx = l->impl->idx;
    l->pending = (idx && impl->fds[idx].revents & POLLIN);
    l = l->listener_next;
  }

  c = d->connector_head;
  while (c) {
    if (c->closed) {
      c->pending_read = false;
      c->pending_write = false;
      c->pending_tick = false;
    } else {
      int idx = c->impl->idx;
      c->pending_read = (idx && impl->fds[idx].revents & POLLIN);
      c->pending_write = (idx && impl->fds[idx].revents & POLLOUT);
    }
    c = c->connector_next;
  }
}