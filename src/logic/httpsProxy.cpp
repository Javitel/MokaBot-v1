#include "httpsProxy.hpp"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_tls.h>
#include <esp_random.h>
#include <lwip/sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/x509_crt.h>
#include "logic/prefsLock.hpp"
#include "logic/webserver.hpp"
#include "taskCores.hpp"

#define HTTPS_PORT          443
#define HTTP_BACKEND_PORT   80
#define HTTPS_NAMESPACE     "https"
// Certificate and key generation (elliptic curve maths) and the TLS handshake
// need a deep stack.
#define HTTPS_TASK_STACK    12288
#define HTTPS_HANDSHAKE_TIMEOUT_S 8
// A connection without traffic for this long is closed, so the next one
// (only one is served at a time) is not kept waiting.
#define HTTPS_IDLE_MS       6000
#define HTTPS_BUFFER        1460

static volatile bool httpsRun = false;
static volatile bool httpsTaskAlive = false;
static String certPem, keyPem, certName;

static int randomBytes(void *, unsigned char *buffer, size_t length){
  esp_fill_random(buffer, length);
  return 0;
}

// Creates a P-256 key and a self-signed certificate for <name>.local.
static bool generateCertificate(const char *name){
  bool ok = false;
  mbedtls_pk_context key;
  mbedtls_x509write_cert crt;
  mbedtls_pk_init(&key);
  mbedtls_x509write_crt_init(&crt);
  char *pem = (char *)malloc(2048);
  String local = String(name) + ".local";
  String subject = "CN=" + local + ",O=MokaBot";
  unsigned char serial[16];
  esp_fill_random(serial, sizeof(serial));
  serial[0] = (serial[0] & 0x7F) | 0x01;   // positive, not zero
  // Browsers match the address against these names, not the CN.
  mbedtls_x509_san_list sanShort = {}, sanLocal = {};
  sanShort.node.type = MBEDTLS_X509_SAN_DNS_NAME;
  sanShort.node.san.unstructured_name.p = (unsigned char *)name;
  sanShort.node.san.unstructured_name.len = strlen(name);
  sanLocal.node.type = MBEDTLS_X509_SAN_DNS_NAME;
  sanLocal.node.san.unstructured_name.p = (unsigned char *)local.c_str();
  sanLocal.node.san.unstructured_name.len = local.length();
  sanLocal.next = &sanShort;

  do{
    if(!pem) break;
    if(mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) break;
    if(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), randomBytes, NULL) != 0) break;
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    if(mbedtls_x509write_crt_set_subject_name(&crt, subject.c_str()) != 0) break;
    if(mbedtls_x509write_crt_set_issuer_name(&crt, subject.c_str()) != 0) break;
    if(mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)) != 0) break;
    // The device has no clock to check dates against: a fixed, long validity.
    if(mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20491231235959") != 0) break;
    if(mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1) != 0) break;
    if(mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) != 0) break;
    if(mbedtls_x509write_crt_set_subject_alternative_name(&crt, &sanLocal) != 0) break;
    if(mbedtls_x509write_crt_pem(&crt, (unsigned char *)pem, 2048, randomBytes, NULL) != 0) break;
    String newCert = pem;
    if(mbedtls_pk_write_key_pem(&key, (unsigned char *)pem, 2048) != 0) break;
    certPem = newCert;
    keyPem = pem;
    certName = name;
    ok = true;
  }while(false);

  free(pem);
  mbedtls_x509write_crt_free(&crt);
  mbedtls_pk_free(&key);
  return ok;
}

// Loads the stored certificate, or makes (and stores) one for the current
// device name.
static bool ensureCertificate(){
  const char *name = getDeviceName();
  if(certName == name && certPem.length() && keyPem.length()) return true;
  Preferences store;
  prefsLock(); store.begin(HTTPS_NAMESPACE, true);
  String storedName = store.getString("name", "");
  String storedCert = store.getString("cert", "");
  String storedKey = store.getString("key", "");
  store.end(); prefsUnlock();
  if(storedName == name && storedCert.length() && storedKey.length()){
    certPem = storedCert; keyPem = storedKey; certName = storedName;
    return true;
  }
  uint32_t start = millis();
  if(!generateCertificate(name)){
    Serial.println("HTTPS: could not create the certificate");
    return false;
  }
  Serial.printf("HTTPS: certificate for %s.local created in %u ms\n", name, (unsigned)(millis() - start));
  prefsLock(); store.begin(HTTPS_NAMESPACE, false);
  store.putString("name", certName);
  store.putString("cert", certPem);
  store.putString("key", keyPem);
  store.end(); prefsUnlock();
  return true;
}

static void setTimeouts(int fd, int seconds){
  struct timeval timeout = { seconds, 0 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

static int connectBackend(){
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if(fd < 0) return -1;
  struct sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(HTTP_BACKEND_PORT);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  setTimeouts(fd, 5);
  if(connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0){
    close(fd);
    return -1;
  }
  return fd;
}

static bool sendAll(int fd, const char *data, int length){
  while(length > 0){
    int sent = send(fd, data, length, 0);
    if(sent <= 0) return false;
    data += sent; length -= sent;
  }
  return true;
}

static bool tlsSendAll(esp_tls_t *tls, const char *data, int length){
  while(length > 0){
    int sent = esp_tls_conn_write(tls, data, length);
    if(sent == ESP_TLS_ERR_SSL_WANT_READ || sent == ESP_TLS_ERR_SSL_WANT_WRITE) continue;
    if(sent <= 0) return false;
    data += sent; length -= sent;
  }
  return true;
}

// One HTTPS connection: TLS handshake, then bytes both ways until either side
// closes or nothing moves for HTTPS_IDLE_MS.
static void serveClient(int clientFd, char *buffer){
  setTimeouts(clientFd, HTTPS_HANDSHAKE_TIMEOUT_S);
  esp_tls_cfg_server_t config = {};
  config.servercert_buf = (const unsigned char *)certPem.c_str();
  config.servercert_bytes = certPem.length() + 1;
  config.serverkey_buf = (const unsigned char *)keyPem.c_str();
  config.serverkey_bytes = keyPem.length() + 1;
  esp_tls_t *tls = esp_tls_init();
  if(!tls) return;
  // Fails for a browser that has not accepted the certificate yet (it closes
  // the connection to show its warning) and when memory is short.
  if(esp_tls_server_session_create(&config, clientFd, tls) != 0){
    esp_tls_server_session_delete(tls);
    return;
  }
  int backendFd = connectBackend();
  uint32_t lastTraffic = millis();
  while(backendFd >= 0 && httpsRun && millis() - lastTraffic < HTTPS_IDLE_MS){
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(clientFd, &readable);
    FD_SET(backendFd, &readable);
    struct timeval wait = { 0, 250000 };
    // Decrypted bytes already buffered by the TLS layer do not show in select().
    bool clientData = esp_tls_get_bytes_avail(tls) > 0;
    if(!clientData){
      int ready = select((clientFd > backendFd ? clientFd : backendFd) + 1, &readable, NULL, NULL, &wait);
      if(ready < 0) break;
      if(ready == 0) continue;
      clientData = FD_ISSET(clientFd, &readable);
    }
    else FD_ZERO(&readable);
    if(clientData){
      int length = esp_tls_conn_read(tls, buffer, HTTPS_BUFFER);
      if(length == ESP_TLS_ERR_SSL_WANT_READ || length == ESP_TLS_ERR_SSL_WANT_WRITE) continue;
      if(length <= 0 || !sendAll(backendFd, buffer, length)) break;
      lastTraffic = millis();
    }
    if(FD_ISSET(backendFd, &readable)){
      int length = recv(backendFd, buffer, HTTPS_BUFFER, 0);
      if(length <= 0 || !tlsSendAll(tls, buffer, length)) break;
      lastTraffic = millis();
    }
  }
  if(backendFd >= 0) close(backendFd);
  esp_tls_server_session_delete(tls);
}

static void httpsTask(void *){
  char *buffer = (char *)malloc(HTTPS_BUFFER);
  int listenFd = -1;
  if(buffer && ensureCertificate()){
    listenFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(HTTPS_PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    int yes = 1;
    if(listenFd >= 0) setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if(listenFd < 0 || bind(listenFd, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(listenFd, 4) != 0){
      Serial.println("HTTPS: could not listen on port 443");
      if(listenFd >= 0) close(listenFd);
      listenFd = -1;
    }
    else Serial.printf("HTTPS: https://%s.local\n", getDeviceName());
  }
  while(httpsRun && listenFd >= 0){
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(listenFd, &readable);
    struct timeval wait = { 0, 300000 };
    if(select(listenFd + 1, &readable, NULL, NULL, &wait) <= 0) continue;
    int clientFd = accept(listenFd, NULL, NULL);
    if(clientFd < 0) continue;
    // A renamed device gets a certificate for its new name.
    if(ensureCertificate()) serveClient(clientFd, buffer);
    close(clientFd);
  }
  if(listenFd >= 0) close(listenFd);
  free(buffer);
  httpsTaskAlive = false;
  vTaskDelete(NULL);
}

void httpsStart(void){
  if(httpsTaskAlive) return;
  httpsRun = true;
  httpsTaskAlive = true;
  // Below the web server's priority: a handshake is a burst of CPU work.
  if(xTaskCreatePinnedToCore(httpsTask, "https", HTTPS_TASK_STACK, NULL, 2, NULL, CORE_I2C) != pdPASS){
    httpsTaskAlive = false;
    Serial.println("HTTPS: could not start (memory)");
  }
}

void httpsStop(void){
  httpsRun = false;
  for(int k = 0; k < 100 && httpsTaskAlive; k++) vTaskDelay(pdMS_TO_TICKS(100));
}
