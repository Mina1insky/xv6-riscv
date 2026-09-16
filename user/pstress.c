// Pipe stress test for the slab pipe cache.
//
// Covers repeated create/close, several concurrent processes holding
// more than one slab worth of pipes, fork with cross-closed ends, and
// reads/writes after the peer end has been closed.

#include "kernel/types.h"
#include "user/user.h"

static void
fail(char *s)
{
  printf("pstress: %s failed\n", s);
  exit(1);
}

static void
loop_create_close(int n)
{
  int fd[2];
  int i;

  for (i = 0; i < n; i++) {
    if (pipe(fd) < 0)
      fail("pipe");
    close(fd[0]);
    close(fd[1]);
  }
}

// Each process has NOFILE=16 descriptors, so a single process can hold
// only a few pipes.  Concurrent children keep more than one slab of
// pipe objects alive at the same time.
static void
many_alive(int nkids)
{
  int ready[2], go[2];
  int fd[5][2];
  int i, j, pid, st;
  char c;

  if (nkids > 8)
    nkids = 8;
  if (pipe(ready) < 0 || pipe(go) < 0)
    fail("pipe alive sync");

  for (i = 0; i < nkids; i++) {
    pid = fork();
    if (pid < 0)
      fail("fork alive");
    if (pid == 0) {
      close(ready[0]);
      close(go[1]);
      for (j = 0; j < 5; j++)
        if (pipe(fd[j]) < 0)
          fail("pipe alive");
      if (write(ready[1], "r", 1) != 1)
        fail("ready write");
      close(ready[1]);
      if (read(go[0], &c, 1) != 0)
        exit(1);
      for (j = 0; j < 5; j++) {
        close(fd[j][0]);
        close(fd[j][1]);
      }
      close(go[0]);
      exit(0);
    }
  }

  close(ready[1]);
  close(go[0]);
  for (i = 0; i < nkids; i++)
    if (read(ready[0], &c, 1) != 1)
      fail("ready read");
  close(ready[0]);
  close(go[1]); // EOF releases all children
  for (i = 0; i < nkids; i++) {
    wait(&st);
    if (st != 0)
      fail("alive child");
  }
}

static void
fork_pipes(int n)
{
  int fd[2];
  int i, pid, st;
  char c;

  for (i = 0; i < n; i++) {
    if (pipe(fd) < 0)
      fail("pipe fork");
    pid = fork();
    if (pid < 0)
      fail("fork");
    if (pid == 0) {
      close(fd[1]);
      if (read(fd[0], &c, 1) != 1)
        exit(1);
      close(fd[0]);
      exit(0);
    }
    close(fd[0]);
    if (write(fd[1], "x", 1) != 1)
      fail("write fork");
    close(fd[1]);
    wait(&st);
    if (st != 0)
      fail("child");
  }
}

static void
closed_read(void)
{
  int fd[2];

  if (pipe(fd) < 0)
    fail("pipe closed read");
  close(fd[0]);
  if (write(fd[1], "x", 1) != -1)
    fail("write to closed read");
  close(fd[1]);
}

static void
closed_write(void)
{
  int fd[2];
  char c;

  if (pipe(fd) < 0)
    fail("pipe closed write");
  close(fd[1]);
  if (read(fd[0], &c, 1) != 0)
    fail("read at eof");
  close(fd[0]);
}

int
main(void)
{
  printf("pstress: start\n");
  loop_create_close(200);
  many_alive(5);
  fork_pipes(30);
  closed_read();
  closed_write();
  loop_create_close(200);
  printf("pstress: OK\n");
  exit(0);
}
