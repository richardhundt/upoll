
#include "up.h"

#include <stdio.h>
#include <string.h>

#if (defined WIN32 || defined _WIN32)
# include <winsock2.h>
# include <ws2tcpip.h>
#else
# include <sys/types.h>
# include <sys/uio.h>
# include <unistd.h>
# include <sys/socket.h>
# include <netinet/in.h>
#endif


int main(int argc, char** argv) {
#if (defined WIN32 || defined _WIN32)
  WSADATA wsadata;
  int rc = WSAStartup(MAKEWORD(2,0), &wsadata); 
  if (rc) return 1;
#endif

  upoll_t* upq = upoll_create(32);
  intptr_t sd1 = usocket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
  printf("server: %li\n", sd1);

  printf("bind: %d\n", ubind(sd1, "127.0.0.1", "1976"));
  printf("listen: %d\n", ulisten(sd1, 128));

  upoll_event_t ev1, ev2;
  upoll_event_t events[8];

  ev1.events = UPOLLIN;
  ev1.data.fd = sd1;

  upoll_ctl(upq, UPOLL_CTL_ADD, sd1, &ev1);

  int r = 0, x = 0;
  while (1) {
    int i, e;
    e = upoll_wait(upq, events, 8, -1);
    printf("events: %i\n", e);
    if (x++ == 50) return 0;
    for (i = 0; i < e; i++) {
      char buf[4096];
      if (events[i].events & UPOLLERR) {
        printf("ERROR on %li\n", events[i].data.fd);
        upoll_ctl(upq, UPOLL_CTL_DEL, events[i].data.fd, NULL);
        uclose(events[i].data.fd);
      }
      else if (events[i].events & UPOLLIN) {
        printf("have POLLIN on %li\n", events[i].data.fd);
        if (events[i].data.fd == sd1) {
          int sd2 = uaccept(sd1);
          printf("uaccept: %i\n", sd2);
          ev2.events = UPOLLIN;
          ev2.data.fd = sd2;
          upoll_ctl(upq, UPOLL_CTL_ADD, sd2, &ev2);
        }
        else {
          int cfd = events[i].data.fd;
          memset(buf, 0, 4096);
          printf("client input...\n");
          r = uread(cfd, buf, 4096);
          printf("read %i bytes\n", r);
          if (r == 0) {
            printf("EOF DETECTED\n");
            upoll_ctl(upq, UPOLL_CTL_DEL, events[i].data.fd, NULL);
            uclose(events[i].data.fd);
          }
          else {
            ev2.events = UPOLLOUT;
            upoll_ctl(upq, UPOLL_CTL_MOD, cfd, &ev2);
          }
        }
      }
      else if (events[i].events & UPOLLOUT) {
        printf("client writable...\n");
        int cfd = events[i].data.fd;
        int w = uwrite(cfd, buf, r);
        ev2.events = UPOLLIN;
        printf("wrote %i bytes, mod for UPOLLIN again\n", w);
        upoll_ctl(upq, UPOLL_CTL_MOD, cfd, &ev2);
      }
    }
  }

  return 0;
}

