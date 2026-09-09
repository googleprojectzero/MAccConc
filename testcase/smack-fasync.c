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

// Testcase for fedc88e38ce ("smack: fix cred UAF in smack_file_send_sigiotask()")

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int pipefds[2];

/* runs in the same thread as test_thread1() */
void test_setup(void) {
  seteuid(1);
  signal(SIGIO, SIG_IGN);
  pipe(pipefds);
  fcntl(pipefds[0], F_SETFL, O_ASYNC);
  struct f_owner_ex owner = {
    .type = F_OWNER_TID,
    .pid = gettid()
  };
  fcntl(pipefds[0], F_SETOWN_EX, &owner);
}
void test_thread1(void) {
  access("/", R_OK);
}
void test_thread2(void) {
  write(pipefds[1], "A", 1);
}
void test_end(void) {}
