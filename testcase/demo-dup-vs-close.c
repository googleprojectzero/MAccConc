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

static int test_fd;
static int dup_res, dup_errno;

void test_setup(void) {
  test_fd = open("/", O_PATH);
}
void test_thread1(void) {
  dup_res = dup(test_fd);
  dup_errno = errno;
}
void test_thread2(void) {
  close(test_fd);
}
void test_end(void) {
  printf("dup(%d) = %d (%s)\n",
      test_fd,
      dup_res,
      dup_res == -1 ? strerror(dup_errno) : "success");
}
