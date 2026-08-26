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

#include "kcov-common.h"

#include "basic.h"
#include <capstone/x86.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <linux/vm_sockets.h>
#include <dwarf.h>
#include <elfutils/libdw.h>
#include <capstone/capstone.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <variant>
#include <unordered_map>
#include <unordered_set>

#include "common.h"

// thread code already needs it for glfwPostEmptyEvent()
#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>

//#include "easy_ipc.h"

struct DIStack {
  std::vector<struct kcov_di_stack_elem> thread_distacks[2];
  size_t thread_indices[2] = {SIZE_MAX,SIZE_MAX};
  void swap() {
    if (thread_distacks[1].size() == 0)
      return;
    std::swap(thread_distacks[0], thread_distacks[1]);
    std::swap(thread_indices[0], thread_indices[1]);
  }
};
struct DIStacksState {
  std::vector<DIStack> distacks;
  std::vector<struct ipc_distack> as_ipc_distacks() {
    std::vector<struct ipc_distack> res;
    unsigned int flagidx = 0;
    for (DIStack& dis : distacks) {
      if (dis.thread_distacks[0].size() >= 32 || dis.thread_distacks[1].size() >= 32)
        errx(1, "distack too big");
      if (dis.thread_distacks[1].size() == 0)
        continue;
      for (int i=0; i<2; i++) {
        struct ipc_distack ipc_dis = {
          .num_elems = (int)dis.thread_distacks[i].size(),
          .type = (i==0) ? DI_STACK_WAKE_POST : DI_STACK_WAIT,
          .flagidx = flagidx,
          .thread_idx = (unsigned int)dis.thread_indices[i]
        };
        memcpy(ipc_dis.elems, dis.thread_distacks[i].data(), dis.thread_distacks[i].size() * sizeof(struct kcov_di_stack_elem));
        assert(ipc_dis.num_elems < 32);
        res.push_back(ipc_dis);
      }
      flagidx++;
    }
    return res;
  }

  void handle_addition(size_t thread_idx, std::vector<struct kcov_di_stack_elem> stack_elems) {
    if (distacks.size() == 0 ||
        (distacks[distacks.size()-1].thread_distacks[1].size() != 0)) {
      distacks.emplace_back();
    }
    DIStack &dis = distacks[distacks.size()-1];
    size_t part = (dis.thread_distacks[0].size() == 0) ? 0 : 1;
    if (part == 1 && thread_idx == dis.thread_indices[0])
      return;
    dis.thread_distacks[part] = stack_elems;
    dis.thread_indices[part] = thread_idx;
  }
};

static DIStacksState distate;
static int listen_sock;
static std::atomic<int> incoming_state;
static std::atomic<std::shared_ptr<struct KcovTraceSet>> current_trace_set;
static std::atomic<std::shared_ptr<struct DIStacksState>> current_distate = std::make_shared<DIStacksState>();
static void listen_init(void) {
  listen_sock = SYSCHK(socket(AF_VSOCK, SOCK_STREAM, 0));
  struct sockaddr_vm listen_addr = {
    .svm_family = AF_VSOCK,
    .svm_port = 0x4b434f56 /* "KCOV" */,
    .svm_cid = VMADDR_CID_ANY
  };
  SYSCHK(bind(listen_sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)));
  SYSCHK(listen(listen_sock, 16));
}
static int recv_full(int sockfd, void *buf, size_t size) {
  size_t done = 0;
  while (done < size) {
    // std::min() hack because vsock recv is fragile and can fail with ENOMEM :/
    ssize_t res = recv(sockfd, (char *)buf+done, std::min((size_t)4096,size-done), MSG_WAITALL);
    if (res == 0) {
      errno = EPROTO;
      return -1;
    }
    if (res < 0)
      return res;
    done += (size_t)res;
  }
  return 0;
}
static int send_full(int sockfd, void *buf, size_t size) {
  size_t done = 0;
  while (done < size) {
    // std::min() hack because vsock send is fragile and can fail with ENOMEM :/
    ssize_t res = send(sockfd, (char *)buf+done, std::min((size_t)4096,size-done), MSG_WAITALL);
    if (res == 0) {
      errno = EPROTO;
      return -1;
    }
    if (res < 0)
      return res;
    done += (size_t)res;
  }
  return 0;
}
static void *listen_threadfn(void *dummy) {
  while (1) {
    int sock = SYSCHK(accept(listen_sock, NULL, NULL));
    incoming_state = 1;
    glfwPostEmptyEvent();

    unsigned int num_threads;
continue_reading:
    if (recv_full(sock, &num_threads, sizeof(num_threads))) {
      perror("receive num of threads");
      goto abort;
    }
    if (num_threads == 0x12345678) {
      std::shared_ptr<struct DIStacksState> distate = current_distate;
      auto stacks = distate->as_ipc_distacks();
      unsigned int num_distacks = stacks.size();
      if (send_full(sock, &num_distacks, sizeof(num_distacks))) {
        perror("send");
        goto abort;
      }
      if (num_distacks > 0) {
        if (send_full(sock, stacks.data(), sizeof(struct ipc_distack) * num_distacks)) {
          perror("send2");
          goto abort;
        }
      }
      goto continue_reading;
    }
    if (num_threads == 0) {
      perror("received invalid num of threads");
      goto abort;
    }
    {
      std::shared_ptr<KcovTraceSet> trace_set(new KcovTraceSet);
      for (unsigned int i=0; i<num_threads; i++) {
        unsigned long elem_count;
        if (recv_full(sock, &elem_count, sizeof(elem_count))) {
          perror("receive size");
          goto abort;
        }
        printf("received count: %lu\n", elem_count);
        KcovTrace *trace = &trace_set->traces.emplace_back(i);
        trace->data.resize(elem_count);
        if (recv_full(sock, trace->data.data(), elem_count*sizeof(unsigned long))) {
          perror("receive data");
          goto abort;
        }
      }
      // commit
      current_trace_set = trace_set;
    }
abort:
    close(sock);
    incoming_state = 0;
    glfwPostEmptyEvent();
  }
}

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <stdio.h>

static void glfw_error_callback(int error, const char* description) {
  fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

struct VARangeSet : RangeSet {
  void sub_inlines_inside(Dwarf_Die *die) {
    Dwarf_Die child;
    if (dwarf_child(die, &child) == 0) {
      do {
        if (dwarf_tag(&child) == DW_TAG_inlined_subroutine) {
          VARangeSet child_ranges(&child, false);
          sub(child_ranges);
        } else {
          sub_inlines_inside(&child);
        }
      } while (dwarf_siblingof(&child, &child) == 0);
    }
  }
  explicit VARangeSet(Dwarf_Die *die, bool exclude_inline) {
    ptrdiff_t offset = 0;
    Dwarf_Addr base, start, end;
    while (true) {
      offset = dwarf_ranges(die, offset, &base, &start, &end);
      if (offset < 0)
        errx(1, "dwarf_ranges() error");
      if (offset == 0)
        break;
      if (start != end)
        add(Range(start, end-1));
    }

    if (exclude_inline)
      sub_inlines_inside(die);
  }
  VARangeSet() {}
};

static struct Dwarf *vmlinux_dwarf;
static int vmlinux_fd;
static unsigned long FUNC__sanitizer_cov_trace_pc, FUNC__sanitizer_cov_trace_pc_entry;
struct LineCoveragePoint {
  LineCoveragePoint(int lineno) : lineno(lineno) {}
  int lineno;
  unsigned long count = 0;
};
class SourceLoc {
public:
  SourceLoc(const char *file, int lineno) : file(file), lineno(lineno) {}
  SourceLoc(Dwarf_Line* line) {
    dwarf_lineno(line, &lineno);
    file = dwarf_linesrc(line, NULL, NULL);
  }
  const char *file;
  int lineno;
};

class BasicBlock {
public:
  BasicBlock(Range range) : range(range) {}
  Range range;
  //bool entrypoint = false;
  std::vector<BasicBlock*> predecessors;
  std::vector<BasicBlock*> targets;
  bool direct_coverage = false;
  std::unordered_set<unsigned long> kcov_ret_ips;
  std::vector<std::pair<Range, SourceLoc>> source_locs;
};
class CFG {
public:
  std::set<Range> ranges;
  void split_at(unsigned long addr) {
    auto range_node = ranges.extract(Range(addr));
    if (range_node.empty()) {
      // can legitimately happen if addr is at end of function address range
      return;
    }
    Range orig = range_node.value();
    if (orig.start == addr) {
      ranges.insert(orig);
      return;
    }
    Range part2 = Range(addr, orig.end);
    orig.end = addr-1;
    ranges.insert(orig);
    ranges.insert(part2);
  }
  std::map<Range, BasicBlock> bbs;
  void make_bbs(void) {
    for (Range r : ranges)
      bbs.emplace(r, BasicBlock(r));
  }
};
struct FunctionInfo {
  struct FunctionInfo *machine_func;
  unsigned long entry_addr;
  bool is_inline = false;
  struct {
    const char *file = nullptr;
    Dwarf_Word line = 0;
    Dwarf_Word column = 0;
  } inline_caller;
  VARangeSet ip_ranges;
  VARangeSet ip_ranges_own;
  Dwarf_Die cudie = {};
  Dwarf_Die prog_die = {};
  std::vector<struct FunctionInfo> inline_children;
  std::map<Range, struct FunctionInfo *> children_by_va;
  std::unordered_map<unsigned long, std::vector<LineCoveragePoint>> line_cov_by_va;
  struct FunctionInfo *child_by_va(unsigned long va) {
    auto it = children_by_va.find(Range(va, va));
    if (it == children_by_va.end())
      return nullptr;
    return it->second;
  }
  std::string name;

  bool has_srcinfo = false;
  const char *filename;
  int min_line = INT_MAX;
  int max_line = 0;
  std::optional<std::vector<std::string>> lines;
  std::vector<std::vector<size_t>> line_posmaps;
  std::vector<std::string>& get_lines() {
    if (!lines) {
      if (min_line == INT_MAX) {
        min_line = 1;
        max_line = 1;
      }
      lines.emplace();
      std::string full_filename = filename;
      if (full_filename.size() && full_filename[0] != '/') {
        Dwarf_Attribute attrmem;
        const char *comp_dir = dwarf_formstring(dwarf_attr(&cudie, DW_AT_comp_dir, &attrmem));
        if (comp_dir) {
          full_filename = std::string(comp_dir) + "/" + full_filename;
        }
      }
      std::ifstream file(full_filename);
      if (file.is_open()) {
        for (int i=1; i<min_line; i++) {
          std::string dummy;
          std::getline(file, dummy);
        }
        for (int i=min_line; i<=max_line; i++) {
          std::string line;
          std::getline(file, line);
          std::string line_expanded;
          std::vector<size_t> posmap;
          for (size_t i = 0; i < line.size(); i++) {
            posmap.push_back(line_expanded.size());
            if (line[i] == '\t') {
              line_expanded.append("        ");
            } else {
              line_expanded.push_back(line[i]);
            }
          }
          lines->push_back(line_expanded);
          line_posmaps.push_back(posmap);
        }
      }
    }
    return *lines;
  }

  int fixup_column_tabs(int lineno, int column) {
    get_lines();
    if (lineno < min_line || (size_t)lineno >= min_line + line_posmaps.size())
      return 0;
    if (column == 0)
      return 0;

    std::vector<size_t> &posmap = line_posmaps[lineno - min_line];
    if ((size_t)column-1 >= posmap.size())
      return 0;
    return line_posmaps[lineno - min_line][column-1];
  }

  // per machine function
  CFG cfg;
};
static std::unordered_map<unsigned long, struct FunctionInfo> func_by_addr;

struct SourcePoint {
  bool valid = false;
  const char *file; // irrelevant if valid==true

  // following are only initialized if valid
  int line;
  int column_raw;
  int column_fixedwidth;

  SourcePoint(struct FunctionInfo *fi, unsigned long ip) {
    Dwarf_Line *dl = dwarf_getsrc_die(&fi->cudie, ip);
    file = dwarf_linesrc(dl, NULL, NULL);
    if (!file || strcmp(file, fi->filename))
      return;
    dwarf_lineno(dl, &line);
    dwarf_linecol(dl, &column_raw);
    column_fixedwidth = fi->fixup_column_tabs(line, column_raw);
    valid = true;
  }
};

static std::optional<Dwarf_Die> funcdie_by_addr(Dwarf_Die *cudie, unsigned long addr) {
  Dwarf_Die *scopes;
  int num_scopes = dwarf_getscopes(cudie, addr, &scopes);
  if (num_scopes < 0)
    return std::nullopt;
  if (num_scopes == 0)
    return std::nullopt;
  std::string funcname;
  for (int i=num_scopes-1; i>=0; i--) {
    int tag = dwarf_tag(scopes+i);
    if (tag == DW_TAG_subprogram) {
      Dwarf_Die retval = scopes[i];
      free(scopes);
      return retval;
    }
  }
  free(scopes);
  return std::nullopt;
}
static void populate_func_name(struct FunctionInfo *fi) {
  Dwarf_Die *scopes;
  int num_scopes = dwarf_getscopes_die(&fi->prog_die, &scopes);
  if (num_scopes < 0) {
    fi->name = "<getting scopes failed>";
    return;
  }
  if (num_scopes == 0) {
    fi->name = "<not in any scope>";
    return;
  }
  std::string funcname;
  for (int i=num_scopes-1; i>=0; i--) {
    int tag = dwarf_tag(scopes+i);
    if (tag == DW_TAG_compile_unit)
      continue;
    const char *name = dwarf_diename(scopes+i);
    if (name != nullptr) {
      if (funcname.size() != 0)
        funcname.append("::");
      funcname.append(name);
    }
  }
  free(scopes);
  fi->name = funcname;
}

// assumes that fi->inline_children is final
static void populate_children_by_va(struct FunctionInfo *fi) {
  for (struct FunctionInfo &child : fi->inline_children) {
    for (Range range : child.ip_ranges.ranges) {
      assert(!fi->children_by_va.contains(range));
      fi->children_by_va[range] = &child;
    }
  }
}
static void populate_func_with_dwarf(struct FunctionInfo *fi);
static void populate_func_inlines(struct FunctionInfo *fi, Dwarf_Die *die) {
  Dwarf_Die child;
  if (dwarf_child(die, &child) == 0) {
    do {
      if (dwarf_tag(&child) == DW_TAG_inlined_subroutine) {
        //VARangeSet child_ranges(&child, false);
        //sub(child_ranges);
        struct FunctionInfo *child_fi = &fi->inline_children.emplace_back();
        child_fi->machine_func = fi->machine_func;
        child_fi->is_inline = true;
        child_fi->cudie = fi->cudie;
        child_fi->prog_die = child;
        //populate_func_name(child_fi);
        child_fi->name = dwarf_diename(&child) ?: "<unknown inline>";
        child_fi->filename = dwarf_decl_file(&child) ?: "<?>";
        populate_func_with_dwarf(child_fi);
        fi->ip_ranges_own.sub(child_fi->ip_ranges);
      } else {
        populate_func_inlines(fi, &child);
      }
    } while (dwarf_siblingof(&child, &child) == 0);
  }
}

static unsigned char *get_bytes(unsigned long addr, unsigned long num_bytes) {
  Elf *elf = dwarf_getelf(vmlinux_dwarf);
  for (Elf_Scn *scn = elf_nextscn(elf, nullptr); scn; scn = elf_nextscn(elf, scn)) {
    Elf64_Shdr *shdr = elf64_getshdr(scn);
    if (!(shdr->sh_flags & SHF_ALLOC))
      continue;
    if (shdr->sh_addr > addr)
      continue;
    size_t off_in_section = addr - shdr->sh_addr;
    if (off_in_section > shdr->sh_size)
      continue;
    if (shdr->sh_size - off_in_section < num_bytes)
      continue;

    unsigned char *data = (unsigned char *)malloc(num_bytes);
    if (!data)
      err(1, "malloc data");
    if (pread(vmlinux_fd, data, num_bytes, off_in_section + shdr->sh_offset) != (ssize_t)num_bytes)
      errx(1, "vmlinux read");
    return data;
  }
  errx(1, "unable to find section");
}

static void scan_machine_func_code(struct FunctionInfo *fi) {
  if (!fi->filename)
    return;

  csh handle;
  if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK)
    errx(1, "cs_open");
  cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);

  CFG cfg;
  for (const Range& range : fi->ip_ranges.ranges)
    cfg.ranges.insert(range);
  
  std::vector<std::pair<unsigned long, unsigned long>> edges;
  std::vector<unsigned long> kcov_cov_addrs;
  for (const Range& range : fi->ip_ranges.ranges) {
    unsigned char *insn_bytes = get_bytes(range.start, range.size());
    cs_insn *insns;
    ssize_t count = cs_disasm(handle, insn_bytes, range.size(), range.start, 0, &insns);
    if (count > 0) {
      for (ssize_t i = 0; i < count; i++) {
        cs_insn *insn = &insns[i];
        unsigned long insn_end = insn->address + insn->size;
        if (cs_insn_group(handle, insn, X86_GRP_RET))
          cfg.split_at(insn_end);
        if (cs_insn_group(handle, insn, X86_GRP_BRANCH_RELATIVE) &&
                !cs_insn_group(handle, insn, X86_GRP_CALL) &&
                insn->detail->x86.op_count >= 1) {
          cs_x86_op *op0 = &insn->detail->x86.operands[0];
          if (op0->type == X86_OP_IMM) {
            cfg.split_at(insn_end);
            if (insn->id != X86_INS_JMP)
              edges.push_back(std::make_pair((unsigned long)insn->address, insn_end));
            unsigned long target = op0->imm;
            if (fi->ip_ranges.has(target)) {
              cfg.split_at(target);
              edges.push_back(std::make_pair((unsigned long)insn->address, target));
              //printf("jump 0x%lx -> 0x%lx\n", insn_end, target);
            }
          }
        }
        if (cs_insn_group(handle, insn, X86_GRP_CALL) && insn->detail->x86.op_count == 1) {
          unsigned long ret_ip = insn->address + insn->size;
          cs_x86_op *op = &insn->detail->x86.operands[0];

          if (op->type == X86_OP_IMM &&
                ((unsigned long)op->imm == FUNC__sanitizer_cov_trace_pc ||
                 (unsigned long)op->imm == FUNC__sanitizer_cov_trace_pc_entry)) {
            kcov_cov_addrs.push_back(ret_ip);
            /*
            Dwarf_Line *dl = dwarf_getsrc_die(&fi->cudie, insn->address);
            const char *dl_file = dwarf_linesrc(dl, NULL, NULL);
            int lineno;
            dwarf_lineno(dl, &lineno);
            if (dl_file && lineno != 0 && strcmp(dl_file, fi->filename) == 0)
              fi->line_cov_by_va.emplace(ret_ip, lineno);
              */
          }
        }
      }
      cs_free(insns, count);
    }
    free(insn_bytes);
  }
  cfg.make_bbs();
  //cfg.bbs.at(Range(fi->entry_addr)).entrypoint = true;
  for (auto& edge: edges) {
    //printf("EDGE: 0x%lx -> 0x%lx\n", edge.first, edge.second);
    cfg.bbs.at(Range(edge.first)).targets.push_back(&cfg.bbs.at(Range(edge.second)));
  }
  for (auto &iter : cfg.bbs) {
    BasicBlock *bb = &iter.second;
    for (BasicBlock *successor : bb->targets) {
      successor->predecessors.push_back(bb);
    }
  }
  for (unsigned long addr : kcov_cov_addrs) {
    cfg.bbs.at(Range(addr)).direct_coverage = true;
    cfg.bbs.at(Range(addr)).kcov_ret_ips.insert(addr);
  }

  Dwarf_Lines *lines;
  size_t nlines;
  if (dwarf_getsrclines(&fi->cudie, &lines, &nlines))
    return;
  for (size_t i=0; i+1 < nlines; i++) {
    Dwarf_Line *line = dwarf_onesrcline(lines, i);
    Dwarf_Addr line_addr = 0;
    dwarf_lineaddr(line, &line_addr);
    if (!fi->ip_ranges.has(line_addr))
      continue;
    Dwarf_Addr next_line_addr;
    dwarf_lineaddr(dwarf_onesrcline(lines, i+1), &next_line_addr);
    if (line_addr == next_line_addr) {
      /*
       * This DWARF line record does not correspond to any machine code; this
       * can happen, for example, because of barrier() calls.
       */
      continue;
    }
    while (line_addr < next_line_addr) {
      if (!fi->ip_ranges.has(line_addr))
        break;
      BasicBlock *bb = &cfg.bbs.at(Range(line_addr));
      bb->source_locs.push_back(std::make_pair(Range(line_addr, std::min(next_line_addr-1, bb->range.end)), SourceLoc(line)));
      line_addr = bb->range.end+1;
    }
  }
  fi->cfg = std::move(cfg);
}

static void scan_func_code(struct FunctionInfo *fi) {
  for (auto& bb_ : fi->machine_func->cfg.bbs) {
    BasicBlock *bb = &bb_.second;
    if (!bb->direct_coverage)
      continue;
    for (auto& locinfo : bb->source_locs) {
      if (strcmp(locinfo.second.file, fi->filename))
        continue;
      if (fi->ip_ranges_own.ranges.count(locinfo.first)) {
        for (unsigned long kcov_ret_ip : bb->kcov_ret_ips) {
          fi->line_cov_by_va[kcov_ret_ip].push_back(LineCoveragePoint(locinfo.second.lineno));
        }
      }
    }
  }
}

static void populate_func_with_dwarf(struct FunctionInfo *fi) {
  fi->ip_ranges_own = fi->ip_ranges = VARangeSet(&fi->prog_die, false);

  const char *filename = dwarf_decl_file(&fi->prog_die);
  if (filename)
    fi->filename = filename;
  if (fi->is_inline) {
    do {
      Dwarf_Files *files;
      size_t nfiles;
      if (dwarf_getsrcfiles(&fi->cudie, &files, &nfiles))
        break;

      Dwarf_Attribute attr_mem;
      if (dwarf_formudata(dwarf_attr(&fi->prog_die, DW_AT_call_line, &attr_mem), &fi->inline_caller.line))
        break;
      if (dwarf_formudata(dwarf_attr(&fi->prog_die, DW_AT_call_column, &attr_mem), &fi->inline_caller.column))
        break;
      Dwarf_Word call_file_num;
      if (dwarf_formudata(dwarf_attr(&fi->prog_die, DW_AT_call_file, &attr_mem), &call_file_num))
        break;
      fi->inline_caller.file = dwarf_filesrc(files, call_file_num, NULL, NULL);
    } while (0);
  }

  if (!fi->is_inline)
    scan_machine_func_code(fi);
  populate_func_inlines(fi, &fi->prog_die);
  populate_children_by_va(fi);
  scan_func_code(fi);
}
static void ensure_func_srcinfo(struct FunctionInfo *fi) {
  if (fi->has_srcinfo)
    return;
  fi->has_srcinfo = true;

  {
    int decl_line;
    if (dwarf_decl_line(&fi->prog_die, &decl_line) == 0 && decl_line != 0) {
      fi->min_line = fi->max_line = decl_line;
    }
  }

  Dwarf_Lines *lines;
  size_t nlines;
  if (dwarf_getsrclines(&fi->cudie, &lines, &nlines))
    return;
  for (size_t i=0; i<nlines; i++) {
    Dwarf_Line *line = dwarf_onesrcline(lines, i);
    Dwarf_Addr line_addr = 0;
    dwarf_lineaddr(line, &line_addr);

    if (i < nlines+1) {
      Dwarf_Addr next_line_addr;
      dwarf_lineaddr(dwarf_onesrcline(lines, i+1), &next_line_addr);
      if (line_addr == next_line_addr) {
        /*
         * This DWARF line record does not correspond to any machine code; this
         * can happen, for example, because of barrier() calls.
         */
        continue;
      }
    }

    // can't use dwarf_linecontext() for checking whether this is an inline
    // line, it relies on some weird NVIDIA CUDA extension
    if (!fi->ip_ranges_own.has(line_addr))
      continue;

    // hack
    if (strcmp(fi->filename, dwarf_linesrc(line, NULL, NULL))) {
      fprintf(stderr, "skipping %s in %s\n", dwarf_linesrc(line, NULL, NULL), fi->name.c_str());
      continue;
    }
    int lineno;
    dwarf_lineno(line, &lineno);
    /*
     * 6.2.2 State Machine Registers:
     * "Lines are numbered beginning at 1.
     * The compiler may emit the value 0 in cases
     * where an instruction cannot be attributed to any
     * source line."
     */
    if (lineno != 0) {
      if (lineno < fi->min_line)
        fi->min_line = lineno;
      if (lineno > fi->max_line)
        fi->max_line = lineno;
    }
  }

  for (FunctionInfo& inline_child_fi : fi->inline_children) {
    if (!inline_child_fi.inline_caller.file || strcmp(inline_child_fi.inline_caller.file, fi->filename))
      continue;

    if ((int)inline_child_fi.inline_caller.line > fi->max_line)
      fi->max_line = inline_child_fi.inline_caller.line;
  }
}

static struct FunctionInfo *vmlinux_funcinfo_by_entry_addr(unsigned long addr) {
  if (func_by_addr.contains(addr))
    return &func_by_addr[addr];

  struct FunctionInfo *fi = &func_by_addr[addr];
  fi->machine_func = fi;
  fi->entry_addr = addr;
  Dwarf_Die cudie;
  if (dwarf_addrdie(vmlinux_dwarf, addr-1, &cudie) == NULL) {
    fi->name = "<no CU>";
    fprintf(stderr, "no CU for %lx\n", addr);
    return fi;
  }
  fi->cudie = cudie;

  printf("has die?\n");
  std::optional<Dwarf_Die> funcdie = funcdie_by_addr(&cudie, addr-1);
  if (!funcdie) {
    printf("no die?\n");
    fi->name = "<getting DIE failed>";
    return fi;
  }
  fi->prog_die = *funcdie;
  populate_func_name(fi);
  printf("has cu?\n");
  if (fi->prog_die.cu)
    populate_func_with_dwarf(fi);
  return fi;
}

static unsigned long get_die_address(Dwarf_Die *die) {
  Dwarf_Attribute loc_attr;
  if (dwarf_attr_integrate(die, DW_AT_location, &loc_attr) == NULL)
    errx(1, "%s: no location attribute", __func__);
  Dwarf_Op *loc_expr;
  size_t loc_expr_len;
  if (dwarf_getlocation(&loc_attr, &loc_expr, &loc_expr_len) != 0)
    errx(1, "%s: getlocation failed", __func__);
  if (loc_expr_len < 1)
    errx(1, "%s: loc_expr_len < 1", __func__);
  Dwarf_Attribute var_loc_attr_decoded;
  if (dwarf_getlocation_attr(&loc_attr, loc_expr, &var_loc_attr_decoded))
    errx(1, "%s: dwarf_getlocation_attr", __func__);
  Dwarf_Addr addr;
  if (dwarf_formaddr(&var_loc_attr_decoded, &addr))
    errx(1, "%s: dwarf_formaddr", __func__);
  return addr;
}

static std::optional<Range> vmlinux_get_var_range(const char *name) {
  Dwarf_CU *cu = nullptr;
  Dwarf_Die cudie;
  std::optional<Range> result;
  while (dwarf_get_units(vmlinux_dwarf, cu, &cu, NULL, NULL, &cudie, NULL) == 0) {
    Dwarf_Die var_die;
    if (dwarf_getscopevar(&cudie, 1, name, 0, NULL, 0, 0, &var_die) != 0)
      continue;

    // get size
    Dwarf_Attribute type_attr;
    if (dwarf_attr_integrate(&var_die, DW_AT_type, &type_attr) == nullptr)
      continue;
    Dwarf_Die type_die;
    if (dwarf_formref_die(&type_attr, &type_die) == nullptr)
      continue;
    Dwarf_Word size;
    dwarf_aggregate_size(&type_die, &size);

    if (result)
      errx(1, "duplicate result for '%s'", name);
    unsigned long address = get_die_address(&var_die);
    result = Range(address, address+size-1);
  }

  if (result)
    printf("'%s' is 0x%lx (size 0x%lx)\n", name, result->start, result->end - result->start + 1);
  else
    printf("unable to find '%s'\n", name);
  return result;
}

static unsigned long vmlinux_get_func_addr(const char *name) {
  Dwarf_CU *cu = nullptr;
  Dwarf_Die cudie;
  std::optional<unsigned long> result;
  while (dwarf_get_units(vmlinux_dwarf, cu, &cu, NULL, NULL, &cudie, NULL) == 0) {
    Dwarf_Die child;
    if (dwarf_child(&cudie, &child) == 0) {
      do {
        if (dwarf_tag(&child) != DW_TAG_subprogram)
          continue;
        if (strcmp(dwarf_diename(&child), name) != 0)
          continue;
        if (result)
          errx(1, "duplicate result for '%s'", name);
        Dwarf_Addr entry_addr;
        if (dwarf_entrypc(&child, &entry_addr))
          continue; // probably an extern declaration?
        result = entry_addr;
      } while (dwarf_siblingof(&child, &child) == 0);
    }
  }

  if (!result)
    errx(1, "unable to find '%s'\n", name);
  printf("'%s' is 0x%lx\n", name, *result);
  return *result;
}

struct FunctionInfoView {
  FunctionInfoView(FunctionInfo *backing) : backing(backing) {
    if (backing) {
      line_cov_by_va = backing->line_cov_by_va;
      for (auto& elem : line_cov_by_va) {
        for (auto& covpoint : elem.second) {
          line_cov_by_line[covpoint.lineno].push_back(&covpoint);
        }
      }
    }
  }
  struct FunctionInfo *backing;

  std::unordered_map<struct FunctionInfo*, struct FunctionInfoView> active_inline_children;
  std::unordered_set<struct FunctionInfo*> active_inline_children_set;
  std::vector<struct StableGraphPosElem> active_machine_children;
  std::map<unsigned long, std::vector<struct memory_access_record*>> mars;

  std::unordered_map<int, std::vector<LineCoveragePoint*>> line_cov_by_line;
  std::unordered_map<unsigned long, std::vector<LineCoveragePoint>> line_cov_by_va;

  struct FunctionInfoView *inline_by_fi(struct FunctionInfo *fi) {
    auto [elem, inserted] = active_inline_children.try_emplace(fi, fi);
    return &elem->second;
  }
  struct FunctionInfoView *inline_by_addr(unsigned long addr, bool make_visible) {
    if (!backing)
      return nullptr;
    struct FunctionInfo *child = backing->child_by_va(addr);
    if (!child)
      return this;
    if (make_visible)
      active_inline_children_set.insert(child);
    return inline_by_fi(child)->inline_by_addr(addr, make_visible);
  }
};

struct StableGraphPosElem {
  struct FunctionInfo *fi;
  uint64_t parent_idx;
  size_t thread_idx;
  bool is_spooky;
  unsigned long caller_ip = 0;
  StableGraphPosElem(size_t thread_idx) : fi(nullptr), parent_idx(0), thread_idx(thread_idx), is_spooky(false) {}
  StableGraphPosElem(size_t thread_idx, struct FunctionInfo *fi, uint64_t parent_idx, bool is_spooky) :
      fi(fi), parent_idx(parent_idx), thread_idx(thread_idx), is_spooky(is_spooky) {}
  bool operator==(const StableGraphPosElem &other) const {
    return fi == other.fi &&
        parent_idx == other.parent_idx &&
        thread_idx == other.thread_idx;
  }
};
struct StableGraphPos {
  std::vector<StableGraphPosElem> elems;
  size_t thread_idx = SIZE_MAX;
  size_t match_depth, nomatch_depth, nomatch_depth_noinline;
  bool ready = true;
  void track_reset() {
    match_depth = 0;
    nomatch_depth = 0;
    nomatch_depth_noinline = 0;
  }
  void track_enter(StableGraphPosElem e) {
    if (thread_idx != e.thread_idx)
      return;
    if (!ready)
      return;

    if (nomatch_depth || match_depth >= elems.size() || e != elems[match_depth]) {
      nomatch_depth++;
      if (e.fi && !e.fi->is_inline)
        nomatch_depth_noinline++;
    } else {
      match_depth++;
    }
  }
  void track_exit(StableGraphPosElem e) {
    if (thread_idx != e.thread_idx)
      return;
    if (!ready)
      return;

    if (nomatch_depth) {
      nomatch_depth--;
      if (e.fi && !e.fi->is_inline)
        nomatch_depth_noinline--;
    } else {
      assert(match_depth > 0);
      match_depth--;
    }
  }
  bool inside_match(size_t cur_thread_idx) { return ready && cur_thread_idx == thread_idx && match_depth == elems.size(); }
  bool matching(size_t cur_thread_idx) { return inside_match(cur_thread_idx) && nomatch_depth == 0; }
  bool matching_with_inline(size_t cur_thread_idx) { return inside_match(cur_thread_idx) && nomatch_depth_noinline == 0; }

  /*
  bool matching_fuzzyinline() { return nomatch_depth == 0 && match_depth == elems.size(); }
  size_t inline_count = 0;
  void calc_inline_len() {
    for (size_t i = elems.size()-1; i >= 0 && elems[i].fi && elems[i].fi->is_inline)
      inline_count++;
  }
  */
};
static StableGraphPos stable_selected;

static bool is_item_clicked_notify(ImGuiMouseButton mouse_button = ImGuiMouseButton_Left) {
  bool result = ImGui::IsItemClicked(mouse_button);
  if (result)
    glfwPostEmptyEvent();
  return result;
}

const float STACK_INDENT = 10;
const ImVec4 INLINE_FUNC_COLOR(0, 0.8, 0, 1);
const ImVec4 MEMORY_ACCESS_COLOR(0, 0, 0.8, 1);
const ImVec4 SOURCE_MEMORY_ACCESS_COLOR(1, 0, 0, 1);
const ImVec4 SOURCE_INTERFERENCE_COLOR(1, 0.5, 0, 1);
const ImVec4 SOURCE_CALL_COLOR(0, 0, 1, 1);

struct StackGraphElem : StableGraphPosElem {
  std::unordered_map<struct FunctionInfo *, uint64_t> child_counts;
};
struct StackGraphState {
private:
  size_t thread_idx;
  std::vector<struct StackGraphElem> full_stack = {StackGraphElem(thread_idx)}; // ORDER DEPENDENCY
  int depth_emitted = 1;
  //int selected_depth = -1;
  void invariant_check() {
    if (stable_selected.ready && stable_selected.thread_idx == thread_idx) {
      assert(full_stack.size() - 1 == stable_selected.match_depth + stable_selected.nomatch_depth);
    }
  }
  void pop_internal(int count) {
    for (int i=0; i<count; i++) {
      invariant_check();
      stable_selected.track_exit(full_stack.back());
      full_stack.pop_back();
      invariant_check();
    }
    post_trunc();
  }

public:
  StableGraphPosElem& last_elem() {
    assert(full_stack.size() >= 2);
    return full_stack.back();
  }
  StackGraphState(size_t thread_idx) : thread_idx(thread_idx) {}
  std::vector<FunctionInfo*> get_real_func_stack() {
    std::vector<FunctionInfo*> result;
    for (size_t i=1; i < full_stack.size(); i++) {
      if (!full_stack[i].fi || full_stack[i].fi->is_inline)
        continue;
      result.push_back(full_stack[i].fi);
    }
    return result;
  }
  void post_trunc() {
    depth_emitted = std::min(depth_emitted, (int)full_stack.size());
  }
  void select_inline_addr(unsigned long addr) {
    assert(full_stack.size() > 0);
    invariant_check();
    if (full_stack.size() == 1)
      return;

    auto iter = full_stack.end()-1;
    int num_unconfirmed = 0;
    while (iter->fi && iter->fi->is_inline) {
      iter--;
      num_unconfirmed++;
    }
    bool is_spooky = full_stack[full_stack.size() - num_unconfirmed - 1].is_spooky;
    //fprintf(stderr, "full_stack.size()=%zu num_unconfirmed=%d\n", full_stack.size(), num_unconfirmed);
    struct FunctionInfo *fullfunc = iter->fi;
    if (fullfunc == nullptr)
      return;
    // stop using iter from here on
    struct FunctionInfo *fi_new = fullfunc;
    while (true) {
      fi_new = fi_new->child_by_va(addr);
      if (num_unconfirmed) {
        if (fi_new == nullptr) {
          pop_internal(num_unconfirmed);
          break;
        } else if (full_stack[full_stack.size() - num_unconfirmed].fi == fi_new) {
          num_unconfirmed--;
        } else {
          pop_internal(num_unconfirmed);
          num_unconfirmed = 0;
          invariant_check();
          full_stack.emplace_back(StableGraphPosElem(thread_idx, fi_new, 0, is_spooky));
          stable_selected.track_enter(full_stack.back());
          invariant_check();
        }
      } else {
        if (fi_new == nullptr) {
          break;
        } else {
          invariant_check();
          full_stack.emplace_back(StableGraphPosElem(thread_idx, fi_new, 0, is_spooky));
          stable_selected.track_enter(full_stack.back());
          invariant_check();
        }
      }
    }
  }
  void push_fullfunc(struct FunctionInfo *fi, unsigned long caller_ip) {
    StackGraphElem &parent = full_stack.back();
    invariant_check();
    bool is_spooky = fi && parent.fi && (!parent.fi->ip_ranges.has(caller_ip) || full_stack.back().is_spooky);
    full_stack.emplace_back(StableGraphPosElem(thread_idx, fi, full_stack.back().child_counts[fi]++, is_spooky));
    stable_selected.track_enter(full_stack.back());
    invariant_check();
    /*
    if (stable_selected.matching()) {
      selected_depth = 0;
    } else if (selected_depth >= 0) {
      selected_depth++;
    }
    */
  }
  void pop_fullfunc() {
    while (full_stack.size()>1 && full_stack.back().fi && full_stack.back().fi->is_inline)
      pop_internal(1);
    if (full_stack.size()==1) {
      fprintf(stderr, "stack underflow?\n");
      depth_emitted = 0;
      return;
    }
    pop_internal(1);
    /*
    if (selected_depth >= 0)
      selected_depth--;
      */
  }
  /*
  bool cur_selected() {
    return selected_depth == 0;
  }
  */
  void draw_line_tab(int depth, bool hilight) {
    if (depth == -1)
      depth = (full_stack.size()-1);

    // first part: indent based on thread ID
    ImGui::Dummy(ImVec2(thread_idx * 40, ImGui::GetTextLineHeight()));
    ImGui::SameLine(0, 0);

    // second part: graph indent
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();

    ImVec2 tab_size(depth * STACK_INDENT, ImGui::GetTextLineHeight());
    ImVec2 p1(p0.x + tab_size.x, p0.y + ImGui::GetTextLineHeightWithSpacing());
    ImVec4 color;
    switch (thread_idx % 4) {
    case 0: color = ImVec4(0  , 0  , 0  , 1); break;
    case 1: color = ImVec4(1  , 0  , 0  , 1); break;
    case 2: color = ImVec4(0.3, 0.3, 0.3, 1); break;
    case 3: color = ImVec4(1  , 0.3, 0.3, 1); break;
    }
    if (hilight)
      color = ImVec4(1, 1, 0, 1);
    draw_list->AddRectFilled(p0, p1, ImGui::GetColorU32(color));
    for (int tabi = 1; tabi < depth; tabi++) {
      float line_x = p0.x + tabi * STACK_INDENT;
      ImVec2 line_p1(line_x, p0.y);
      ImVec2 line_p2(line_x+2, p1.y);
      draw_list->AddRectFilled(line_p1, line_p2, ImGui::GetColorU32(ImVec4(0.5, 0.5, 0.5, 1)));
    }
    ImGui::Dummy(tab_size);
    ImGui::SameLine(0, 0);
  }
  bool in_spooky() {
    return full_stack.back().is_spooky;
  }
  void emit(bool with_inline_tail = true) {
    int limit = (int)full_stack.size();
    if (!with_inline_tail) {
      while (full_stack.size() > 1 && full_stack[limit-1].fi->is_inline)
        limit--;
    }
    while (depth_emitted < limit) {
      struct FunctionInfo *fi = full_stack[depth_emitted].fi;
      if (!fi) {
        depth_emitted++;
        continue;
      }

      draw_line_tab(depth_emitted-1, false);

      //bool sel = depth_emitted == ((int)full_stack.size() - 1 - selected_depth);
      bool sel = stable_selected.inside_match(thread_idx) && stable_selected.nomatch_depth == (full_stack.size() - 1 - depth_emitted);
      ImVec4 line_color =
        sel ? ImVec4(1, 0, 0, 1) :
        fi->is_inline ? INLINE_FUNC_COLOR :
        ImVec4(0, 0, 0, 1);
      ImGui::TextColored(line_color, "%s", fi->name.c_str());
      if (is_item_clicked_notify()) {
        printf("clicked %s (current: m=%zu/%zu nm=%zu)\n", fi->name.c_str(), stable_selected.match_depth, stable_selected.elems.size(), stable_selected.nomatch_depth);
        stable_selected.ready = false;
        stable_selected.elems.clear();
        for (int i=1; i<depth_emitted+1; i++)
          stable_selected.elems.push_back(full_stack[i]);
        stable_selected.thread_idx = thread_idx;
      }

      depth_emitted++;
    }
  }
};

void ensure_initial_pass(KcovTraceSet *ts) {
  if (ts->initial_pass_done)
    return;
  ts->initial_pass_done = true;
  trace_set_to_interference_sets(ts);
}

static std::vector<kcov_di_stack_elem> make_distack(KcovTraceIter *ti_orig, struct StackGraphState *gs_orig) {
  std::vector<FunctionInfo*> real_stack = gs_orig->get_real_func_stack();
  std::vector<int> parent_indices(real_stack.size()+1, -1);
  KcovTraceIter ti(ti_orig->thread_idx, ti_orig->trace);
  int match_depth = 0;
  int skip_depth = 0;
  for (; ti.get_index() <= ti_orig->get_index(); ti.advance()) {
    assert(!ti.at_end());
    switch (ti.record_type()) {
      case KCOV_RECORDFLAG_TYPE_ENTRY: {
        if (skip_depth > 0 || (size_t)match_depth >= real_stack.size() || real_stack[match_depth]->entry_addr != ti.ip_address()) {
          skip_depth++;
          continue;
        }
        parent_indices[match_depth]++;
        match_depth++;
        for (size_t i=match_depth; i<parent_indices.size(); i++)
          parent_indices[i] = -1;
        continue;
      }
      case KCOV_RECORDFLAG_TYPE_EXIT: {
        if (skip_depth > 0) {
          skip_depth--;
          continue;
        }
        assert(match_depth > 0);
        match_depth--;
        break;
      }
      case KCOV_RECORDFLAG_TYPE_EESUM: {
        if (-ti.eesum_mindelta() > (int)skip_depth) {
          int new_skip = (-ti.eesum_mindelta()) - skip_depth;
          skip_depth += new_skip;
          match_depth -= new_skip;
          assert(match_depth >= 0);
        }
        skip_depth += ti.eesum_delta();
        if (skip_depth < 0) {
          match_depth += skip_depth;
          assert(match_depth >= 0);
          skip_depth = 0;
        }
        break;
      }
      case KCOV_RECORDFLAG_TYPE_MEMORY: {
        if (skip_depth > 0 || (size_t)match_depth != real_stack.size())
          continue;
        if (ti.ip_address() != ti_orig->ip_address())
          continue;
        parent_indices[match_depth]++;
        if (ti.get_index() != ti_orig->get_index())
          continue;

        std::vector<kcov_di_stack_elem> di_stack;
        for (size_t i=0; i<real_stack.size(); i++)
          di_stack.push_back((struct kcov_di_stack_elem){.ip = real_stack[i]->entry_addr, .parent_idx = (unsigned int)parent_indices[i]});
        di_stack.push_back((struct kcov_di_stack_elem){.ip = ti_orig->ip_address(), .parent_idx = (unsigned int)parent_indices[real_stack.size()]});
        return di_stack;
      }
    }
  }
  assert(0);
}

static RangeSet ignored_data_ranges;

int main(int argc, char **argv) {
  if (argc != 2)
    errx(1, "bad invocation");
  vmlinux_fd = SYSCHK(open(argv[1], O_RDONLY));
  vmlinux_dwarf = dwarf_begin(vmlinux_fd, DWARF_C_READ);

  FUNC__sanitizer_cov_trace_pc = vmlinux_get_func_addr("__sanitizer_cov_trace_pc");
  FUNC__sanitizer_cov_trace_pc_entry = vmlinux_get_func_addr("__sanitizer_cov_trace_pc_entry");

  //vmlinux_funcinfo_by_entry_addr(vmlinux_get_func_addr("exit_files")+1);
  //exit(0);

  auto rcu_state_range = vmlinux_get_var_range("rcu_state");
  if (rcu_state_range)
    ignored_data_ranges.add(*rcu_state_range);

  listen_init();
  pthread_t listen_thread;
  if (pthread_create(&listen_thread, NULL, listen_threadfn, NULL))
    errx(1, "pthread_create");

  glfwSetErrorCallback(glfw_error_callback);
  if (!glfwInit())
    return 1;

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

  float main_scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor());
  GLFWwindow* window = glfwCreateWindow((int)(1280 * main_scale), (int)(800 * main_scale), "KCOV viewer", nullptr, nullptr);
  if (window == nullptr)
    return 1;
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1); // Enable vsync

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO(); (void)io;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  ImGui::StyleColorsLight();
  ImGuiStyle& style = ImGui::GetStyle();
  style.ScaleAllSizes(main_scale);
  style.FontScaleDpi = main_scale;

  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init("#version 130");

  std::optional<Range> filter_range;
  std::optional<Range> hover_range, next_hover_range;
  bool show_inline_leaves = false;
  bool interference_set_filter = false;
  bool line_coverage_color = false;

  while (!glfwWindowShouldClose(window))
  {
    glfwWaitEvents();
    if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0)
    {
      ImGui_ImplGlfw_Sleep(10);
      continue;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) && stable_selected.elems.size() > 1) {
      stable_selected.elems.pop_back();
    }

    std::shared_ptr<struct KcovTraceSet> trace_set = current_trace_set;
    hover_range = next_hover_range;
    next_hover_range = std::nullopt;

    // Start the Dear ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("main window", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);

    if (trace_set) {
      ensure_initial_pass(trace_set.get());

      if (trace_set->traces.size() >= 2) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(filter_range ? ImGuiCol_TextDisabled : ImGuiCol_Text));
        ImGui::Checkbox("filter to communication points [?]", &interference_set_filter);
        ImGui::PopStyleColor();

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNone) && ImGui::BeginTooltip()) {
          ImGui::Text("Filter the trace to show only memory accesses which");
          ImGui::Text("could cause different behavior if they were reordered.");
          ImGui::Text("That means:");
          ImGui::Text(" ");
          ImGui::Text(" - Any memory access if another execution context writes/frees the same memory location.");
          ImGui::Text(" - Any write/free operation if another execution context accesses the same memory location.");
          ImGui::Text(" ");
          ImGui::Text("Also show the call graphs leading to such memory accesses.");
          ImGui::Text("This has no effect while a DATA FILTER with a specific address is applied (see right of here).");
          ImGui::EndTooltip();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_Separator), " | ");
        ImGui::SameLine();
      } else {
        interference_set_filter = false;
      }

      if (filter_range) {
        ImGui::Text("DATA FILTER: %lx/%lx [click to clear]", filter_range->start, filter_range->end+1-filter_range->start);
        if (is_item_clicked_notify())
          filter_range = std::nullopt;
      } else {
        if (interference_set_filter) {
          ImGui::Text("DATA FILTER: interference");
        } else {
          ImGui::Text("DATA FILTER: none");
        }
      }
      ImGui::SameLine();
      ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_Separator), " | ");
      ImGui::SameLine();

      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4((filter_range || interference_set_filter) ? ImGuiCol_TextDisabled : ImGuiCol_Text));
      ImGui::Checkbox("all inline functions", &show_inline_leaves);
      ImGui::PopStyleColor();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNone) && ImGui::BeginTooltip()) {
        ImGui::Text("Show all inline functions (which perform at least one memory access).");
        ImGui::Text("Typically too spammy for anything other than toy examples.");
        ImGui::Text("Only effective when no filter is enabled.");
        ImGui::EndTooltip();
      }
      ImGui::SameLine();
      ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_Separator), " | ");
      ImGui::SameLine();

      ImGui::Text("<HOVER MOUSE HERE FOR HELP>");
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNone) && ImGui::BeginTooltip()) {
          ImGui::SeparatorText("Left side of screen");
          ImGui::Text("You see interleaved call graphs of traces from the execution contexts, normally:");
          StackGraphState(0).draw_line_tab(3, false);
          ImGui::Text(" Thread 1 is shown with black indent.");
          StackGraphState(1).draw_line_tab(3, false);
          ImGui::Text(" Thread 2 is shown with red indent.");
          StackGraphState(2).draw_line_tab(3, false);
          ImGui::Text(" Background work from thread 1 (if any) is shown with gray indent.");
          ImGui::Text("Normal functions are shown in black.");
          ImGui::TextColored(INLINE_FUNC_COLOR, "Inline functions are shown in green.");
          ImGui::Text("Left-click a function to show it in the source code view.");
          ImGui::Text("There are two types of filtered views:");
          ImGui::Text(" - tick 'filter to communication points' to show all communication points");
          ImGui::Text(" - left-click on a memory access (either in this view or in the 'source code' view to show all overlapping accesses");
          ImGui::TextColored(MEMORY_ACCESS_COLOR, "Memory accesses are shown in blue once a filter is applied.");
          StackGraphState(0).draw_line_tab(3, true);
          ImGui::TextColored(MEMORY_ACCESS_COLOR, "Hovering over a memory access highlights all overlapping accesses.");
          ImGui::TextColored(MEMORY_ACCESS_COLOR, "Memory access lines contain the components:");
          ImGui::TextColored(MEMORY_ACCESS_COLOR, " - access type flags:");
          ImGui::TextColored(MEMORY_ACCESS_COLOR, "   - 'R' for read");
          ImGui::TextColored(MEMORY_ACCESS_COLOR, "   - 'W' for write");
          ImGui::TextColored(MEMORY_ACCESS_COLOR, "   - 'F' for free");
          ImGui::TextColored(MEMORY_ACCESS_COLOR, " - data address and access size (in the format `<address>/<size>`)");
          ImGui::TextColored(MEMORY_ACCESS_COLOR, " - data value *before* the access (in the format `old=<value>`)");
          ImGui::Text(" ");
          ImGui::Text("Note that the trace is only ordered based on the timings of memory accesses.");
          ImGui::Text("Other events are placed based on when the next memory access happened.");

          ImGui::SeparatorText("Right side of screen: ordering constraints");
          ImGui::Text("Shows ordering constraints that will be forced the next time you re-run the testcase in the guest.");
          ImGui::Text("Populated by right-clicking on memory accesses in the left-side menu.");
          ImGui::Text("The order of elements can be swapped.");

          ImGui::SeparatorText("Right side of screen: source code");
          ImGui::Text("Displays source code for the function that was left-clicked in the left-side view.");
          ImGui::Text("  Note that sometimes the displayed lines may not perfectly contain the entire function in the source file.");
          ImGui::Text("Code coverage is shown through coloring of source lines, but this is very approximate.");
          ImGui::Text("Information about memory accesses is shown below code lines:");
          ImGui::TextColored(SOURCE_MEMORY_ACCESS_COLOR, "    Reads show the value that was read, writes show WRITE.");
          ImGui::TextColored(SOURCE_INTERFERENCE_COLOR, "    Accesses that could interfere with ones in another context are marked INTERFERENCE.");
          ImGui::Text("    Clicking an access filters the left-side trace view to accesses that overlap with this access.");
          ImGui::TextColored(SOURCE_CALL_COLOR, "Calls that have executed are shown in blue.");
          ImGui::TextColored(SOURCE_CALL_COLOR, "Click on a call to step into the callee.");

          ImGui::SeparatorText("Keyboard bindings");
          ImGui::Text("ARROW LEFT: go up one function in the call graph");

          ImGui::EndTooltip();
      }

      ImGui::BeginChild("trace-data", ImVec2(ImGui::GetContentRegionAvail().x * 0.5, 0), 0, ImGuiWindowFlags_HorizontalScrollbar);
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 1));

      KcovTraceIterSet tis(trace_set.get());
      std::unordered_map<struct KcovTraceIter *, struct StackGraphState> gss;
      for (KcovTraceIter &ti : tis.iters) {
        gss.emplace(&ti, ti.thread_idx);
      }

      stable_selected.track_reset();
      stable_selected.ready = true;

      struct FunctionInfoView selected_fi(stable_selected.elems.size() ? stable_selected.elems.back().fi : nullptr);

      while (!tis.at_end()) {
        KcovTraceIter &ti = *tis.cur_iter();
        struct StackGraphState &gs = gss.at(&ti);

        switch (ti.record_type()) {
        case KCOV_RECORDFLAG_TYPE_NORMAL: {
          gs.select_inline_addr(ti.ip_address()-1);
          FunctionInfoView *inline_fi = selected_fi.inline_by_addr(ti.ip_address()-1, false);
          if (inline_fi && inline_fi->line_cov_by_va.contains(ti.ip_address())) {
            auto& covs = inline_fi->line_cov_by_va.at(ti.ip_address());
            for (auto& cov : covs)
              cov.count++;
          }
          else if (selected_fi.backing && stable_selected.matching_with_inline(ti.thread_idx))
            printf("WARNING: %s: no %lx\n", selected_fi.backing->name.c_str(), ti.ip_address());
          break;
        }
        case KCOV_RECORDFLAG_TYPE_ENTRY: {
          struct FunctionInfo *fi = vmlinux_funcinfo_by_entry_addr(ti.ip_address());

          bool suppress = fi->name == "__is_insn_slot_addr" ||
            fi->name == "is_bpf_text_address" ||
            fi->name == "in_gate_area_no_mm" ||
            fi->name == "is_vmalloc_addr";

          gs.select_inline_addr(ti.caller_ip()-1);
          selected_fi.inline_by_addr(ti.caller_ip()-1, true);
          bool caller_is_selected = stable_selected.matching(ti.thread_idx);
          gs.push_fullfunc(fi, ti.caller_ip()-1);
          if (!suppress && !filter_range && !interference_set_filter && !gs.in_spooky())
            gs.emit();
          if (!suppress && selected_fi.backing && caller_is_selected && !gs.in_spooky()) {
            StableGraphPosElem gpe = gs.last_elem();
            gpe.caller_ip = ti.caller_ip()-1;
            selected_fi.active_machine_children.push_back(gpe);
          }

          break;
        }
        case KCOV_RECORDFLAG_TYPE_EXIT:
          gs.pop_fullfunc();
          break;
        case KCOV_RECORDFLAG_TYPE_EESUM:
          if (ti.eesum_mindelta() > 0)
            errx(1, "eesum_mindelta positive!");
          if (ti.eesum_mindelta() > ti.eesum_delta())
            errx(1, "eesum_mindelta not minimum!");
          for (int i=0; i<-ti.eesum_mindelta(); i++)
            gs.pop_fullfunc();
          for (int i=0; i<ti.eesum_delta()-ti.eesum_mindelta(); i++)
            gs.push_fullfunc(nullptr, 0);
          //ImGui::Text("EESUM delta=%hd mindelta=%hd", ti.eesum_delta(), ti.eesum_mindelta());
          break;
        case KCOV_RECORDFLAG_TYPE_MEMORY: {
          gs.select_inline_addr(ti.ip_address()-1);
          bool hidden_spooky = gs.in_spooky() && (ti.mar()->flags & MEMORY_ACCESS_RECORD_FREE) == 0;
          if ((filter_range || interference_set_filter) && !hidden_spooky) {
            bool want_this = (!filter_range) ?
                trace_set->has_interference(ti.thread_idx, ti.mar()) :
                ((*filter_range <=> Range(ti.mar())) == std::weak_ordering::equivalent);

            if (want_this && ignored_data_ranges.has(ti.mar()->data_address))
              want_this = false;
            if (gs.in_spooky())
              want_this = false;

            if (want_this) {
              gs.emit();

              bool hilight = hover_range && *hover_range <=> Range(ti.mar()) == std::weak_ordering::equivalent;
              gs.draw_line_tab(-1, hilight);
              ImGui::TextColored(MEMORY_ACCESS_COLOR, "MEM %c%c%c %016lx/%x old=0x%lx",
                ((ti.mar()->flags & (MEMORY_ACCESS_RECORD_WRITE)) == 0) ? 'R' : '-',
                (ti.mar()->flags & (MEMORY_ACCESS_RECORD_WRITE|MEMORY_ACCESS_RECORD_RMW)) ? 'W' : '-',
                (ti.mar()->flags & MEMORY_ACCESS_RECORD_FREE) ? 'F' : '-',
                (unsigned long)ti.mar()->data_address, ti.mar()->size, (unsigned long)ti.mar()->value);
              if (ImGui::IsItemHovered())
                next_hover_range = Range(ti.mar());
              if (is_item_clicked_notify())
                filter_range = Range(ti.mar());
              if (is_item_clicked_notify(ImGuiMouseButton_Right)) {
                std::vector<kcov_di_stack_elem> distack = make_distack(&ti, &gs);
                std::shared_ptr<struct DIStacksState> distate = current_distate;
                // COW
                distate = std::make_shared<struct DIStacksState>(*distate);
                distate->handle_addition(ti.thread_idx, distack);
                current_distate = distate;
              }
            }
          } else if (!hidden_spooky) {
            if (stable_selected.matching_with_inline(ti.thread_idx))
              gs.emit();
            else if (show_inline_leaves)
              gs.emit(show_inline_leaves);
          }
          /*
          if (stable_selected.matching()) assert(stable_selected.matching_with_inline());
          if (selected_fi.backing && stable_selected.matching_with_inline()) {
            printf("MAR - ip_ranges(%zu) says %d\n", selected_fi.backing->ip_ranges.ranges.size(), selected_fi.backing->ip_ranges.has(ti.ip_address()-1));
            printf("%016lx\n", ti.ip_address()-1);
            for (Range range : selected_fi.backing->ip_ranges.ranges) {
              printf("%016lx-%016lx\n", range.start, range.end);
            }
          }
          */
          if (selected_fi.backing && stable_selected.matching_with_inline(ti.thread_idx) && selected_fi.backing->ip_ranges.has(ti.ip_address()-1)) {
            selected_fi.inline_by_addr(ti.ip_address()-1, true)->mars[ti.ip_address()-1].push_back(ti.mar());
          }
          break;
        }
        case KCOV_RECORDFLAG_TYPE_WAIT:
          gs.emit();
          gs.draw_line_tab(-1, false);
          ImGui::TextColored(ImVec4(0.5, 0.5, 0, 1), "DELAY INJECTION: WAIT (bit %u, %s)",
              ti.wakewait_bit_idx(), ti.wait_timed_out() ? "TIMEOUT" : "OK");
          break;
        case KCOV_RECORDFLAG_TYPE_WAKE:
          gs.emit();
          gs.draw_line_tab(-1, false);
          ImGui::TextColored(ImVec4(0.5, 0.5, 0, 1), "DELAY INJECTION: WAKE (bit %u)",
              ti.wakewait_bit_idx());
          break;
        default:
          errx(1, "unexpected record type 0x%lx", ti.record_type());
        }
        ti.advance();
      }
      ImGui::PopStyleVar();
      ImGui::EndChild();


      ImGui::SameLine();
      ImGui::BeginChild("right-side");

      ImGui::SeparatorText("ordering constraints");
      if (ImGui::Button("clear")) {
        current_distate = std::make_shared<struct DIStacksState>();
      }
      {
        std::shared_ptr<struct DIStacksState> distate = current_distate;
        for (size_t i = 0; i < distate->distacks.size();) {
        //for (DIStack& dis : distate->distacks) {
          char del_button_label[100];
          sprintf(del_button_label, "delete-%zu", i);
          if (ImGui::Button(del_button_label)) {
            distate->distacks.erase(distate->distacks.begin() + i);
            continue;
          }
          DIStack &dis = distate->distacks[i];
          for (size_t thread_idx = 0; thread_idx < 2; thread_idx++) {
            if (dis.thread_distacks[thread_idx].size() == 0) {
              ImGui::Text("<?>");
            } else {
              ImGui::Text("[T%zu]", dis.thread_indices[thread_idx]);
              //ImGui::SameLine();
              auto& stack = dis.thread_distacks[thread_idx];
              for (size_t i = 0; i<stack.size(); i++) {
                struct kcov_di_stack_elem &elem = stack[i];
                const char *name = (i == stack.size()-1) ? "<ACC>" : vmlinux_funcinfo_by_entry_addr(elem.ip)->name.c_str();
                ImGui::Dummy(ImVec2(i * 20, ImGui::GetTextLineHeight()));
                ImGui::SameLine(0, 0);
                ImGui::Text("%s:%u", name, (unsigned int)elem.parent_idx);
                //ImGui::SameLine();
              }
              //ImGui::NewLine();
            }

            if (thread_idx == 0) {
              ImGui::Text("  ==>  ");
              if (is_item_clicked_notify())
                dis.swap();
            }
          }

          ImGui::Text(" "); // hack...
          i++;
        }
      }
      ImGui::SeparatorText("source code");
      ImGui::Checkbox("color source lines based on code coverage (UNRELIABLE)", &line_coverage_color);
      ImGui::BeginChild("source-code", ImVec2(0, 0), 0, ImGuiWindowFlags_HorizontalScrollbar);
      if (selected_fi.backing) {
        ensure_func_srcinfo(selected_fi.backing);

        std::map<int, std::map<int, std::vector<std::vector<struct memory_access_record*>>>> mars_by_loc;
        std::map<int, std::map<int, std::vector<StableGraphPosElem>>> calls_by_loc;
        for (FunctionInfo *child_info : selected_fi.active_inline_children_set) {
          if (child_info->is_inline && child_info->inline_caller.file &&
                  strcmp(child_info->inline_caller.file, selected_fi.backing->filename) == 0) {
            int linecol = selected_fi.backing->fixup_column_tabs(child_info->inline_caller.line, child_info->inline_caller.column);
            StableGraphPosElem sgpe(stable_selected.thread_idx, child_info, 0, false);
            calls_by_loc[child_info->inline_caller.line][linecol].push_back(sgpe);
          }
        }
        for (StableGraphPosElem& sgpe : selected_fi.active_machine_children) {
          SourcePoint srcp(selected_fi.backing, sgpe.caller_ip);
          if (srcp.valid)
            calls_by_loc[srcp.line][srcp.column_fixedwidth].push_back(sgpe);
        }
        for (auto [mar_ip, mar_vec] : selected_fi.mars) {
          selected_fi.inline_by_addr(mar_ip, false);

          SourcePoint srcp(selected_fi.backing, mar_ip);
          if (!srcp.valid || srcp.line <= 0) {
            ImGui::Text("MAR in %s (%lx)", srcp.file?:"NULL", mar_ip);
            continue;
          }
          mars_by_loc[srcp.line][srcp.column_fixedwidth].push_back(mar_vec);
        }
        std::unordered_set<int> covered_lines;
        ImGui::Text("%s  %d-%d", selected_fi.backing->filename, selected_fi.backing->min_line, selected_fi.backing->max_line);
        std::vector<std::string>& lines = selected_fi.backing->get_lines();
        for (int i=0; i<(int)lines.size(); i++) {
          int lineno = selected_fi.backing->min_line+i;
          ImVec4 line_color;
          if (!line_coverage_color || !selected_fi.line_cov_by_line.contains(lineno)) {
            line_color = ImVec4(0, 0, 0, 1); // black
          } else {
            auto& points = selected_fi.line_cov_by_line.at(lineno);
            int num_points = points.size();
            int num_covered = 0;
            for (auto& point : points) {
              if (point->count > 0)
                num_covered++;
            }
            if (num_points == num_covered) {
              line_color = ImVec4(0, 0.8, 0, 1); // green
            } else if (num_covered == 0) {
              line_color = ImVec4(0.8, 0, 0, 1); // red
            } else {
              line_color = ImVec4(0.8, 0.8, 0, 1); // yellow
            }
          }
          ImGui::TextColored(line_color, "%04d  %s", lineno, lines[i].c_str());
          for (auto [col, mar_vecs] : mars_by_loc[lineno]) {
            for (auto mar_vec : mar_vecs) {
              bool first = true;
              for (struct memory_access_record *mar : mar_vec) {
                if (mar->size == 1 || mar->size == 2 || mar->size == 4 || mar->size == 8) {
                  if (first) {
                    ImGui::Text("      %*s", col, "");
                    if (trace_set->has_interference(stable_selected.thread_idx, mar)) {
                      ImGui::SameLine(0, 0);
                      ImGui::TextColored(SOURCE_INTERFERENCE_COLOR, "INTERFERENCE ");
                    }
                    if ((mar->flags & MEMORY_ACCESS_RECORD_WRITE) && !(mar->flags & MEMORY_ACCESS_RECORD_RMW)) {
                      ImGui::SameLine(0, 0);
                      ImGui::TextColored(SOURCE_MEMORY_ACCESS_COLOR, "WRITE");
                      if (is_item_clicked_notify())
                        filter_range = Range(mar);
                      break;
                    }
                  } else {
                    ImGui::SameLine(0, 0);
                    ImGui::TextColored(SOURCE_MEMORY_ACCESS_COLOR, "/");
                  }
                  ImGui::SameLine(0, 0);
                  if (mar->flags & MEMORY_ACCESS_RECORD_VALUE) {
                    ImGui::TextColored(SOURCE_MEMORY_ACCESS_COLOR, "0x%" PRIx64, (uint64_t)mar->value);
                  } else {
                    ImGui::TextColored(SOURCE_MEMORY_ACCESS_COLOR, "<?>");
                  }
                  if (is_item_clicked_notify())
                    filter_range = Range(mar);
                  first = false;
                }
              }
            }
          }
          for (auto [col, calls_by_col] : calls_by_loc[lineno]) {
            for (StableGraphPosElem &sgpe : calls_by_col) {
              ImGui::Text("      %*s", col, "");
              ImGui::SameLine(0, 0);
              if (sgpe.fi->is_inline) {
                ImGui::TextColored(SOURCE_CALL_COLOR, "-> %s (inline)", sgpe.fi->name.c_str());
              } else {
                ImGui::TextColored(SOURCE_CALL_COLOR, "-> %s", sgpe.fi->name.c_str());
              }
              if (is_item_clicked_notify()) {
                stable_selected.ready = false;
                stable_selected.elems.push_back(sgpe);
              }
            }
          }
        }
      }
      ImGui::EndChild();
      ImGui::EndChild();
    } else {
      ImGui::Text("no trace yet (waiting for you to run `kcov-vsock-client testcase/<name>.so` in guest VM for the first time)");
    }

    ImGui::End();
    //ImGui::ShowDemoWindow();

    // Rendering
    ImGui::Render();
    int display_w, display_h;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(window);
  }
  return 0;
}
