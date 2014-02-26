
#include "up.h"

#include <stdio.h>
#include <string.h>

#if (defined WIN32 || defined _WIN32)
# include <winsock2.h>
# include <ws2tcpip.h>
#else
# include <sys/socket.h>
# include <netinet/in.h>
#endif


int main(int argc, char** argv) {
  int upfd = upoll_create();
  int sd1 = usocket(PF_INET, SOCK_STREAM, IPPROTO_TCP);

  printf("bind: %d\n", ubind(sd1, "127.0.0.1", 1976));
  printf("listen: %d\n", ulisten(sd1, 128));

  upoll_event_t ev1, ev2;
  upoll_event_t events[8];

  ev1.events = UPOLLIN;
  ev1.data.fd = sd1;

  upoll_ctl(upfd, UPOLL_CTL_ADD, sd1, &ev1);

  int r = 0, x = 0;
  while (1) {
    int i, e;
    e = upoll_wait(upfd, events, 8, -1);
    printf("events: %i\n", e);
    if (x++ == 10) return 0;
    for (i = 0; i < e; i++) {
      char buf[UP_BUF_SIZE];
      if (events[i].events & UPOLLERR) {
        printf("ERROR on %li\n", events[i].data.fd);
        uclose(events[i].data.fd);
      }
      else if (events[i].events & UPOLLIN) {
        printf("have POLLIN\n");
        if (events[i].data.fd == sd1) {
          int sd2 = uaccept(sd1);
          printf("uaccept: %i\n", sd2);
          ev2.events = UPOLLIN;
          ev2.data.fd = sd2;
          upoll_ctl(upfd, UPOLL_CTL_ADD, sd2, &ev2);
        }
        else {
          int cfd = events[i].data.fd;
          memset(buf, 0, UP_BUF_SIZE);
          printf("client input...\n");
          r = uread(cfd, buf, UP_BUF_SIZE);
          printf("uread %i bytes\n", r);
          ev2.events = UPOLLOUT;
          upoll_ctl(upfd, UPOLL_CTL_MOD, cfd, &ev2);
        }
      }
      else if (events[i].events & UPOLLOUT) {
        printf("client writable...\n");
        int cfd = events[i].data.fd;
        uwrite(cfd, buf, r);
        ev2.events = UPOLLIN;
        printf("wrote, mod for UPOLLIN again\n");
        upoll_ctl(upfd, UPOLL_CTL_MOD, cfd, &ev2);
      }
    }
  }

  return 0;
}

