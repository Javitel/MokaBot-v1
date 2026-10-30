#include "mdnsResponder.hpp"
#include <Arduino.h>
#include <string.h>
#include <strings.h>
#include "lwip/sockets.h"

#define MDNS_PORT        5353
#define MDNS_GROUP       "224.0.0.251"
#define MDNS_TTL_S       120     // record lifetime in multicast answers
#define MDNS_LEGACY_TTL  10      // RFC 6762: at most 10 s in legacy answers
#define DNS_TYPE_A       1
#define DNS_TYPE_ANY     255
#define DNS_CLASS_IN     1
#define DNS_CACHE_FLUSH  0x8000  // class bit: this answer replaces cached ones
#define DNS_FLAGS_ANSWER 0x8400  // response, authoritative

static int mdnsSocket = -1;
static char fqdn[40];            // "<hostname>.local"
static char hostLabel[32];
static uint8_t address[4];
static struct ip_mreq membership;

// Reads a (possibly compressed) DNS name at *pos as "label.label"; advances
// *pos past it. False if malformed.
static bool readName(const uint8_t *msg, int len, int *pos, char *out, int outSize) {
  int p = *pos, n = 0, jumps = 0;
  bool jumped = false;
  while (true) {
    if (p >= len) return false;
    uint8_t length = msg[p];
    if ((length & 0xC0) == 0xC0) {
      if (p + 1 >= len || ++jumps > 8) return false;
      if (!jumped) *pos = p + 2;
      jumped = true;
      p = ((length & 0x3F) << 8) | msg[p + 1];
      continue;
    }
    if (length & 0xC0) return false;
    p++;
    if (length == 0) break;
    if (p + length > len || n + length + 2 > outSize) return false;
    if (n) out[n++] = '.';
    memcpy(out + n, msg + p, length);
    n += length;
    p += length;
  }
  out[n] = '\0';
  if (!jumped) *pos = p;
  return true;
}

static int putName(uint8_t *buf) {
  int n = 0;
  size_t length = strlen(hostLabel);
  buf[n++] = length;
  memcpy(buf + n, hostLabel, length);
  n += length;
  buf[n++] = 5;
  memcpy(buf + n, "local", 5);
  n += 5;
  buf[n++] = 0;
  return n;
}

static int put16(uint8_t *buf, uint16_t value) {
  buf[0] = value >> 8;
  buf[1] = value & 0xFF;
  return 2;
}

// The answer "<hostname>.local A <address>"; legacy answers repeat the
// question and keep the query's ID.
static int buildAnswer(uint8_t *buf, uint16_t id, bool legacy) {
  int n = 0;
  n += put16(buf + n, legacy ? id : 0);
  n += put16(buf + n, DNS_FLAGS_ANSWER);
  n += put16(buf + n, legacy ? 1 : 0);   // questions
  n += put16(buf + n, 1);                // answers
  n += put16(buf + n, 0);
  n += put16(buf + n, 0);
  if (legacy) {
    n += putName(buf + n);
    n += put16(buf + n, DNS_TYPE_A);
    n += put16(buf + n, DNS_CLASS_IN);
  }
  n += putName(buf + n);
  n += put16(buf + n, DNS_TYPE_A);
  n += put16(buf + n, legacy ? DNS_CLASS_IN : DNS_CLASS_IN | DNS_CACHE_FLUSH);
  uint32_t ttl = legacy ? MDNS_LEGACY_TTL : MDNS_TTL_S;
  n += put16(buf + n, ttl >> 16);
  n += put16(buf + n, ttl & 0xFFFF);
  n += put16(buf + n, 4);
  memcpy(buf + n, address, 4);
  return n + 4;
}

static void sendAnswer(const struct sockaddr_in *to, uint16_t id, bool legacy) {
  uint8_t buf[96];
  int length = buildAnswer(buf, id, legacy);
  sendto(mdnsSocket, buf, length, 0, (const struct sockaddr *)to, sizeof(*to));
}

static struct sockaddr_in groupAddress() {
  struct sockaddr_in group = {};
  group.sin_family = AF_INET;
  group.sin_port = htons(MDNS_PORT);
  group.sin_addr.s_addr = inet_addr(MDNS_GROUP);
  return group;
}

bool mdnsResponderStart(const char *hostname, const uint8_t ip[4]) {
  mdnsResponderStop();
  strlcpy(hostLabel, hostname, sizeof(hostLabel));
  snprintf(fqdn, sizeof(fqdn), "%s.local", hostLabel);
  memcpy(address, ip, 4);

  mdnsSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (mdnsSocket < 0) return false;
  int yes = 1;
  setsockopt(mdnsSocket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  struct sockaddr_in local = {};
  local.sin_family = AF_INET;
  local.sin_port = htons(MDNS_PORT);
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  struct in_addr interfaceAddress;
  memcpy(&interfaceAddress.s_addr, address, 4);
  membership.imr_multiaddr.s_addr = inet_addr(MDNS_GROUP);
  membership.imr_interface = interfaceAddress;
  // RFC 6762: IP TTL 255; our own packets are not looped back to us.
  uint8_t ttl = 255, loop = 0;
  if (bind(mdnsSocket, (struct sockaddr *)&local, sizeof(local)) < 0 ||
      setsockopt(mdnsSocket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) < 0) {
    Serial.println("mDNS: could not join the multicast group");
    close(mdnsSocket);
    mdnsSocket = -1;
    return false;
  }
  setsockopt(mdnsSocket, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
  setsockopt(mdnsSocket, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
  setsockopt(mdnsSocket, IPPROTO_IP, IP_MULTICAST_IF, &interfaceAddress, sizeof(interfaceAddress));

  // Unsolicited announcement: caches learn the address right away.
  struct sockaddr_in group = groupAddress();
  sendAnswer(&group, 0, false);
  Serial.printf("mDNS: answering %s\n", fqdn);
  return true;
}

void mdnsResponderPoll(void) {
  if (mdnsSocket < 0) return;
  uint8_t msg[512];
  while (true) {
    struct sockaddr_in from;
    socklen_t fromLength = sizeof(from);
    int len = recvfrom(mdnsSocket, msg, sizeof(msg), MSG_DONTWAIT, (struct sockaddr *)&from, &fromLength);
    if (len < 0) return;          // nothing (more) to read
    if (len < 12) continue;
    uint16_t id = (msg[0] << 8) | msg[1];
    uint16_t flags = (msg[2] << 8) | msg[3];
    if (flags & 0xF800) continue; // a response, or not a standard query
    int questions = (msg[4] << 8) | msg[5];
    bool asked = false;
    int pos = 12;
    for (int q = 0; q < questions; q++) {
      char name[64];
      if (!readName(msg, len, &pos, name, sizeof(name)) || pos + 4 > len) break;
      uint16_t type = (msg[pos] << 8) | msg[pos + 1];
      uint16_t qclass = ((msg[pos + 2] << 8) | msg[pos + 3]) & 0x7FFF;   // without the unicast-response bit
      pos += 4;
      if (strcasecmp(name, fqdn) == 0 && (type == DNS_TYPE_A || type == DNS_TYPE_ANY) &&
          (qclass == DNS_CLASS_IN || qclass == DNS_TYPE_ANY)) {
        asked = true;
      }
    }
    if (!asked) continue;
    bool legacy = ntohs(from.sin_port) != MDNS_PORT;
    if (legacy) {
      sendAnswer(&from, id, true);
    }
    else {
      struct sockaddr_in group = groupAddress();
      sendAnswer(&group, 0, false);
    }
  }
}

void mdnsResponderStop(void) {
  if (mdnsSocket < 0) return;
  setsockopt(mdnsSocket, IPPROTO_IP, IP_DROP_MEMBERSHIP, &membership, sizeof(membership));
  close(mdnsSocket);
  mdnsSocket = -1;
}
