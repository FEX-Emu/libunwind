/* libunwind - a platform-independent unwind library
   Copyright (C) 2002 Hewlett-Packard Co
   Copyright (C) 2007 David Mosberger-Tang
        Contributed by David Mosberger-Tang <dmosberger@gmail.com>

   Modified for x86_64 by Max Asbock <masbock@us.ibm.com>

This file is part of libunwind.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.  */

#include "libunwind_i.h"
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#if defined(HAVE_SYS_SYSCALL_H)
# include <sys/syscall.h>
#endif
#include <stdatomic.h>

#include "unwind_i.h"

#ifdef UNW_REMOTE_ONLY

/* unw_local_addr_space is a NULL pointer in this case.  */
unw_addr_space_t unw_local_addr_space;

#else /* !UNW_REMOTE_ONLY */

static struct unw_addr_space local_addr_space;

unw_addr_space_t unw_local_addr_space = &local_addr_space;

static void
put_unwind_info (unw_addr_space_t as, unw_proc_info_t *proc_info, void *arg)
{
  /* it's a no-op */
}

static int
get_dyn_info_list_addr (unw_addr_space_t as, unw_word_t *dyn_info_list_addr,
                        void *arg)
{
#ifndef UNW_LOCAL_ONLY
# pragma weak _U_dyn_info_list_addr
  if (!_U_dyn_info_list_addr)
    return -UNW_ENOINFO;
#endif
  // Access the `_U_dyn_info_list` from `LOCAL_ONLY` library, i.e. libunwind.so.
  *dyn_info_list_addr = _U_dyn_info_list_addr ();
  return 0;
}

static int mem_validate_pipe[2] = {-1, -1};

#ifdef HAVE_PIPE2
static inline void
do_pipe2 (int pipefd[2])
{
  int result UNUSED = pipe2 (pipefd, O_CLOEXEC | O_NONBLOCK);
}
#else
static inline void
set_pipe_flags (int fd)
{
  int fd_flags = fcntl (fd, F_GETFD, 0);
  int status_flags = fcntl (fd, F_GETFL, 0);

  fd_flags |= FD_CLOEXEC;
  fcntl (fd, F_SETFD, fd_flags);

  status_flags |= O_NONBLOCK;
  fcntl (fd, F_SETFL, status_flags);
}

static inline void
do_pipe2 (int pipefd[2])
{
  pipe (pipefd);
  set_pipe_flags(pipefd[0]);
  set_pipe_flags(pipefd[1]);
}
#endif

static inline void
open_pipe (void)
{
  if (mem_validate_pipe[0] != -1)
    close (mem_validate_pipe[0]);
  if (mem_validate_pipe[1] != -1)
    close (mem_validate_pipe[1]);

  do_pipe2 (mem_validate_pipe);
}

ALWAYS_INLINE
static int
write_validate (unw_word_t addr)
{
  int ret = -1;
  ssize_t bytes = 0;

  do
    {
      char buf;
      bytes = read (mem_validate_pipe[0], &buf, 1);
    }
  while ( errno == EINTR );

  int valid_read = (bytes > 0 || errno == EAGAIN || errno == EWOULDBLOCK);
  if (!valid_read)
    {
      // re-open closed pipe
      open_pipe ();
    }

  do
    {
#ifdef HAVE_SYS_SYSCALL_H
       /* use syscall insteadof write() so that ASAN does not complain */
       ret = syscall (SYS_write, mem_validate_pipe[1], addr, 1);
#else
	  ret = write (mem_validate_pipe[1], (void *)addr, 1);
#endif
    }
  while ( errno == EINTR );

  return ret;
}

static int (*mem_validate_func) (unw_word_t addr, size_t len);
static int msync_validate (unw_word_t addr, size_t len)
{
  if (msync ( (void *)uwn_page_start (addr), len, MS_ASYNC) != 0)
    {
      return -1;
    }

  return write_validate (addr);
}

#ifdef HAVE_MINCORE
static int mincore_validate (unw_word_t addr, size_t len)
{
  unsigned char mvec[2]; /* Unaligned access may cross page boundary */

  /* mincore could fail with EAGAIN but we conservatively return -1
     instead of looping. */
  if (mincore ((void *)uwn_page_start (addr), len, mvec) != 0)
    {
      return -1;
    }

  return write_validate (addr);
}
#endif

/* Initialise memory validation method. On linux kernels <2.6.21,
   mincore() returns incorrect value for MAP_PRIVATE mappings,
   such as stacks. If mincore() was available at compile time,
   check if we can actually use it. If not, use msync() instead. */
HIDDEN void
tdep_init_mem_validate (void)
{
  open_pipe ();

  const char *force_mem_validate = getenv ("UNW_X86_64_FORCE_MEM_VALIDATE");

  if (force_mem_validate != NULL)
    {
#ifdef HAVE_MINCORE
      if (strcmp(force_mem_validate, "mincore") == 0)
        {
          Debug(1, "force using mincore to validate memory\n");
          mem_validate_func = mincore_validate;
          return;
        }
#endif
      if (strcmp(force_mem_validate, "msync") == 0)
        {
          Debug(1, "force using msync to validate memory\n");
          mem_validate_func = msync_validate;
          return;
        }
      Debug(1, "cannot force %s to validate memory, unknown method \n",
              force_mem_validate);
    }

  // Work out dynamically what memory validation function to use.
#ifdef HAVE_MINCORE
  unsigned char present = 1;
  size_t len = unw_page_size;
  unw_word_t addr = uwn_page_start((unw_word_t)&present);
  unsigned char mvec[1];
  int ret;
  while ((ret = mincore ((void*)addr, len, (unsigned char *)mvec)) == -1 &&
         errno == EAGAIN) {}
  if (ret == 0)
    {
      Debug(1, "using mincore to validate memory\n");
      mem_validate_func = mincore_validate;
    }
  else
#endif
    {
      Debug(1, "using msync to validate memory\n");
      mem_validate_func = msync_validate;
    }
}

/* Cache of already validated addresses */
#define NLGA 4
#if defined(HAVE___CACHE_PER_THREAD) && HAVE___CACHE_PER_THREAD
// thread-local variant
static _Thread_local unw_word_t last_good_addr[NLGA];
static _Thread_local int lga_victim;

static int
is_cached_valid_mem(unw_word_t addr)
{
  addr = uwn_page_start (addr);
  int i;
  for (i = 0; i < NLGA; i++)
    {
      if (addr == last_good_addr[i])
        return 1;
    }
  return 0;
}

static void
cache_valid_mem(unw_word_t addr)
{
  addr = uwn_page_start (addr);
  int i, victim;
  victim = lga_victim;
  for (i = 0; i < NLGA; i++) {
    if (last_good_addr[victim] == 0) {
      last_good_addr[victim] = addr;
      return;
    }
    victim = (victim + 1) % NLGA;
  }

  /* All slots full. Evict the victim. */
  last_good_addr[victim] = addr;
  victim = (victim + 1) % NLGA;
  lga_victim = victim;
}

#else
// global, thread safe variant
static _Atomic unw_word_t last_good_addr[NLGA];
static _Atomic int lga_victim;

static int
is_cached_valid_mem(unw_word_t addr)
{
  int i;
  addr = uwn_page_start (addr);
  for (i = 0; i < NLGA; i++)
    {
      if (addr == atomic_load(&last_good_addr[i]))
        return 1;
    }
  return 0;
}

static void
cache_valid_mem(unw_word_t addr)
{
  int i, victim;
  victim = atomic_load(&lga_victim);
  unw_word_t zero = 0;
  addr = uwn_page_start (addr);
  for (i = 0; i < NLGA; i++) {
    if (atomic_compare_exchange_strong(&last_good_addr[victim], &zero, addr)) {
      return;
    }
    victim = (victim + 1) % NLGA;
  }

  /* All slots full. Evict the victim. */
  atomic_store(&last_good_addr[victim], addr);
  victim = (victim + 1) % NLGA;
  atomic_store(&lga_victim, victim);
}
#endif

static int
validate_mem (unw_word_t addr, size_t len)
{
  if (len == 0)
    return 0;

  if (uwn_page_start (addr) == 0)
    return -1;

  unw_word_t lastbyte = addr + (len - 1); // highest addressed byte of data to access
  while (1)
    {
      if (!is_cached_valid_mem(addr))
        {
          if (mem_validate_func (addr, len) == -1)
            return -1;
          cache_valid_mem(addr);
        }
      // If we're still on the same page, we're done.
      unw_word_t stride = (long)len-1 < unw_page_size ? (long)len-1 : unw_page_size;
      len -= stride;
      addr += stride;
      if (uwn_page_start (addr) == uwn_page_start (lastbyte))
        break;
    }

  return 0;
}

static int
access_mem (unw_addr_space_t as, unw_word_t addr, unw_word_t *val, int write,
            void *arg)
{
  if (unlikely (write))
    {
      Debug (16, "mem[%016lx] <- %lx\n", addr, *val);
      *(unw_word_t *) addr = *val;
    }
  else
    {
      /* validate address */
      if (unlikely (AS_ARG_GET_VALIDATE(arg))
          && unlikely (validate_mem (addr, sizeof (unw_word_t)))) {
        Debug (16, "mem[%016lx] -> invalid\n", addr);
        return -1;
      }
      *val = *(unw_word_t *) addr;
      Debug (16, "mem[%016lx] -> %lx\n", addr, *val);
    }
  return 0;
}

static int
access_reg (unw_addr_space_t as, unw_regnum_t reg, unw_word_t *val, int write,
            void *arg)
{
  unw_word_t *addr;
  ucontext_t *uc = AS_ARG_GET_UC_PTR(arg);

  if (unw_is_fpreg (reg))
    goto badreg;

  if (!(addr = x86_64_r_uc_addr (uc, reg)))
    goto badreg;

  if (write)
    {
      *(unw_word_t *) addr = *val;
      Debug (12, "%s <- 0x%016lx\n", unw_regname (reg), *val);
    }
  else
    {
      *val = *(unw_word_t *) addr;
      Debug (12, "%s -> 0x%016lx\n", unw_regname (reg), *val);
    }
  return 0;

 badreg:
  Debug (1, "bad register number %u\n", reg);
  return -UNW_EBADREG;
}

static int
access_fpreg (unw_addr_space_t as, unw_regnum_t reg, unw_fpreg_t *val,
              int write, void *arg)
{
  ucontext_t *uc = AS_ARG_GET_UC_PTR(arg);
  unw_fpreg_t *addr;

  if (!unw_is_fpreg (reg))
    goto badreg;

  if (!(addr = x86_64_r_uc_addr (uc, reg)))
    goto badreg;

  if (write)
    {
      Debug (12, "%s <- %08lx.%08lx.%08lx\n", unw_regname (reg),
             ((long *)val)[0], ((long *)val)[1], ((long *)val)[2]);
      *(unw_fpreg_t *) addr = *val;
    }
  else
    {
      *val = *(unw_fpreg_t *) addr;
      Debug (12, "%s -> %08lx.%08lx.%08lx\n", unw_regname (reg),
             ((long *)val)[0], ((long *)val)[1], ((long *)val)[2]);
    }
  return 0;

 badreg:
  Debug (1, "bad register number %u\n", reg);
  /* attempt to access a non-preserved register */
  return -UNW_EBADREG;
}

static int
get_static_proc_name (unw_addr_space_t as, unw_word_t ip,
                      char *buf, size_t buf_len, unw_word_t *offp,
                      void *arg)
{
  return _Uelf64_get_proc_name (as, getpid (), ip, buf, buf_len, offp);
}

HIDDEN void
x86_64_local_addr_space_init (void)
{
  memset (&local_addr_space, 0, sizeof (local_addr_space));
  local_addr_space.caching_policy = UNWI_DEFAULT_CACHING_POLICY;
  local_addr_space.acc.find_proc_info = dwarf_find_proc_info;
  local_addr_space.acc.put_unwind_info = put_unwind_info;
  local_addr_space.acc.get_dyn_info_list_addr = get_dyn_info_list_addr;
  local_addr_space.acc.access_mem = access_mem;
  local_addr_space.acc.access_reg = access_reg;
  local_addr_space.acc.access_fpreg = access_fpreg;
  local_addr_space.acc.resume = x86_64_local_resume;
  local_addr_space.acc.get_proc_name = get_static_proc_name;
  unw_flush_cache (&local_addr_space, 0, 0);
}

#endif /* !UNW_REMOTE_ONLY */
