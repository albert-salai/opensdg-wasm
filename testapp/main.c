#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "opensdg.h"
#include "testapp.h"
#include "devismart.h"
#include "devismart_protocol.h"

static int read_file(void *buffer, int size, const char *name)
{
    FILE *f = fopen(name, "rb");
    size_t res;

    if (!f)
      return -1;

    res = fread(buffer, size, 1, f);

    fclose(f);
    return res == 1 ? 0 : -1;
}

static int write_file(void *buffer, int size, const char *name)
{
    FILE *f = fopen(name, "wb");
    size_t res;

    if (!f)
        return -1;

    res = fwrite(buffer, size, 1, f);

    fclose(f);

    if (res == 1)
        return 0;

    printf("Failed to write file %s!\n", name);
    return -1;
}

void print_result(osdg_result_t res)
{
    printf("%s\n", osdg_get_result_str(res));
}

void print_client_error(osdg_connection_t conn)
{
  size_t len = osdg_get_last_result_str(conn, NULL, 0);
  char* buffer = malloc(len);

  if (!buffer)
  {
    printf("Failed to allocate result string!\n");
    return;
  }

  osdg_get_last_result_str(conn, buffer, len);
  printf("%s\n", buffer);
  free(buffer);
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

void hexdump(const unsigned char *data, unsigned int size)
{
  unsigned int i;

  for (i = 0; i < size; i++)
    printf("%02x", data[i]);
}

struct pairing_data
{
  osdg_key_t	peerId;
  char			description[256];
};

static struct pairing_data curr_pairing;

int add_pairing(const osdg_key_t peerId, const char *description) {
  printf("add_pairing(), peerId: ");
  hexdump(peerId, sizeof(osdg_key_t));
  printf(", description: \"%s\"\n", description);

  memcpy(curr_pairing.peerId, peerId, sizeof(osdg_key_t));
  strcpy(curr_pairing.description, description);

  return write_file(&curr_pairing, sizeof(curr_pairing), "osdg_test_pairing.bin");
}

static osdg_connection_t curr_peer;

void print_status(osdg_connection_t conn, enum osdg_connection_state status)
{
    switch (status)
    {
    case osdg_closed:
        printf(" connection closed\n");
        break;
    case osdg_connected:
        printf(" connection established\n");
        break;
    case osdg_error:
        printf(" connection failed: ");
        print_client_error(conn);
        break;
    default:
        printf(" invalid status %u\n", status); /* You should not see this */
        break;
    }
}

static void grid_status_changed(osdg_connection_t conn, enum osdg_connection_state status)
{
    printf("Grid");
    print_status(conn, status);
}

static void peer_status_changed(osdg_connection_t conn, enum osdg_connection_state status)
{
    printf("Peer");
    print_status(conn, status);

    if (status == osdg_closed) {
        curr_peer = NULL;
        osdg_connection_destroy(conn);
    }
}

static void pairing_status_changed(osdg_connection_t conn, enum osdg_connection_state status)
{
	const unsigned char *peerId;

    switch (status)
    {
    case osdg_pairing_complete:
        peerId = osdg_get_peer_id(conn);
		printf("pairing_status_changed(): osdg_pairing_complete with peerId ");
        hexdump(peerId, sizeof(osdg_key_t));
        putchar('\n');

        devismart_config_connect(conn);
		break;

	case osdg_error:
		printf("pairing_status_changed(): osdg_error: ");
        print_client_error(conn);
        break;

	default:
        printf("Invalid pairing status %u\n", status); /* You should not see this */
        break;
    }
}

static osdg_result_t default_peer_receive_data(osdg_connection_t conn, const void *data, unsigned int length)
{
    printf("Received data from the peer: ");
    dump_data(data, length);
    return osdg_no_error;
}

static void connect_to_peer(osdg_connection_t client)
{
  const char *protocol = DEVISMART_PROTOCOL_NAME;
  osdg_connection_t peer;

  if (curr_peer != NULL)
  {
    printf("Already connected to peer\n");
    return;
  }

  peer = osdg_connection_create();
  if (!peer)
  {
    printf("Failed to create peer!\n");
    return;
  }

  osdg_set_state_change_callback(peer, peer_status_changed);

  osdg_result_t err = osdg_set_receive_data_callback(peer, devismart_receive_data);
  if (err) {
      printf("Failed to set data receive callback: ");
      print_result(err);
      osdg_connection_destroy(peer);
      return;
  }

  err = osdg_connect_to_remote(client, peer, curr_pairing.peerId, protocol);
  if (err) {
    printf("Failed to start connection: ");
    print_result(err);
    osdg_connection_destroy(peer);
    return;
  }

  printf("Created connection to peer\n");
  curr_peer = peer;
}

static void pair_remote_peer(osdg_connection_t client, char *argStr)
{
  const char *otp = getWord(&argStr);
  osdg_connection_t peer;
  osdg_result_t res;

  peer = osdg_connection_create();
  if (!peer)
  {
      printf("Failed to create peer!\n");
      return;
  }

  osdg_set_state_change_callback(peer, pairing_status_changed);

  res = osdg_pair_remote(client, peer, otp);
  if (res != osdg_no_error)
  {
      printf("Failed to start connection: ");
      print_result(res);
      osdg_connection_destroy(peer);
      return;
  }
}

static void close_connection()
{
	if (curr_peer != NULL) {
		osdg_connection_close(curr_peer);
	}
}

static void send_data(char *argStr)
{
	if (curr_peer != NULL) {
		osdg_result_t res = devismart_send(curr_peer, argStr);

		if (res != osdg_no_error)
			print_result(res);
	}
}

static void set_ping_interval(osdg_connection_t client, char *argStr)
{
    const char *arg = getWord(&argStr);
    char *end;
    unsigned int val = strtoul(arg, &end, 10);
    osdg_result_t r;

    if (arg == end)
    {
        printf("Invalid ping interval %s!\n", arg);
        return;
    }

    r = osdg_set_ping_interval(client, val);
    if (r != osdg_no_error)
    {
        printf("Failed to set ping interval: ");
        print_result(r);
    }
}

static osdg_connection_t client;

osdg_connection_t get_grid_connection(void)
{
    return client;
}

static void grid_control(char* argStr)
{
    const char* action = getWord(&argStr);
    osdg_result_t res;

    if (!strcmp(action, "connect")) {
        res = osdg_connect_to_danfoss(client);

	} else if (!strcmp(action, "disconnect")) {
        res = osdg_connection_close(client);

	} else {
        printf("Invalid subcommand %s\n", action);
        return;
    }

    if (res != osdg_no_error)
        print_client_error(client);
}


int main(int argc, const char *const *argv)
{
  unsigned int logmask = OSDG_LOG_ERRORS;
  struct osdg_version ver;
  osdg_key_t clientKey;
  int i;
  osdg_result_t r;

  /* This switches off DOS compatibility mode on Windows console */
  setlocale(LC_ALL, "");

  osdg_get_version(&ver);
  printf("Using libopensdg v%u.%u.%u\n", ver.major, ver.minor, ver.patch);

  for (i = 1; i < argc; i++)
  {
      if (!strcmp(argv[i], "-l"))
      {
          logmask = atoi(argv[i + 1]);
          printf("Logging mask set to 0x%08X\n", logmask);
          i++;
      }
  }

  /* The only thing we can call before osdg_init() */
  osdg_set_log_mask(logmask);

  r = osdg_init();
  if (r)
  {
      printf("Failed to initialize OSDG: ");
      print_result(r);
      return 255;
  }

  i = read_file(clientKey, sizeof(clientKey), "osdg_test_private_key.bin");
  if (!i)
  {
    printf("Loaded private key:  ");
    dump_data(clientKey, sizeof(clientKey));
  }
  else
  {
      osdg_create_private_key(clientKey);
      printf("Generated private key: ");
      dump_data(clientKey, sizeof(clientKey));
      write_file(clientKey, sizeof(clientKey), "osdg_test_private_key.bin");
  }

  i = read_file(&curr_pairing, sizeof(curr_pairing), "osdg_test_pairing.bin");
  if (!i)
  {
    printf("Loaded pairing peer: ");
    dump_data(curr_pairing.peerId, sizeof(curr_pairing.peerId));
  } else {
    printf("not yet paired.\n");
  }

  client = osdg_connection_create();
  if (!client)
  {
    printf("Failed to create client!\n");
    return 255;
  }

  osdg_set_state_change_callback(client, grid_status_changed);
  osdg_set_private_key(client, clientKey);

  /* Retry until we succeed. This helped to catch a bug. */
  do {
      r = osdg_connect_to_danfoss(client);
      if (r != osdg_no_error)
      {
          print_client_error(client);
          sleep(1);
      }
  } while (r != osdg_no_error);

  printf("Enter command; \"help\" to get help\n");

  for (;;)
  {
    char buffer[256];
    char *p = buffer;
    const char *cmd;

    putchar('>');
    fgets(buffer, sizeof(buffer), stdin);

    cmd = getWord(&p);

    if (!cmd[0])
        continue;

    if (!strcmp(cmd, "help"))
    {
        printf("help                        - this help\n"
                "close                      - close connection to peer\n"
                "connect                    - connect to peer\n"
                "grid [connect|disconnect]  - manually control grid connection\n"
                "show pairing               - show current pairing\n"
                "show peer                  - show current peer\n"
                "pair [OTP]                 - pair with the given OTP\n"
                "ping [interval]            - set grid ping interval in seconds\n"
                "send [connection #] [data] - send data to a peer\n"
                "quit                       - end session\n"
                "whoami                     - print own peer information\n");
    }
    else if (!strcmp(cmd, "connect"))
    {
        connect_to_peer(client);
    }
    else if (!strcmp(cmd, "pair"))
    {
        pair_remote_peer(client, p);
    }
    else if (!strcmp(cmd, "close"))
    {
        close_connection();
    }
    else if (!strcmp(cmd, "grid"))
    {
        grid_control(p);
    }
    else if (!strcmp(cmd, "show"))
    {
        cmd = getWord(&p);
        if (!strcmp(cmd, "pairing")) {
			hexdump(curr_pairing.peerId, sizeof(osdg_key_t));
			printf(" %s\n", curr_pairing.description);
		}
        else if (!strcmp(cmd, "peer")) {
			hexdump(osdg_get_peer_id(curr_peer), sizeof(osdg_key_t));
			putchar('\n');
		}
        else
        printf("Unknown item %s", cmd);
    }
    else if (!strcmp(cmd, "ping"))
    {
        set_ping_interval(client, p);
    }
    else if (!strcmp(cmd, "send"))
    {
        send_data(p);
    }
    else if (!strcmp(cmd, "quit"))
    {
        break;
    }
    else if (!strcmp(cmd, "whoami"))
    {
        printf("Private key: ");
        hexdump(clientKey, sizeof(osdg_key_t));
        printf("\nPeer ID    : ");
        hexdump(osdg_get_my_peer_id(client), sizeof(osdg_key_t));
        putchar('\n');
    }
    else
    {
        printf("Unknown command %s\n", cmd);
    }
  }

  osdg_connection_close(client);
  osdg_connection_destroy(client);

  osdg_shutdown();
  return 0;
}


/*
Using libopensdg v1.0.3
Logging mask set to 0x00000003
Loaded private key:  77ac12e539999a83c4cff5a97dba416f3e885d0985134ae008205a789f1ce5d5
Loaded pairing peer: 6e737908d4f454e0385e251bb69b66d9c1e8bd0d71db73842dbe603b2ee45c03
connect_to_host(): socket 4 Connected to 5.179.92.182:443 (mode_grid, tunnelId (nil))
Enter command; "help" to get help
>Grid connection established

>
>pair 7040251
>connect_to_host(): socket 5 Connected to 5.179.92.182:443 (mode_pairing, tunnelId 0x7f88000c70)
closesocket(): closed socket 5 to 5.179.92.182:443
pairing_status_changed(): osdg_pairing_complete with peerId 9f671f92b2e8dc48c24f0b070a6703d426a70066f0389c8b6ff33fc3ccca1c2e
Peer[0] connection refused; code 1
DeviSmart config connection failed: Connection refused by peer

>
>
>pair 1631684
>connect_to_host(): socket 5 Connected to 5.179.92.182:443 (mode_pairing, tunnelId 0x7f88000c70)
closesocket(): closed socket 5 to 5.179.92.182:443
pairing_status_changed(): osdg_pairing_complete with peerId 9f671f92b2e8dc48c24f0b070a6703d426a70066f0389c8b6ff33fc3ccca1c2e
connect_to_host(): socket 5 Connected to 5.179.92.182:443 (mode_peer, tunnelId 0x7f88000c50)
DeviSmart config connection established
Requesting DEVISmart config on connection 0x556c9a2000
Sending configuration request: {"phoneName":"OpenSDG test","phonePublicKey":"3d83d4a8aee86ab48d864b1eda8c7e3c6e96e6b85b4d77bf8b2d7cfdb795fe21","chunkedMessage":true}

>
>
>pair 9275472
>connect_to_host(): socket 6 Connected to 5.179.92.182:443 (mode_pairing, tunnelId 0x7f88000c70)
closesocket(): closed socket 6 to 5.179.92.182:443
pairing_status_changed(): osdg_pairing_complete with peerId 9f671f92b2e8dc48c24f0b070a6703d426a70066f0389c8b6ff33fc3ccca1c2e

connect_to_host(): socket 6 Connected to 5.179.92.182:443 (mode_peer, tunnelId 0x7f88000c30)
DeviSmart config connection established
Requesting DEVISmart config on connection 0x556c9a2290
Sending configuration request: {"phoneName":"OpenSDG test","phonePublicKey":"3d83d4a8aee86ab48d864b1eda8c7e3c6e96e6b85b4d77bf8b2d7cfdb795fe21","chunkedMessage":true}

Full size of chunked data: 502
parse_config_data(): {"rooms":[{"sortOrder":3,"zone":"Living","roomName":"Schlafzimmer"},{"sortOrder":4,"zone":"Living","roomName":"Büro"},{"roomName":"Wohnzimmer","zone":"Living","sortOrder":0},{"roomName":"Bad","zone":"None","sortOrder":0},{"roomName":"WC","zone":"Living","sortOrder":1},{"zone":"Living","roomName":"Kaminzimmer","sortOrder":5},{"sortOrder":2,"zone":"Living","roomName":"Küche"}],"housePeerId":"6e737908d4f454e0385e251bb69b66d9c1e8bd0d71db73842dbe603b2ee45c03","houseEditUsers":false,"houseName":"M41"}
add_pairing(), peerId: 6e737908d4f454e0385e251bb69b66d9c1e8bd0d71db73842dbe603b2ee45c03, description: "M41"
closesocket(): closed socket 6 to 5.179.92.182:443
DeviSmart config connection closed

>
>
>receive_data(): Connection[0x556c9a2000] closed by peer
Conn[0x556c9a2000] died: Connection closed by peer system code 0
closesocket(): closed socket 5 to 5.179.92.182:443
DeviSmart config connection failed: Connection closed by peer

>
>
>connect
Created connection to peer
>connect_to_host(): socket 5 Connected to 5.179.92.182:443 (mode_peer, tunnelId 0x7f88000c30)
Peer connection established
MDG_CONNECTION_COUNT        1  03
GLOBAL_POWERCYCLECOUNTER    1  00
GLOBAL_SOFTWAREBUILDREVISION   2  0000
GLOBAL_NUMBEROFENDPOINTS    1  07
GLOBAL_DIVISIONID           1  03
GLOBAL_BRANDID              1  00
GLOBAL_PRODUCTID            2  0110
GLOBAL_COUNTRYISOCODE       4  03303020
GLOBAL_REVISION             4  07010101
GLOBAL_SERIALNUMBER         4  01380000
GLOBAL_TEXTREVISION         2  ffff
GLOBAL_AVAILABLEENDPOINTS  32  ff01000000000000000000000000000000000000000000000000000000000000
GLOBAL_HARDWAREREVISION     2  0101
GLOBAL_SOFTWAREREVISION     2  0701
GLOBAL_PRODUCTIONDATE Wed 17.2.2021 23:09:26 UTC
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_RESET_LEVEL   1  00
TESTANDPRODUCTION_DEVICE_DESCRIPTION  33  200000000000000000000000000000000000000000000000000000000000000000
TESTANDPRODUCTION_RESTARTSIMPLELINK   1  01
TESTANDPRODUCTION_IS_RESTARTING_SIMPLELINK   1  00
TESTANDPRODUCTION_UNCOMPENSATED_ROOM 2.56
code 29717                       1  00
code 29718                       2  0000
code 29719                       2  0000
WIFI_ERROR_CODE             2  0000
WIFI_ROLE                   1  01
WIFI_RESET                  1  00
WIFI_OPERATIONAL_STATE      1  02
WIFI_CHANNEL                1  01
WIFI_SSID_AP ""
WIFI_CONNECTED_SSID "Hintersberg2"
WIFI_CONNECT_SSID "Hintersberg2"
WIFI_CONNECT_KEY "Melkstatt41!"
WIFI_CONNECT                1  01
WIFI_NETWORK_PROCESSOR_POWER   1  00
WIFI_MAX_LONG_SLEEP         2  2003
WIFI_TX_POWER               1  01
WIFI_MDG_READY_FOR_RESTART   1  00
WIFI_NVM_READY_FOR_RESTART   1  00
WIFI_SCAN_SSID_0 "Hintersberg2"
WIFI_SCAN_SSID_1 ""
WIFI_SCAN_SSID_2 ""
WIFI_SCAN_SSID_3 ""
WIFI_SCAN_SSID_4 ""
WIFI_SCAN_SSID_5 ""
WIFI_SCAN_SSID_6 ""
WIFI_SCAN_SSID_7 ""
WIFI_SCAN_SSID_8 ""
WIFI_SCAN_SSID_9 ""
WIFI_SCAN_STRENGTH_0        1  e7
WIFI_SCAN_STRENGTH_1        1  00
WIFI_SCAN_STRENGTH_2        1  00
WIFI_SCAN_STRENGTH_3        1  00
WIFI_SCAN_STRENGTH_4        1  00
WIFI_SCAN_STRENGTH_5        1  00
WIFI_SCAN_STRENGTH_6        1  00
WIFI_SCAN_STRENGTH_7        1  00
WIFI_SCAN_STRENGTH_8        1  00
WIFI_SCAN_STRENGTH_9        1  00
WIFI_DISCONNECT_COUNT       2  0000
WIFI_SKIP_AP_MODE           1  01
WIFI_CONNECTED_STRENGTH     2  d7ff
WIFI_UPDATE_CONNECTED_STRENGTH   1  00
code 29806                       1  02
code 29807                       1  00
MDG_ERROR_CODE              2  0000
MDG_CONNECTED_TO_SERVER     1  01
MDG_SHOULD_CONNECT          1  01
MDG_PAIRING_COUNT           1  04
MDG_PAIRING_0_ID 3944b602c51ad4552384b8e4cf8d6e2fb72f106cd35f68c7d616fdf7c3c9aa41
MDG_PAIRING_0_DESCRIPTION "switchboard"
MDG_PAIRING_0_PAIRING_TIME   6  342d08590319
MDG_PAIRING_0_PAIRING_TYPE   1  0f
MDG_PAIRING_1_ID 3d83d4a8aee86ab48d864b1eda8c7e3c6e96e6b85b4d77bf8b2d7cfdb795fe21
MDG_PAIRING_1_DESCRIPTION "OpenSDG test"
MDG_PAIRING_1_PAIRING_TIME   6  251807bc0319
MDG_PAIRING_1_PAIRING_TYPE   1  00
MDG_PAIRING_2_ID d30b02a727bc5b3dfc6d4e9dc455c33956463987e042ac670c30e0e3434bbc05
MDG_PAIRING_2_DESCRIPTION "Maxi"
MDG_PAIRING_2_PAIRING_TIME   6  252410cd0517
MDG_PAIRING_2_PAIRING_TYPE   1  00
MDG_PAIRING_3_ID 9f671f92b2e8dc48c24f0b070a6703d426a70066f0389c8b6ff33fc3ccca1c2e
MDG_PAIRING_3_DESCRIPTION "Albi"
MDG_PAIRING_3_PAIRING_TIME   6  0e0210160918
MDG_PAIRING_3_PAIRING_TYPE   1  0f
MDG_PAIRING_4_ID
MDG_PAIRING_4_DESCRIPTION ""
MDG_PAIRING_4_PAIRING_TIME   6  000000000000
MDG_PAIRING_4_PAIRING_TYPE   1  00
MDG_PAIRING_5_ID 0000000000000000000000000000000000000000000000000000000000000000
MDG_PAIRING_5_DESCRIPTION ""
MDG_PAIRING_5_PAIRING_TIME   6  000000000000
MDG_PAIRING_5_PAIRING_TYPE   1  00
MDG_PAIRING_6_ID 0000000000000000000000000000000000000000000000000000000000000000
MDG_PAIRING_6_DESCRIPTION ""
MDG_PAIRING_6_PAIRING_TIME   6  000000000000
MDG_PAIRING_6_PAIRING_TYPE   1  00
MDG_PAIRING_7_ID 0000000000000000000000000000000000000000000000000000000000000000
MDG_PAIRING_7_DESCRIPTION ""
MDG_PAIRING_7_PAIRING_TIME   6  000000000000
MDG_PAIRING_7_PAIRING_TYPE   1  00
MDG_PAIRING_8_ID 0000000000000000000000000000000000000000000000000000000000000000
MDG_PAIRING_8_DESCRIPTION ""
MDG_PAIRING_8_PAIRING_TIME   6  000000000000
MDG_PAIRING_8_PAIRING_TYPE   1  00
MDG_PAIRING_9_ID 0000000000000000000000000000000000000000000000000000000000000000
MDG_PAIRING_9_DESCRIPTION ""
MDG_PAIRING_9_PAIRING_TIME   6  000000000000
MDG_PAIRING_9_PAIRING_TYPE   1  00
MDG_REVOKE_SPECIFIC_PAIRING  33  000000000000000000000000000000000000000000000000000000000000000000
MDG_REVOKE_ALL_PAIRINGS     1  00
MDG_CONNECTION_COUNT        1  03
MDG_PENDING_PAIRING        33  200000000000000000000000000000000000000000000000000000000000000000
MDG_ADD_PAIRING 3d83d4a8aee86ab48d864b1eda8c7e3c6e96e6b85b4d77bf8b2d7cfdb795fe21
MDG_SERVER_DISCONNECT_COUNT   2  0000
MDG_PAIRING_NOTIFICATION_TOKEN 255  fe0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
MDG_PAIRING_NOTIFICATION_SUBSCRIPTIONS   4  00000000
MDG_PAIRING_0_NOTIFICATION_TOKEN "e528969b6aed56536b4d0f627ff9cdc67185ff4f9c8ee41df8935a0967ca9989"
MDG_PAIRING_0_NOTIFICATION_SUBSCRIPTIONS   4  0f000000
MDG_PAIRING_2_NOTIFICATION_TOKEN ""
MDG_PAIRING_2_NOTIFICATION_SUBSCRIPTIONS   4  00000000
MDG_PAIRING_3_NOTIFICATION_TOKEN "7137bb87dc7ed5989c040fcb8e04ecdfc679e5ed8cf7782099724023b60f7084"
MDG_PAIRING_3_NOTIFICATION_SUBSCRIPTIONS   4  0f000000
MDG_PAIRING_4_NOTIFICATION_TOKEN ""
MDG_PAIRING_4_NOTIFICATION_SUBSCRIPTIONS   4  00000000
MDG_PAIRING_5_NOTIFICATION_TOKEN ""
MDG_PAIRING_5_NOTIFICATION_SUBSCRIPTIONS   4  00000000
MDG_PAIRING_6_NOTIFICATION_TOKEN ""
MDG_PAIRING_6_NOTIFICATION_SUBSCRIPTIONS   4  00000000
MDG_PAIRING_7_NOTIFICATION_TOKEN ""
MDG_PAIRING_7_NOTIFICATION_SUBSCRIPTIONS   4  00000000
MDG_PAIRING_8_NOTIFICATION_TOKEN ""
MDG_PAIRING_8_NOTIFICATION_SUBSCRIPTIONS   4  00000000
MDG_PAIRING_9_NOTIFICATION_TOKEN ""
MDG_PAIRING_9_NOTIFICATION_SUBSCRIPTIONS   4  00000000
MDG_PAIRING_1_NOTIFICATION_TOKEN ""
MDG_PAIRING_1_NOTIFICATION_SUBSCRIPTIONS   4  00000000
MDG_PAIRING_DESCRIPTION    33  200000000000000000000000000000000000000000000000000000000000000000
MDG_PAIRING_TYPE            1  00
MDG_CONFIRM_SYSTEM_WIZARD_INFO   7  06000000000000
SOFTWAREUPDATE_ERROR_CODE   2  0000
SOFTWAREUPDATE_DOWNLOAD_PUSHED_UPDATE   2  0000
SOFTWAREUPDATE_CHECK_FOR_UPDATE   1  01
SOFTWAREUPDATE_INSTALLATION_STATE   1  00
SOFTWAREUPDATE_INSTALLATION_PROGRESS   1  00
code 30597                       2  1402
code 30598                       1  00
code 30599                       1  00
code 30600                       1  00
code 30601                       1  00
SYSTEM_TIME_OFFSET          2  3c00
SYSTEM_WINDOW_OPEN          1  00
SYSTEM_LOCAL_CONFIRM_REQUEST   1  00
SYSTEM_LOCAL_CONFIRM_RESPONSE   1  00
SYSTEM_TIME_OFFSET_TABLE  161  a0270a1e040427031b0408260a1f040426031c0408250a19040425031d0408240a1a040424031e0408230a1c04042303190408220a1d040422031a0408210a1e040421031b0408200a1f040420031c04081f0a1a04041f031e04081e0a1b04041e031f04081d0a1c04041d031904081c0a1d04041c031a04081b0a1f04041b031c04081a0a1904041a031d0408190a1a040419031e0408180a1b04041808164008
SYSTEM_MDG_CONNECT_PROGRESS   1  04
SYSTEM_MDG_CONNECT_PROGRESS_MAX   1  04
SYSTEM_MDG_CONNECT_ERROR    1  00
SYSTEM_MDG_LOG_UNTIL        6  000000000000
GLOBAL_SOFTWAREREVISION     2  0c01
code 29276                       1  01
code 29277                       1  00
GLOBAL_NUMBEROFENDPOINTS    1  8c
GLOBAL_DIVISIONID           1  01
GLOBAL_BRANDID              1  00
GLOBAL_PRODUCTID            2  0000
GLOBAL_COUNTRYISOCODE       4  03303020
GLOBAL_REVISION             4  14020001
GLOBAL_SERIALNUMBER         4  00000000
GLOBAL_TEXTREVISION         2  ffff
GLOBAL_AVAILABLEENDPOINTS  32  fffffffffffffffffffffffffffffffffff00000000000000000000000000000
GLOBAL_SOFTWAREREVISION     2  0000
code    64                       2  0000
code   126                       1  00
code   127                       6  050000000000
code  4097                       1  01
GLOBAL_POWERCYCLECOUNTER    1  57
GLOBAL_POWERCYCLECOUNTER    1  00
GLOBAL_POWERCYCLECOUNTER    1  00
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
GLOBAL_REVISION             4  14020001
GLOBAL_REVISION             4  00000000
GLOBAL_REVISION             4  00000000
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   304                       6  1200149b0319
code   304                       6  1200149b0319
code   304                       6  1200149b0319
code   776                       2  0080
code   776                       2  0080
code   776                       2  0080
code   785                       2  a00f
code   785                       2  a00f
code   785                       2  a00f
code   786                       2  b80b
code   786                       2  b80b
code   786                       2  b80b
code   787                       2  8813
code   787                       2  8813
code   787                       2  8813
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   802                       2  c800
code   802                       2  c800
code   802                       2  c800
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
code  4099                       1  00
code  4099                       1  00
code  4099                       1  00
code  4101                       1  00
code  4101                       1  00
code  4101                       1  00
code  4102                       1  00
code  4102                       1  00
code  4102                       1  00
code  4103                       1  00
code  4103                       1  00
code  4103                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4215                       1  00
code  4215                       1  00
code  4215                       1  00
code  4216                       2  8c0a
code  4216                       2  8c0a
code  4216                       2  8c0a
code  4218                       2  7c15
code  4218                       2  7c15
code  4218                       2  7c15
code  4219                       2  c800
code  4219                       2  c800
code  4219                       2  c800
code  4220                       1  06
code  4220                       1  06
code  4220                       1  06
code  4221                       2  3408
code  4221                       2  3408
code  4221                       2  3408
code  4222                       1  00
code  4222                       1  00
code  4222                       1  00
code  4229                       1  02
code  4229                       1  02
code  4229                       1  02
code  4352                       1  01
code  4352                       1  00
code  4352                       1  00
code  4353                       1  00
code  4353                       1  00
code  4353                       1  00
code  5927                       1  00
code  5927                       1  00
code  5927                       1  00
code 28691                       1  40
code 28691                       1  40
code 28691                       1  40
code 28695                       4  ffffffff
code 28695                       4  ffffffff
code 28695                       4  ffffffff
code 28696                       1  40
code 28696                       1  40
code 28696                       1  40
code 28724                       1  00
code 28724                       1  00
code 28724                       1  00
code 28725                       1  00
code 28725                       1  00
code 28725                       1  00
code 28726                       2  b400
code 28726                       2  b400
code 28726                       2  b400
code 28727                       2  0000
code 28727                       2  0000
code 28727                       2  0000
code 28728                       1  00
code 28728                       1  00
code 28728                       1  00
code 28730                       1  00
code 28730                       1  00
code 28730                       1  00
code 28731                       2  b400
code 28731                       2  b400
code 28731                       2  b400
code 28732                       2  0000
code 28732                       2  0000
code 28732                       2  0000
code 28735                       2  0000
code 28735                       2  0000
code 28735                       2  0000
code 28736                       2  ff0f
code 28736                       2  0000
code 28736                       2  0000
code 28737                       2  ff0f
code 28737                       2  0000
code 28737                       2  0000
code 28739                       2  7000
code 28739                       2  0000
code 28739                       2  0000
code 28740                       1  78
code 28740                       1  78
code 28740                       1  78
code 28741                       1  00
code 28741                       1  00
code 28741                       1  00
code 28742                       4  3c000000
code 28742                       4  3c000000
code 28742                       4  3c000000
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  ff
code   780                       1  ff
code   780                       1  e0
code   780                       1  35
code   780                       1  00
code   780                       1  00
code   780                       1  3b
code   780                       1  3b
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
code   780                       1  00
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
code  4104                       1  07
code  4104                       1  07
code  4104                       1  01
code  4104                       1  01
code  4104                       1  03
code  4104                       1  03
code  4104                       1  02
code  4104                       1  04
code  4104                       1  05
code  4104                       1  05
code  4104                       1  06
code  4104                       1  06
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4104                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  01
code  4608                       1  01
code  4608                       1  01
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4608                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code  4609                       1  00
code 28692                       1  3c
code 28692                       1  3c
code 28692                       1  3c
code 28692                       1  3c
code 28692                       1  3c
code 28692                       1  3c
code 28692                       1  28
code 28692                       1  28
code 28692                       1  3c
code 28692                       1  3c
code 28692                       1  3c
code 28692                       1  3c
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28692                       1  0f
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28705                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code 28738                       1  00
code     4                       4  e24625e6
code     4                       4  aaee39e6
code     4                       4  f88b33e6
code     4                       4  e26a21e6
code     4                       4  965939e6
code     4                       4  20bc2fe6
code     4                       4  4d1f22e6
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   128                       7  80410100802000
code   128                       7  80420100802000
code   128                       7  80430100802000
code   128                       7  80440100802000
code   128                       7  80450100802000
code   128                       7  80460100802000
code   128                       7  80470100802000
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   768                       2  0b07
code   768                       2  0108
code   768                       2  1808
code   768                       2  f807
code   768                       2  d406
code   768                       2  de07
code   768                       2  e307
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   768                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   772                       2  0080
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   778                       1  00
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   783                       1  64
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   800                       2  0807
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   801                       2  2c01
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   802                       2  1400
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   809                       2  0080
code   816                       2  d606
code   816                       2  0208
code   816                       2  3408
code   816                       2  0208
code   816                       2  a406
code   816                       2  0208
code   816                       2  d007
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
code   816                       2  3408
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  e803
code  1287                       2  9001
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1287                       2  f401
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1288                       2  ac0d
code  1289                       2  d606
code  1289                       2  0208
code  1289                       2  3408
code  1289                       2  0208
code  1289                       2  a406
code  1289                       2  6c07
code  1289                       2  d007
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1289                       2  3408
code  1290                       2  d606
code  1290                       2  0208
code  1290                       2  3408
code  1290                       2  9e07
code  1290                       2  a406
code  1290                       2  0208
code  1290                       2  d007
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1290                       2  e803
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1291                       2  a406
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1292                       2  0807
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  1293                       2  ac0d
code  4106                       1  01
code  4106                       1  01
code  4106                       1  01
code  4106                       1  01
code  4106                       1  01
code  4106                       1  01
code  4106                       1  01
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4106                       1  00
code  4107                       1  01
code  4107                       1  01
code  4107                       1  01
code  4107                       1  01
code  4107                       1  01
code  4107                       1  01
code  4107                       1  01
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4107                       1  14
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaa2a00aaaaaaaaaaaaaaaa
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaa2a00aaaaaaaaaaaaaaaa
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaa2a00aaaaaaaaaaaaaaaa
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaa2a00aaaaaaaaaaaaaaaa
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaa82aaaaaaaaaaaaaa
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaa82aaaaaaaaaaaaaa
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaa82aaaaaaaaaaaaaa
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4115                       1  00
code  4115                       1  01
code  4115                       1  01
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4115                       1  00
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4117                       2  0000
code  4118                       2  d606
code  4118                       2  0208
code  4118                       2  3408
code  4118                       2  9e07
code  4118                       2  a406
code  4118                       2  6c07
code  4118                       2  d007
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4118                       2  e803
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4119                       2  0000
code  4120                       1  01
code  4120                       1  01
code  4120                       1  01
code  4120                       1  01
code  4120                       1  01
code  4120                       1  01
code  4120                       1  01
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4120                       1  00
code  4121                       1  01
code  4121                       1  01
code  4121                       1  01
code  4121                       1  01
code  4121                       1  01
code  4121                       1  01
code  4121                       1  01
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4121                       1  14
code  4128                       2  0c00
code  4128                       2  0000
code  4128                       2  3000
code  4128                       2  0000
code  4128                       2  0003
code  4128                       2  000c
code  4128                       2  0300
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4128                       2  0000
code  4129                       2  0000
code  4129                       2  4000
code  4129                       2  0000
code  4129                       2  8000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4129                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4130                       2  0000
code  4144                       1  05
code  4144                       1  c7
code  4144                       1  c4
code  4144                       1  7a
code  4144                       1  05
code  4144                       1  27
code  4144                       1  0c
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4144                       1  00
code  4145                       1  05
code  4145                       1  c7
code  4145                       1  c4
code  4145                       1  7a
code  4145                       1  05
code  4145                       1  27
code  4145                       1  0c
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4145                       1  00
code  4146                       1  05
code  4146                       1  c7
code  4146                       1  c4
code  4146                       1  7a
code  4146                       1  05
code  4146                       1  27
code  4146                       1  0c
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4146                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4210                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4212                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4213                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4214                       1  00
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code  4228                       1  01
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28675                       1  00
code 28697                       2  1e00
code 28697                       2  2000
code 28697                       2  2100
code 28697                       2  2800
code 28697                       2  2a00
code 28697                       2  1b00
code 28697                       2  2600
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28697                       2  1e00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
code 28928                       1  00
SYSTEM_ROOM_NAME "Wohnzimmer"
SYSTEM_ROOM_NAME "WC"
SYSTEM_ROOM_NAME "Küche"
SYSTEM_ROOM_NAME "Bad"
SYSTEM_ROOM_NAME "Schlafzimmer"
SYSTEM_ROOM_NAME "Büro"
SYSTEM_ROOM_NAME "Kaminzimmer"
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
SYSTEM_ROOM_NAME ""
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     4                       4  00000000
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code     5                       1  ff
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   126                       1  00
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   127                       6  050000000000
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   128                       7  000000000000ff
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
code   995                       4  00000000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
TESTANDPRODUCTION_ERROR_CODE   2  0000
GLOBAL_POWERCYCLECOUNTER    1  57
code     4                       4  16b690e3
code    42                       2  0100
code  4100                       2  0080
code  4184                       1  00
code  4186                       1  00
code  4187                       6  050000000000
code  4188                       6  050000000000
code  4189                      10  09000000000000000000
code  4190                      10  09000000000000000000
code  4194                       1  00
code  4195                       1  00
code  4196                       1  00
TESTANDPRODUCTION_RESET_LEVEL   1  00
code  4198                       1  00
code  4199                       1  03
code  4217                       1  00
code  4354                       1  00
code  4355                       1  00
code  4356                       2  0000
code  4357                       1  00
code  4358                       1  00
code  4359                       1  00
code  4360                       1  00
code  4361                       1  00
code  4362                       1  00
code  4363                       2  0000
code  4364                       1  00
code  4365                       1  00
code  4366                       2  0000
code  4367                       2  0000
code  4368                       2  0000
code  4369                       1  00
code  4370                       1  00
code  4371                       2  0000
code  4372                       2  0000
code  4373                       2  0000
code  4374                       1  00
code  4375                       1  00
code  4376                       2  0200
code  4377                       2  e803
code  4378                       2  0000
code  4379                       1  1b
code  4380                       2  0000
code  4381                       4  204e0000
code 28743                       1  00
code 28744                      17  1000000000000000000000000000000000
code 28745                       1  00
code 40000                       1  00
code 40001                       2  0000
code 40002                      65  4000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
code 40003                      65  4000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
code 40004                       1  00
code 40005                       1  00
code 40006                       1  00
code 40007                       2  0000
code 40008                       2  0000
code 40009                       2  0000
code 40010                       2  0000
code 40011                       9  080000000000000000
code 40012                       3  020000
code 40013                       2  0100
code 40014                      20  1300000000000000000000000000000000000000
code 40015                      65  4000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
code 40016                       2  0100
code 40017                      17  1000000000000000000000000000000000
code 40018                      25  18000000000000000000000000000000000000000000000000
code 40019                       5  0400000000
code  2955                       2  dc05
code  4106                       1  00
code  4107                       1  14
code  4108                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4109                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4110                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4111                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4112                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4113                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4114                      13  0caaaaaaaaaaaaaaaaaaaaaaa2
code  4200                       7  06f7ffffffffff
code  4201                       7  06000000000000
code  4202                       1  00
code  4203                       1  00
code  4204                       1  00
SYSTEM_HOUSE_NAME "M41"
SYSTEM_ZONE_NAME ""
SYSTEM_READY_RESTART        2  e803
NVM_CONF_SYSTEM_WIZARD     15  000000000000000000000000000000
*/
