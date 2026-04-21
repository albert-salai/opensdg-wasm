#include "client.h"
#include "mainloop.h"
#include "socket.h"

#include <sys/select.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

static const char* ConnectionModeStr[4] = { "mode_none", "mode_grid", "mode_peer", "mode_pairing" };

int closesocket(int s) {
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
	int err = getpeername(s, (struct sockaddr *) &addr, &addr_len);
	if (! err) {
		char ip_str[INET6_ADDRSTRLEN];
		if (addr.ss_family == AF_INET) {
			struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;
			inet_ntop(AF_INET, &addr4->sin_addr, ip_str, sizeof(ip_str));
			printf("closesocket(): closed socket %d to %s:%d\n", s, ip_str, ntohs(addr4->sin_port));

		} else if (addr.ss_family == AF_INET6) {
			struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&addr;
			inet_ntop(AF_INET6, &addr6->sin6_addr, ip_str, sizeof(ip_str));
			printf("closesocket(): closed socket %d to %s:%d\n", s, ip_str, ntohs(addr6->sin6_port));

		} else {
			printf("closesocket(): Unknown address family\n");
		}
	}

	return close(s);
}


int connect_to_host(struct _osdg_connection *client, const char *host, unsigned short port) {
    struct addrinfo *addr, *ai;
    int res;
    SOCKET s;

    res = getaddrinfo(host, NULL, NULL, &addr);
    if (res)
    {
        LOG(CONNECTION, "connect_to_host(): Failed to resolve %s: %s", host, gai_strerror(res));
        return 0;
    }

    for (ai = addr; ai; ai = ai->ai_next)
    {
        struct sockaddr *addr = ai->ai_addr;

        if (addr->sa_family == AF_INET)
        {
            ((struct sockaddr_in *)addr)->sin_port = htons(port);
        }
        else if (addr->sa_family == AF_INET6)
        {
            ((struct sockaddr_in6 *)addr)->sin6_port = htons(port);
        }
        else
        {
            LOG(CONNECTION, "connect_to_host(): Ignoring unknown address family %u for host %s", addr->sa_family, host);
            continue;
        }

        s = socket(addr->sa_family, SOCK_STREAM, 0);
        if (s == -1)
        {
            client->errorKind = osdg_socket_error;
            client->errorCode = errno;
            LOG(ERRORS, "connect_to_host(): Failed to open socket for AF %d", addr->sa_family);
            res = -1;
            break;
        }

        res = connect(s, addr, (int)ai->ai_addrlen);
        if (!res)
        {
            static unsigned long nonblock = 1;
            LOG(CONNECTION, "connect_to_host(): socket %d Connected to %s:%u (%s, tunnelId %p)", s, host, port, ConnectionModeStr[client->mode], client->tunnelId);

			res = ioctl(s, FIONBIO, &nonblock);
            if (res)
            {
                client->errorKind = osdg_socket_error;
                client->errorCode = errno;
                LOG(ERRORS, "connect_to_host(): Failed to set non-blocking mode");
                closesocket(s);
                break; /* It's a serious error, will return -1 */
            }

			client->sock = s;

            res = start_connection(client);
            if (!res)
            {
                mainloop_send_client_request(&client->req, mainloop_add_connection);
                res = 1; /* Connected */
                break;
            }

            /* This will also close the socket */
            connection_shutdown(client);
        }
        else
        {
            LOG(CONNECTION, "connect_to_host(): Failed to connect to %s:%u", host, port);
            closesocket(s);
            res = 0;
        }
        res = 0; /* OK to try the next address */
    }

    freeaddrinfo(addr);
    return res;
}

int receive_data(struct _osdg_connection *client) {
    while (client->bytesLeft) {			// bytesLeft: number of packet bytes still to be received
        int ret = recv(client->sock, &client->receiveBuffer[client->bytesReceived], client->bytesLeft, 0);
        if (ret < 0) {
            int err = errno;
            if (err == EWOULDBLOCK) {
                return 0; /* Need more data */
			}

            client->errorKind = osdg_socket_error;
            client->errorCode = err;
            return ret;
        }

		if (ret == 0) {
            LOG(ERRORS, "receive_data(): Connection[%p] closed by peer", client);
			client->errorKind = osdg_connection_closed;
            return -1;
        }

        client->bytesReceived += ret;
        client->bytesLeft     -= ret;
    }

    return client->bytesReceived;			// bytesReceived or 0 (need more data) or -1 (recv error)
}

/* For simplicity this function is currently blocking */
osdg_result_t send_data(const unsigned char *buffer, int size, struct _osdg_connection *client) {
    while (size) {
        int ret = send(client->sock, buffer, size, 0);		// returns the number sent or -1

        if (ret >= 0) {
			size   -= ret;
			buffer += ret;

		} else if (errno == EWOULDBLOCK) {
			fd_set wfds;
			FD_ZERO(&wfds);
			FD_SET(client->sock, &wfds);

			ret = select((int) client->sock + 1, NULL, &wfds, NULL, NULL);
			if (ret < 0  &&  errno != EINTR) {
				return osdg_socket_error;
			}

		} else {
			return osdg_socket_error;
        }
    }

    return osdg_no_error;
}
