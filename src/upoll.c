#include "upoll.h"

static utable_t* __utable;

static utable_t* utable_create(void) {
  utable_t* ut = (utable_t*)calloc(1, sizeof(utable_t));
  ut->size = 64;
  ut->ufds = calloc(ut->size, sizeof(ufd_t*));
  ut->nfds = 0;
#if defined(__WINDOWS__)
  WSADATA wsadata;
  int rc = WSAStartup(MAKEWORD(2,2), &wsadata); 
#endif
  return ut;
}

static utable_t* utable(void) {
  if (!__utable) __utable = utable_create();
  return __utable;
}

ufd_t* ufd_open() {

  utable_t* ut = utable();
  ufd_t* ufd = NULL;

  int i;
  for (i = 0; i < ut->nfds; i++) {
    if (ut->ufds[i]->refcnt == 0) {
      ufd = ut->ufds[i];
      memset(ufd, 0, sizeof(ufd_t));
      ufd->fileno = i;
      break;
    }
  }
  if (ufd == NULL) {
    if (ut->nfds >= ut->size) {
      ut->size *= 2;
      ut->ufds = (ufd_t**)realloc((void*)ut->ufds, ut->size * sizeof(ufd_t*));
    }
    ufd = malloc(sizeof(ufd_t));
    memset(ufd, 0, sizeof(ufd_t));
    assert(ufd != NULL); /* XXX: refactor to ENOMEM at call site */
    ufd->fileno = ut->nfds++;
    ut->ufds[ufd->fileno] = ufd;
  }

  ufd->fd = -1;
  ufd->refcnt = 1;
  ulist_init(&ufd->tasks);

  return ufd;
}
ufd_t* ufd_find(int fileno) {
  utable_t* ut = utable();
  if (fileno >= ut->nfds || fileno < 0 || ut->ufds[fileno]->refcnt == 0) {
    return NULL;
  }
  return ut->ufds[fileno];
}
int ufd_close(int fileno) {
  utable_t* ut = utable();
  if (fileno >= ut->nfds || fileno < 0 || ut->ufds[fileno]->refcnt == 0) {
    return -1;
  }
  ufd_t* ufd = ut->ufds[fileno];
  memset(ufd, 0, sizeof(ufd_t));
  ufd->fd = -1;
  return 0;
}

static int _op_file_close(ufd_t* ufd) {
  if (ufd->ops->flush) ufd->ops->flush(ufd, 0);
#if defined(__WINDOWS__)
  if (ufd->fd >= 0) closesocket((SOCKET)ufd->fd);
#else
  if (ufd->fd >= 0) close(ufd->fd);
#endif
  ulist_t* q;
  ulist_scan(q, &ufd->tasks) {
    utask_t* t = ulist_data(q, utask_t, tasks);
    ulist_remove(&t->ready);
    ulist_remove(&t->queue);
    ulist_append(&t->upoll->clean, &t->queue);
  }
  ufd_close(ufd->fileno);
  return 0;
}
static int _op_file_drain(ufd_t* ufd, uint32_t hint) {
  ubuf_t* buf = &ufd->rdbuf;
  if (buf->nput < UP_BUF_SIZE) {
#if defined(__WINDOWS__)
    int r = recv(
      (SOCKET)ufd->fd, buf->base + buf->nput, UP_BUF_SIZE - buf->nput, 0
    );
#else
    int r = read(ufd->fd, buf->base + buf->nput, UP_BUF_SIZE - buf->nput);
#endif
    if (r > 0) {
      buf->nput += r;
    }
    else if ((r == 0) && (hint & UPOLLIN)) {
      up_fset(ufd, UP_EOF | UP_ERROR);
      hint |= UPOLLERR; /* select() says pollin but 0 read - means HUP */
    }
    else if (hint) {
#if defined(__WINDOWS__)
      if (WSAGetLastError() != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        up_fset(ufd, UP_ERROR);
        ufd->error = WSAGetLastError();
        return -1;
      }
#else
      if (errno != EAGAIN) {
        up_fset(ufd, UP_ERROR);
        ufd->error = errno;
        return -1;
      }
#endif
    }
  }
  return 0;
}
static int _op_file_flush(ufd_t* ufd, uint32_t hint) {
  ubuf_t* buf = &ufd->wrbuf;
  if (buf->nput > 0) {
#if defined(__WINDOWS__)
    int w = send(
      (SOCKET)ufd->fd, buf->base + buf->nget, buf->nput - buf->nget, 0
    );
#else
    int w = write(ufd->fd, buf->base + buf->nget, buf->nput - buf->nget);
#endif
    if (w > 0) {
      buf->nget += w;
      if (buf->nget == buf->nput) {
        memset(buf, 0, sizeof(ubuf_t));
      }
    }
    else if (errno != EAGAIN) {
      up_fset(ufd, UP_ERROR);
      ufd->error = errno;
    }
  }
  return 0;
}
static int _op_sock_drain_listen(ufd_t* ufd, uint32_t hint) {
  if (hint & UPOLLIN) ufd->rdbuf.nput++;
  return 0;
}

static int _op_file_react(ufd_t* ufd, uint32_t hint) {
  if (hint & UPOLLIN && ufd->ops->drain) {
    ufd->ops->drain(ufd, hint);
  }
  if (hint & UPOLLOUT && ufd->ops->flush) {
    ufd->ops->flush(ufd, hint);
  }
  if (hint & UPOLLERR) {
    up_fset(ufd, UP_ERROR);
  }
  utask(&ufd->tasks, hint);
  return 0;
}

static int _op_file_read(ufd_t* ufd, char* ptr, size_t len) {
  ufd->ops->drain(ufd, 0);
  ubuf_t* buf = &ufd->rdbuf;
  if (buf->nput > buf->nget) {
    int max = buf->nput - buf->nget;
    if (len > max) len = max;
    memcpy(ptr, buf->base + buf->nget, len);
    buf->nget += len;
    if (buf->nput == buf->nget) {
      memset(buf, 0, sizeof(ubuf_t));
    }
    return len;
  }
  return -EAGAIN;
}
static int _op_file_write(ufd_t* ufd, const char* ptr, size_t len) {
  ubuf_t* buf = &ufd->wrbuf;
  ufd->ops->flush(ufd, 0);
  if (buf->nput < UP_BUF_SIZE) {
    size_t max = UP_BUF_SIZE - buf->nput;
    if (len > max) len = max;
    memcpy(buf->base + buf->nput, ptr, len);
    buf->nput += len;
    ufd->ops->flush(ufd, 0);
    return len;
  }
  return -EAGAIN;
}

static uops_t _file_ops = {
  .react = _op_file_react,
  .drain = _op_file_drain,
  .flush = _op_file_flush,
  .close = _op_file_close,
  .read  = _op_file_read,
  .write = _op_file_write
};

int ufdopen(intptr_t fd) {
  ufd_t* ufd = ufd_open();
  ufd->fd = fd;
  ufd->ops = &_file_ops;
#if defined(__WINDOWS__)
  unsigned long flags = 1;
  ioctlsocket((SOCKET)fd, FIONBIO, &flags);
#else
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
  return ufd->fileno;
}
int uread(int fd, char* buf, size_t len) {
  ufd_t* ufd = ufd_find(fd);
  if (ufd == NULL) return -EBADF;
  if (!ufd->ops->read) return -EINVAL;
  return ufd->ops->read(ufd, buf, len);
}
int uwrite(int fd, const char* buf, size_t len) {
  ufd_t* ufd = ufd_find(fd);
  if (ufd == NULL) return -EBADF;
  if (!ufd->ops->write) return -EINVAL;
  return ufd->ops->write(ufd, buf, len);
}

static int _op_pass_react(ufd_t* ufd, uint32_t hint);
static int _op_sock_bind(ufd_t* ufd, const char* host, unsigned int port);
static int _op_sock_listen(ufd_t* ufd, int backlog);
static int _op_sock_accept(ufd_t* ufd);
static int _op_sock_connect(ufd_t* ufd, const char* host, unsigned int port);
static int _op_sock_shutdown(ufd_t* ufd);
/*
static int _op_sock_send(ufd_t* ufd, const void*, size_t, const struct sockaddr*);
static int _op_sock_recv(ufd_t* ufd, void*, size_t, const struct sockaddr*);
*/

static int _op_sock_bind(ufd_t* ufd, const char* host, unsigned int port) {
  static uops_t _bound_sock_ops = {
    .react    = _op_pass_react,
    .close    = _op_file_close,
    .listen   = _op_sock_listen,  
    .shutdown = _op_sock_shutdown,
    /*
    .send     = _op_sock_send,
    .recv     = _op_sock_recv
    */
  };

  char serv[16];
  snprintf(serv, 16, "%d", port);

  struct addrinfo* info;
  struct addrinfo  hint;
  memset(&hint, 0, sizeof(hint));

  hint.ai_family   = ufd->info[0];
  hint.ai_socktype = ufd->info[1];

  int rc = getaddrinfo(host, serv, &hint, &info);
  if (rc < 0) return -errno;

  int optval = 1;
#if defined(__WINDOWS__)
  rc = setsockopt(
    (SOCKET)ufd->fd, SOL_SOCKET, SO_REUSEADDR, (char*)&optval, sizeof(optval)
  );
#else
  rc = setsockopt(ufd->fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
#endif
  if (rc < 0) return -errno;

  rc = bind(ufd->fd, info->ai_addr, info->ai_addrlen);
  if (rc < 0) return -errno;

  freeaddrinfo(info);

  ufd->ops = &_bound_sock_ops;
  return 0;
}

static int _op_sock_connect(ufd_t* ufd, const char* host, unsigned int port) {
  static uops_t _conn_sock_ops = {
    .react    = _op_file_react,
    .drain    = _op_file_drain,
    .flush    = _op_file_flush,
    .close    = _op_file_close,
    .read     = _op_file_read,
    .write    = _op_file_write,
    .shutdown = _op_sock_shutdown
  };

  char serv[16];
  snprintf(serv, 16, "%d", port);

  struct addrinfo* info;
  struct addrinfo  hint;
  memset(&hint, 0, sizeof(hint));

  hint.ai_family   = ufd->info[0];
  hint.ai_socktype = ufd->info[1];

  int rc = getaddrinfo(host, serv, &hint, &info);
  if (rc < 0) return -errno;

#if defined(__WINDOWS__)
  rc = connect((SOCKET)ufd->fd, info->ai_addr, info->ai_addrlen);
  if (rc < 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
    return -WSAGetLastError();
  }
#else
  rc = connect(ufd->fd, info->ai_addr, info->ai_addrlen);
  if (rc < 0 && errno != EINPROGRESS) return -errno;
#endif

  freeaddrinfo(info);

  ufd->ops = &_conn_sock_ops;
  return 0;
}

static int _op_sock_shutdown(ufd_t* ufd) {
  if (ufd->fd < 0) return -EBADF;
#if defined(__WINDOWS__)
  int rc = shutdown((SOCKET)ufd->fd, SD_BOTH);
  if (rc < 0) return -WSAGetLastError();
#else
  int rc = shutdown(ufd->fd, SHUT_RDWR);
  if (rc < 0) return -errno;
#endif
  return 0;
}

static int _op_sock_accept(ufd_t* ufd) {
  static uops_t _client_sock_ops = {
    .react    = _op_file_react,
    .drain    = _op_file_drain,
    .flush    = _op_file_flush,
    .close    = _op_file_close,
    .read     = _op_file_read,
    .write    = _op_file_write,
    .shutdown = _op_sock_shutdown
  };

  if (ufd->rdbuf.nput > 0) {
    ufd->rdbuf.nput--;
  }
  else {
    return -EAGAIN;
  }

  struct sockaddr addr;

  addr.sa_family = AF_INET;
  socklen_t addr_len;

#if defined(__WINDOWS__)
  intptr_t fd = (intptr_t)accept((SOCKET)ufd->fd, &addr, &addr_len);
  if (fd == INVALID_SOCKET) return -WSAGetLastError();
#else
  intptr_t fd = accept(ufd->fd, &addr, &addr_len);
  if (fd < 0) return -errno;
#endif

  ufd_t* cfd = ufd_open();
  cfd->fd = fd;
  cfd->ops = &_client_sock_ops;

  return cfd->fileno;
}

static int _op_sock_listen(ufd_t* ufd, int backlog) {
  static uops_t _server_sock_ops = {
    .react    = _op_file_react,
    .drain    = _op_sock_drain_listen,
    .close    = _op_file_close,
    .accept   = _op_sock_accept
  };

  int rc = listen(ufd->fd, backlog);
  if (rc < 0) return -errno;
  ufd->ops = &_server_sock_ops;
  return 0;
}


static int _op_pass_react(ufd_t* ufd, uint32_t hint) {
  utask(&ufd->tasks, hint);
  return 0;
}

int usocket(int domain, int type, int proto) {
  static uops_t _sock_ops = {
    .react    = _op_pass_react,
    .close    = _op_file_close,
    .bind     = _op_sock_bind,
    .connect  = _op_sock_connect,
    /*
    .send     = _op_sock_send,
    .recv     = _op_sock_recv
    */
  };

  intptr_t fd = (intptr_t)socket(domain, type, proto);

#if defined(__WINDOWS__)
  if (fd < 0) return -WSAGetLastError();
#else
  if (fd < 0) return -errno;
#endif

  ufd_t* ufd = ufd_open();
  ufd->fd = fd;

#if defined(__WINDOWS__)
  unsigned long flags = 1;
  ioctlsocket((SOCKET)fd, FIONBIO, &flags);
#else
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif

  ufd->ops = &_sock_ops;

  ufd->info[0] = domain;
  ufd->info[1] = type;
  ufd->info[2] = proto;

  return ufd->fileno;
}

int ubind(int sock, const char* name, unsigned int port) {
  ufd_t* ufd = ufd_find(sock);
  if (ufd == NULL) return -EBADF;
  if (!ufd->ops->bind) return -EINVAL;
  return ufd->ops->bind(ufd, name, port);
}

int uconnect(int sock, const char* name, unsigned int port) {
  ufd_t* ufd = ufd_find(sock);
  if (ufd == NULL) return -EBADF;
  if (!ufd->ops->connect) return -EINVAL;
  return ufd->ops->connect(ufd, name, port);
}

int ulisten(int sock, int backlog) {
  ufd_t* ufd = ufd_find(sock);
  if (ufd == NULL) return -EBADF;
  if (!ufd->ops->listen) return -EINVAL;
  return ufd->ops->listen(ufd, backlog);
}

int uaccept(int server) {
  ufd_t* ufd = ufd_find(server);
  if (ufd == NULL) return -EBADF;
  if (!ufd->ops->accept) return -EINVAL;
  return ufd->ops->accept(ufd);
}

int ushutdown(int sock) {
  ufd_t* ufd = ufd_find(sock);
  if (ufd == NULL) return -EBADF;
  if (!ufd->ops->shutdown) return -EINVAL;
  return ufd->ops->shutdown(ufd);
}

int usend(int sock, const void* buf, size_t len, const struct sockaddr* addr) {
  ufd_t* ufd = ufd_find(sock);
  if (ufd == NULL) return -EBADF;
  if (!ufd->ops->send) return -EINVAL;
  return ufd->ops->send(ufd, buf, len, addr);
}

int urecv(int sock, void* buf, size_t len, const struct sockaddr* addr) {
  ufd_t* ufd = ufd_find(sock);
  if (ufd == NULL) return -EBADF;
  if (!ufd->ops->recv) return -EINVAL;
  return ufd->ops->recv(ufd, buf, len, addr);
}

int uclose(int fd) {
  ufd_t* ufd = ufd_find(fd);
  if (ufd == NULL) return -EBADF;
  return ufd->ops->close(ufd);
}

int upoll_create() {
  ufd_t* ufd = ufd_open();
  upoll_t* upq = (upoll_t*)calloc(1, sizeof(upoll_t));
  ufd->data = upq;

  ulist_init(&upq->alive);
  ulist_init(&upq->clean);
  ulist_init(&upq->ready);

#if defined(HAVE_KQUEUE)
  ufd->fd = kqueue();
  assert(ufd->fd > 0);
#elif defined(HAVE_EPOLL)
  ufd->fd = epoll_create(1);
#endif
  return ufd->fileno;
}

void utask(ulist_t* tasks, uint32_t hint) {
  ulist_t* q;
  ulist_scan(q, tasks) {
    utask_t* t = ulist_data(q, utask_t, tasks);
    if (ulist_empty(&t->ready) && t->apply(t, hint)) {
      ulist_remove(&t->queue);
      ulist_append(&t->upoll->ready, &t->ready);
    }
  }
}

static int utask_apply(utask_t* task, uint32_t hint) {
  ufd_t* ufd = task->ufd;
  if (ufd == NULL || ufd->refcnt == 0) {
    up_fset(task, UP_DELETE);
    return 0;
  }

  task->events |= hint;

  if (hint == 0) {
    if (ufd->ops->flush) ufd->ops->flush(ufd, hint);
    if (ufd->ops->drain) ufd->ops->drain(ufd, hint);
  }

  if (ufd->rdbuf.nput > ufd->rdbuf.nget) {
    task->events |= UPOLLIN;
  }
  else {
    task->events &= ~UPOLLIN;
  }

  if (ufd->wrbuf.nput < UP_BUF_SIZE) {
    task->events |= UPOLLOUT;
  }

  if (up_fget(ufd, UP_ERROR) || ufd->error || hint & UPOLLERR) {
    task->events |= UPOLLERR;
    task->events &= ~UPOLLOUT;
  }
  task->events &= (task->event.events | UPOLLERR);
  return task->events;
}

static inline utask_t* utask_find(ulist_t* list, upoll_t* upq) {
  ulist_t* q;
  utask_t* t; 
  ulist_scan(q, list) {
    t = ulist_data(q, utask_t, tasks);
    if (t->upoll == upq) return t;
  }
  return NULL;
}

#if defined(HAVE_KQUEUE)
int upoll_ctl_kqueue(ufd_t* upd, int op, ufd_t* ufd, upoll_event_t* event) {
  assert(upd->fd >= 0);
  struct timespec ts;
  ts.tv_sec  = 0;
  ts.tv_nsec = 0;

  struct kevent kch[4];
  int nch = 0, rc = 0, i = 0;
  int flags = EV_RECEIPT;
  switch (op) {
    case UPOLL_CTL_ADD: {
      flags |= (EV_ADD | EV_ENABLE);
      if (event->events & UPOLLET) flags |= EV_CLEAR;
      if (event->events & UPOLLIN) {
        EV_SET(&kch[nch++], ufd->fd, EVFILT_READ, flags, 0, 0, ufd);
      }
      if (event->events & UPOLLOUT) {
        EV_SET(&kch[nch++], ufd->fd, EVFILT_WRITE, flags, 0, 0, ufd);
      }
    }
    case UPOLL_CTL_DEL: {
      if (event->events & UPOLLIN) {
        EV_SET(&kch[nch++], ufd->fd, EVFILT_READ, EV_DELETE, 0, 0, ufd);
      }
      if (event->events & UPOLLOUT) {
        EV_SET(&kch[nch++], ufd->fd, EVFILT_WRITE, EV_DELETE, 0, 0, ufd);
      }
    }
    case UPOLL_CTL_MOD: {
      if (event->events & UPOLLIN) {
        flags |= (EV_ADD | EV_ENABLE);
        if (event->events & UPOLLET) flags |= EV_CLEAR;
        EV_SET(&kch[nch++], ufd->fd, EVFILT_READ, flags, 0, 0, ufd);
      }
      else {
        flags |= (EV_ADD | EV_DISABLE);
        EV_SET(&kch[nch++], ufd->fd, EVFILT_READ, flags, 0, 0, ufd);
      }
      flags = EV_RECEIPT;
      if (event->events & UPOLLOUT) {
        flags |= (EV_ADD | EV_ENABLE);
        if (event->events & UPOLLET) flags |= EV_CLEAR;
        EV_SET(&kch[nch++], ufd->fd, EVFILT_WRITE, flags, 0, 0, ufd);
      }
      else {
        flags |= (EV_ADD | EV_DISABLE);
        EV_SET(&kch[nch++], ufd->fd, EVFILT_WRITE, flags, 0, 0, ufd);
      }
    }
  }
  rc = kevent(upd->fd, kch, nch, kch, nch, &ts);
  if (rc < 0) return -errno;
  for (i = 0; i < rc; i++) {
    if (kch[i].data) return -kch[i].data;
  }
  return 0;
}
#elif defined(HAVE_EPOLL)
int upoll_ctl_epoll(ufd_t* upd, int op, ufd_t* ufd, upoll_event_t* event) {
  assert(upd->fd >= 0);
  struct epoll_event evt;

  evt.events = 0;
  evt.data.ptr = ufd;

  if (event->events & UPOLLIN) {
    evt.events |= EPOLLIN;
  }
  if (event->events & UPOLLOUT) {
    evt.events |= EPOLLOUT;
  }
  if (event->events & UPOLLET) {
    evt.events |= EPOLLET;
  }

  int rc = 0;
  switch (op) {
    case UPOLL_CTL_ADD: {
      rc = epoll_ctl(upd->fd, EPOLL_CTL_ADD, ufd->fd, &evt);
      break;
    }
    case UPOLL_CTL_DEL: {
      rc = epoll_ctl(upd->fd, EPOLL_CTL_DEL, ufd->fd, &evt);
      break;
    }
    case UPOLL_CTL_MOD: {
      rc = epoll_ctl(upd->fd, EPOLL_CTL_MOD, ufd->fd, &evt);
      break;
    }
  }
  if (rc < 0) return -errno;
  return rc;
}
#endif

int upoll_ctl(int upfd, int op, int fd, upoll_event_t* event) {
  ufd_t* upd = ufd_find(upfd); 
  assert(upd != NULL);
  assert(upd->data != NULL);
  upoll_t* upq = (upoll_t*)upd->data;

  ufd_t* ufd = ufd_find(fd);
  if (ufd == NULL) return EBADF;

  switch (op) {
    case UPOLL_CTL_ADD: {
      utask_t* task = utask_find(&ufd->tasks, upq);
      if (!task) {
        if (ulist_empty(&upq->clean)) {
          task = (utask_t*)calloc(1, sizeof(utask_t));
          ulist_init(&task->queue);
          ulist_init(&task->ready);
          ulist_init(&task->tasks);
          task->apply  = utask_apply;
        }
        else {
          ulist_t* item = ulist_next(&upq->clean);
          ulist_remove(item);
          task = ulist_data(item, utask_t, queue);
        }
        task->upoll = upq;
        task->event = *event;
        task->events = 0;

        task->ufd = ufd;

        ulist_append(&upq->alive, &task->queue);
        ulist_append(&ufd->tasks, &task->tasks);
      }
      break;
    }
    case UPOLL_CTL_DEL: {
      utask_t* task = utask_find(&ufd->tasks, upq);
      if (!task) return -ENOENT;
      ulist_remove(&task->ready);
      ulist_remove(&task->tasks);
      ulist_remove(&task->queue);
      ulist_append(&upq->clean, &task->queue);
      break;
    }
    case UPOLL_CTL_MOD: {
      utask_t* task = utask_find(&ufd->tasks, upq);
      if (!task) return -ENOENT;
      task->event = *event;
      task->events = 0;
      if (!(event->events & UPOLLET) && task->apply(task, 0)) {
        if (ulist_empty(&task->ready)) {
          ulist_append(&upq->ready, &task->ready);
        }
      }
      else {
        ulist_remove(&task->ready);
        ulist_remove(&task->queue);
        ulist_append(&upq->alive, &task->queue);
      }
      break;
    }
    default: {
      return -EINVAL;
    }
  }
#if defined(HAVE_KQUEUE)
  return upoll_ctl_kqueue(upd, op, ufd, event);
#elif defined(HAVE_EPOLL)
  return upoll_ctl_epoll(upd, op, ufd, event);
#else
  return 0;
#endif
}

#if defined(HAVE_KQUEUE)
int upoll_wait_kqueue(ufd_t* upd, int nev, int timeout) {
  int i, n;
  int nkev = nev * 2;
  struct timespec  ts;
  struct timespec* tsp = &ts;

  struct kevent kevs[nkev];

  ts.tv_nsec = 0;
  if (timeout < 0) {
    tsp = NULL;
  }
  else if (timeout == 0)  
    ts.tv_sec = 0;
  else {
    ts.tv_sec  = (timeout / 1000);
    ts.tv_nsec = (timeout % 1000) * 1000000;
  }

  n = kevent(upd->fd, NULL, 0, kevs, nkev, tsp);

  for (i = 0; i < n; i++) {
    uint32_t hint = 0;
    ufd_t* u = (ufd_t*)kevs[i].udata;
    if (kevs[i].filter == EVFILT_READ) {
      hint = UPOLLIN;
    }
    else if (kevs[i].filter == EVFILT_WRITE) {
      hint = UPOLLOUT;
    }
    if (kevs[i].flags & (EV_ERROR | EV_EOF)) {
      hint &= ~UPOLLOUT;
      hint |= UPOLLERR;
    }
    u->ops->react(u, hint);
  }

  return n;
}
#elif defined(HAVE_EPOLL)
int upoll_wait_epoll(ufd_t* upd, int nev, int timeout) {
  struct epoll_event evts[nev];
  int i, n;
  n = epoll_wait(upd->fd, evts, nev, timeout);
  for (i = 0; i < n; i++) {
    uint32_t hint;
    ufd_t* u = (ufd_t*)evts[i].data.ptr;
    if (evts[i].events & EPOLLIN) hint |= UPOLLIN;
    if (evts[i].events & EPOLLOUT) hint |= UPOLLOUT;
    if (evts[i].events & (EPOLLERR|EPOLLHUP)) hint |= UPOLLERR;
    u->ops->react(u, hint);
  }
  return n;
}
#elif defined(HAVE_POLL)
int upoll_wait_poll(ufd_t* upd, int nev, int timeout) {
  upoll_t* upq = (upoll_t*)upd->data;

  /* FD_SETSIZE should be smaller than OPEN_MAX */
  if (nev > FD_SETSIZE) nev = FD_SETSIZE;

  ufd_t* ufds[nev];
  int n, i, nfds = 0;
  struct pollfd pfds[nev];

  utask_t* t = NULL;
  ulist_t* s = ulist_mark(&upq->alive);
  ulist_t* q = ulist_next(&upq->alive);

  while (q != s && nfds < nev) {
    t = ulist_data(q, utask_t, queue);
    q = ulist_next(q);

    ufds[nfds] = t->ufd;
    pfds[nfds].events = 0;
    pfds[nfds].fd = t->ufd->fd;
    if (t->event.events & UPOLLIN) {
      pfds[nfds].events |= POLLIN;
    }
    if (t->event.events & UPOLLOUT) {
      pfds[nfds].events |= POLLOUT;
    }
    nfds++;
  }

  n = poll(pfds, nfds, timeout);
  if (n > 0) {
    uint32_t hint = 0;
    for (i = 0; i < nfds; i++) {
      hint = 0;
      if (pfds[i].revents) {
        if (pfds[i].revents & POLLIN ) hint |= UPOLLIN;
        if (pfds[i].revents & POLLOUT) hint |= UPOLLOUT;
        if (pfds[i].revents & (POLLERR|POLLHUP|POLLNVAL)) hint |= UPOLLERR;
        ufds[i]->ops->react(ufds[i], hint);
      }
    }
  }

  return n;
}
#else
int upoll_wait_select(ufd_t* upd, int nev, int timeout) {
  upoll_t* upq = (upoll_t*)upd->data;

  if (nev > FD_SETSIZE) nev = FD_SETSIZE;

  ufd_t* ufds[nev];
  int i, maxfd = 0, n = 0, nfds = 0;

  fd_set pollin, pollout, pollerr;

  FD_ZERO(&pollin);
  FD_ZERO(&pollout);
  FD_ZERO(&pollerr);

  struct timeval  tv;
  struct timeval* tvp = &tv;

  tv.tv_usec = 0;
  if (timeout < 0) {
    tvp = NULL;
  }
  else if (timeout == 0)  
    tv.tv_sec = 0;
  else {
    tv.tv_sec  = (timeout / 1000);
    tv.tv_usec = (timeout % 1000) * 1000;
  }

  utask_t* t = NULL;
  ulist_t* s = ulist_mark(&upq->alive);
  ulist_t* q = ulist_next(&upq->alive);

  while (q != s && nfds < nev && nfds < FD_SETSIZE) {
    t = ulist_data(q, utask_t, queue);
    q = ulist_next(q);

    assert(ulist_empty(&t->ready));

    ufds[nfds] = t->ufd;
    if (t->event.events & UPOLLIN) {
      FD_SET(t->ufd->fd, &pollin);
    }
    if (t->event.events & UPOLLOUT) {
      FD_SET(t->ufd->fd, &pollout);
    }
    FD_SET(t->ufd->fd, &pollerr);
    if (maxfd < t->ufd->fd) maxfd = t->ufd->fd;
    nfds++;
  }

# if defined(__WINDOWS__)
  int rc = select(0, &pollin, &pollout, &pollerr, tvp);
  if (rc == SOCKET_ERROR) {
    assert(WSAGetLastError() == WSAENOTSOCK);
    return -1;
  }
# else
  int rc = select(maxfd + 1, &pollin, &pollout, &pollerr, tvp);
  if (rc == -1) {
    assert(errno == EINTR || errno == EBADF);
    return -1;
  }
# endif
  n = 0;
  for (i = 0; i < nfds; i++) {
    uint32_t hint = 0;
    if (FD_ISSET(ufds[i]->fd, &pollin)) {
      hint |= UPOLLIN;
    }

    if (FD_ISSET(ufds[i]->fd, &pollerr)) {
      hint |= UPOLLERR;
    }
    else if (FD_ISSET(ufds[i]->fd, &pollout)) {
      hint |= UPOLLOUT;
    }

    if (hint) {
      ufds[i]->ops->react(ufds[i], hint);
      n++;
    }
  }
  return n;
}
#endif

int upoll_wait(int upfd, struct upoll_event *evs, int nev, int timeout) {
  ufd_t*   upd = ufd_find(upfd); 
  upoll_t* upq = (upoll_t*)upd->data;

  int e = 0;

  utask_t* t = NULL;

  ulist_t* s = ulist_mark(&upq->ready);
  ulist_t* q = ulist_next(&upq->ready);
  while (q != s) {
    t = ulist_data(q, utask_t, ready);
    q = ulist_next(q);
    if (!t->apply(t, 0)) {
      ulist_remove(&t->ready);
      ulist_remove(&t->queue);
      if (up_fget(t, UP_DELETE)) {
        ulist_remove(&t->tasks);
        ulist_append(&upq->clean, &t->queue);
      }
      else {
        t->events = 0;
        ulist_append(&upq->alive, &t->queue);
      }
    }
  }

  if (ulist_empty(&upq->ready)) {
#if defined(HAVE_KQUEUE)
    upoll_wait_kqueue(upd, nev, timeout);
#elif defined(HAVE_EPOLL)
    upoll_wait_epoll(upd, nev, timeout);
#elif defined(HAVE_POLL)
    upoll_wait_poll(upd, nev, timeout);
#else
    upoll_wait_select(upd, nev, timeout);
#endif
  }

  s = ulist_mark(&upq->ready);
  q = ulist_next(&upq->ready);

  while (q != s && e < nev) {
    t = ulist_data(q, utask_t, ready);
    q = ulist_next(q);
    ulist_remove(&t->ready);
    ulist_remove(&t->queue);
    if (t->apply(t, 0)) {
      memcpy(&evs[e], t, sizeof(upoll_event_t));
      evs[e].events = t->events;
      if (t->events & UPOLLET) {
        t->events = 0;
        ulist_append(&upq->alive, &t->queue);
      }
      else {
        ulist_append(&upq->ready, &t->ready);
      }
      e++;
    }
    else {
      ulist_append(&upq->alive, &t->queue);
    }
  }
  return e;
}


