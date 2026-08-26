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
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

static int pipefds[2];
static char *vma;

static int test_res, test_errno;

void test_setup(void) {
  vma = mmap(NULL, 0x200000, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  madvise(vma, 0x200000, MADV_HUGEPAGE);
  vma[0] = 'A';
  pipe2(pipefds, O_NONBLOCK);
  //system("cat /proc/$PPID/smaps | grep -A30 ' rwxp '");
}
void test_thread1(void) {
  struct iovec iov = {
    .iov_base = vma,
    .iov_len = 1
  };
  test_res = vmsplice(pipefds[1], &iov, 1, 0);
  test_errno = errno;
}
void test_thread2(void) {
  munmap(vma+0x1000, 0x1000);
}
void test_end(void) {
  printf("vmsplice() = %d (%s)", test_res, test_res==-1?strerror(test_errno):"success");
  unsigned char spliced_char;
  if (read(pipefds[0], &spliced_char, 1) == 1)
    printf(", spliced 0x%02hhx\n", spliced_char);
  else
    printf(", pipe read failed\n");
}
