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

#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

static int fd1, fd2;
struct stat statbuf = {.st_uid = 1234, .st_gid = 1234};
void test_setup(void) {
  unlink("/tmp/testfile");
  fd1 = open("/tmp/testfile", O_RDWR|O_CREAT, 0666);
  fd2 = open("/tmp/testfile", O_RDWR|O_CREAT, 0666);
  if (fd1 == -1 || fd2 == -1)
    err(1, "open /tmp/testfile failed");
  fstat(fd2, &statbuf); // set I_CTIME_QUERIED
}
void test_thread1(void) {
  fchown(fd1, 1, 1);
}
void test_thread2(void) {
  fstat(fd2, &statbuf);
}
void test_end(void) {
  printf("uid=%u gid=%u\n", statbuf.st_uid, statbuf.st_gid);
}
