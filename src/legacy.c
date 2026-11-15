#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_RESPONSE (1024 * 1024)

static int transfer(int fd, void *data, size_t len, int sending)
{
    unsigned char *p = data;
    while (len) {
        ssize_t n = sending ? send(fd, p, len, MSG_NOSIGNAL) : recv(fd, p, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            fprintf(stderr, "Kasa network %s failed: %s\n",
                    sending ? "send" : "receive", n ? strerror(errno) : "connection closed");
            return 0;
        }
        p += n;
        len -= (size_t)n;
    }
    return 1;
}

json_object *legacy_query(const char *ip, int port, const char *request)
{
    struct sockaddr_in addr = {0};
    struct timeval timeout = {5, 0};
    size_t len = strlen(request);
    unsigned char *buf = NULL, key = 171;
    uint32_t n;
    json_object *reply = NULL;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return NULL; }
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) {
        perror("setsockopt"); goto done;
    }
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        perror("Kasa connect"); goto done;
    }
    buf = malloc(len);
    if (!buf) { perror("malloc"); goto done; }
    for (size_t i = 0; i < len; i++) { key ^= (unsigned char)request[i]; buf[i] = key; }
    n = htonl((uint32_t)len);
    if (!transfer(fd, &n, 4, 1) || !transfer(fd, buf, len, 1) ||
        !transfer(fd, &n, 4, 0)) goto done;
    len = ntohl(n);
    if (!len || len > MAX_RESPONSE) { fprintf(stderr, "Invalid Kasa response length\n"); goto done; }
    free(buf);
    buf = malloc(len);
    if (!buf) { perror("malloc"); goto done; }
    if (!transfer(fd, buf, len, 0)) goto done;
    key = 171;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = buf[i];
        buf[i] ^= key;
        key = c;
    }
    reply = parse_json(buf, len);
done:
    free(buf);
    close(fd);
    return reply;
}
