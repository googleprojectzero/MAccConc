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

#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "third_party/kcov.h"

#include <algorithm>
#include <compare>
#include <map>
#include <optional>
#include <set>
#include <stdio.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct Range {
  unsigned long start; // inclusive
  unsigned long end; // inclusive
  Range(unsigned long start, unsigned long end) : start(start), end(end) {
    assert(start <= end);
  }
  explicit Range(unsigned long addr) : start(addr), end(addr) {}
  explicit Range(struct memory_access_record *mar) : start(mar->data_address), end(mar->data_address + mar->size - 1) {}
  // This is not actually a weak ordering, it's really more of a partial ordering.
  std::weak_ordering operator<=>(const Range &other) const {
    if (end < other.start)
      return std::weak_ordering::less;
    if (start > other.end)
      return std::weak_ordering::greater;
    // hack: consider overlapping intervals equivalent so that set lookups
    // return any ranges that overlap the lookup
    return std::weak_ordering::equivalent;
  }
  void merge(const Range &other) {
    assert((*this <=> other) == std::weak_ordering::equivalent);
    if (other.start < start)
      start = other.start;
    if (other.end > end)
      end = other.end;
  }
  unsigned long size() const {
    return (end+1)-start;
  }
};

struct RangeSet {
  std::set<Range> ranges;
  void add(Range new_range) {
    auto [begin,end] = ranges.equal_range(new_range);
    for (const Range& old : std::ranges::subrange(begin, end)) {
      new_range.merge(old);
    }
    ranges.erase(begin, end);
    ranges.insert(new_range);
  }
  void add(RangeSet other_set) {
    for (const Range &other_range : other_set.ranges)
      add(other_range);
  }
  void sub(Range sub_range) {
    auto [begin,end] = ranges.equal_range(sub_range);
    if (begin == end)
      return;
    auto last = end;
    last--;

    std::optional<Range> begin_leftover;
    if (begin->start < sub_range.start)
      begin_leftover = Range(begin->start, sub_range.start-1);

    std::optional<Range> end_leftover;
    if (last->end > sub_range.end)
      end_leftover = Range(sub_range.end+1, last->end);

    ranges.erase(begin, end);
    if (begin_leftover)
      ranges.insert(*begin_leftover);
    if (end_leftover)
      ranges.insert(*end_leftover);
  }
  void sub(RangeSet &other_set) {
    for (const Range &other_range : other_set.ranges)
      sub(other_range);
  }
  bool has(unsigned long addr) {
    return ranges.contains(Range(addr, addr));
  }

  static RangeSet intersection(RangeSet a, RangeSet b) {
    // this is inefficient but easier than open-coding it...
    RangeSet a_excl = a;
    a_excl.sub(b);

    RangeSet res = a;
    res.sub(a_excl);
    return res;
  }
};

struct CallGraphNode {
  std::vector<CallGraphNode> children;
  unsigned long index;
  CallGraphNode(unsigned long index) : index(index) {}
};

struct KcovTrace {
  std::vector<unsigned long> data;
  size_t thread_idx;
  KcovTrace(size_t thread_idx) : thread_idx(thread_idx) {}

  struct CallGraphNode root_cg = CallGraphNode(ULONG_MAX);
};

struct KcovTraceSet {
  std::vector<struct KcovTrace> traces;
  bool initial_pass_done = false;
  std::vector<RangeSet> interference_sets_on_write, interference_sets_on_any;
  std::map<Range, unsigned int> range_labels;
  std::vector<Range> ranges_by_label;

  bool has_interference(size_t thread_idx, const struct memory_access_record *mar) {
    Range r = Range(mar->data_address, mar->data_address + mar->size - 1);
    if (interference_sets_on_any[thread_idx].ranges.contains(r))
      return true;
    if ((mar->flags & (MEMORY_ACCESS_RECORD_RMW|MEMORY_ACCESS_RECORD_WRITE)) &&
        interference_sets_on_write[thread_idx].ranges.contains(r))
      return true;
    return false;
  }
};
struct KcovTraceIter {
private:
  unsigned long index = 0;
  unsigned long next_mar = 0;

public:
  size_t thread_idx;
  struct KcovTrace *trace;
  KcovTraceIter(size_t thread_idx, struct KcovTrace *trace) : thread_idx(thread_idx), trace(trace) {}
  KcovTraceIter(struct KcovTraceIter *other, unsigned long index) : index(index), next_mar(index), thread_idx(other->thread_idx), trace(other->trace) {}
  unsigned long get_index() { return index; }
  unsigned long record_type() {
    return trace->data[index] & KCOV_RECORDFLAG_TYPEMASK;
  }
  unsigned long record_size() {
    switch (record_type()) {
    case KCOV_RECORDFLAG_TYPE_ENTRY:
      //printf("entry %lx -> %lx\n", caller_ip(), ip_address());
      return 2;
    case KCOV_RECORDFLAG_TYPE_NORMAL:
    case KCOV_RECORDFLAG_TYPE_EXIT:
    case KCOV_RECORDFLAG_TYPE_EESUM:
    case KCOV_RECORDFLAG_TYPE_WAIT:
    case KCOV_RECORDFLAG_TYPE_WAKE:
      return 1;
    case KCOV_RECORDFLAG_TYPE_MEMORY:
      return 5; // TODO receive from traced kernel
    default:
      errx(1, "unexpected record type 0x%lx (full word: 0x%lx)", record_type(), trace->data[index]);
    }
  }
  void advance() {
    index += record_size();
    if (next_mar < index)
      next_mar = index;
  }
  bool at_end() {
    return index >= trace->data.size() || index + record_size() > trace->data.size();
  }

  // type-specific helpers

  // for KCOV_RECORDFLAG_TYPE_ENTRY
  unsigned long caller_ip() { return trace->data[index+1]; }
  // for everything except KCOV_RECORDFLAG_TYPE_EESUM
  unsigned long ip_address() { return trace->data[index] | ~KCOV_RECORD_IP_MASK; }
  // for KCOV_RECORDFLAG_TYPE_EESUM
  int16_t eesum_delta() {    return (int16_t)((uint16_t)(trace->data[index]        & 0xffff)); }
  int16_t eesum_mindelta() { return (int16_t)((uint16_t)((trace->data[index] >> 16) & 0xffff)); }
  // for KCOV_RECORDFLAG_TYPE_MEMORY
  struct memory_access_record* mar() { return (struct memory_access_record *)(trace->data.data() + index); }
  // for KCOV_RECORDFLAG_TYPE_WAIT and KCOV_RECORDFLAG_TYPE_WAKE
  unsigned wakewait_bit_idx() { return trace->data[index] & 0xffff; }
  // for KCOV_RECORDFLAG_TYPE_WAIT
  bool wait_timed_out() { return trace->data[index] & KCOV_WAIT_TIMEOUT; }

  uint64_t next_mar_time() {
    KcovTraceIter mar_iter(this, next_mar);
    while (!mar_iter.at_end() && mar_iter.record_type() != KCOV_RECORDFLAG_TYPE_MEMORY)
      mar_iter.advance();
    next_mar = mar_iter.index;
    return mar_iter.at_end() ? 0 : mar_iter.mar()->time;
  }
};
// trace iterator with machine function stack (does not track inline functions)
struct KcovTraceIterWithStack : KcovTraceIter {
  struct TIStackFrame {
    unsigned long addr;
    unsigned long parent_idx;
    uint64_t next_mar_time;
    std::unordered_map<unsigned long, unsigned long> child_counts;
    TIStackFrame(unsigned long addr, unsigned long parent_idx, uint64_t next_mar_time) :
        addr(addr), parent_idx(parent_idx), next_mar_time(next_mar_time) {}
  };
  std::vector<TIStackFrame> stack = {TIStackFrame(0, 0, 0)};

private:
  void process_item_postvisit() {
    switch (record_type()) {
    case KCOV_RECORDFLAG_TYPE_ENTRY: {
      TIStackFrame &frame = stack.back();
      stack.push_back(TIStackFrame(ip_address(), frame.child_counts[ip_address()]++, next_mar_time()));
      break;
    }
    case KCOV_RECORDFLAG_TYPE_EXIT: {
      if (stack.size() == 1)
        errx(1, "KcovTraceIterWithStack: inconsistent stack state");
      stack.pop_back();
      break;
    }
    case KCOV_RECORDFLAG_TYPE_EESUM: {
      int pop_count = -eesum_mindelta();
      int push_count = pop_count + eesum_delta();
      if (pop_count < 0 || (unsigned int)pop_count > stack.size()-1 || push_count < 0)
        errx(1, "KcovTraceIterWithStack: inconsistent stack state");
      for (int i=0; i<pop_count; i++)
        stack.pop_back();
      for (int i=0; i<push_count; i++)
        stack.push_back(TIStackFrame(0, 0, next_mar_time()));
      break;
    }
    case KCOV_RECORDFLAG_TYPE_MEMORY: {
      TIStackFrame &frame = stack.back();
      frame.child_counts[ip_address()]++;
      break;
    }
    case KCOV_RECORDFLAG_TYPE_NORMAL:
    case KCOV_RECORDFLAG_TYPE_WAIT:
    case KCOV_RECORDFLAG_TYPE_WAKE:
      break;
    default:
      errx(1, "unexpected record type 0x%lx", record_type());
    }
  }

public:
  KcovTraceIterWithStack(size_t thread_idx, struct KcovTrace *trace)
      : KcovTraceIter(thread_idx, trace) {}
  
  void advance() {
    process_item_postvisit();
    KcovTraceIter::advance();
  }

  std::vector<struct kcov_di_stack_elem> get_di_stack(void) {
    if (record_type() != KCOV_RECORDFLAG_TYPE_MEMORY)
      errx(1, "get_di_stack() at non-MEMORY position");
    std::vector<struct kcov_di_stack_elem> result;
    for (size_t i = 1; i < stack.size(); i++) {
      result.push_back((struct kcov_di_stack_elem) {
        .ip = stack[i].addr,
        .parent_idx = stack[i].parent_idx
      });
    }
    result.push_back((struct kcov_di_stack_elem) {
      .ip = ip_address(),
      .parent_idx = stack.back().child_counts[ip_address()]
    });
    return result;
  }
};

void dump_di_stack(std::vector<struct kcov_di_stack_elem> &vec) {
  for (struct kcov_di_stack_elem &e : vec) {
    printf("%llx:%llu ", e.ip, e.parent_idx);
  }
  printf("\n");
}

struct KcovTraceIterSet {
private:
  KcovTraceIter *old_cur = nullptr;

public:
  std::vector<KcovTraceIter> iters;
  KcovTraceIterSet(struct KcovTraceSet *ts) {
    for (KcovTrace &trace : ts->traces)
      iters.emplace_back(trace.thread_idx, &trace);
  }
  KcovTraceIter *cur_iter() {
    KcovTraceIter *cur = (old_cur && !old_cur->at_end()) ? old_cur : nullptr;
    uint64_t cur_time = old_cur ? old_cur->next_mar_time() : UINT64_MAX;
    for (KcovTraceIter &ti : iters) {
      if (ti.at_end())
        continue;
      uint64_t ti_time = ti.next_mar_time();
      if (ti_time < cur_time) {
        cur = &ti;
        cur_time = ti_time;
      }
    }
    return cur;
  }
  bool at_end() {
    for (KcovTraceIter &ti : iters) {
      if (!ti.at_end())
        return false;
    }
    return true;
  }
};

void trace_to_data_addr_range_sets(KcovTrace *trace, RangeSet *rs_all, RangeSet *rs_write) {
  KcovTraceIter ti(SIZE_MAX, trace); // don't care about thread index here
  unsigned long skip_depth = 0;

  while (!ti.at_end()) {
    switch (ti.record_type()) {
    case KCOV_RECORDFLAG_TYPE_ENTRY: {
      if (skip_depth == 0) {
        if (false/*TODO check ti.ip_address() against ignorelist*/)
          skip_depth = 1;
      } else {
        skip_depth++;
      }
      break;
    }
    case KCOV_RECORDFLAG_TYPE_EXIT: {
      if (skip_depth > 0)
        skip_depth--;
      break;
    }
    case KCOV_RECORDFLAG_TYPE_EESUM: {
      if (-ti.eesum_mindelta() >= (int)skip_depth)
        skip_depth = 0;
      if (skip_depth)
        skip_depth += ti.eesum_delta();
      break;
    }
    case KCOV_RECORDFLAG_TYPE_MEMORY: {
      struct memory_access_record *mar = ti.mar();
      if (skip_depth == 0 && mar->size > 0) {
        Range newrange(mar->data_address, mar->data_address + mar->size - 1);
        // TODO ignore rcu_state
        //if (newrange.addr - rcu_state_base < rcu_state_len)
        //  continue;
        rs_all->add(newrange);
        if (mar->flags & (MEMORY_ACCESS_RECORD_RMW|MEMORY_ACCESS_RECORD_WRITE))
          rs_write->add(newrange);
      }
      break;
    }
    }
    ti.advance();
  }
}

void trace_set_to_interference_sets(KcovTraceSet *trace_set) {
  trace_set->interference_sets_on_write.resize(trace_set->traces.size());
  trace_set->interference_sets_on_any.resize(trace_set->traces.size());
  if (trace_set->traces.size() < 2)
    return;

  std::vector<RangeSet> rs_all_by_idx(trace_set->traces.size());
  std::vector<RangeSet> rs_write_by_idx(trace_set->traces.size());
  for (size_t i = 0; i < trace_set->traces.size(); i++)
    trace_to_data_addr_range_sets(&trace_set->traces[i], &rs_all_by_idx[i], &rs_write_by_idx[i]);

  for (size_t view_i = 0; view_i < trace_set->traces.size(); view_i++) {

    RangeSet other_access_ranges;
    for (size_t other_i = 0; other_i < trace_set->traces.size(); other_i++) {
      if (view_i == other_i)
        continue;
      other_access_ranges.add(rs_all_by_idx[other_i]);
    }
    trace_set->interference_sets_on_write[view_i].add(RangeSet::intersection(other_access_ranges, rs_write_by_idx[view_i]));

    RangeSet other_write_ranges;
    for (size_t other_i = 0; other_i < trace_set->traces.size(); other_i++) {
      if (view_i == other_i)
        continue;
      other_write_ranges.add(rs_write_by_idx[other_i]);
    }
    trace_set->interference_sets_on_any[view_i].add(RangeSet::intersection(other_write_ranges, rs_all_by_idx[view_i]));
  }

  RangeSet all_ranges_for_labeling;
  for (size_t i = 0; i < trace_set->traces.size(); i++) {
    all_ranges_for_labeling.add(trace_set->interference_sets_on_any[i]);
    all_ranges_for_labeling.add(trace_set->interference_sets_on_write[i]);
  }
  for (Range r : all_ranges_for_labeling.ranges) {
    trace_set->range_labels.emplace(r, (unsigned int)trace_set->ranges_by_label.size());
    trace_set->ranges_by_label.push_back(r);
  }
}


/* KALLSYMS */

struct ksym {
  unsigned long addr;
  unsigned long len;
  const char *name;
};
static std::vector<struct ksym> ksyms_addrsorted;
static unsigned long kimage_start, kimage_end;
static unsigned long rcu_state_base, rcu_state_len;
static std::unordered_set<unsigned long> ignored_functions;
static const char *find_sym_name(unsigned long addr, unsigned long *off, unsigned long *len) {
  struct ksym needle = {.addr = addr, .len=1};
  auto iter = std::lower_bound(ksyms_addrsorted.begin(), ksyms_addrsorted.end(),
                          needle, [](struct ksym a, struct ksym b) {
    return a.addr + a.len <= b.addr;
  });
  if (iter == ksyms_addrsorted.end()) {
    if (off)
      *off = 0;
    if (len)
      *len = 0;
    return "<?>";
  }
  struct ksym *sym = &*iter;
  if (off)
    *off = addr - sym->addr;
  if (len)
    *len = sym->len;
  return sym->name;
}
static unsigned long find_sym_addr(const char *name, unsigned long *sizep) {
  for (struct ksym &sym : ksyms_addrsorted) {
    if (strcmp(sym.name, name) == 0) {
      if (sizep != NULL)
        *sizep = sym.len;
      return sym.addr;
    }
  }
  return 0;
}
static void kallsyms_load(void) {
  int fd = open("/proc/kallsyms", O_RDONLY);
  if (fd == -1)
    err(1, "open kallsyms");
  size_t buf_len = 100*1024*1024;
  char *buf = (char*)malloc(buf_len+1);
  if (!buf)
    errx(1, "malloc");
  size_t buf_used = 0;
  while (1) {
    int res = read(fd, buf+buf_used, buf_len-buf_used);
    if (res < 0)
      err(1, "read kallsyms");
    if (res == 0) {
      buf[buf_used] = '\0';
      break;
    }
    buf_used += res;
  }
  close(fd);

  char *p = buf;
  while (1) {
    char *endp;
    unsigned long addr = strtoul(p, &endp, 16);
    const char *name = endp + 3;
    p = strchr(p, '\n');
    if (p)
      *p = '\0';
    ksyms_addrsorted.push_back((struct ksym){.addr=addr, .name = name});
    if (!p)
      break;
    p++;
  }
  for (size_t i = 0; i+1 < ksyms_addrsorted.size(); i++)
    ksyms_addrsorted[i].len = ksyms_addrsorted[i+1].addr - ksyms_addrsorted[i].addr;
  if (ksyms_addrsorted.size() != 0)
    ksyms_addrsorted[ksyms_addrsorted.size()-1].len = 0;
  kimage_start = find_sym_addr("_text", NULL);
  kimage_end = find_sym_addr("_end", NULL);
  rcu_state_base = find_sym_addr("rcu_state", &rcu_state_len);
  for (const char *ignored_function_name : {
          "pick_next_task_fair",
          "__mod_zone_page_state"
        }) {
    unsigned long addr = find_sym_addr(ignored_function_name, NULL);
    if (addr != 0)
      ignored_functions.insert(addr);
  }
}
