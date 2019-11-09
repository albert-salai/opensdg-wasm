#include <ctype.h>
#include <locale.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#endif

#include "opensdg.h"

#ifdef _WIN32

static void printWSAError(const char *msg, int err)
{
    char *str;

    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, err, LANG_USER_DEFAULT, (LPSTR)&str, 1, NULL);
    fprintf(stderr, "%s: %s\n", msg, str);
    LocalFree(str);
}

static void initSockets(void)
{
  WSADATA wsData;
  int err = WSAStartup(MAKEWORD(2, 2), &wsData);

  if (err)
  {
    printWSAError("Failed to initialize winsock", err);
    exit(255);
  }
}

#else

static void printWSAError(const char *msg, int err)
{
  fprintf(stderr, "%s: %s\n", msg, strerror(err));
}

static inline void initSockets(void)
{
}

static inline void WSACleanup(void)
{
}

#endif

int read_file(unsigned char *buffer, int size, const char *name)
{
    FILE *f = fopen(name, "rb");

    if (!f)
      return -1;

    if (fread(buffer, size, 1, f) != 1)
      return -1;

    fclose(f);
    return 0;
}

static void print_client_error(osdg_client_t client)
{
  enum osdg_error_kind kind = osdg_client_get_error_kind(client);

  switch (kind)
  {
  case osdg_socket_error:
    printWSAError("Socket I/O error", osdg_client_get_error_code(client));
    break;
  case osdg_encryption_error:
    printf("Libsodium encryption error\n");
    break;
  case osdg_decryption_error:
    printf("Libsodium decryption error\n");
    break;
  default:
    printf("Unknon error kind %d\n", kind);
    break;
  }
}

const char *getWord(char **p)
{
  char *buffer = *p;
  char *end;

  for (end = buffer; *end; end++)
  {
    if (isspace(*end))
    {
      *end++ = 0;
      while (isspace(*end))
        end++;
      break;
    }
  }
  *p = end;
  return buffer;
}

static pthread_t inputThread;

static void *input_loop(void *arg)
{
  osdg_client_t client = arg;
  int ret = osdg_client_main_loop(client);

  if (ret)
    print_client_error(client);
  else
    printf("Main loop exited normally\n");

  return NULL;
}

/* Danfoss cloud servers */
static const struct osdg_endpoint servers[] =
{
  {"77.66.11.90" , 443},
  {"77.66.11.92" , 443},
  {"5.179.92.180", 443},
  {"5.179.92.182", 443},
  {NULL, 0}
};

int main()
{
  SOCKET s;
  osdg_key_t clientKey;
  osdg_client_t client;
  int r;

  /* This switches off DOS compatibility mode on Windows console */
  setlocale(LC_ALL, "");
  initSockets();

  r = read_file(clientKey, sizeof(clientKey), "osdg_test_private_key.bin");
  if (r)
  {
      /* TODO: Generate and save the key */
      printf("Failed to load private key! Leaving uninitialized!\n");
  }

  client = osdg_client_create(clientKey, 1536);
  if (!client)
  {
    printf("Failed to create client!\n");
    return 255;
  }

  r = osdg_client_connect_to_server(client, servers);
  if (r == 0)
  {
    printf("Successfully connected\n");

    r = pthread_create(&inputThread, NULL, input_loop, client);
    if (!r)
    {
      printf("Enter command; \"help\" to get help\n");

      for (;;)
      {
        char buffer[256];
        char *p = buffer;
        const char *cmd;

        putchar('>');
        fgets(buffer, sizeof(buffer), stdin);

        cmd = getWord(&p);

        if (!strcmp(cmd, "help"))
        {
          printf("help              - this help\n"
                 "connect [peer Id] - connect to peer\n"
                 "quit              - end session\n");
        }
        else if (!strcmp(cmd, "quit"))
        {
          break;
        }
        else
        {
          printf("Unknown command %s\n", cmd);
        }
      }
    }
    else
    {
      printf("Failed to start input thread!\n");
    }
  }
  else
  {
    print_client_error(client);
  }

  WSACleanup();
  osdg_client_destroy(client);
  return 0;
}
