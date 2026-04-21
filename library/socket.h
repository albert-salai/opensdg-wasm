#ifndef INTERNAL_SOCKET_H
#define INTERNAL_SOCKET_H

#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <unistd.h>

int closesocket(int s);
int connect_to_host(struct _osdg_connection *client, const char *host, unsigned short port);
int receive_data(struct _osdg_connection *client);
osdg_result_t send_data(const unsigned char *buffer, int size, struct _osdg_connection *client);

#endif
