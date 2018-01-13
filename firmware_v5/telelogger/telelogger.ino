/******************************************************************************
* Reference sketch for a vehicle telematics data feed for Freematics Hub
* Works with Freematics ONE+
* Developed by Stanley Huang https://www.facebook.com/stanleyhuangyc
* Distributed under BSD license
* Visit http://freematics.com/hub for information about Freematics Hub
* Visit http://freematics.com/products for hardware information
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
* THE SOFTWARE.
******************************************************************************/

#include <FreematicsONE.h>
#include "config.h"

// logger states
#define STATE_STORAGE_READY 0x1
#define STATE_OBD_READY 0x2
#define STATE_GPS_READY 0x4
#define STATE_MEMS_READY 0x8
#define STATE_NET_READY 0x10
#define STATE_CONNECTED 0x20
#define STATE_ALL_GOOD 0x40

#define PIN_LED 4

#if MEMS_MODE
float accBias[3] = {0}; // calibrated reference accelerometer data
#endif
int lastSpeed = 0;
uint32_t lastSpeedTime = 0;
uint32_t distance = 0;
uint32_t lastSentTime = 0;
uint32_t lastSyncTime = 0;
uint8_t deviceTemp = 0; // device temperature

class CTeleLogger :
#if NET_DEVICE == NET_SERIAL
public CTeleClientSerialUSB
#elif NET_DEVICE == NET_BLE
public CTeleClientBLE
#elif NET_DEVICE == NET_WIFI
public CTeleClientWIFI
#elif NET_DEVICE == NET_SIM800
public CTeleClientSIM800
#elif NET_DEVICE == NET_SIM5360
public CTeleClientSIM5360
#else
public CTeleClient
#endif
#if ENABLE_OBD
,public COBDSPI
#else
,public CFreematicsESP32
#endif
{
public:
  bool setup()
  {
    clearState(STATE_ALL_GOOD);
    distance = 0;

#if MEMS_MODE
    if (!checkState(STATE_MEMS_READY)) {
      Serial.print("MEMS...");
      if (mems.memsInit()) {
        setState(STATE_MEMS_READY);
        Serial.println("OK");
        blePrint("MEMS OK");
      } else {
        Serial.println("NO");
      }
    }
#endif

#if ENABLE_OBD
    // initialize OBD communication
    if (!checkState(STATE_OBD_READY)) {
      Serial.print("OBD...");
      if (!init()) {
        Serial.println("NO");
        return false;
      }
      Serial.println("OK");
      blePrint("OBD OK");
      setState(STATE_OBD_READY);
    }
#endif

#ifdef ENABLE_GPS
    // start serial communication with GPS receiver
    if (!checkState(STATE_GPS_READY)) {
      Serial.print("GPS...");
      if (gpsInit(GPS_SERIAL_BAUDRATE)) {
        setState(STATE_GPS_READY);
        Serial.println("OK");
        blePrint("GPS OK");
      } else {
        Serial.println("NO");
      }
    }
#endif

#if NET_DEVICE == NET_WIFI
    for (byte attempts = 0; attempts < 3; attempts++) {
      Serial.print("WIFI(SSID:");
      Serial.print(WIFI_SSID);
      Serial.print(")...");
      if (netBegin() && netSetup(WIFI_SSID, WIFI_PASSWORD)) {
        blePrint("WIFI OK");
        Serial.println("OK");
        setState(STATE_NET_READY);
        break;
      } else {
        Serial.println("NO");
      }
    }
    if (!checkState(STATE_NET_READY)) {
      return false;
    }
#elif NET_DEVICE == NET_SIM800 || NET_DEVICE == NET_SIM5360
    // initialize network module
    if (!checkState(STATE_NET_READY)) {
      Serial.print(netDeviceName());
      Serial.print("...");
      if (netBegin()) {
        Serial.println("OK");
        blePrint("NET OK");
        setState(STATE_NET_READY);
      } else {
        Serial.println("NO");
        return false;
      }
    }
    Serial.print("CELL(APN:");
    Serial.print(CELL_APN);
    Serial.print(")");
    if (netSetup(CELL_APN)) {
      blePrint("CELL OK");
      String op = getOperatorName();
      if (op.length()) {
        Serial.println(op);
        blePrint(op);
      } else {
        Serial.println("OK");
      }
    } else {
      Serial.println("NO");
      return false;
    }
#endif

    if (checkState(STATE_MEMS_READY)) {
      calibrateMEMS();
    }

#if NET_DEVICE == NET_WIFI || NET_DEVICE == NET_SIM800 || NET_DEVICE == NET_SIM5360
    Serial.print("IP...");
    String ip = getIP();
    if (ip.length()) {
      Serial.println(ip);
      blePrint(ip);
    } else {
      Serial.println("NO");
    }
    int csq = getSignal();
    if (csq > 0) {
      Serial.print("CSQ...");
      Serial.print((float)csq / 10, 1);
      Serial.println("dB");
    }
#endif

    txCount = 0;
    cache.init(RAM_CACHE_SIZE);
    netbuf.init(256);
    if (!login()) {
      return false;
    }
    setState(STATE_CONNECTED);

    cache.header(feedid);
    lastSyncTime = millis();
#if NET_DEVICE == NET_SIM800 || NET_DEVICE == NET_SIM5360
    // log signal level
    if (csq) cache.log(PID_CSQ, csq);
#endif

    // check system time
    time_t utc;
    bool utcValid = false;
    time(&utc);
   	struct tm *btm = gmtime(&utc);
    if (btm->tm_year > 100) {
      // valid system time available
      char buf[64];
      sprintf(buf, "%04u-%02u-%02u %02u:%02u:%02u",
        1900 + btm->tm_year, btm->tm_mon + 1, btm->tm_mday, btm->tm_hour, btm->tm_min, btm->tm_sec);
      Serial.print("UTC:");
      Serial.println(buf);
      utcValid = true;
    }

#if STORAGE_TYPE != STORAGE_NONE
    // need valid current date for data storage
    GPS_DATA gdata = {0};
    unsigned long date = 0;
    if (utcValid) {
      date = (unsigned long)(1900 + btm->tm_year) * 10000 + (btm->tm_mon + 1) * 100 + btm->tm_mday;
    } else if (checkState(STATE_GPS_READY)) {
      // wait for GPS signal to get UTC
      Serial.print("Waiting GPS time..");
      for (int i = 0; gdata.date == 0 && i < 60; i++) {
        Serial.print('.');
        sleep(1000);
        gpsGetData(&gdata);
      }
      Serial.println();
      if (gdata.date) {
        unsigned int year = (gdata.date % 100) + 2000;
        unsigned int month = (gdata.date / 100) % 100;
        unsigned int day = (gdata.date / 10000);
        date = (unsigned long)year * 10000 + month * 100 + day;
      }
    }
    if (!checkState(STATE_STORAGE_READY)) {
      // init storage
      if (store.init()) {
        setState(STATE_STORAGE_READY);
        if (store.begin(date)) {
          cache.setForward(&store);
        }
      }
    }
#endif

    setState(STATE_ALL_GOOD);
    return true;
  }
  String executeCommand(const char* cmd)
  {
    String result;
    Serial.println(cmd);
    if (!strncmp(cmd, "LED", 3) && cmd[4]) {
      m_ledMode = atoi(cmd + 4);
      digitalWrite(PIN_LED, (m_ledMode == 2) ? HIGH : LOW);
      result = "OK";
    } else if (!strcmp(cmd, "REBOOT")) {
#if STORAGE_TYPE != STORAGE_NONE
      if (checkState(STATE_STORAGE_READY)) {
        store.end();
        clearState(STATE_STORAGE_READY);
      }
#endif
      ESP.restart();
      // never reach here
    } else if (!strcmp(cmd, "STANDBY")) {
      clearState(STATE_ALL_GOOD);
      result = "OK";
    } else if (!strncmp(cmd, "OBD", 3) && cmd[4]) {
      // send OBD command
      String obdcmd = cmd + 4;
      obdcmd += '\r';
      char buf[256];
      if (sendCommand(obdcmd.c_str(), buf, sizeof(buf), OBD_TIMEOUT_LONG) > 0) {
        Serial.println(buf);
        for (int n = 4; buf[n]; n++) {
          switch (buf[n]) {
          case '\r':
          case '\n':
            result += ' ';
            break;
          default:
            result += buf[n];
          }
        }
      } else {
        result = "TIMEOUT";
      }
    }
    return result;
  }
  bool processCommand(char* data)
  {
    char *p;
    if (!(p = strstr(data, "TK="))) return false;
    uint32_t token = atol(p + 3);
    if (!(p = strstr(data, "CMD="))) return false;
    char *cmd = p + 4;

    if (token > lastCmdToken) {
      // new command
      String result = executeCommand(cmd);
      // send command response
      char buf[256];
      snprintf(buf, sizeof(buf), "TK=%lu,MSG=%s", token, result.c_str());
      for (byte attempts = 0; attempts < 3; attempts++) {
        Serial.println("Sending ACK...");
        if (notifyServer(EVENT_ACK, SERVER_KEY, buf)) {
          Serial.println("ACK sent");
          break;
        }
      }
    }else {
      // previously executed command
      char buf[64];
      snprintf(buf, sizeof(buf), "TK=%lu,DUP=1", token);
      for (byte attempts = 0; attempts < 3; attempts++) {
        Serial.println("Sending ACK...");
        if (notifyServer(EVENT_ACK, SERVER_KEY, buf)) {
          Serial.println("ACK sent");
          break;
        }
      }
    }
  }

  void loop()
  {
    cache.timestamp(millis());

    if ((txCount % 100) == 1) {
      int temp = deviceTemp;
#ifdef ESP32
      temp = (int)readChipTemperature() * 165 / 255 - 40;
#endif
      cache.log(PID_DEVICE_TEMP, temp);
    }

#if ENABLE_OBD
    // process OBD data if connected
    if (checkState(STATE_OBD_READY)) {
      processOBD();
    }
#endif

#if ENABLE_GPS
    // process GPS data if connected
    if (checkState(STATE_GPS_READY)) {
      processGPS();
    }
#endif

#if ENABLE_OBD
    // read and log car battery voltage , data in 0.01v
    {
      int v = getVoltage() * 100;
      cache.log(PID_BATTERY_VOLTAGE, v);
    }
#endif

#if MEMS_MODE
    // process MEMS data if available
    if (checkState(STATE_MEMS_READY)) {
      processMEMS();
    }
#endif

    uint32_t t = millis();

    // check incoming datagram
    do {
      int len;
      char *data = netReceive(&len, 0);
      if (data) {
        data[len] = 0;
        if (!verifyChecksum(data)) {
          Serial.print("Checksum mismatch:");
          Serial.print(data);
          break;
        }
        char *p = strstr(data, "EV=");
        if (!p) break;
        int eventID = atoi(p + 3);
        switch (eventID) {
        case EVENT_COMMAND:
          processCommand(data);
          break;
        case EVENT_SYNC:
          lastSyncTime = t;
          break;
        }
      }
    } while(0);

    if (SERVER_SYNC_INTERVAL && t - lastSyncTime >= SERVER_SYNC_INTERVAL * 1000) {
      Serial.println("NO SYNC");
      connErrors++;
    } else if (t - lastSentTime >= DATA_SENDING_INTERVAL && cache.samples() > 0) {
      // start data chunk
      if (m_ledMode == 0) digitalWrite(PIN_LED, HIGH);
      //Serial.println(cache.buffer()); // print the content to be sent
      Serial.print('[');
      Serial.print(txCount);
      Serial.print("] ");
      cache.tailer();
      // transmit data
      if (netSend(cache.buffer(), cache.length())) {
        connErrors = 0;
        txCount++;
        // output some stats
        char buf[64];
#if STORAGE_TYPE == STORAGE_NONE
        sprintf(buf, "%u bytes sent", cache.length());
#else
        sprintf(buf, "%uB sent %lu KB saved", cache.length(), store.size() >> 10);
#endif
        blePrint(buf);
        Serial.println(buf);
        // purge cache and place a header
        cache.header(feedid);
      } else {
        connErrors++;
        Serial.println("Unsent");
        blePrint("Unsent");
        cache.untailer(); // remove tail
      }
      if (m_ledMode == 0) digitalWrite(PIN_LED, LOW);
      if (getConnErrors() >= MAX_CONN_ERRORS_RECONNECT) {
        netClose();
        netOpen(SERVER_HOST, SERVER_PORT);
        lastSyncTime = 0; // sync on next time
      }
      lastSentTime = t;
    }

#if ENABLE_OBD
    if (errors > MAX_OBD_ERRORS) {
      reset();
      clearState(STATE_OBD_READY | STATE_ALL_GOOD);
    }
#endif

    // check serial input and store in command buffer
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\r' || c == '\n') {
        if (serialCommand.length() > 0) {
          String result = executeCommand(serialCommand.c_str());
          serialCommand = "";
          Serial.println(result);
        }
      } else if (serialCommand.length() < 32) {
        serialCommand += c;
      }
    }

    if (deviceTemp >= COOLING_DOWN_TEMP) {
      // device too hot, cool down
      Serial.println("Cooling down - ");
      Serial.print(deviceTemp);
      Serial.println('C');
      delay(10000);
    }
  }
  bool login()
  {
#if NET_DEVICE == NET_WIFI || NET_DEVICE == NET_SIM800 || NET_DEVICE == NET_SIM5360
      // retrieve additional vehicle data for server submission
      String data;
      data = "VIN=";
#if ENABLE_OBD
      char buf[128];
      if (getVIN(buf, sizeof(buf))) {
        Serial.print("VIN:");
        Serial.println(buf);
        data += buf;
      } else {
        data += DEFAULT_VIN;
      }
      // load DTC
      uint16_t dtc[6];
      byte dtcCount = readDTC(dtc, sizeof(dtc) / sizeof(dtc[0]));
      if (dtcCount > 0) {
        Serial.print("DTC:");
        Serial.println(dtcCount);
        data += ",DTC=";
        int bytes = 0;
        for (byte i = 0; i < dtcCount; i++) {
          bytes += sprintf(buf + bytes, "%X;", dtc[i]);
        }
        buf[bytes - 1] = 0;
        data += buf;
      }
#else
      data += DEFAULT_VIN;
#endif
    // connect to telematics server
    for (byte attempts = 0; attempts < 3; attempts++) {
      Serial.print("LOGIN...");
      if (!netOpen(SERVER_HOST, SERVER_PORT)) {
        Serial.println("NO");
        continue;
      }
      // login Freematics Hub
      if (!notifyServer(EVENT_LOGIN, SERVER_KEY, data.c_str())) {
        netClose();
        Serial.println("NO");
        continue;
      } else {
        Serial.println("OK");
      }

      Serial.print("SERVER:");
      Serial.println(serverName());

      Serial.print("FEED ID:");
      Serial.println(feedid);

      return true;
    }
    return false;
#elif NET_DEVICE == NET_BLE
    blePrint(payload);
    return true;
#endif
  }
  bool verifyChecksum(char* data)
  {
    uint8_t sum = 0;
    char *s = strrchr(data, '*');
    if (!s) return false;
    for (char *p = data; p < s; p++) sum += *p;
    if (hex2uint8(s + 1) == sum) {
      *s = 0;
      return true;
    }
    return false;
  }
  bool notifyServer(byte event, const char* serverKey, const char* payload = 0)
  {
    netbuf.header(feedid);
    String req = "EV=";
    req += (unsigned int)event;
    req += ",TS=";
    req += millis();
    if (serverKey) {
      req += ",SK=";
      req += serverKey;
    }
    if (payload) {
      req += ',';
      req += payload;
    }
    netbuf.dispatch(req.c_str(), req.length());
    netbuf.tailer();
    //Serial.println(netbuf.buffer());
    for (byte attempts = 0; attempts < 3; attempts++) {
      if (!netSend(netbuf.buffer(), netbuf.length())) {
        Serial.print('.');
        continue;
      }
#if NET_DEVICE == NET_BLE
        return true;
#endif
      if (event == EVENT_ACK) return true; // no reply for ACK
      // receive reply
      int len;
      char *data = netReceive(&len);
      if (!data) {
        // no reply yet
        Serial.print('.');
        continue;
      }
      data[len] = 0;
      // verify checksum
      if (!verifyChecksum(data)) {
        Serial.print("Checksum mismatch:");
        Serial.print(data);
        continue;
      }
      char pattern[16];
      sprintf(pattern, "EV=%u", event);
      if (!strstr(data, pattern)) {
        Serial.print("Invalid reply");
        continue;
      }
      if (event == EVENT_LOGIN) {
        // extract info from server response
        char *p = strstr(data, "TM=");
        if (p) {
          // set local time from server
          unsigned long tm = atol(p + 3);
          struct timeval tv = { .tv_sec = (time_t)tm, .tv_usec = 0 };
          settimeofday(&tv, NULL);
        }
        p = strstr(data, "SN=");
        if (p) {
          char *q = strchr(p, ',');
          if (q) *q = 0;
          m_serverName = p + 3;
        }
        feedid = hex2uint16(data);
      }
      Serial.print(' ');
      connErrors = 0;
      return true;
    }
    return false;
  }
  void shutDownNet()
  {
    Serial.print(netDeviceName());
    netClose();
    netEnd();
    clearState(STATE_NET_READY);
    Serial.println(" OFF");
  }
  void standby()
  {
      if (checkState(STATE_NET_READY)) {
        if (checkState(STATE_CONNECTED)) {
          notifyServer(EVENT_LOGOUT, SERVER_KEY);
        }
        shutDownNet();
      }
#if STORAGE_TYPE != STORAGE_NONE
      if (checkState(STATE_STORAGE_READY)) {
        cache.uninit();
        store.end();
        clearState(STATE_STORAGE_READY);
      }
#endif
#if ENABLE_GPS
      if (checkState(STATE_GPS_READY)) {
        Serial.println("GPS OFF");
        gpsInit(0); // turn off GPS power
      }
#endif
#if ENABLE_OBD
      if (errors > MAX_OBD_ERRORS) {
        // inaccessible OBD treated as end of trip
        feedid = 0;
      }
#endif
      clearState(STATE_OBD_READY | STATE_GPS_READY | STATE_NET_READY | STATE_CONNECTED);
      Serial.println("Standby");
      blePrint("Standby");
#if MEMS_MODE
      if (checkState(STATE_MEMS_READY)) {
        calibrateMEMS();
        for (;;) {
          delay(100);
          // calculate relative movement
          float motion = 0;
          float acc[3];
          mems.memsRead(acc);
          for (byte i = 0; i < 3; i++) {
            float m = (acc[i] - accBias[i]);
            motion += m * m;
          }
          //Serial.println(motion);
          // check movement
          if (motion > WAKEUP_MOTION_THRESHOLD * WAKEUP_MOTION_THRESHOLD) {
            break;
          }
        }
      }
#else
      while (!init()) Serial.print('.');
#endif
      Serial.println("Wakeup");
      blePrint("Wakeup");
  }
  bool checkState(byte flags) { return (m_state & flags) == flags; }
  void setState(byte flags) { m_state |= flags; }
  void clearState(byte flags) { m_state &= ~flags; }
private:
#if ENABLE_OBD
  void processOBD()
  {
      int speed = readSpeed();
      if (speed == -1) {
        return;
      }
      cache.log(0x100 | PID_SPEED, speed);
      cache.log(PID_TRIP_DISTANCE, distance);
      // poll more PIDs
      const byte pids[]= {PID_RPM, PID_ENGINE_LOAD, PID_THROTTLE};
      int value;
      for (byte i = 0; i < sizeof(pids) / sizeof(pids[0]); i++) {
        if (readPID(pids[i], value)) {
          cache.log(0x100 | pids[i], value);
        }
        delay(1);
      }
      static byte count = 0;
      if ((count++ % 50) == 0) {
        const byte pidTier2[] = {PID_INTAKE_TEMP, PID_COOLANT_TEMP, PID_BAROMETRIC, PID_AMBIENT_TEMP, PID_ENGINE_FUEL_RATE};
        byte pid = pidTier2[count / 50];
        if (isValidPID(pid) && readPID(pid, value)) {
          cache.log(0x100 | pid, value);
        }
        delay(1);
      }
    }
    int readSpeed()
    {
        int value;
        if (readPID(PID_SPEED, value)) {
           uint32_t t = millis();
           distance += (value + lastSpeed) * (t - lastSpeedTime) / 3600 / 2;
           lastSpeedTime = t;
           lastSpeed = value;
           return value;
        } else {
          return -1;
        }
    }
#endif
#if MEMS_MODE
    void processMEMS()
    {
        // load and store accelerometer
        float acc[3];
        int16_t temp;
        mems.memsRead(acc, 0, 0, &temp);
        deviceTemp = temp / 10;
        cache.log(PID_ACC, (int16_t)((acc[0] - accBias[0]) * 100), (int16_t)((acc[1] - accBias[1]) * 100), (int16_t)((acc[2] - accBias[2]) * 100));
    }
#endif
#if ENABLE_GPS
    void processGPS()
    {
        static uint16_t lastUTC = 0;
        static uint8_t lastGPSDay = 0;
        GPS_DATA gd = {0};
        // read parsed GPS data
        if (gpsGetData(&gd)) {
            if (gd.date && lastUTC != (uint16_t)gd.time) {
              byte day = gd.date / 10000;
              cache.log(PID_GPS_TIME, gd.time);
              if (lastGPSDay != day) {
                cache.log(PID_GPS_DATE, gd.date);
                lastGPSDay = day;
              }
              cache.logCoordinate(PID_GPS_LATITUDE, gd.lat);
              cache.logCoordinate(PID_GPS_LONGITUDE, gd.lng);
              cache.log(PID_GPS_ALTITUDE, gd.alt);
              cache.log(PID_GPS_SPEED, gd.speed);
              cache.log(PID_GPS_SAT_COUNT, gd.sat);
              lastUTC = (uint16_t)gd.time;
              char buf[32];
              sprintf(buf, "UTC:%08lu SAT:%u", gd.time, (unsigned int)gd.sat);
              Serial.println(buf);
              blePrint(buf);
            }
        }
    }
#endif
#if MEMS_MODE
    void calibrateMEMS()
    {
        Serial.print("ACC BIAS...");
        accBias[0] = 0;
        accBias[1] = 0;
        accBias[2] = 0;
        int n;
        for (n = 0; n < 100; n++) {
          float acc[3] = {0};
          mems.memsRead(acc);
          accBias[0] += acc[0];
          accBias[1] += acc[1];
          accBias[2] += acc[2];
          delay(10);
        }
        accBias[0] /= n;
        accBias[1] /= n;
        accBias[2] /= n;
        Serial.print(accBias[0]);
        Serial.print('/');
        Serial.print(accBias[1]);
        Serial.print('/');
        Serial.println(accBias[2]);
    }
#endif
#if MEMS_MODE == MEMS_ACC
    MPU9250_ACC mems;
#elif MEMS_MODE == MEMS_9DOF
    MPU9250_9DOF mems;
#elif MEMS_MODE == MEMS_DMP
    MPU9250_DMP mems;
#endif
    CStorageRAM cache;
#if STORAGE_TYPE == STORAGE_SD
    CStorageSD store;
#endif
    CStorageRAM netbuf;
    byte m_state = 0;
    byte m_ledMode = 0;
    uint32_t lastCmdToken = 0;
    String serialCommand;
};

CTeleLogger logger;

void setup()
{
    delay(500);
    // initialize USB serial
    Serial.begin(115200);
    Serial.print("ESP32 @ ");
    Serial.print(ESP.getCpuFreqMHz());
    Serial.println("MHz");
    // init LED pin
    pinMode(PIN_LED, OUTPUT);
    // perform initializations
#if ENABLE_BLE
    logger.bleBegin(BLE_DEVICE_NAME);
    delay(100);
#endif
    digitalWrite(PIN_LED, HIGH);
    logger.begin();
    logger.setup();
    digitalWrite(PIN_LED, LOW);
}

void loop()
{
    // error handling
    if (!logger.checkState(STATE_ALL_GOOD)) {
      logger.standby();
      for (byte n = 0; n < 3; n++) {
        digitalWrite(PIN_LED, HIGH);
        logger.setup();
        digitalWrite(PIN_LED, LOW);
        if (logger.checkState(STATE_ALL_GOOD)) break;
        logger.sleep(3000);
      }
      return;
    }
    if (logger.getConnErrors() >= MAX_CONN_ERRORS) {
      digitalWrite(PIN_LED, HIGH);
      logger.shutDownNet();
      logger.setup();
      digitalWrite(PIN_LED, LOW);
      return;
    }
    // collect and transmit data
    logger.loop();
}