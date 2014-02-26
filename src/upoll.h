#ifndef _UPOLL_H_
#define _UPOLL_H_

#include "./include/up.h"

#if (defined (__64BIT__) || defined (__x86_64__))
# define __IS_64BIT__
#else
# define __IS_32BIT__
#endif

#if (defined WIN32 || defined _WIN32)
# undef __WINDOWS__
# define __WINDOWS__
# undef _WIN32_WINNT
# define _WIN32_WINNT 0x0501
#endif

#include <sys/types.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>

#include <fcntl.h>
#include <sys/stat.h>

#if defined (__WINDOWS__)
# include <io.h>
# include <winsock2.h>
# include <ws2tcpip.h>
#else
# include <unistd.h>
# include <stdint.h>
# include <sys/time.h>
# include <sys/socket.h>
# include <netdb.h>
# include <netinet/in.h>
# include <netinet/tcp.h>
# include <arpa/inet.h>
#endif

#if defined(__APPLE__) || defined(__DragonFly__) \
 || defined(__FreeBSD__) || defined(__OpenBSD__) \
 || defined(__NetBSD__)
# undef HAVE_KQUEUE
# define HAVE_KQUEUE 1
#elif defined(__linux__)
# undef HAVE_EPOLL
# define HAVE_EPOLL 1
#elif (defined(__POSIX_VERSION) && (__POSIX_VERSION >= 200112L))
# undef HAVE_POLL
# define HAVE_POLL 1
#else
# undef HAVE_SELECT
# define HAVE_SELECT 1
#endif

#if defined(HAVE_KQUEUE)
# include <sys/event.h>
#elif defined(HAVE_EPOLL)
# include <sys/epoll.h>
#elif defined(HAVE_POLL)
# include <poll.h>
#endif

#define up_flags(D) ((D)->flags)
#define up_fget(D,F) (up_flags(D) & (F))
#define up_fset(D,F) (up_flags(D) |= (F))
#define up_fclr(D,F) (up_flags(D) &= ~(F))

#define UP_DELETE     0x10000000
#define UP_CLEAR      0x20000000
#define UP_EOF        0x40000000
#define UP_ERROR      0x80000000

typedef struct ufd    ufd_t;
typedef struct utask  utask_t;
typedef struct ulist  ulist_t;
typedef struct upoll  upoll_t;
typedef struct utable utable_t;

typedef int (*upoll_react_t)(ufd_t*, uint32_t);
typedef int (*utask_apply_t)(utask_t*, uint32_t);

struct ulist {
  ulist_t*  next;
  ulist_t*  prev;
};

struct upoll {
  ulist_t       alive;          /* all tasks this queue knows about */
  ulist_t       clean;          /* tasks which need cleaning up */
  ulist_t       ready;          /* tasks with pending events */
};

struct utask {
  upoll_event_t event;
  uint16_t      flags;
  uint32_t      events;

  ulist_t       queue;          /* handle for the queue's tasks */
  ulist_t       ready;          /* handle for the ready queue */
  ulist_t       tasks;          /* handle for the ufd */

  upoll_t*      upoll;
  utask_apply_t apply;
  ufd_t*        ufd;
  void*         data;
};

typedef struct {
  char    base[UP_BUF_SIZE];
  size_t  nput;
  size_t  nget;
} ubuf_t;

typedef int ufile_t;
typedef intptr_t usock_t;

typedef int (*uop_react)(ufd_t*, uint32_t hint);
typedef int (*uop_drain)(ufd_t*, uint32_t hint);
typedef int (*uop_flush)(ufd_t*, uint32_t hint);
typedef int (*uop_init)(ufd_t*);
typedef int (*uop_open)(ufd_t*, const char*, int, int);
typedef int (*uop_read)(ufd_t*, char*, size_t);
typedef int (*uop_seek)(ufd_t*, int64_t);
typedef int (*uop_write)(ufd_t*, const char*, size_t);
typedef int (*uop_bind)(ufd_t*, const char*, unsigned int);
typedef int (*uop_listen)(ufd_t*, int);
typedef int (*uop_accept)(ufd_t*);
typedef int (*uop_shutdown)(ufd_t*);
typedef int (*uop_connect)(ufd_t*, const char*, unsigned int);
typedef int (*uop_send)(ufd_t*, const void*, size_t, const struct sockaddr*);
typedef int (*uop_recv)(ufd_t*, void*, size_t, const struct sockaddr*);
typedef int (*uop_close)(ufd_t*);

typedef struct uops {
  uop_react     react;
  uop_drain     drain;
  uop_flush     flush;
  uop_close     close;
  uop_init      init;
  uop_read      read;
  uop_seek      seek;
  uop_write     write;
  uop_bind      bind;
  uop_listen    listen;
  uop_accept    accept;
  uop_shutdown  shutdown;
  uop_connect   connect;
  uop_send      send;
  uop_recv      recv;
} uops_t;

struct ufd {
  int           fileno;
  int           flags;
  intptr_t      fd;
  int           refcnt;
  int           error;
  ulist_t       tasks;
  ubuf_t        rdbuf;
  ubuf_t        wrbuf;
  int           info[3];
  void*         data;
  uops_t*       ops;
};

typedef struct usock_info {
  int domain, type, proto;
} usock_info_t;

struct utable {
  size_t        size;
  size_t        nfds;
  ufd_t**       ufds;
};

ufd_t* ufd_open(void);
ufd_t* ufd_find(int fileno);
int ufd_close(int fileno);

void utask(ulist_t* tasks, uint32_t hint);
static int utask_apply(utask_t* task, uint32_t hint);

#define container_of(ptr, type, member) \
  ((type*) ((char*)(ptr) - offsetof(type, member)))

#define ulist_init(q) (q)->prev = q; (q)->next = q

#define ulist_head(h) (h)->next
#define ulist_next(q) (q)->next

#define ulist_tail(h) (h)->prev
#define ulist_prev(q) (q)->prev

#define ulist_empty(h) (h == (h)->prev)

#define ulist_append(h, x) \
  (x)->prev = (h)->prev; (x)->prev->next = x; \
  (x)->next = h; (h)->prev = x

#define ulist_remove(x) \
  (x)->next->prev = (x)->prev; \
  (x)->prev->next = (x)->next; \
  (x)->prev = x; (x)->next = x

#define ulist_mark(h) (h)

#define ulist_scan(q, h) \
  for ((q) = ulist_head(h); (q) != ulist_mark(h); (q) = ulist_next(q))

#define ulist_data(q, type, link) \
  container_of(q, type, link)

#endif /* _UPOLL_H_ */
