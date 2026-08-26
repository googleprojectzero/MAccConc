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
#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int open_res;
static int tmpfd;

void test_setup(void) {
  tmpfd = open("/tmp/", O_PATH);

  mkdir("/tmp/pathwalktest", 0777);
  mkdir("/tmp/pathwalktest/a", 0777);
  mkdir("/tmp/pathwalktest/a/b", 0777);
  mkdir("/tmp/pathwalktest/c", 0777);
}
void test_thread1(void) {
  open_res = openat(tmpfd, "pathwalktest/a/b/../c", O_RDONLY);
}
void test_thread2(void) {
  rename("/tmp/pathwalktest/a/b", "/tmp/pathwalktest/b");
}
void test_end(void) {
  if (open_res >= 0)
    printf("open succeeded!\n");
  else
    printf("open failed\n");
  system("rm -r /tmp/pathwalktest");
}
