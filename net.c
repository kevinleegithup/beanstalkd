#include <netdb.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "dat.h"
#include "sd-daemon.h"

int
make_server_socket(char *host, char *port)
{
    int fd = -1, flags, r;
    struct linger linger = {0, 0};
    struct addrinfo *airoot, *ai, hints;

    /* See if we got a listen fd from systemd. If so, all socket options etc
     * are already set, so we check that the fd is a TCP listen socket and
     * return. */
    r = sd_listen_fds(1);
    if (r < 0) {
        return twarn("sd_listen_fds"), -1;
    }
    if (r > 0) {
        if (r > 1) {
            twarnx("inherited more than one listen socket;"
                   " ignoring all but the first");
        }
        fd = SD_LISTEN_FDS_START;
        r = sd_is_socket_inet(fd, 0, SOCK_STREAM, 1, 0);
        if (r < 0) {
            errno = -r;
            twarn("sd_is_socket_inet");
            return -1;
        }
        if (!r) {
            twarnx("inherited fd is not a TCP listen socket");
            return -1;
        }
        return fd;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    r = getaddrinfo(host, port, &hints, &airoot);
    if (r == -1)
      return twarn("getaddrinfo()"), -1;

    for(ai = airoot; ai; ai = ai->ai_next) {
      fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd == -1) {
        twarn("socket()");
        continue;
      }

      flags = fcntl(fd, F_GETFL, 0);
      if (flags < 0) {
        twarn("getting flags");
        close(fd);
        continue;
      }

      /////设置为非阻塞 what?
      r = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      if (r == -1) {
        twarn("setting O_NONBLOCK");
        close(fd);
        continue;
      }

      /*
      1、当有一个有相同本地地址和端口的socket1处于TIME_WAIT状态时，而你启动的程序的socket2要占用该地址和端口，你的程序就要用到该选项。 
      2、SO_REUSEADDR允许同一port上启动同一服务器的多个实例(多个进程)。但每个实例绑定的IP地址是不能相同的。在有多块网卡或用IP Alias技术的机器可以测试这种情况。 
      3、SO_REUSEADDR允许单个进程绑定相同的端口到多个socket上，但每个socket绑定的ip地址不同。这和2很相似，区别请看UNPv1。 
      4、SO_REUSEADDR允许完全相同的地址和端口的重复绑定。但这只用于UDP的多播，不用于TCP。
      */
      flags = 1;
      r = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flags, sizeof flags);
      if (r == -1) {
        twarn("setting SO_REUSEADDR on fd %d", fd);
        close(fd);
        continue;
      }
      /*
      设置SO_KEEPALIVE选项来开启KEEPALIVE，然后通过TCP_KEEPIDLE、TCP_KEEPINTVL和TCP_KEEPCNT设置keepalive的开始时间、间隔、次数等参数。
      当然，也可以通过设置/proc/sys/net/ipv4/tcp_keepalive_time、tcp_keepalive_intvl和tcp_keepalive_probes等内核参数来达到目的，但是这样的话，会影响所有的socket，因此建议使用setsockopt设置。
      */
      r = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &flags, sizeof flags);
      if (r == -1) {
        twarn("setting SO_KEEPALIVE on fd %d", fd);
        close(fd);
        continue;
      }
      /*
      linger，顾名思义是延迟延缓的意思，这里是延缓面向连接的socket的close操作。
      默认，close立即返回，但是当发送缓冲区中还有一部分数据的时候，系统将会尝试将数据发送给对端。SO_LINGER可以改变close的行为
      这个选项需要谨慎使用，尤其是强制式关闭，会丢失服务器发给客户端的最后一部分数据。UNP中:
The TIME_WAIT state is our friend and is there to help us(i.e., to let the old duplicate segments expire in the network).
      */
      r = setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger, sizeof linger);
      if (r == -1) {
        twarn("setting SO_LINGER on fd %d", fd);
        close(fd);
        continue;
      }
      /*
      快速发包
      cork是大量发包
      */
      r = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flags, sizeof flags);
      if (r == -1) {
        twarn("setting TCP_NODELAY on fd %d", fd);
        close(fd);
        continue;
      }

      if (verbose) {
          char hbuf[NI_MAXHOST], pbuf[NI_MAXSERV], *h = host, *p = port;
          r = getnameinfo(ai->ai_addr, ai->ai_addrlen,
                  hbuf, sizeof hbuf,
                  pbuf, sizeof pbuf,
                  NI_NUMERICHOST|NI_NUMERICSERV);
          if (!r) {
              h = hbuf;
              p = pbuf;
          }
          if (ai->ai_family == AF_INET6) {
              printf("bind %d [%s]:%s\n", fd, h, p);
          } else {
              printf("bind %d %s:%s\n", fd, h, p);
          }
      }
      r = bind(fd, ai->ai_addr, ai->ai_addrlen);
      if (r == -1) {
        twarn("bind()");
        close(fd);
        continue;
      }

      r = listen(fd, 1024);
      if (r == -1) {
        twarn("listen()");
        close(fd);
        continue;
      }

      break;
    }

    freeaddrinfo(airoot);

    if(ai == NULL)
      fd = -1;

    return fd;
}
