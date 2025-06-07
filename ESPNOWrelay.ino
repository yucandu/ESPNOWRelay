#include <esp_now.h>
#include <WiFi.h>
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <BlynkSimpleEsp32.h>
#include "esp_wifi.h"
#include <SimplePgSQL.h>
#include "esp_sntp.h"
#include "nvs_flash.h"
#include <Preferences.h>
Preferences prefs;
const char* bedroomauth = "8_-CN2rm4ki9P3i_NkPhxIbCiKd5RXhK";  //hubert

uint8_t MAC_KAREN[] = {0x64, 0xE8, 0x33, 0x88, 0x66, 0xD0}; // confirmed
uint8_t MAC_LEON[]  = {0x84, 0xFC, 0xE6, 0x86, 0x5B, 0xB8}; // confirmed


#define maximumReadings 360

int readingCnt = 0;
#define IDLE_TIMEOUT 10000  // 10 seconds
#define MAX_BUFFERED_FILES 500
#define MAX_RAM_BUFFERS 10
#define BATCH_SIZE 12

int batchFill = 0;
uint8_t currentBatchNum = 0;
IPAddress PGIP(192,168,50,197); 
 struct tm timeinfo;
const char* ssid = "mikesnet";
const char* pass = "springchicken";

const char user[] = "wanburst";       // your database user
const char password[] = "elec&9";   // your database password
const char dbname[] = "blynk_reporting";         // your database name

volatile bool isProcessingData = false;
volatile bool dataTransmissionComplete = false;
bool buttonstart = false; // Button state to start transmission
int currentSensorID = 0;  // 24 for Karen, 42 for Leon
volatile bool havedata = false;
uint8_t pendingRequesterMAC[6];
volatile bool hasPendingRequester = false;
bool readyToReboot = false;
WidgetTerminal terminal(V122);

#define every(interval) \
    static uint32_t __every__##interval = millis(); \
    if (millis() - __every__##interval >= interval && (__every__##interval = millis()))

BLYNK_WRITE(V122) {
  if (String("help") == param.asStr()) {
    terminal.println("==List of available commands:==");
    terminal.println("wifi");
    terminal.println("==End of list.==");
  }
  if (String("wifi") == param.asStr()) {
    terminal.print("Connected to: ");
    terminal.println(ssid);
    terminal.print("IP address:");
    terminal.println(WiFi.localIP());
    terminal.print("Signal strength: ");
    terminal.println(WiFi.RSSI());
    printLocalTime();
  }
    terminal.flush();
}


typedef struct {
  float temp1;
  float temp2;
  unsigned long time;
  float volts;
  float pres;
} sensorReadings;

BLYNK_CONNECTED() {
  Blynk.syncVirtual(V121); //flash button
}

BLYNK_WRITE(V121)
{
  if (param.asInt() == 1) {buttonstart = true;}
  if (param.asInt() == 0) {buttonstart = false;}
}

bool isSetNtp = false;

void cbSyncTime(struct timeval *tv) { // callback function to show when NTP was synchronized
  Serial.println("NTP time synched");
  isSetNtp = true;
}

void initTime(String timezone){
  configTzTime(timezone.c_str(), "192.168.50.197");

  while (!isSetNtp) {
        delay(250);
        }

}

sensorReadings Readings[maximumReadings];
sensorReadings batchBuffer[BATCH_SIZE];

volatile bool readyToSend = false;
unsigned long lastReceiveTime = 0;
unsigned long lastSaveTime = 0;
bool needLoadFromFlash = false;
int i = 0;
RTC_DATA_ATTR int arrayCnt = 0;

int WiFiStatus;
WiFiClient client;


void saveReadingsToFlash() {
  prefs.begin("stuff", false, "nvs2");
  ++arrayCnt;
  prefs.putBytes(String(arrayCnt).c_str(), &Readings, sizeof(Readings));
  prefs.putInt("arrayCnt", arrayCnt); // <-- Save arrayCnt to NVS
  prefs.putInt("readingCnt", readingCnt);
  prefs.putInt("currentSensorID", currentSensorID);
  prefs.end();
}


/////////////////////////
//POSTGRESQL CODE BEGIN//
/////////////////////////






char buffer[1024];
PGconnection conn(&client, 0, 1024, buffer);

char tosend[192];
String tosendstr;


#ifndef USE_ARDUINO_ETHERNET
void checkConnection()
{
    int status = WiFi.status();
    if (status != WL_CONNECTED) {
        if (WiFiStatus == WL_CONNECTED) {
            Serial.println("Connection lost");
            WiFiStatus = status;
        }
    }
    else {
        if (WiFiStatus != WL_CONNECTED) {
            Serial.println("Connected");
            WiFiStatus = status;
        }
    }
}

#endif

static PROGMEM const char query_rel[] = "\
SELECT a.attname \"Column\",\
  pg_catalog.format_type(a.atttypid, a.atttypmod) \"Type\",\
  case when a.attnotnull then 'not null ' else 'null' end as \"null\",\
  (SELECT substring(pg_catalog.pg_get_expr(d.adbin, d.adrelid) for 128)\
   FROM pg_catalog.pg_attrdef d\
   WHERE d.adrelid = a.attrelid AND d.adnum = a.attnum AND a.atthasdef) \"Extras\"\
 FROM pg_catalog.pg_attribute a, pg_catalog.pg_class c\
 WHERE a.attrelid = c.oid AND c.relkind = 'r' AND\
 c.relname = %s AND\
 pg_catalog.pg_table_is_visible(c.oid)\
 AND a.attnum > 0 AND NOT a.attisdropped\
    ORDER BY a.attnum";

static PROGMEM const char query_tables[] = "\
SELECT n.nspname as \"Schema\",\
  c.relname as \"Name\",\
  CASE c.relkind WHEN 'r' THEN 'table' WHEN 'v' THEN 'view' WHEN 'm' THEN 'materialized view' WHEN 'i' THEN 'index' WHEN 'S' THEN 'sequence' WHEN 's' THEN 'special' WHEN 'f' THEN 'foreign table' END as \"Type\",\
  pg_catalog.pg_get_userbyid(c.relowner) as \"Owner\"\
 FROM pg_catalog.pg_class c\
     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace\
 WHERE c.relkind IN ('r','v','m','S','f','')\
      AND n.nspname <> 'pg_catalog'\
      AND n.nspname <> 'information_schema'\
      AND n.nspname !~ '^pg_toast'\
  AND pg_catalog.pg_table_is_visible(c.oid)\
 ORDER BY 1,2";

int pg_status = 0;

void doPg(void)
{
    char *msg;
    int rc;
    if (!pg_status) {
        conn.setDbLogin(PGIP,
            user,
            password,
            dbname,
            "utf8");
        pg_status = 1;
        return;
    }

    if (pg_status == 1) {
        rc = conn.status();
        if (rc == CONNECTION_BAD || rc == CONNECTION_NEEDED) {
            char *c=conn.getMessage();
            if (c) Serial.println(c);
            pg_status = -1;
        }
        else if (rc == CONNECTION_OK) {
            pg_status = 2;
            Serial.println("Enter query");
        }
        return;
    }
    if (pg_status == 2) {
        if (!Serial.available()) return;
        char inbuf[192];
        int n = Serial.readBytesUntil('\n',inbuf,191);
        while (n > 0) {
            if (isspace(inbuf[n-1])) n--;
            else break;
        }
        inbuf[n] = 0;

        if (!strcmp(inbuf,"\\d")) {
            if (conn.execute(query_tables, true)) goto error;
            Serial.println("Working...");
            pg_status = 3;
            return;
        }
        if (!strncmp(inbuf,"\\d",2) && isspace(inbuf[2])) {
            char *c=inbuf+3;
            while (*c && isspace(*c)) c++;
            if (!*c) {
                if (conn.execute(query_tables, true)) goto error;
                Serial.println("Working...");
                pg_status = 3;
                return;
            }
            if (conn.executeFormat(true, query_rel, c)) goto error;
            Serial.println("Working...");
            pg_status = 3;
            return;
        }

        if (!strncmp(inbuf,"exit",4)) {
            conn.close();
            Serial.println("Thank you");
            pg_status = -1;
            return;
        }
        if (conn.execute(inbuf)) goto error;
        Serial.println("Working...");
        pg_status = 3;
    }
    if (pg_status == 3) {
        rc=conn.getData();
        if (rc < 0) goto error;
        if (!rc) return;
        if (rc & PG_RSTAT_HAVE_COLUMNS) {
            for (i=0; i < conn.nfields(); i++) {
                if (i) Serial.print(" | ");
                Serial.print(conn.getColumn(i));
            }
            Serial.println("\n==========");
        }
        else if (rc & PG_RSTAT_HAVE_ROW) {
            for (i=0; i < conn.nfields(); i++) {
                if (i) Serial.print(" | ");
                msg = conn.getValue(i);
                if (!msg) msg=(char *)"NULL";
                Serial.print(msg);
            }
            Serial.println();
        }
        else if (rc & PG_RSTAT_HAVE_SUMMARY) {
            Serial.print("Rows affected: ");
            Serial.println(conn.ntuples());
        }
        else if (rc & PG_RSTAT_HAVE_MESSAGE) {
            msg = conn.getMessage();
            if (msg) Serial.println(msg);
        }
        if (rc & PG_RSTAT_READY) {
            pg_status = 2;
            Serial.println("Enter query");
        }
    }
    return;
error:
    msg = conn.getMessage();
    if (msg) Serial.println(msg);
    else Serial.println("UNKNOWN ERROR");
    if (conn.status() == CONNECTION_BAD) {
        Serial.println("Connection is bad");
        pg_status = -1;
    }
}








/////////////////////////
//POSTGRESQL CODE END  //
/////////////////////////
wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();  


void enableWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  Serial.println("Connecting to wifi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("Wi-Fi connected.");
}


/*void disableWiFi() {
  Serial.println("Killing Wi-Fi...");
  Blynk.disconnect();
  delay(100);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);
  
  // Stop and deinit WiFi
  esp_wifi_stop();
  esp_wifi_deinit();
  
  // Delete the default event loop before recreating
  esp_event_loop_delete_default();
  
  delay(100);
  
  // Reinit NVS and event loop
  nvs_flash_deinit();
  nvs_flash_init();
  esp_event_loop_create_default();
  esp_now_deinit();
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  else {Serial.println("ESP-NOW init working!");}
  delay(100);
}*/


void printLocalTime() {
  time_t rawtime;
  struct tm* timeinfo;
  time(&rawtime);
  timeinfo = localtime(&rawtime);
  terminal.print(asctime(timeinfo));
  Serial.println(asctime(timeinfo));
}

void wifiandBlynk() {
//  enableWiFi();
  delay(100);
  Serial.println("Configuring Blynk");
//  Blynk.config(bedroomauth, IPAddress(192, 168, 50, 197), 8080);
  delay(100);
  Serial.println("Connecting Blynk...");
//  Blynk.connect();
Blynk.begin(bedroomauth, ssid, pass, IPAddress(192, 168, 50, 197), 8080);
  while (!Blynk.connected()) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("Init time");
  initTime("EST5EDT,M3.2.0,M11.1.0");
  Serial.println("Set env");
  setenv("TZ","EST5EDT,M3.2.0,M11.1.0",1);  //  Now adjust the TZ.  Clock settings are adjusted to show the new local time
  Serial.println("tzset");
  tzset();
  Serial.println("getlocaltime");
  getLocalTime(&timeinfo);

  time_t rawtime;
  struct tm* timeinfo;
  time(&rawtime);
  timeinfo = localtime(&rawtime);

  Serial.println(asctime(timeinfo));
  Serial.println("Checking for buttonstart...");
    Blynk.syncVirtual(V121); //flash button
    Blynk.run(); // Process Blynk events
    Blynk.syncVirtual(V121); //flash button
    Blynk.run(); // Process Blynk events
    // Give Blynk a moment to update button state
    delay(500);
    if (buttonstart) {
      terminal.println("***RELAY STARTED***");
      terminal.print("Connected to ");
      terminal.println(ssid);
      terminal.print("IP address: ");
      terminal.println(WiFi.localIP());
      printLocalTime();
      terminal.flush();
      ArduinoOTA.setHostname("ESPNOWrelay");
      ArduinoOTA.begin();
      Serial.println("OTA enabled, waiting for updates...");
      while (buttonstart) {
        Blynk.run();
        ArduinoOTA.handle();
        // If button is released, break and disconnect
        // Blynk_WRITE(V121) will update buttonstart
      }
      Serial.println("OTA session ended, WiFi disabled.");
    }
    else {
      Serial.println("No buttonstart.");
    } 
}


// Your existing function, reused
void transmitReadings() {
  Serial.println("Transmitting data...");


  i = 0;
  while (i < readingCnt) {
    doPg();


    if ((pg_status == 2) && (i < readingCnt)) {
      if (currentSensorID == 24) {  // Karen
        tosendstr = "insert into burst values (24,1," + String(Readings[i].time) + "," + String(Readings[i].temp1, 3) + "), " +
                    "(24,2," + String(Readings[i].time) + "," + String(Readings[i].volts, 4) + "), " +
                    "(24,3," + String(Readings[i].time) + "," + String(Readings[i].temp2, 3) + "), " +
                    "(24,6," + String(Readings[i].time) + "," + String(Readings[i].pres, 3) + ")";
      } else if (currentSensorID == 42) {  // Leon
        tosendstr = "insert into burst values (42,1," + String(Readings[i].time) + "," + String(Readings[i].temp1, 3) + "), " +
                    "(42,2," + String(Readings[i].time) + "," + String(Readings[i].volts, 4) + "), " +
                    "(42,3," + String(Readings[i].time) + "," + String(Readings[i].temp2, 3) + "), " +
                    "(42,4," + String(Readings[i].time) + "," + String(Readings[i].pres, 3) + ")";
      }
      conn.execute(tosendstr.c_str());
      pg_status = 3;
      delay(1);
      i++;
    }
    delay(1);
  }
  Serial.printf("Transmission complete! Sent %d readings\n", readingCnt);



}

bool macEquals(const uint8_t *mac1, const uint8_t *mac2) {
  for (int i = 0; i < 6; i++) if (mac1[i] != mac2[i]) return false;
  return true;
}

void sendLocalTimeToC3(const uint8_t *mac) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    time_t now = mktime(&timeinfo); // local-adjusted time
    uint32_t localTimeUnix = static_cast<uint32_t>(now); // 32-bit to send via ESP-NOW

    esp_now_send(mac, (uint8_t *)&localTimeUnix, sizeof(localTimeUnix));
    Serial.print("Sent local time to C3: ");
    Serial.println(localTimeUnix);
  } else {
    Serial.println("Failed to get local time");
  }
}

// Add this global variable to store the last request sequence number per pending requester
uint8_t pendingRequesterSeq = 0;

// Modify onProcessingComplete to include the sequence number in the READY reply
void onProcessingComplete() {
  char completeMsg[9] = {'C','O','M','P','L','E','T','E', pendingRequesterSeq};
  esp_now_send(pendingRequesterMAC, (const uint8_t *)completeMsg, sizeof(completeMsg));
  if (hasPendingRequester) {
    char readyMsg[6] = {'R','E','A','D','Y', pendingRequesterSeq};
    esp_now_send(pendingRequesterMAC, (const uint8_t *)readyMsg, sizeof(readyMsg));
    hasPendingRequester = false;
  }
}


// Update the callback signature
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len) {
  Serial.println("Receiving something...");
  const uint8_t *mac = recv_info->src_addr;
  if (macEquals(mac, MAC_KAREN)) {
    currentSensorID = 24;
  } else if (macEquals(mac, MAC_LEON)) {
    currentSensorID = 42;
  } else {
    currentSensorID = 0;  // Unknown sender
  }
  if (!esp_now_is_peer_exist(mac)) {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, mac, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
    Serial.println("Added C3 as peer");
  }
  if (len == 1 && incomingData[0] == 0) { // Initial time request
    Serial.println("Received time request, sending time...");
    sendLocalTimeToC3(mac);
    return;
  }
  if (len == 5 && strncmp((const char*)incomingData, "HELLO", 5) == 0) {
    Serial.println("Received handshake");
    const char *ackMsg = "ACK";
    esp_now_send(mac, (const uint8_t *)ackMsg, strlen(ackMsg));
    sendLocalTimeToC3(mac);
    return;
  }

  // Handle REQUEST with sequence number
  if (len == 8 && strncmp((const char*)incomingData, "REQUEST", 7) == 0) {
    uint8_t seq = incomingData[7];
    Serial.print("Received ready request, seq: ");
    Serial.println(seq);

    if (!isProcessingData && !readyToSend) {
      Serial.println("Sending READY signal");
      char readyMsg[6] = {'R','E','A','D','Y', seq};
      esp_now_send(mac, (const uint8_t *)readyMsg, sizeof(readyMsg));
      isProcessingData = true;
      readingCnt = 0;
      currentBatchNum = 0;
      memcpy(pendingRequesterMAC, mac, 6);
      pendingRequesterSeq = seq;
    } else {
      memcpy(pendingRequesterMAC, mac, 6);
      pendingRequesterSeq = seq;
      hasPendingRequester = true;
    }
    return;
  }

  // Handle batch data (multiple readings per packet)
  if (isProcessingData && len > 0 && (len % sizeof(sensorReadings)) == 0) {
    Serial.println("Receiving packet");
    int numReadings = len / sizeof(sensorReadings);
    for (int i = 0; i < numReadings; i++) {
      if (readingCnt < maximumReadings) {
        memcpy(&Readings[readingCnt], incomingData + i * sizeof(sensorReadings), sizeof(sensorReadings));
        readingCnt++;
        Serial.printf("Received #%d at %lu\n", readingCnt, millis());
      }
    }
    // Send per-batch ACK
    char ackMsg[5] = {'A','C','K','B', currentBatchNum};
    Serial.printf("Sending ACK for batch %d\n", currentBatchNum);
    esp_now_send(mac, (uint8_t*)ackMsg, sizeof(ackMsg));
    currentBatchNum++;

    lastReceiveTime = millis();

    // If we've received a complete dataset
    if (readingCnt >= maximumReadings) {
      Serial.println("Complete dataset received, marking ready to send");
      isProcessingData = false;
      readyToSend = true;
    }
    return;
  }

  if (!isProcessingData && len > 0 && (len % sizeof(sensorReadings)) == 0) {
    Serial.println("Received data but not in processing mode - ignoring");
  }
}

void loadFromFlash() {
  //prefs.begin("stuff", false, "nvs2");
  currentSensorID = prefs.getInt("currentSensorID", 0);
      while (arrayCnt > 0) {
        
        prefs.getBytes(String(arrayCnt).c_str(), &Readings, sizeof(Readings));
        readingCnt = maximumReadings;
        transmitReadings(); // Transmit the loaded readings
        Serial.printf("Loaded and transmitted %d readings from flash\n", arrayCnt);

        arrayCnt--;
        
      }
      prefs.putInt("arrayCnt", 0);
      prefs.putInt("currentSensorID", 0);
   // prefs.end();
}



void setup() {
  sntp_set_time_sync_notification_cb(cbSyncTime);
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  wifiandBlynk();
  prefs.begin("stuff", false, "nvs2");
  arrayCnt = prefs.getInt("arrayCnt", 0);
  
  if (arrayCnt == 0) {
    Serial.println("No previous readings found, initializing...");
  } else {
    Serial.printf("Found %d previous readings\n", arrayCnt);
    loadFromFlash();
  }
  prefs.end();
  Blynk.disconnect();
  delay(100);
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  


  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  else {Serial.println("ESP-NOW init working!");}

  // Register the new-style callback
  esp_now_register_recv_cb(OnDataRecv);
}

void loop() {
  if (isProcessingData && readingCnt > 0 && (millis() - lastReceiveTime > 5000)) {
    Serial.println("timed out, marking ready to send partial data");
    isProcessingData = false;
    readyToSend = true;
  }

  if (readyToSend) {
    //transmitReadings();
    saveReadingsToFlash();
    onProcessingComplete();
    readyToSend = false;
    Serial.println("Data ready to send, saved to flash");
    lastSaveTime = millis();  // Reset save timer
    readyToReboot = true;
      readingCnt = 0;  // Clear for next batch
  }

  if (readyToReboot && (millis() - lastSaveTime > 5000)) { // Wait 5 seconds after saving
    Serial.println("Rebooting ESP...");
    delay(1000);
    ESP.restart();
  }

  //every(300000) { // 5 minutes in ms
  //if (!isProcessingData && !readyToSend)
  //  {ESP.restart();}

  //}
  delay(1);  // Keep loop efficient
}
