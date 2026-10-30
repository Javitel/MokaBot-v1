#include "discovery.hpp"
#include <Arduino.h>
#include <WiFi.h>
#include "lwip/sockets.h"
#include "logic/webserver.hpp"

#define DISCOVERY_REQUEST "MOKABOT?"

static int discoverySocket = -1;

bool discoveryStart(void) {
  discoveryStop();
  discoverySocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (discoverySocket < 0) return false;
  struct sockaddr_in local = {};
  local.sin_family = AF_INET;
  local.sin_port = htons(MOKA_DISCOVERY_PORT);
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(discoverySocket, (struct sockaddr *)&local, sizeof(local)) < 0) {
    close(discoverySocket);
    discoverySocket = -1;
    return false;
  }
  return true;
}

void discoveryPoll(void) {
  if (discoverySocket < 0) return;
  char request[16];
  while (true) {
    struct sockaddr_in from;
    socklen_t fromLength = sizeof(from);
    int len = recvfrom(discoverySocket, request, sizeof(request) - 1, MSG_DONTWAIT,
                       (struct sockaddr *)&from, &fromLength);
    if (len < 0) return;          // nothing (more) to read
    request[len] = '\0';
    if (strcmp(request, DISCOVERY_REQUEST) != 0) continue;
    char reply[96];
    int length = snprintf(reply, sizeof(reply), "{\"name\":\"%s\",\"ip\":\"%s\"}",
                          getDeviceName(), WiFi.localIP().toString().c_str());
    sendto(discoverySocket, reply, length, 0, (struct sockaddr *)&from, sizeof(from));
  }
}

void discoveryStop(void) {
  if (discoverySocket < 0) return;
  close(discoverySocket);
  discoverySocket = -1;
}
