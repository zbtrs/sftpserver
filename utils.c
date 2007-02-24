#include "sftpserver.h"
#include "utils.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>

void *xmalloc(size_t n) {
  void *ptr;

  if(n) {
    if(!(ptr = malloc(n)))
      fatal("out of memory");
    return ptr;
  } else
    return 0;
}

void *xcalloc(size_t n, size_t size) {
  void *ptr;

  if(n && size) {
    if(!(ptr = calloc(n, size)))
      fatal("out of memory");
    return ptr;
  } else
    return 0;
}

void *xrecalloc(void *ptr, size_t n, size_t size) {
  if(n > SIZE_MAX / size)
      fatal("out of memory");
  n *= size;
  if(n) {
    if(!(ptr = realloc(ptr, n)))
      fatal("out of memory");
    return ptr;
  } else {
    free(ptr);
    return 0;
  }
}

void *xrealloc(void *ptr, size_t n) {
  if(n) {
    if(!(ptr = realloc(ptr, n)))
      fatal("out of memory");
    return ptr;
  } else {
    free(ptr);
    return 0;
  }
}

char *xstrdup(const char *s) {
  return strcpy(xmalloc(strlen(s) + 1), s);
}

static void (*exitfn)(int) attribute((noreturn)) = exit;

void fatal(const char *msg, ...) {
  va_list ap;

  fprintf(stderr, "FATAL: ");
  va_start(ap, msg);
  vfprintf(stderr, msg, ap);
  va_end(ap);
  fputc('\n', stderr);
  exitfn(-1);
}

pid_t xfork(void) {
  pid_t pid;

  if((pid = fork()) < 0)
    fatal("fork: %s", strerror(errno));
  if(!pid)
    exitfn = _exit;
  return pid;
}

/*
Local Variables:
c-basic-offset:2
comment-column:40
fill-column:79
indent-tabs-mode:nil
End:
*/
