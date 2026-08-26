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

#include <pthread.h>
#include <dlfcn.h>
#include <err.h>
#include <fcntl.h>
#include <sched.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include "third_party/kcov.h"

static void (*test_setup)(void);
static void (*test_thread1)(void);
static void (*test_thread2)(void);
static void (*test_end)(void);

static void pin_to(int cpu) {
  cpu_set_t cset;
  CPU_ZERO(&cset);
  CPU_SET(cpu, &cset);
  if (sched_setaffinity(0, sizeof(cpu_set_t), &cset))
    err(1, "set affinity");
}

static void (*dlsym_checked(void *handle, const char *symbol))(void) {
  void *res = dlsym(handle, symbol);
  if (res)
    return (void(*)(void))res;
  errx(1, "unable to lookup symbol '%s': %s", symbol, dlerror());
}

static void load_testcase_dlsyms(const char *shlib_path) {
  void *rtld_handle = dlopen(shlib_path, RTLD_NOW|RTLD_LOCAL|RTLD_NODELETE);
  if (rtld_handle == NULL)
    errx(1, "unable to dlopen('%s'): %s", shlib_path, dlerror());
  test_setup = dlsym_checked(rtld_handle, "test_setup");
  test_thread1 = dlsym_checked(rtld_handle, "test_thread1");
  test_thread2 = dlsym_checked(rtld_handle, "test_thread2");
  test_end = dlsym_checked(rtld_handle, "test_end");
}

static struct kcov_inst {
  int fd;
  unsigned long *cover;
  unsigned long cover_n;

  // only for kcov-vsock-client
  struct kcov_di_stack *distacks;
  struct kcov_set_di_arg di_arg;
} *kcov_insts;

#define COVER_SIZE                  (16 << 20/*MiB*/)

static void kcov_alloc_multi(int num_kcov_insts) {
  kcov_insts = (struct kcov_inst*)SYSCHK(mmap(NULL, sizeof(*kcov_insts)*num_kcov_insts, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0));
  for (int i=0; i<num_kcov_insts; i++) {
    kcov_insts[i].fd = SYSCHK(open("/sys/kernel/debug/kcov", O_RDWR));
    SYSCHK(ioctl(kcov_insts[i].fd, KCOV_INIT_TRACE, COVER_SIZE));
    kcov_insts[i].cover = (unsigned long *)SYSCHK(mmap(NULL, COVER_SIZE * sizeof(unsigned long),
            PROT_READ | PROT_WRITE, MAP_SHARED, kcov_insts[i].fd, 0));
  }
}

static pthread_barrier_t rtic_init_barrier, rtic_finish_barrier;
static void *rtic_thread_fn(void *inner_) {
  bool inner = *(bool*)inner_;
  pin_to(1);
  SYSCHK(ioctl(kcov_insts[1].fd, KCOV_ENABLE, 3/*KCOV_TRACE_MEMORY_ACCESS*/));
  pthread_barrier_wait(&rtic_init_barrier);
  if (inner)
    ioctl(kcov_insts[0].fd, KCOV_SPINWAIT_DI_FLAG, 0);

  __atomic_store_n(&kcov_insts[1].cover[0], 0, __ATOMIC_RELAXED);
  // MEASUREMENT STARTS HERE
  test_thread2();
  // MEASUREMENT ENDS HERE
  kcov_insts[1].cover_n = __atomic_load_n(&kcov_insts[1].cover[0], __ATOMIC_RELAXED);
  if (inner)
    SYSCHK(ioctl(kcov_insts[0].fd, KCOV_WAKE_DI_FLAG, 1));

  pthread_barrier_wait(&rtic_finish_barrier);
  return NULL;
}
static unsigned long SPIN_LIMIT = 1000000;
static void run_testcase_in_child(bool sequential, bool t2_is_inner) {
  pid_t child = SYSCHK(fork());
  if (child != 0) {
    int wstatus;
    SYSCHK(waitpid(child, &wstatus, 0));
    return;
  }

  if (pthread_barrier_init(&rtic_init_barrier, NULL, 2))
    errx(1, "pthread_barrier_init");
  if (pthread_barrier_init(&rtic_finish_barrier, NULL, 2))
    errx(1, "pthread_barrier_init");
  test_setup();
  pthread_t thread;
  if (pthread_create(&thread, NULL, rtic_thread_fn, &t2_is_inner))
    errx(1, "pthread_create");

  pin_to(0);

  pthread_barrier_wait(&rtic_init_barrier);

  // should be the last syscall before we start
  SYSCHK(ioctl(kcov_insts[0].fd, KCOV_ENABLE, KCOV_TRACE_MEMORY_ACCESS));

  __atomic_store_n(&kcov_insts[0].cover[0], 0, __ATOMIC_RELAXED);
  // MEASUREMENT STARTS HERE
  test_thread1();
  // MEASUREMENT ENDS HERE
  kcov_insts[0].cover_n = __atomic_load_n(&kcov_insts[0].cover[0], __ATOMIC_RELAXED);
  if (sequential)
    SYSCHK(ioctl(kcov_insts[0].fd, KCOV_WAKE_DI_FLAG, 0));

  pthread_barrier_wait(&rtic_finish_barrier);
  if (pthread_join(thread, NULL))
    errx(1, "pthread_join");
  SYSCHK(ioctl(kcov_insts[0].fd, KCOV_RESET_DI_FLAGS, 0));
  test_end();
  exit(0);
}

#ifdef __cplusplus
struct DIStack {
  std::vector<struct kcov_di_stack_elem> elems;
  enum di_stack_type type;
  unsigned int flagidx;
  struct kcov_di_stack to_uapi() {
    return (struct kcov_di_stack) {
      .elems = (uintptr_t)elems.data(),
      .num_elems = (unsigned int)elems.size(),
      .type = type,
      .flagidx = flagidx
    };
  }
};

void prep_di(size_t thread_idx, std::vector<DIStack> &di_stacks_vec, int sync_bits_fd) {
  std::vector<struct kcov_di_stack> uapi_stacks;
  std::vector<DIStack> di_stack_vec_(di_stacks_vec);
  std::sort(di_stack_vec_.begin(), di_stack_vec_.end(), [](DIStack& a, DIStack& b) {
    return a.type != DI_STACK_WAKE_POST && b.type == DI_STACK_WAKE_POST;
  });
  for (DIStack& di_stack : di_stack_vec_)
    uapi_stacks.push_back(di_stack.to_uapi());
  struct kcov_set_di_arg di_arg = {
    .stacks = (uintptr_t)uapi_stacks.data(),
    .num_stacks = (unsigned int)uapi_stacks.size(),
    .sync_bits_fd = sync_bits_fd,
    .spin_limit = SPIN_LIMIT
  };
  SYSCHK(ioctl(kcov_insts[thread_idx].fd, KCOV_SET_DI, &di_arg));
}
#endif
