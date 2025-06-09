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
#define IDLE_RESTART_DELAY 10000 // 10 seconds, or whatever value you want
int readingCnt = 0;
#define IDLE_TIMEOUT 10000  // 10 seconds
#define MAX_BUFFERED_FILES 500
#define MAX_RAM_BUFFERS 10
#define BATCH_SIZE 12
#define MAX_UPTIME 300000 // 5 minutes in milliseconds
volatile bool readyToSend = false;
unsigned long lastReceiveTime = 0;
unsigned long lastSaveTime = 0;
bool needLoadFromFlash = false;
int i = 0;
RTC_DATA_ATTR int arrayCnt = 0;

int WiFiStatus;
WiFiClient client;


// Reception state
struct DeviceState {
  bool receiving;
  uint16_t expectedPackets;
  uint16_t receivedPackets;
  bool* packetReceived;
  uint8_t* buffer;
  uint32_t totalSize;
  unsigned long lastActivity;
  uint8_t sensorID;
};

DeviceState deviceStates[2]; // Support for 2 devices
int batchFill = 0;
uint8_t currentBatchNum = 0;
IPAddress PGIP(192,168,50,197); 
 struct tm timeinfo;
const char* ssid = "mikesnet";
const char* pass = "springchicken";

const char user[] = "wanburst";       // your database user
const char password[] = "elec&9";   // your database password
const char dbname[] = "blynk_reporting";         // your database name
unsigned long lastTransmissionTime = 0;
volatile bool isProcessingData = false;
volatile bool dataTransmissionComplete = false;
bool buttonstart = false; // Button state to start transmission
int currentSensorID = 0;  // 24 for Karen, 42 for Leon
volatile bool havedata = false;
volatile bool hasReceivedData = false;
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

typedef struct {
  uint8_t msgType;    // 0=data, 1=start, 2=end, 3=ack, 4=time_request, 5=time_response
  uint16_t packetId;
  uint16_t totalPackets;
  uint16_t dataSize;
  uint8_t data[240];
} espnow_packet_t;



// Remove these duplicate lines:
// Preferences prefs;
// int arrayCnt = 0;
// int readingCnt = maximumReadings;
// int currentSensorID = 0;

unsigned long localTimeUnix = 1672531200; // Default time (2023-01-01)

// Timeout for incomplete transmissions
const unsigned long TRANSMISSION_TIMEOUT = 30000; // 30 seconds

void sendAck(const uint8_t* mac, uint16_t packetId) {
  espnow_packet_t ack;
  ack.msgType = 3; // ACK
  ack.packetId = packetId;
  ack.totalPackets = 0;
  ack.dataSize = 0;
  
  esp_now_send(mac, (uint8_t*)&ack, sizeof(espnow_packet_t));
}

void sendTimeResponse(const uint8_t* mac) {
  espnow_packet_t timeResponse;
  timeResponse.msgType = 5; // Time response
  timeResponse.packetId = 0;
  timeResponse.totalPackets = 0;
  timeResponse.dataSize = sizeof(unsigned long);
  
  memcpy(timeResponse.data, &localTimeUnix, sizeof(unsigned long));
  
  esp_now_send(mac, (uint8_t*)&timeResponse, sizeof(espnow_packet_t));
  Serial.println("Sent time: " + String(localTimeUnix));
}

uint8_t getSensorID(const uint8_t* mac) {
  if (memcmp(mac, MAC_KAREN, 6) == 0) return 1;
  if (memcmp(mac, MAC_LEON, 6) == 0) return 2;
  return 0; // Unknown device
}


DeviceState* getDeviceState(uint8_t sensorID) {
  if (sensorID == 1) return &deviceStates[0];
  if (sensorID == 2) return &deviceStates[1];
  return nullptr;
}

void initializeDeviceState(DeviceState* state, uint16_t totalPackets, uint32_t totalSize, uint8_t sensorID) {
  if (state->packetReceived) free(state->packetReceived);
  if (state->buffer) free(state->buffer);
  
  state->receiving = true;
  state->expectedPackets = totalPackets;
  state->receivedPackets = 0;
  state->packetReceived = (bool*)calloc(totalPackets, sizeof(bool));
  state->buffer = (uint8_t*)malloc(totalSize);
  state->totalSize = totalSize;
  state->lastActivity = millis();
  state->sensorID = sensorID;
  
  Serial.println("Initialized reception for sensor " + String(sensorID) + 
                ": " + String(totalPackets) + " packets, " + String(totalSize) + " bytes");
}

void cleanupDeviceState(DeviceState* state) {
  if (state->packetReceived) {
    free(state->packetReceived);
    state->packetReceived = nullptr;
  }
  if (state->buffer) {
    free(state->buffer);
    state->buffer = nullptr;
  }
  state->receiving = false;
}

bool isTransmissionComplete(DeviceState* state) {
  return state->receivedPackets == state->expectedPackets;
}

void saveReadingsToFlash(uint8_t sensorID) {
  prefs.begin("stuff", false, "nvs2");
  ++arrayCnt;
  
  String key = String(arrayCnt) + "_" + String(sensorID);
  prefs.putBytes(key.c_str(), &Readings, sizeof(Readings));
  prefs.putInt("arrayCnt", arrayCnt);
  prefs.putInt("readingCnt", readingCnt);
  prefs.putInt("currentSensorID", sensorID);
  
  prefs.end();
  
  Serial.println("Saved array " + String(arrayCnt) + " for sensor " + String(sensorID) + " to flash");
}

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
  const uint8_t *mac = info->src_addr;
  espnow_packet_t packet;
  memcpy(&packet, incomingData, sizeof(packet));
  
  uint8_t sensorID = getSensorID(mac);
  if (sensorID == 0) {
    Serial.println("Unknown device MAC");
    return;
  }
  
  DeviceState* state = getDeviceState(sensorID);
  if (!state) return;
  
  state->lastActivity = millis();
  
  lastTransmissionTime = millis(); // Update global activity timer
  
  switch (packet.msgType) {
    case 1: // Start packet
      Serial.println("Start packet from sensor " + String(sensorID));
      hasReceivedData = true;
      initializeDeviceState(state, packet.totalPackets, packet.dataSize, sensorID);
      sendAck(mac, packet.packetId);
      break;
      
    case 0: // Data packet
      if (state->receiving && packet.packetId > 0 && packet.packetId <= state->expectedPackets) {
        uint16_t arrayIndex = packet.packetId - 1;
        
        if (!state->packetReceived[arrayIndex]) {
          // Calculate offset in buffer
          uint32_t offset = arrayIndex * 240;
          uint32_t copySize = min((uint32_t)packet.dataSize, state->totalSize - offset);
          
          memcpy(state->buffer + offset, packet.data, copySize);
          state->packetReceived[arrayIndex] = true;
          state->receivedPackets++;
          
          Serial.println("Received data packet " + String(packet.packetId) + "/" + 
                        String(state->expectedPackets) + " from sensor " + String(sensorID));
        }
        
        sendAck(mac, packet.packetId);
        
        // Check if transmission is complete
        if (isTransmissionComplete(state)) {
          Serial.println("All data packets received from sensor " + String(sensorID));
        }
      }
      break;
      
    case 2: // End packet
      Serial.println("End packet from sensor " + String(sensorID));
      
      if (state->receiving && isTransmissionComplete(state)) {
        // Copy data to Readings array and save
        memcpy(&Readings, state->buffer, sizeof(Readings));
        if (sensorID == 1) {
          currentSensorID = 24; // Karen
        } else if (sensorID == 2) {
          currentSensorID = 42; // Leon
        }
        //currentSensorID = sensorID;
        saveReadingsToFlash(currentSensorID);
        
        Serial.println("Data transmission completed successfully for sensor " + String(sensorID));
      } else {
        Serial.println("Incomplete transmission from sensor " + String(sensorID) + 
                      " (" + String(state->receivedPackets) + "/" + String(state->expectedPackets) + ")");
      }
      
      cleanupDeviceState(state);
      sendAck(mac, packet.packetId);
      break;
      
    case 4: // Time request
      Serial.println("Time request from sensor " + String(sensorID));
    time_t now = time(nullptr); // local-adjusted time
     localTimeUnix = static_cast<uint32_t>(now); // 32-bit to send via ESP-NOW
      sendTimeResponse(mac);
      break;
  }
}

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  // Handle send status if needed
}

void checkTimeouts() {
  unsigned long currentTime = millis();
  
  for (int i = 0; i < 2; i++) {
    DeviceState* state = &deviceStates[i];
    if (state->receiving && (currentTime - state->lastActivity) > TRANSMISSION_TIMEOUT) {
      Serial.println("Transmission timeout for sensor " + String(state->sensorID));
      cleanupDeviceState(state);
    }
  }
}

bool allTransmissionsComplete() {
  for (int i = 0; i < 2; i++) {
    if (deviceStates[i].receiving) {
      return false;
    }
  }
  return true;
}

void checkForRestart() {
  unsigned long currentTime = millis();
  
  // Check for 5-minute uptime restart (only if not busy)
  if ((currentTime > MAX_UPTIME) && allTransmissionsComplete()) {
    Serial.println("5-minute uptime reached. Restarting in 2 seconds...");
    Serial.flush();
    delay(2000);
    ESP.restart();
  }
  
  // Original restart logic after receiving data
  if (!hasReceivedData) return;
  
  if (allTransmissionsComplete() && 
      (currentTime - lastTransmissionTime) > IDLE_RESTART_DELAY) {
    
    Serial.println("All transmissions complete. Restarting in 2 seconds...");
    Serial.flush();
    delay(2000);
    ESP.restart();
  }
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
      else {
        Serial.println("Unknown sensor ID, skipping transmission.");
        i++;
        continue;
      }
      conn.execute(tosendstr.c_str());
      pg_status = 3;
      delay(10);
      i++;
    }
    delay(10);
  }
  Serial.printf("Transmission complete! Sent %d readings\n", readingCnt);



}



void loadFromFlash() {
  //prefs.begin("stuff", false, "nvs2");
  
      while (arrayCnt > 0) {
        String key = String(arrayCnt) + "_" + String(currentSensorID);
        Serial.print("Loading key ");
        Serial.println(key);
        prefs.getBytes(key.c_str(), &Readings, sizeof(Readings));
        readingCnt = maximumReadings;
        transmitReadings(); // Transmit the loaded readings
        Serial.printf("Loaded and transmitted reading #%d from flash\n", arrayCnt);

        arrayCnt--;
        
      }
      prefs.putInt("arrayCnt", 0);
      prefs.putInt("currentSensorID", 0);
   // prefs.end();
}



void setup() {
  sntp_set_time_sync_notification_cb(cbSyncTime);
    memset(deviceStates, 0, sizeof(deviceStates));
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  wifiandBlynk();
  prefs.begin("stuff", false, "nvs2");
  arrayCnt = prefs.getInt("arrayCnt", 0);
  currentSensorID = prefs.getInt("currentSensorID", 0);
  
  if (arrayCnt == 0) {
    Serial.println("No previous readings found, initializing...");
  } else {
    Serial.printf("Found %d previous readings\n", arrayCnt);
    Serial.printf("Current Sensor ID: %d\n", currentSensorID);
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
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);
  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo)); // Zero-initialize
  
  // Add KAREN
  memcpy(peerInfo.peer_addr, MAC_KAREN, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_STA; // <-- Add this line
  esp_now_add_peer(&peerInfo);
  
  // Add LEON
  memcpy(peerInfo.peer_addr, MAC_LEON, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_STA; // <-- Add this line
  esp_now_add_peer(&peerInfo);
    Serial.println("ESP32 Receiver initialized and ready");
}

void loop() {
  checkTimeouts();
  checkForRestart();
  delay(100);
}
