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

#include "basic.h"
#include "kcov-common.h"
#include "runner-common.h"
#include <pthread.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define NUM_KCOV_INSTS 2

void prep_di_for_aba_test(std::vector<struct kcov_di_stack_elem> *di_stack) {
  struct kcov_set_di_arg di_arg = {.sync_bits_fd=-1, .spin_limit = SPIN_LIMIT};
  struct kcov_di_stack di_stacks[2];
  if (di_stack != nullptr) {
    di_stacks[0] = di_stacks[1] = {
      .elems = (uintptr_t)di_stack->data(),
      .num_elems = (uint32_t)di_stack->size()
    };
    di_stacks[0].type = DI_STACK_WAKE_PRE;
    di_stacks[0].flagidx = 0;
    di_stacks[1].type = DI_STACK_WAIT;
    di_stacks[1].flagidx = 1;
    di_arg.stacks = (uintptr_t)di_stacks;
    di_arg.num_stacks = 2;
  }
  SYSCHK(ioctl(kcov_insts[0].fd, KCOV_SET_DI, &di_arg));
}

int main(int argc, char **argv) {
  if (argc != 2)
    errx(1, "invocation: %s <path to test .so>", argv[0]);
  fprintf(stderr, "loading kallsyms\n");
  kallsyms_load();
  fprintf(stderr, "RCU state (excluded): base=%lx len=%lx\n", rcu_state_base, rcu_state_len);

  fprintf(stderr, "loading testcase\n");
  load_testcase_dlsyms(argv[1]);

  fprintf(stderr, "initializing kcov\n");
  kcov_alloc_multi(NUM_KCOV_INSTS);

  fprintf(stderr, "collecting A-B coverage\n");
  prep_di_for_aba_test(nullptr);
  run_testcase_in_child(true, true);

  KcovTraceSet ts;
  ts.traces.push_back(KcovTrace(0));
  ts.traces.push_back(KcovTrace(1));
  ts.traces.at(0).data.assign(kcov_insts[0].cover+1, kcov_insts[0].cover+1+kcov_insts[0].cover_n);
  ts.traces.at(1).data.assign(kcov_insts[1].cover+1, kcov_insts[1].cover+1+kcov_insts[1].cover_n);
  trace_set_to_interference_sets(&ts);
  KcovTraceIterWithStack ti(0, &ts.traces.at(0));
  std::vector<std::vector<struct kcov_di_stack_elem>> preempt_candidates;
  for (; !ti.at_end(); ti.advance()) {
    if (ti.record_type() == KCOV_RECORDFLAG_TYPE_MEMORY) {
      if (rcu_state_base != 0 && (Range(ti.mar()) <=> Range(rcu_state_base, rcu_state_base+rcu_state_len-1)) == std::weak_ordering::equivalent)
        continue;
      if (ts.has_interference(0, ti.mar()))
        preempt_candidates.push_back(ti.get_di_stack());
    }
  }

  fprintf(stderr, "testing candidates\n");
  unsigned int outcome_injection_failed = 0;
  unsigned int outcome_wait_timeout = 0;
  unsigned int outcome_reordered = 0;
  for (auto& preempt_candidate : preempt_candidates) {
    prep_di_for_aba_test(&preempt_candidate);
    run_testcase_in_child(false, true);

    bool found_wait = false;
    bool wait_timed_out = false;
    KcovTrace t0_withdelay(0);
    t0_withdelay.data.assign(kcov_insts[0].cover+1, kcov_insts[0].cover+1+kcov_insts[0].cover_n);
    for (KcovTraceIter ti_withdelay(0, &t0_withdelay); !ti_withdelay.at_end(); ti_withdelay.advance()) {
      if (ti_withdelay.record_type() != KCOV_RECORDFLAG_TYPE_WAIT)
        continue;
      if (found_wait)
        errx(1, "duplicate wait");
      found_wait = true;
      if (ti_withdelay.wait_timed_out())
        wait_timed_out = true;
    }
    if (!found_wait) {
      outcome_injection_failed++;
    } else if (wait_timed_out) {
      outcome_wait_timeout++;
    } else {
      outcome_reordered++;
    }
  }
  fprintf(stderr, "stats:  injection-failed:%u  wait-timeout:%u  reordered:%u\n",
    outcome_injection_failed, outcome_wait_timeout, outcome_reordered);
}
