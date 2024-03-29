#include <ESP8266WiFi.h>
#ifdef USE_UDP
#include <WiFiUdp.h>
WiFiUDP Udp;
IPAddress ip(10, 42, 8, 89);
int port = 1234;
#else
#include <WiFiClient.h>
WiFiClient client;
const char* host = "10.42.8.89";
const uint16_t port = 8090;
#endif



extern "C" {
#include "user_interface.h"
  typedef void (*freedom_outside_cb_t)(uint8 status);
  int  wifi_register_send_pkt_freedom_cb(freedom_outside_cb_t cb);
  void wifi_unregister_send_pkt_freedom_cb(void);
  int  wifi_send_pkt_freedom(uint8 *buf, int len, bool sys_seq);
}

#define ETH_MAC_LEN 6

uint8_t broadcast1[3] = { 0x01, 0x00, 0x5e };
uint8_t broadcast2[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
uint8_t broadcast3[3] = { 0x33, 0x33, 0x00 };

struct beaconinfo
{
  uint8_t bssid[ETH_MAC_LEN];
  uint8_t ssid[33];
  int ssid_len;
  int channel;
  int err;
  signed rssi;
  uint8_t capa[2];
};

struct clientinfo
{
  uint8_t bssid[ETH_MAC_LEN];
  uint8_t station[ETH_MAC_LEN];
  uint8_t ap[ETH_MAC_LEN];
  int channel;
  int err;
  signed rssi;
  uint16_t seq_n;
};

struct RxControl {
  signed rssi: 8;
  unsigned rate: 4;
  unsigned is_group: 1;
  unsigned: 1;
  unsigned sig_mode: 2;
  unsigned legacy_length: 12;
  unsigned damatch0: 1;
  unsigned damatch1: 1;
  unsigned bssidmatch0: 1;
  unsigned bssidmatch1: 1;
  unsigned MCS: 7;
  unsigned CWB: 1;
  unsigned HT_length: 16;
  unsigned Smoothing: 1;
  unsigned Not_Sounding: 1;
  unsigned: 1;
  unsigned Aggregation: 1;
  unsigned STBC: 2;
  unsigned FEC_CODING: 1;
  unsigned SGI: 1;
  unsigned rxend_state: 8;
  unsigned ampdu_cnt: 8;
  unsigned channel: 4;
  unsigned: 12;
};

struct LenSeq {
  uint16_t length;
  uint16_t seq;
  uint8_t  address3[6];
};

struct sniffer_buf {
  struct RxControl rx_ctrl;
  uint8_t buf[36];
  uint16_t cnt;
  struct LenSeq lenseq[1];
};

struct sniffer_buf2 {
  struct RxControl rx_ctrl;
  uint8_t buf[112];
  uint16_t cnt;
  uint16_t len;
};

struct clientinfo parse_data(uint8_t *frame, uint16_t framelen, signed rssi, unsigned channel)
{
  struct clientinfo ci;
  ci.channel = channel;
  ci.err = 0;
  ci.rssi = rssi;
  int pos = 36;
  uint8_t *bssid;
  uint8_t *station;
  uint8_t *ap;
  uint8_t ds;

  ds = frame[1] & 3;    //Set first 6 bits to 0
  switch (ds) {
    // p[1] - xxxx xx00 => NoDS   p[4]-DST p[10]-SRC p[16]-BSS
    case 0:
      bssid = frame + 16;
      station = frame + 10;
      ap = frame + 4;
      break;
    // p[1] - xxxx xx01 => ToDS   p[4]-BSS p[10]-SRC p[16]-DST
    case 1:
      bssid = frame + 4;
      station = frame + 10;
      ap = frame + 16;
      break;
    // p[1] - xxxx xx10 => FromDS p[4]-DST p[10]-BSS p[16]-SRC
    case 2:
      bssid = frame + 10;
      // hack - don't know why it works like this...
      if (memcmp(frame + 4, broadcast1, 3) || memcmp(frame + 4, broadcast2, 3) || memcmp(frame + 4, broadcast3, 3)) {
        station = frame + 16;
        ap = frame + 4;
      } else {
        station = frame + 4;
        ap = frame + 16;
      }
      break;
    // p[1] - xxxx xx11 => WDS    p[4]-RCV p[10]-TRM p[16]-DST p[26]-SRC
    case 3:
      bssid = frame + 10;
      station = frame + 4;
      ap = frame + 4;
      break;
  }

  memcpy(ci.station, station, ETH_MAC_LEN);
  memcpy(ci.bssid, bssid, ETH_MAC_LEN);
  memcpy(ci.ap, ap, ETH_MAC_LEN);

  ci.seq_n = frame[23] * 0xFF + (frame[22] & 0xF0);
  return ci;
}

struct beaconinfo parse_beacon(uint8_t *frame, uint16_t framelen, signed rssi)
{
  struct beaconinfo bi;
  bi.ssid_len = 0;
  bi.channel = 0;
  bi.err = 0;
  bi.rssi = rssi;
  int pos = 36;

  if (frame[pos] == 0x00) {
    while (pos < framelen) {
      switch (frame[pos]) {
        case 0x00: //SSID
          bi.ssid_len = (int) frame[pos + 1];
          if (bi.ssid_len == 0) {
            memset(bi.ssid, '\x00', 33);
            break;
          }
          if (bi.ssid_len < 0) {
            bi.err = -1;
            break;
          }
          if (bi.ssid_len > 32) {
            bi.err = -2;
            break;
          }
          memset(bi.ssid, '\x00', 33);
          memcpy(bi.ssid, frame + pos + 2, bi.ssid_len);
          bi.err = 0;  // before was error??
          break;
        case 0x03: //Channel
          bi.channel = (int) frame[pos + 2];
          pos = -1;
          break;
        default:
          break;
      }
      if (pos < 0) break;
      pos += (int) frame[pos + 1] + 2;
    }
  } else {
    bi.err = -3;
  }

  bi.capa[0] = frame[34];
  bi.capa[1] = frame[35];
  memcpy(bi.bssid, frame + 10, ETH_MAC_LEN);
  return bi;
}

#define PKTLEN 12
#define PKTBUFFLEN PKTLEN*85*4
#define PKTBUFFSEND PKTLEN*85*3
char pktBuff[PKTBUFFLEN] = "";
int pktBuffPos = 0;

void print_beacon(beaconinfo beacon)
{
  /*if (beacon.err != 0) return;
  Serial.print("BC ");
  for (int i = 0; i < 6; i++) Serial.printf("%02x", beacon.bssid[i]);
  Serial.printf(" [%32s] ", beacon.ssid);
  Serial.printf(" %2d", beacon.channel);
  Serial.printf(" %4d", beacon.rssi);
  Serial.println();*/
}

void print_client(clientinfo ci)
{
  if (ci.err != 0) return;
  
  /*Serial.printf("DI ");
  for (int i = 0; i < 6; i++) Serial.printf("%02x", ci.station[i]);
  Serial.print(" ");
  for (int i = 0; i < 6; i++) Serial.printf("%02x", ci.bssid[i]);
  Serial.print(" ");
  for (int i = 0; i < 6; i++) Serial.printf("%02x", ci.ap[i]);
  Serial.printf(" %4d\r\n", ci.rssi);*/
  if (pktBuffPos >= PKTBUFFLEN - PKTLEN) {
    Serial.println("Buffer full!");
    return;
  }

  char* ptr = pktBuff + pktBuffPos;
  //pktBuffPos += sprintf(pktBuff+pktBuffPos, "DI ");
  for (int i = 0; i < 6; i++) pktBuffPos += sprintf(pktBuff+pktBuffPos, "%02x", ci.station[i]);
  //pktBuffPos += sprintf(pktBuff+pktBuffPos, " ");
  //for (int i = 0; i < 6; i++) pktBuffPos += sprintf(pktBuff+pktBuffPos, "%02x", ci.bssid[i]);
  //pktBuffPos += sprintf(pktBuff+pktBuffPos, " ");
  //for (int i = 0; i < 6; i++) pktBuffPos += sprintf(pktBuff+pktBuffPos, "%02x", ci.ap[i]);
  //pktBuffPos += sprintf(pktBuff+pktBuffPos, "%03d\n", abs(ci.rssi));

  //Serial.print(ptr);
}

void promisc_cb(uint8_t *buf, uint16_t len)
{
  int i = 0;
  uint16_t seq_n_new = 0;
  if (len == 12) {
    struct RxControl *sniffer = (struct RxControl*) buf;
  } else if (len == 128) {
    struct sniffer_buf2 *sniffer = (struct sniffer_buf2*) buf;
    struct beaconinfo beacon = parse_beacon(sniffer->buf, 112, sniffer->rx_ctrl.rssi);
    print_beacon(beacon);
  } else {
    struct sniffer_buf *sniffer = (struct sniffer_buf*) buf;
    //Is data or QOS?
    if ((sniffer->buf[0] == 0x08) || (sniffer->buf[0] == 0x88)) {
      struct clientinfo ci = parse_data(sniffer->buf, 36, sniffer->rx_ctrl.rssi, sniffer->rx_ctrl.channel);
      if (memcmp(ci.bssid, ci.station, ETH_MAC_LEN)) {
        print_client(ci);
      }
    }
  }
}

unsigned int channel = 1;

void setup() {
  WiFi.persistent(false);
  Serial.begin(115200);
  wifi_set_opmode(STATION_MODE);            // Promiscuous works only with station mode
  wifi_set_channel(channel);
  wifi_promiscuous_enable(0);
  wifi_set_promiscuous_rx_cb(promisc_cb);   // Set up promiscuous callback
  wifi_promiscuous_enable(1);
}

bool clientConnect() {
  if (WiFi.status() == WL_CONNECTED) return true;
  struct station_config conf;
  conf.threshold.authmode = AUTH_WPA_PSK;
  strcpy(reinterpret_cast<char*>(conf.ssid), "<YOUR SSID>");
  strcpy(reinterpret_cast<char*>(conf.password), "<YOUR PASSWORD>");
  conf.threshold.rssi = -127;
  //conf.open_and_wep_mode_disable = true;
  conf.bssid_set = 0;
  wifi_station_set_config_current(&conf);
  wifi_station_connect();
  wifi_station_dhcpc_start();
  int timeout = 10;
  while (WiFi.status() != WL_CONNECTED) {
    Serial.println("Connecting to WiFi network ("+String(timeout)+")... ");
    delay(1000);
    timeout -= 1;
    if (timeout < 0) return false;
  }
  return true;
}

void transmitPacket() {
  if (pktBuffPos < 1) return;
  wifi_promiscuous_enable(0);
  delay(100);
  bool enabled = WiFi.enableSTA(true);
  if (!enabled) {
    Serial.println("Enable failed!");
    return;
  }

  if (clientConnect()) {
    Serial.println("Connecting was succesfull!");
    #ifdef USE_UDP
      Udp.beginPacket(ip, port);
      Udp.write(pktBuff, pktBuffPos);
      Udp.endPacket();
    #else
      if (!client.connect(host, port)) {
        Serial.println("connection failed");
      } else {
        int currPos = 0;
        int timeout = 10;
        while (currPos < pktBuffPos) {
          int tx = client.write(pktBuff+currPos, pktBuffPos);
          currPos += tx;
          Serial.println("tx = "+String(tx)+" ("+String(currPos)+"/"+String(pktBuffPos)+")");
          if (tx < 1) timeout -= 1;
          if (timeout < 0) {Serial.println("TIMEOUT DURING TX"); break; }
        }
        
        client.stop();
        pktBuffPos = 0;
      }
    #endif
    delay(500);
  }

  wifi_station_disconnect();
  wifi_promiscuous_enable(1);
}

void loop() {
  if (channel == 15) { channel = 1; }
  Serial.println("Switching to channel "+String(channel)+", "+String(pktBuffPos/PKTLEN)+" of "+String(PKTBUFFLEN/PKTLEN)+" entries in buffer.");
  if (pktBuffPos > PKTBUFFSEND) transmitPacket();
  wifi_set_channel(channel);
  delay(2000);
  channel++;
}
