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
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <err.h>
#include <sched.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/types.h>
#include "third_party/kcov.h"
#include <elfutils/libdw.h>

#include <algorithm>
#include <vector>
#include <memory>
#include <format>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <utility>

static void dump_kcov_header() {
  printf("LEGEND:\n");
  printf("  type: R=read  W=write  M=modify(read+write)  F=free  A=atomic\n");
  printf("\n");
  printf("ID    range data address      size type thread 1       thread 2\n");
  printf("----- ----- ------------     ----- ---- --------       --------\n");
}

class ThreadInfo {
public:
  int indent_level;
  std::string prefix;
  unsigned long cur_idx = 0;
};

class AccessInfo {
public:
  std::vector<struct kcov_di_stack_elem> stack_elems;
  ThreadInfo *thread;
};
class AccessInfoMap {
public:
  std::unordered_map<std::string, AccessInfo> map;
  std::string add(AccessInfo ai) {
    std::string label = ai.thread->prefix + std::format("{:04}", ai.thread->cur_idx++);
    map[label] = std::move(ai);
    return label;
  }
};

// for debugging
static bool print_every_access;

static std::vector<std::pair<uint64_t, std::string>> dump_kcov(KcovTraceSet *ts, size_t thread_idx, ThreadInfo *thread, AccessInfoMap *aim) {
  std::vector<std::pair<uint64_t, std::string>> result;
  unsigned long stack_depth_printed = 1;
  for (KcovTraceIterWithStack ti(thread_idx, &ts->traces.at(thread_idx)); !ti.at_end(); ti.advance()) {
    stack_depth_printed = std::min(stack_depth_printed, ti.stack.size());
    if (ti.record_type() != KCOV_RECORDFLAG_TYPE_MEMORY &&
        ti.record_type() != KCOV_RECORDFLAG_TYPE_WAIT &&
        ti.record_type() != KCOV_RECORDFLAG_TYPE_WAKE)
      continue;
    bool is_interference_memory_access;

    if (ti.record_type() == KCOV_RECORDFLAG_TYPE_MEMORY) {
      is_interference_memory_access = ts->has_interference(thread_idx, ti.mar());
      if (!is_interference_memory_access && !print_every_access)
        continue;
    }

    // delayed printing of ENTER only when we actually print an access
    while (stack_depth_printed < ti.stack.size()) {
      auto& stack_elem = ti.stack.at(stack_depth_printed);

      std::string line;
      for (unsigned long spaces=0; spaces<40+thread->indent_level+(stack_depth_printed-1)*2; spaces++)
        line.push_back(' ');
      line.append("\x1b[");
      line.append((thread->indent_level==0) ? "106" : "103");
      line.append("m");
      char linebuf[1000];
      snprintf(linebuf, sizeof(linebuf), "\x1b[38;2;150;0;0m%s[%lu]\x1b[0m",
          find_sym_name(stack_elem.addr, NULL, NULL), stack_elem.parent_idx);
      line.append(linebuf);
      line.push_back('\n');
      result.push_back(std::make_pair<>(stack_elem.next_mar_time, line));

      stack_depth_printed++;
    }

    std::string line;
    if (ti.record_type() == KCOV_RECORDFLAG_TYPE_MEMORY) {
      unsigned long data_addr = ti.mar()->data_address;
      AccessInfo access_info = {
        .stack_elems = ti.get_di_stack(),
        .thread = thread
      };
      line.append(aim->add(std::move(access_info)));
      line.push_back(' ');

      unsigned int range_id = is_interference_memory_access ? ts->range_labels[Range(ti.mar())] : 63;
      const char *bgcolor;
      switch (range_id % 8) {
        // hue rotating, saturation=50%, value=100%
        case 0: bgcolor = "255;128;128"; break;
        case 1: bgcolor = "255;223;128"; break;
        case 2: bgcolor = "191;255;128"; break;
        case 3: bgcolor = "128;255;159"; break;
        case 4: bgcolor = "128;255;255"; break;
        case 5: bgcolor = "128;159;255"; break;
        case 6: bgcolor = "191;128;255"; break;
        case 7: bgcolor = "255;128;223"; break;
      }
      const char *fgcolor;
      switch ((range_id / 8) % 8) {
        // hue rotating starting at 20 degrees, saturation=100%, value=50%
        case 0: fgcolor = "128;42;0"; break;
        case 1: fgcolor = "117;128;0"; break;
        case 2: fgcolor = "21;128;0"; break;
        case 3: fgcolor = "0;128;74"; break;
        case 4: fgcolor = "0;85;128"; break;
        case 5: fgcolor = "11;0;128"; break;
        case 6: fgcolor = "106;0;128"; break;
        case 7: fgcolor = "128;0;53"; break;
      }
      char buf[128];
      snprintf(buf, sizeof(buf), "\x1b[38;2;%sm" "\x1b[48;2;%sm"   "R%04u ",
                                            fgcolor,        bgcolor, range_id);
      line.append(buf);

      const char *access_type_char =
          (ti.mar()->flags & MEMORY_ACCESS_RECORD_FREE) ? "\x1b[31mF\x1b[0m" :
          (ti.mar()->flags & MEMORY_ACCESS_RECORD_RMW) ? "\x1b[33mM\x1b[0m" :
          (ti.mar()->flags & MEMORY_ACCESS_RECORD_WRITE) ? "\x1b[33mW\x1b[0m" :
          "R";
      char metabuf[1000];
      snprintf(metabuf, sizeof(metabuf), "%016lx "   "%5lu "          "\x1b[0m %s"               "%c  ",
                                          data_addr, (unsigned long)ti.mar()->size, access_type_char,
                                          (ti.mar()->flags & MEMORY_ACCESS_RECORD_ATOMIC) ? 'A' : ' ');
      line.append(metabuf);
    } else {
      for (unsigned int i=0; i<40; i++)
        line.push_back(' ');
    }

    for (unsigned long spaces=0; spaces<thread->indent_level+(ti.stack.size()-1)*2; spaces++)
      line.push_back(' ');

    line.append("\x1b[");
    line.append((thread->indent_level==0) ? "106" : "103");
    line.append("m");

    if (ti.record_type() == KCOV_RECORDFLAG_TYPE_MEMORY) {
      unsigned long symoff, symlen;
      const char *symname = find_sym_name(ti.ip_address() - 1, &symoff, &symlen);
      symoff += 1; /* fix up the -1 above */
      char symbuf[1000];
      snprintf(symbuf, sizeof(symbuf), "%s+0x%lx/0x%lx", symname, symoff, symlen);
      line.append(symbuf);
    } else {
      if (ti.record_type() == KCOV_RECORDFLAG_TYPE_WAKE) {
        line.append("***WAKE***");
      } else if (ti.wait_timed_out()) {
        line.append("***WAIT TIMEOUT***");
      } else {
        line.append("***WAIT SUCCESS***");
      }
    }
    line.append("\x1b[0m");
    line.push_back('\n');
    result.push_back(std::make_pair<>(ti.next_mar_time(), line));
  }

  // verify
  uint64_t last = 0;
  for (auto& line: result) {
    assert(line.first >= last);
    last = line.first;
  }

  return result;
}

static void print_merged_lines(std::vector<std::vector<std::pair<uint64_t, std::string>>> timed_linesets) {
  std::vector<size_t> indices;
  for (size_t i=0; i<timed_linesets.size(); i++)
    indices.push_back(0);
  while (true) {
    size_t pick = SIZE_MAX;
    uint64_t pick_time = UINT64_MAX;
    for (size_t i=0; i<timed_linesets.size(); i++) {
      if (indices[i] >= timed_linesets[i].size())
        continue;
      uint64_t cur_time = timed_linesets[i][indices[i]].first;
      if (pick == SIZE_MAX || cur_time < pick_time) {
        pick = i;
        pick_time = cur_time;
      }
    }
    if (pick == SIZE_MAX)
      break;
    printf("%s", timed_linesets[pick][indices[pick]].second.c_str());
    indices[pick]++;
  }
}

int main(int argc, char **argv) {
  if (argc < 2)
    errx(1, "invocation: %s <path to test .so>", argv[0]);

  if (argc == 3 && strcmp(argv[2], "--print-every-access") == 0)
    print_every_access = true;

  sync();
  setbuf(stdout, NULL);
  setbuf(stderr, NULL);
  kallsyms_load();
  load_testcase_dlsyms(argv[1]);
  kcov_alloc_multi(2);

  std::vector<DIStack> di_stacks_a, di_stacks_b;
  int next_di_flag_idx = 0;
  while (true) {
    prep_di(0, di_stacks_a, -1);
    prep_di(1, di_stacks_b, kcov_insts[0].fd);
    run_testcase_in_child(false, false);

    KcovTraceSet ts;
    ts.traces.push_back(KcovTrace(0));
    ts.traces.push_back(KcovTrace(1));
    ts.traces.at(0).data.assign(kcov_insts[0].cover+1, kcov_insts[0].cover+1+kcov_insts[0].cover_n);
    ts.traces.at(1).data.assign(kcov_insts[1].cover+1, kcov_insts[1].cover+1+kcov_insts[1].cover_n);
    trace_set_to_interference_sets(&ts);

    printf("=====  filtered to interference set, no RCU core  =====\n");
    dump_kcov_header();
    ThreadInfo t1;
    t1.indent_level = 0;
    t1.prefix = "A";
    ThreadInfo t2;
    t2.indent_level = 15;
    t2.prefix = "B";
    AccessInfoMap aim;
    auto t2_lines = dump_kcov(&ts, 1, &t2, &aim);
    auto t1_lines = dump_kcov(&ts, 0, &t1, &aim);
    std::vector<std::vector<std::pair<uint64_t, std::string>>> timed_linesets = {t1_lines, t2_lines};
    print_merged_lines(timed_linesets);

    while (1) {
      printf("enter command R or C ([R]un / [C]onstraint)> ");
      char cmd[100];
      if (fgets(cmd, sizeof(cmd), stdin) == nullptr)
        exit(0);
      if (cmd[0] == 'R') {
        break;
      } else if (cmd[0] == 'C') {
        printf("Enter the IDs of two accesses for which an ordering constraint should be applied.\n");
        printf("The IDs are shown in the leftmost column (format 'A<number>' or 'B<number>').\n");
        printf("ID of access that should happen first> ");
        char trigger_line[1000];
        if (fgets(trigger_line, sizeof(trigger_line), stdin) == nullptr)
          exit(1);
        *strchrnul(trigger_line, '\n') = '\0';
        if (!aim.map.contains(std::string(trigger_line))) {
          printf("unknown access\n");
          continue;
        }
        AccessInfo *trigger_ai = &aim.map[std::string(trigger_line)];

        printf("ID of access that should happen afterwards> ");
        char wait_line[1000];
        if (fgets(wait_line, sizeof(wait_line), stdin) == nullptr)
          exit(1);
        *strchrnul(wait_line, '\n') = '\0';
        if (!aim.map.contains(std::string(wait_line))) {
          printf("unknown access\n");
          continue;
        }
        AccessInfo *wait_ai = &aim.map[std::string(wait_line)];

        if (trigger_ai->thread == wait_ai->thread) {
          printf("specified two accesses on same thread???");
          continue;
        }

        std::vector<DIStack> *di_stacks_trigger = (trigger_ai->thread == &t1 ? &di_stacks_a : &di_stacks_b);
        std::vector<DIStack> *di_stacks_wait = (wait_ai->thread == &t1 ? &di_stacks_a : &di_stacks_b);

        di_stacks_trigger->push_back((struct DIStack){
          .elems = trigger_ai->stack_elems,
          .type = DI_STACK_WAKE_POST,
          .flagidx = (unsigned int)next_di_flag_idx
        });

        di_stacks_wait->push_back((struct DIStack){
          .elems = wait_ai->stack_elems,
          .type = DI_STACK_WAIT,
          .flagidx = (unsigned int)next_di_flag_idx
        });

        next_di_flag_idx++;
        printf("ok\n");
      } else {
        printf("unknown command\n");
      }
    }
  }
  return 0;
}
