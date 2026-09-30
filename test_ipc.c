#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

int main(int argc, char* argv[]) {
    const char* sock_path = "/tmp/mpv_iptv.sock";
    if (argc > 1) sock_path = argv[1];

    const char* msg = "{\"command\":[\"show-text\",\"TEST OSD FROM C\",5000]}\n";
    if (argc > 2) msg = argv[2];

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        fprintf(stderr, "socket() failed: %s\n", strerror(errno));
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    fprintf(stderr, "Connecting to %s...\n", sock_path);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "connect() failed: %s\n", strerror(errno));
        close(sock);
        return 2;
    }

    fprintf(stderr, "Sending: %s\n", msg);
    ssize_t sent = send(sock, msg, strlen(msg), MSG_NOSIGNAL);
    fprintf(stderr, "send() = %zd\n", sent);

    // Read response
    char buf[1024];
    ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        buf[n] = '\0';
        fprintf(stderr, "Response: %s\n", buf);
    } else {
        fprintf(stderr, "No response (or error: %s)\n", strerror(errno));
    }

    close(sock);
    return 0;
}
