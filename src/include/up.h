#ifndef _UP_H_
#define _UP_H_

#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>

#define UP_BUF_SIZE 4096

#define UPOLL_CTL_ADD 1
#define UPOLL_CTL_DEL 2
#define UPOLL_CTL_MOD 3

#define UPOLLIN  1
#define UPOLLOUT 2
#define UPOLLERR 4
#define UPOLLET  8

typedef union upoll_data {
  void      *ptr;
  intptr_t  fd;
  uint32_t  u32;
  uint64_t  u64;
} upoll_data_t;

typedef struct upoll_event {
  uint32_t      events;
  upoll_data_t  data;
} upoll_event_t;

int upoll_create();
int upoll_ctl(int upfd, int op, int fd, struct upoll_event *event);
int upoll_wait(int upfd, struct upoll_event *events, int maxevents, int timeout);

int uclose(int fd);
int ufdopen(intptr_t fd);
int uread(int fd, char* buf, size_t len);
int uwrite(int fd, const char* buf, size_t len);

int usocket(int domain, int type, int proto);
int ubind(int sock, const char* name, unsigned int port);
int ulisten(int sock, int backlog);
int uaccept(int server);
int uconnect(int sock, const char* name, unsigned int port);
int ushutdown(int sock);

#endif /* _UP_H_ */

