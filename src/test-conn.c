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

  int rc = uconnect(sd1, "www.google.com", 80);
  if (rc < 0) printf("ERROR: %d\n", rc);

  upoll_event_t ev1;
  upoll_event_t evs[1];

  ev1.events = UPOLLOUT;
  ev1.data.fd = sd1;

  int i, e, r, w;
  char buf[UP_BUF_SIZE];
  memset(buf, 0, sizeof buf);

  const char* msg = "GET / HTTP/1.0\r\n\r\n";

  upoll_ctl(upfd, UPOLL_CTL_ADD, sd1, &ev1);

  printf("about to POLLOUT\n");
  e = upoll_wait(upfd, evs, 1, -1);
  printf("poll[1] got %d events\n", e);
  w = uwrite(sd1, msg, strlen(msg));
  printf("write finished %i\n", w);

  ev1.events = UPOLLIN;
  upoll_ctl(upfd, UPOLL_CTL_MOD, sd1, &ev1);
  printf("about to POLLIN\n");
  e = upoll_wait(upfd, evs, 1, -1);
  printf("poll[2] got %d events\n", e);
  r = uread(sd1, buf, UP_BUF_SIZE);

  uclose(sd1);
  printf("READ: %i bytes, %s\n", r, buf);

  return 0;
}

