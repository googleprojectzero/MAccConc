/*
 * Copyright 2026 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _GNU_SOURCE
#include "basic.h"
#include "runner-common.h"
#include "common.h"
#include <pthread.h>
#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <linux/vm_sockets.h>
#include "third_party/kcov.h"

static int send_full(int sockfd, void *buf, size_t size) {
  size_t done = 0;
  while (done < size) {
    ssize_t res = send(sockfd, (char *)buf+done, size-done, 0);
    if (res == -1 && errno == EAGAIN)
      continue;
    if (res == 0)
      errx(1, "short send");
    if (res < 0)
      err(1, "send_full");
    done += (size_t)res;
  }
  return 0;
}
static int recv_full(int sockfd, void *buf, size_t size) {
  size_t done = 0;
  while (done < size) {
    size_t num = size-done;
    if (num > 4096)
      num = 4096;
    ssize_t res = recv(sockfd, (char *)buf+done, num, 0);
    if (res == -1 && errno == EAGAIN)
      continue;
    if (res == 0)
      errx(1, "short recv");
    if (res < 0)
      err(1, "recv_full");
    done += (size_t)res;
  }
  return 0;
}

#define NUM_KCOV_INSTS 3
static unsigned int num_distacks;

static pthread_barrier_t init_barrier, finish_barrier;

static void *thread_fn(void *dummy) {
  pin_to(1);
  //prep_thread2();
  SYSCHK(ioctl(kcov_insts[1].fd, KCOV_ENABLE, 3/*KCOV_TRACE_MEMORY_ACCESS*/));
  pthread_barrier_wait(&init_barrier);

  __atomic_store_n(&kcov_insts[1].cover[0], 0, __ATOMIC_RELAXED);
  // MEASUREMENT STARTS HERE
  test_thread2();
  // MEASUREMENT ENDS HERE
  kcov_insts[1].cover_n = __atomic_load_n(&kcov_insts[1].cover[0], __ATOMIC_RELAXED);

  pthread_barrier_wait(&finish_barrier);
  return NULL;
}

int main(int argc, char **argv) {
  if (argc != 2)
    errx(1, "invocation: %s <path to test .so>", argv[0]);
  load_testcase_dlsyms(argv[1]);

  int vsock = SYSCHK(socket(AF_VSOCK, SOCK_STREAM, 0));
  struct sockaddr_vm vsock_addr = {
    .svm_family = AF_VSOCK,
    .svm_port = 0x4b434f56 /* "KCOV" */,
    .svm_cid = VMADDR_CID_HOST
  };
  SYSCHK(connect(vsock, (struct sockaddr *)&vsock_addr, sizeof(vsock_addr)));

  unsigned int get_distacks_marker = 0x12345678;
  SYSCHK(send_full(vsock, &get_distacks_marker, sizeof(get_distacks_marker)));
  SYSCHK(recv_full(vsock, &num_distacks, sizeof(num_distacks)));

  kcov_alloc_multi(NUM_KCOV_INSTS);
  for (int i=0; i<NUM_KCOV_INSTS; i++) {
    kcov_insts[i].distacks = malloc(sizeof(struct kcov_di_stack) * num_distacks);
    kcov_insts[i].di_arg = (struct kcov_set_di_arg){
      .stacks = (unsigned long)kcov_insts[i].distacks,
      .sync_bits_fd = (i==0) ? -1 : kcov_insts[0].fd,
      .spin_limit = SPIN_LIMIT
    };
  }

  if (num_distacks) {
    struct ipc_distack *ipc_distacks = malloc(sizeof(struct ipc_distack) * num_distacks);
    SYSCHK(recv_full(vsock, ipc_distacks, sizeof(struct ipc_distack) * num_distacks));

    for (int i=0; i<num_distacks; i++) {
      struct ipc_distack *ids = &ipc_distacks[i];
      if (!ids->num_elems)
        continue;
      if (ids->thread_idx >= NUM_KCOV_INSTS)
        errx(1, "received DI thread_idx oob");
      if (ids->num_elems >= 32)
        errx(1, "received DI stack with too many elems");
      struct kcov_inst *inst = &kcov_insts[ids->thread_idx];
      struct kcov_di_stack *kds = &inst->distacks[inst->di_arg.num_stacks++];
      kds->elems = (unsigned long)ids->elems;
      kds->num_elems = ids->num_elems;
      kds->type = ids->type;
      kds->flagidx = ids->flagidx;
    }
    for (int i=0; i<NUM_KCOV_INSTS; i++)
      SYSCHK(ioctl(kcov_insts[i].fd, KCOV_SET_DI, &kcov_insts[i].di_arg));
  }

  test_setup();

  if (pthread_barrier_init(&init_barrier, NULL, 2))
    errx(1, "pthread_barrier_init");
  if (pthread_barrier_init(&finish_barrier, NULL, 2))
    errx(1, "pthread_barrier_init");
  pthread_t thread;
  if (pthread_create(&thread, NULL, thread_fn, NULL))
    errx(1, "pthread_create");

  pin_to(0);
  //prep_thread1();

  struct kcov_remote_arg *rarg = malloc(sizeof(struct kcov_remote_arg) + sizeof(__aligned_u64));
  rarg->trace_mode = 3/*KCOV_TRACE_MEMORY_ACCESS*/;
  rarg->area_size = 1*1024*1024;
  rarg->num_handles = 0;
  rarg->common_handle = gettid();
  SYSCHK(ioctl(kcov_insts[0].fd, KCOV_ENABLE, 3/*KCOV_TRACE_MEMORY_ACCESS*/));
  SYSCHK(ioctl(kcov_insts[2].fd, KCOV_REMOTE_ENABLE, rarg));
  pthread_barrier_wait(&init_barrier);

  __atomic_store_n(&kcov_insts[2].cover[0], 0, __ATOMIC_RELAXED);
  __atomic_store_n(&kcov_insts[0].cover[0], 0, __ATOMIC_RELAXED);
  // MEASUREMENT STARTS HERE
  test_thread1();
  // MEASUREMENT ENDS HERE
  kcov_insts[0].cover_n = __atomic_load_n(&kcov_insts[0].cover[0], __ATOMIC_RELAXED);

  pthread_barrier_wait(&finish_barrier);
  sleep(1); // HACK
  kcov_insts[2].cover_n = __atomic_load_n(&kcov_insts[2].cover[0], __ATOMIC_RELAXED);
  if (pthread_join(thread, NULL))
    errx(1, "pthread_join");

  unsigned int num_threads = NUM_KCOV_INSTS;
  SYSCHK(send_full(vsock, &num_threads, sizeof(num_threads)));
  for (int i=0; i<NUM_KCOV_INSTS; i++) {
    SYSCHK(send_full(vsock, &kcov_insts[i].cover_n, sizeof(kcov_insts[i].cover_n)));
    SYSCHK(send_full(vsock, kcov_insts[i].cover+1, kcov_insts[i].cover_n * sizeof(unsigned long)));
  }
  test_end();

  return 0;
}
