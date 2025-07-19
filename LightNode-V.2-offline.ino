#include "config.h"
#include "shared_libraries.h"

// Network objects
IPAddress local_IP; 
IPAddress gateway;  
IPAddress subnet;   
WiFiManager wifiManager;
ESP8266WebServer server(80);

String APSSID = String(SERIAL_NUMBER) + "-AP";
char hostURL[128];
int deviceId = 0;
char ipString[16] = "";
char deviceHostname[50] = ""; 
bool isRegistered = false;  //State of the device if registered or not
bool isLEDOn = false; // State of the LED
bool isTesting = false; // State of test light
bool isPaused = false; //State of paused time
bool isFree = false; //State of free light
bool isOpenTime = false; //State of open time
unsigned int watchdogIntervalMinutes = 0;
int storedTimeInSeconds = 0; //Running timer of the device
unsigned long lastMillis = 0; // Last recorded time
bool APLastButtonState = HIGH; // Last state of the AP button
char startDateTime[20];
int thread;
int writeInterval;
int disconnectCounter = 0;
bool sentOnRestartHeartbeat = false;

void setup()
{
  InitializeSerial();

  //Initialize relay pin first to avoid light flicks on restart
  InitializeRelayPin();
  
  delay(1000);
  InitializeSPIFFS();
  InitializeComponents();
  InitializeOTA();

  LoadConfig();
  ConnectToWiFi();

//    ForceReset(true); //Used to hard reset the device. Usually for testing purpose
  if (!isRegistered) {
    RegisterUpdateDevice(true);
  }
  else 
  {
    RegisterUpdateDevice(false);
  }

  RemainingTimeCleanup();
  
  //Setup device APIs
  SetupServerAPI();

  sentOnRestartHeartbeat = SentOnRestartHeartbeat();

  //MAKE THIS A SYSTEM PROPERTY
  //ConfigureRelayStateOnStartUp();
}

void loop()
{
  server.handleClient();
  yield();

  // Non-blocking OTA handling
  static unsigned long otaPreviousMillis = 0;
  unsigned long otaCurrentMillis = millis();
  if (otaCurrentMillis - otaPreviousMillis >= 500) { // Handle OTA every 500ms
      otaPreviousMillis = otaCurrentMillis;
      ArduinoOTA.handle();
  }

  // Non-blocking relay timing
  static unsigned long relayPreviousMillis = 0;
  unsigned long relayCurrentMillis = millis();
  if (relayCurrentMillis - relayPreviousMillis >= 1000) { // Every second
      relayPreviousMillis = relayCurrentMillis;
      ManageRelayTiming();
  }

  static unsigned long heartbeatPreviousMillis = 0;
  unsigned long heartbeatCurrentMillis = millis();
  if (heartbeatCurrentMillis - heartbeatPreviousMillis >= 60000) { // Every minute
      heartbeatPreviousMillis = heartbeatCurrentMillis;
      if (sentOnRestartHeartbeat)
      {
        SendRegularHeartbeat();  
      }
      else
      {
        sentOnRestartHeartbeat = SentOnRestartHeartbeat();
      }
  }

  static unsigned long failedConnectionPreviousMillis = 0;
  unsigned long failedConnectionCurrentMillis = millis();
  if (failedConnectionCurrentMillis - failedConnectionPreviousMillis >= 60000) { // Every minute
      failedConnectionPreviousMillis = failedConnectionCurrentMillis;
      if (disconnectCounter == MAX_DISCONNECT_COUNTS)
      {
         disconnectCounter = 0;  
         sentOnRestartHeartbeat = false;
//         ESP.restart();
         RemainingTimeCleanup();
         ForceReset(false);
      }
  }
}

void RemainingTimeCleanup()
{
  isLEDOn = false;
  EEPROM.put(STORED_TIME_IN_SECONDS, 0);
  storedTimeInSeconds = 0;
  EEPROM.put(IS_LED_ON, isLEDOn);
  digitalWrite(RELAY_PIN, LOW);
  EEPROM.commit();                               
}

void ForceReset(bool resetEEPROM)
{
  if (resetEEPROM)
  {
    if (ResetEEPROMSPIFFS()) {
       wifiManager.resetSettings();
       delay(2000);
       ESP.reset();
    } 
  }
  else 
  {
    sentOnRestartHeartbeat = false;
    wifiManager.startConfigPortal(APSSID.c_str());
//    delay(2000);
//    ESP.reset();
  }
}

void RestartDevice()
{
    Serial.println("Restarting device...");  
    SPIFFS.end();  
    delay(200);
    ESP.restart();
}

void CreateSPIFFSFile() {
  //TIME LOGGING
  File file = SPIFFS.open(TIME_FILE_PATH, "w");
  if (!file) {
    Serial.println("Failed to open file for writing");
    return;
  }

  int initialTime = 0;
  file.println(initialTime);
  file.close();

  Serial.println(F("Time file created with initial value: ") + String(initialTime));
}

void SetupServerAPI() 
{
  server.on("/api/delete", HTTP_POST, []() 
  {
      String errorMessage = "";
      bool errorOccurred = false;

      if (server.hasArg("plain")) 
      { 
          String body = server.arg("plain");
          Serial.println("Request body: " + body);
  
          // Parse JSON body
          StaticJsonDocument<256> doc;
          DeserializationError error = deserializeJson(doc, body);
  
          if (error) {
              errorMessage = "Invalid JSON in request body";
              errorOccurred = true;
              Serial.println("ERROR: " + errorMessage);
          } 
          else 
          {
              // Extract values from the parsed JSON
              int device_id = doc["device_id"];

              //Validate device id
              if (device_id != deviceId)
              {
                  errorMessage = "Device does not match the configuration.";
                  errorOccurred = true;
                  Serial.println("ERROR: " + errorMessage);
              }
              else
              {
                digitalWrite(RELAY_PIN, LOW);
                Serial.println("Request to delete device received.");
                server.send(200, "text/html", "<html><body><h1>Delete Device Request</h1><p>Received device delete request.</p></body></html>");
              }
          }
      } 
      else 
      {
          errorMessage = "Missing request body";
          errorOccurred = true;
      }
  
      // Respond to the client
      if (errorOccurred) {
          String jsonResponse = "{\"response\":\"" + errorMessage + "\"}";
          server.send(400, "application/json", jsonResponse);
      } 
      else 
      {
          // Attempt to reset EEPROM and SPIFFS
          if (!ResetEEPROMSPIFFS()) {
              errorMessage = "Failed to reset EEPROM and SPIFFS.";
              errorOccurred = true;
          }
          else {
              errorMessage = "";
              errorOccurred = false;
          }
    
          delay(5000);
          DeleteDevice(errorOccurred, errorMessage);
      }
  });

  server.on("/api/span", HTTP_POST, []() {
      
      Serial.println("START/EXTEND Time request received");
  
      bool errorOccurred = false;
      String errorMessage;
      int temp_storedTimeInSeconds = 0;
      int currentThread = 0;
      String spanType = "START";
  
      if (server.hasArg("plain")) { 
          String body = server.arg("plain");
          Serial.println("Request body: " + body);
  
          // Parse JSON body
          StaticJsonDocument<256> doc;
          DeserializationError error = deserializeJson(doc, body);
  
          if (error) {
              errorMessage = "Invalid JSON in request body";
              errorOccurred = true;
              Serial.println("ERROR: " + errorMessage);
          } 
          else 
          {
              // Extract values from the parsed JSON
              int timeInSeconds = doc["time"] | 0; 
              int transactionThread = doc["thread"] | 0;
              const char* startDateTimeArg = doc["startdatetime"] | ""; 
              int device_id = doc["device_id"];
              isLEDOn = true;
              isOpenTime = false;
  
              Serial.println("time: " + String(timeInSeconds));
              Serial.println("thread: " + String(transactionThread));
              Serial.println("startDateTime: " + String(startDateTimeArg));

              //Validate device id
              if (device_id != deviceId)
              {
                  errorMessage = "Device does not match the configuration.";
                  errorOccurred = true;
                  Serial.println("ERROR: " + errorMessage);
              }
              else
              {
                // Validate and process values
                if (timeInSeconds <= 0) {
                    errorMessage = "Invalid or negative time value!";
                    errorOccurred = true;
                    Serial.println("ERROR: " + errorMessage);
                } else 
                {
                    temp_storedTimeInSeconds = ReadTimeFromSPIFFS();
    
                    Serial.println("storedTimeInSeconds value from EEPROM: " + String(temp_storedTimeInSeconds));
                    if (temp_storedTimeInSeconds < 0) {
                        errorMessage = "Failed to read time from EEPROM.";
                        errorOccurred = true;
                        Serial.println("ERROR: " + errorMessage);
                    } 
                    else 
                    {
                        if (temp_storedTimeInSeconds > 0)
                        {
                            spanType = "EXTEND";
                        }
                        else
                        {
                            timeInSeconds = timeInSeconds + GRACE_PERIOD_SECONDS;
                        }
                        
                        temp_storedTimeInSeconds += timeInSeconds;
                        storedTimeInSeconds = temp_storedTimeInSeconds;
                        writeInterval = CalculateWriteInterval(storedTimeInSeconds);
  
                        Serial.println("Calculated Write Interval: " + String(writeInterval));
                        
                        // Store values in EEPROM
                        char startDateTime[20];
                        strncpy(startDateTime, startDateTimeArg, sizeof(startDateTime) - 1);
                        startDateTime[sizeof(startDateTime) - 1] = '\0';
                        EEPROM.get(THREAD, currentThread);
  
                        if (transactionThread > 0)
                        {
                            EEPROM.put(THREAD, transactionThread);  
                            EEPROM.put(START_DATE_TIME, startDateTime);
                        }
              
                        EEPROM.put(STORED_TIME_IN_SECONDS, storedTimeInSeconds);
                        EEPROM.put(IS_LED_ON, isLEDOn);
                        EEPROM.put(IS_OPEN_TIME, isOpenTime);
                        EEPROM.put(SPIFFS_WRITE_INTERVAL, writeInterval);
    
                        if (!EEPROM.commit()) {
                            errorMessage = "Failed to commit data to EEPROM.";
                            errorOccurred = true;
                            Serial.println("ERROR: " + errorMessage);
                        } else {
                            if (!WriteTimeToSPIFFS(storedTimeInSeconds)) {
                                errorMessage = "Failed to write time to SPIFFS.";
                                errorOccurred = true;
                            } else {
                                //Turn on the light/relay
                                digitalWrite(RELAY_PIN, HIGH);
                            }
                        }
                    }
                }
              }
          }
      } 
      else 
      {
          errorMessage = "Missing request body";
          errorOccurred = true;
      }
  
      // Respond to the client
      if (errorOccurred) {
          String jsonResponse = "{\"response\":\"" + errorMessage + "\"}";
          server.send(400, "application/json", jsonResponse);
      } else {
          String jsonResponse = "{\"thread\": " + String(currentThread) + "}";
          server.send(200, "application/json", jsonResponse);
          Serial.println("---" + spanType + " RATED TIME THREAD---");
      }
  });

  server.on("/api/opentime/start", HTTP_POST, []() {
      
      Serial.println("START Open Time request received");
  
      bool errorOccurred = false;
      String errorMessage;
      int currentThread = 0;
  
      if (server.hasArg("plain")) { 
          String body = server.arg("plain");
          Serial.println("Request body: " + body);
  
          // Parse JSON body
          StaticJsonDocument<256> doc;
          DeserializationError error = deserializeJson(doc, body);
  
          if (error) {
              errorMessage = "Invalid JSON in request body";
              errorOccurred = true;
              Serial.println("ERROR: " + errorMessage);
          } else {
              // Extract values from the parsed JSON
              int transactionThread = doc["thread"] | 0;
              const char* startDateTimeArg = doc["startdatetime"] | ""; 
              isLEDOn = true;
              isOpenTime = true;
  
              Serial.println("thread: " + String(transactionThread));
              Serial.println("startDateTime: " + String(startDateTimeArg));
              
              storedTimeInSeconds = 0;
              writeInterval = CalculateWriteInterval(storedTimeInSeconds);

              Serial.println("Calculated Write Interval: " + String(writeInterval));
                      
              // Store values in EEPROM
              char startDateTime[20];
              strncpy(startDateTime, startDateTimeArg, sizeof(startDateTime) - 1);
              startDateTime[sizeof(startDateTime) - 1] = '\0';
              EEPROM.get(THREAD, currentThread);

              EEPROM.put(THREAD, transactionThread);  
              EEPROM.put(START_DATE_TIME, startDateTime);
              EEPROM.put(STORED_TIME_IN_SECONDS, 0);
              EEPROM.put(IS_LED_ON, isLEDOn);
              EEPROM.put(IS_OPEN_TIME, isOpenTime);
              EEPROM.put(SPIFFS_WRITE_INTERVAL, writeInterval);

              if (!EEPROM.commit()) {
                errorMessage = "Failed to commit data to EEPROM.";
                errorOccurred = true;
                Serial.println("ERROR: " + errorMessage);
              } else 
              {
                if (!WriteTimeToSPIFFS(storedTimeInSeconds)) 
                {
                   errorMessage = "Failed to write time to SPIFFS.";
                   errorOccurred = true;
                } 
                else 
                {
                   //Turn on the light/relay
                   digitalWrite(RELAY_PIN, HIGH);
                }
              }
          }
      } else {
          errorMessage = "Missing request body";
          errorOccurred = true;
      }
  
      // Respond to the client
      if (errorOccurred) {
          String jsonResponse = "{\"response\":\"" + errorMessage + "\"}";
          server.send(400, "application/json", jsonResponse);
      } else {
          String jsonResponse = "{\"thread\": " + String(currentThread) + "}";
          server.send(200, "application/json", jsonResponse);
          Serial.println("---START OPEN TIME THREAD---");
      }
  });
  
  server.on("/api/stop", HTTP_POST, []() {

      Serial.println("END Time request received");
      
      bool errorOccurred = false;
      String errorMessage;
  
      // Attempt to update EEPROM
      isLEDOn = false;
      isPaused = false;
      isOpenTime = false;
      storedTimeInSeconds = 0;
      int currentThread = 0;
      const char defaultStartDateTime[] = "";
      strncpy(startDateTime, defaultStartDateTime, sizeof(startDateTime));
      EEPROM.get(THREAD, currentThread);
      
      EEPROM.put(IS_LED_ON, isLEDOn);
      EEPROM.put(IS_PAUSED, isPaused);
      EEPROM.put(IS_OPEN_TIME, isOpenTime);
      EEPROM.put(START_DATE_TIME, "");
      EEPROM.put(STORED_TIME_IN_SECONDS, 0);
      EEPROM.put(THREAD, 0);
      EEPROM.put(SPIFFS_WRITE_INTERVAL, 0);
      
      if (!EEPROM.commit()) {
          errorMessage = "Failed to commit data to EEPROM.";
          errorOccurred = true;
          Serial.println("ERROR: " + errorMessage);
      }
      else 
      {
          if (!WriteTimeToSPIFFS(storedTimeInSeconds)) 
          {
              errorMessage = "Failed to write time to SPIFFS.";
              errorOccurred = true;
              Serial.println("ERROR: " + errorMessage);
          } 
          else 
          {
              //Turn off the light/relay
              digitalWrite(RELAY_PIN, LOW);
          }
      }

      if (errorOccurred) {
          String jsonResponse = "{\"response\":\"" + errorMessage + "\"}";
          server.send(400, "application/json", jsonResponse);
      } else {
          String jsonResponse = "{\"thread\": " + String(currentThread) + "}";
          server.send(200, "application/json", jsonResponse);
          Serial.println("---END THREAD---");
      }
  });

  server.on("/api/pause", HTTP_POST, []() {
      
      Serial.println("PAUSE Time request received");
      
      bool errorOccurred = false;
      String errorMessage;

      // Attempt to pause the device
      isPaused = true;
      isLEDOn = false;
      int currentThread = 0;
      EEPROM.get(THREAD, currentThread);
      
      EEPROM.put(IS_PAUSED, isPaused);
      EEPROM.put(IS_LED_ON, isLEDOn);

      if (!EEPROM.commit()) {
          errorMessage = "Failed to commit data to EEPROM.";
          errorOccurred = true;
          Serial.println("ERROR: " + errorMessage);
      }
      else 
      {
          // Attempt to turn off the LED
          digitalWrite(RELAY_PIN, LOW);
      }

      if (errorOccurred) {
          String jsonResponse = "{\"response\":\"" + errorMessage + "\"}";
          server.send(400, "application/json", jsonResponse);
      } else {
          String jsonResponse = "{\"thread\": " + String(currentThread) + "}";
          server.send(200, "application/json", jsonResponse);
          Serial.println("---PAUSED THREAD---");
      }
  });

  server.on("/api/resume", HTTP_POST, []() {

    Serial.println("RESUME Time request received");
  
    bool errorOccurred = false;
    String errorMessage;
    int currentThread = 0;
    isPaused = false;
    isLEDOn = true;
  
    if (server.hasArg("plain")) {
      String body = server.arg("plain");
      Serial.println("Request body: " + body);
  
      // Parse JSON body
      StaticJsonDocument < 256 > doc;
      DeserializationError error = deserializeJson(doc, body);
  
      if (error) {
        errorMessage = "Invalid JSON in request body";
        errorOccurred = true;
        Serial.println("ERROR: " + errorMessage);
      } else {
        // Extract values from the parsed JSON
        int timeInSeconds = doc["remainingTime"] | 0;
        bool isOpenTime = doc["openTime"];
  
        Serial.println("remainingTime: " + String(timeInSeconds));
  
        // Validate and process values
        if (timeInSeconds <= 0) {
          errorMessage = "Invalid or negative time value!";
          errorOccurred = true;
          Serial.println("ERROR: " + errorMessage);
        } else {
          storedTimeInSeconds = timeInSeconds;
          currentThread = 0;
          EEPROM.get(THREAD, currentThread);
          EEPROM.put(IS_PAUSED, isPaused);
          EEPROM.put(IS_OPEN_TIME, isOpenTime);
          EEPROM.put(STORED_TIME_IN_SECONDS, storedTimeInSeconds);
          EEPROM.put(IS_PAUSED, isPaused);
          EEPROM.put(IS_LED_ON, isLEDOn);
  
          if (!EEPROM.commit()) {
            errorMessage = "Failed to commit data to EEPROM.";
            errorOccurred = true;
            Serial.println("ERROR: " + errorMessage);
          } else {
            if (!WriteTimeToSPIFFS(storedTimeInSeconds)) {
              errorMessage = "Failed to write time to SPIFFS.";
              errorOccurred = true;
            } else {
              //Turn on the light/relay
              digitalWrite(RELAY_PIN, HIGH);
            }
          }
        }
      }
    } else {
      errorMessage = "Missing request body";
      errorOccurred = true;
    }
  
    if (errorOccurred) {
      String jsonResponse = "{\"response\":\"" + errorMessage + "\"}";
      server.send(400, "application/json", jsonResponse);
    } else {
      String jsonResponse = "{\"thread\": " + String(currentThread) + "}";
      server.send(200, "application/json", jsonResponse);
      Serial.println("---RESUME THREAD---");
    }
  });

  server.on("/api/test", HTTP_POST, []() {

      bool errorOccurred = false;
      String errorMessage;

      if (server.hasArg("plain")) 
      {
        String body = server.arg("plain");
        Serial.println("Request body: " + body);
    
        // Parse JSON body
        StaticJsonDocument < 256 > doc;
        DeserializationError error = deserializeJson(doc, body);
  
        if (error) 
        {
          errorMessage = "Invalid JSON in request body";
          errorOccurred = true;
          Serial.println("ERROR: " + errorMessage);
        } 
        else 
        {
          // Extract values from the parsed JSON
          int device_id = doc["device_id"];

          //Validate device id
          if (device_id != deviceId)
          {
             errorMessage = "Device does not match the configuration.";
             errorOccurred = true;
             Serial.println("ERROR: " + errorMessage);
          }
          else
          {
            Serial.println("TEST Device request received");
            storedTimeInSeconds = 10; // 10 seconds of testing
      
            isTesting = true;
            
          }
        }
      }
      else 
      {
          errorMessage = "Missing request body";
          errorOccurred = true;
      }

      // Respond to the client
      if (errorOccurred) {
          String jsonResponse = "{\"response\":\"" + errorMessage + "\"}";
          server.send(400, "application/json", jsonResponse);
      } else {
          server.send(200, "text/plain", "Device test initiated. Time set to 10000 seconds");
          Serial.println("---TEST THREAD---");
      }
  });

  server.on("/api/startfree", HTTP_POST, []() {

      Serial.println("START FREE LIGHT Device request received");

      //WE DO NOT SET isLEDOn TO TRUE
      //SO WHEN THE POWER HAS BEEN RESTARTED
      //THEN FREE LIGHT WOULD BE RETAINED
      
      bool errorOccurred = false;
      String errorMessage;
      int currentThread = 0;

      if (server.hasArg("plain")) { 
          String body = server.arg("plain");
          Serial.println("Request body: " + body);
  
          // Parse JSON body
          StaticJsonDocument<256> doc;
          DeserializationError error = deserializeJson(doc, body);
  
          if (error) {
              errorMessage = "Invalid JSON in request body";
              errorOccurred = true;
              Serial.println("ERROR: " + errorMessage);
          } else {
              // Extract values from the parsed JSON
              int transactionThread = doc["thread"] | 0;
              int timeInSeconds = doc["span"] | 0; 
              isFree = true;

              Serial.println("time: " + String(timeInSeconds));
              Serial.println("thread: " + String(transactionThread));

              if (timeInSeconds <= 0) {
                  errorMessage = "Invalid or negative time value!";
                  errorOccurred = true;
                  Serial.println("ERROR: " + errorMessage);
              }
              else
              {
                  storedTimeInSeconds = timeInSeconds;
                  writeInterval = 1000;

                  EEPROM.put(THREAD, transactionThread);  
                  EEPROM.put(STORED_TIME_IN_SECONDS, storedTimeInSeconds);
                  EEPROM.put(IS_FREE, isFree);
                  EEPROM.put(SPIFFS_WRITE_INTERVAL, writeInterval);
    
                  if (!EEPROM.commit()) {
                    errorMessage = "Failed to commit data to EEPROM.";
                    errorOccurred = true;
                    Serial.println("ERROR: " + errorMessage);
                  } 
                  else 
                  {
                    if (!WriteTimeToSPIFFS(storedTimeInSeconds)) 
                    {
                       errorMessage = "Failed to write time to SPIFFS.";
                       errorOccurred = true;
                    } 
                    else 
                    {
                       //Turn on the light/relay
                       digitalWrite(RELAY_PIN, HIGH);
                    }
                  }
              }
          }
      } 
      else 
      {
          errorMessage = "Missing request body";
          errorOccurred = true;
      }
  
      // Respond to the client
      if (errorOccurred) {
          String jsonResponse = "{\"response\":\"" + errorMessage + "\"}";
          server.send(400, "application/json", jsonResponse);
      } else {
          String jsonResponse = "{\"thread\": " + String(currentThread) + "}";
          server.send(200, "application/json", jsonResponse);
          Serial.println("---START FREE LIGHT THREAD---");
      }
  });

  server.on("/api/stopfree", HTTP_POST, []() {

      Serial.println("STOP FREE LIGHT Device request received");
      
      bool errorOccurred = false;
      String errorMessage;
      int currentThread = 0;
      EEPROM.get(THREAD, currentThread);

      isFree = false;
      EEPROM.put(IS_FREE, isFree);
      EEPROM.put(STORED_TIME_IN_SECONDS, 0);
      
      if (!EEPROM.commit()) 
      {
          errorMessage = "Failed to commit data to EEPROM.";
          errorOccurred = true;
          Serial.println("ERROR: " + errorMessage);
      } 
      else 
      {
           //Turn off the light/relay
           digitalWrite(RELAY_PIN, LOW);
      }        

      if (errorOccurred) {
          String jsonResponse = "{\"response\":\"" + errorMessage + "\"}";
          server.send(400, "application/json", jsonResponse);
      } else {
          String jsonResponse = "{\"thread\": " + String(currentThread) + "}";
          server.send(200, "application/json", jsonResponse);
          Serial.println("---STOP FREE LIGHT THREAD---");
      }
  });

  server.on("/api/ondemand/heartbeat", HTTP_POST, []() {
      Serial.println("ONDEMAND HEARTBEAT request received");
      String jsonResponse = "{\"heartbeat\": \"active\"}";
      server.send(200, "application/json", jsonResponse);
  });

  server.begin();
  Serial.println("HTTP server started");
}

void ManageRelayTiming() {
    if (isOpenTime) {
        if (isOpenTime) {
            storedTimeInSeconds++;
            Serial.println("Remaining time: " + String(storedTimeInSeconds));
            if (storedTimeInSeconds % writeInterval == 0) {
                Serial.println("Writing remaining time in  SPIFFS: " + String(storedTimeInSeconds));
                WriteTimeToSPIFFS(storedTimeInSeconds);
            }
        } else {
            digitalWrite(RELAY_PIN, HIGH);
        }
    } else {
        if (!isPaused) {
            if (storedTimeInSeconds > 0) {
                if (isTesting) {
                    digitalWrite(RELAY_PIN, !digitalRead(RELAY_PIN));
                    storedTimeInSeconds--;
                    Serial.println("Remaining time: " + String(storedTimeInSeconds));
                } else {
                    if (!isLEDOn) {
                        digitalWrite(RELAY_PIN, HIGH);
                        isLEDOn = true;
                        EEPROM.put(IS_LED_ON, isLEDOn);
                        EEPROM.commit();
                    }

                    storedTimeInSeconds--;
                    Serial.println("Remaining time: " + String(storedTimeInSeconds));
                    if (storedTimeInSeconds % writeInterval == 0) {
                        Serial.println("Writing remaining time in  SPIFFS: " + String(storedTimeInSeconds));
                        WriteTimeToSPIFFS(storedTimeInSeconds);
                    }
                }
            } else {
                if (isLEDOn || isTesting) {
                    digitalWrite(RELAY_PIN, LOW);
                    isTesting = false;
                    isLEDOn = false;
                    EEPROM.put(IS_LED_ON, isLEDOn);
                    EEPROM.commit();
                }
            }
        }
    }
}

int CalculateWriteInterval(int totalTimeInSeconds) {
    const int MIN_INTERVAL = MIN_SPIFFS_WRITE_INTERVAL; 
    const int MAX_WRITES = MAX_SPIFFS_WRITES_COUNTS;

    int interval = totalTimeInSeconds / MAX_WRITES;

    if (interval < MIN_INTERVAL) {
        interval = MIN_INTERVAL;
    }

    return interval;
}

int ReadTimeFromSPIFFS() {
  File file = SPIFFS.open(TIME_FILE_PATH, "r");
  if (!file) {
    Serial.println("Failed to open time file for reading");
    return 0;
  }

  String timeString = file.readString();
  file.close();
  Serial.println("Read time from SPIFFS: " + timeString);
  return timeString.toInt();
}

bool WriteTimeToSPIFFS(int time) {
    File file = SPIFFS.open(TIME_FILE_PATH, "w");
    if (!file) {
        Serial.println("Failed to open time file for writing");
        return false;
    }

    file.println(time);
    file.close();
    Serial.println("Written time to SPIFFS: " + String(time));
    return true;
}

bool ResetEEPROMSPIFFS() {
  bool success = true;
  
  // Reset EEPROM
  for (int i = 0; i < 512; ++i) {
    EEPROM.write(i, 0);
  }
  if (!EEPROM.commit()) {
//    logMessage("Error: Failed to commit EEPROM reset.");
    success = false;
  }

  // Format SPIFFS
  if (!SPIFFS.format()) {
//    logMessage("Error: Failed to format SPIFFS.");
    success = false;
  }

  // Close SPIFFS
  SPIFFS.end();

  if (success) {
    Serial.println("EEPROM and SPIFFS reset complete");
  }

  return success;
}

void DeleteDevice(bool errorOccured, String errorMessage)
{
  WiFiClient client;
  HTTPClient http;
  bool success = true;

  if (errorOccured)
  {
    success = false;
  }

  String payload = F("{"); //Payload JSON Opening
  payload += F("\"DeviceID\":") + String(deviceId) + F(",");
  payload += F("\"Success\":\"") + String(success) + F("\",");
  payload += F("\"ErrorMessage\":\"") + String(errorMessage) + F("\"");
  payload += F("}"); //Payload JSON Closure
  Serial.println(String(payload));

  Serial.println(String(hostURL) + DEVICE_DELETE_RESPONSE_URL);
  http.begin(client, String(hostURL) + DEVICE_DELETE_RESPONSE_URL);  
  
  http.addHeader(F("Content-Type"), F("application/json"));

  int httpResponseCode = http.POST(payload);
  Serial.println(httpResponseCode);

  if (httpResponseCode >= 200 && httpResponseCode < 300) 
  {
    String response = http.getString();

    StaticJsonDocument<200> doc;
    DeserializationError error = deserializeJson(doc, response);

    if (!error) {
        bool isSuccess = doc["success"];
        if (isSuccess) {
            ForceReset(true);
        }
        
      } else {
        Serial.println(F("Failed to parse JSON response"));
        return;
      }
  }
  else 
  {
    
  }
}

void RegisterUpdateDevice(bool restartDevice)
{
  WiFiClient client;
  HTTPClient http;
  bool isInsert = false;
  http.setTimeout(50000);
  IPAddress ipAddress = WiFi.localIP();
  byte ip[4] = { ipAddress[0], ipAddress[1], ipAddress[2], ipAddress[3] };
  EEPROM.put(IP_STRING, ip);

  IPAddress retrievedIP(ip[0], ip[1], ip[2], ip[3]);
  
  String payload = F("{"); //Payload JSON Opening

  if (deviceId < 1)
  {
    isInsert = true;
    Serial.println(String(hostURL) + REGISTER_DEVICE_URL);
    http.begin(client, String(hostURL) + REGISTER_DEVICE_URL);  

    payload += F("\"SerialNumber\":\"") + String(SERIAL_NUMBER) + F("\",");
  }
  else 
  {
    Serial.println(String(hostURL) + UPDATE_DEVICE_URL);
    http.begin(client, String(hostURL) + UPDATE_DEVICE_URL);  
    
    payload += F("\"DeviceID\":") + String(deviceId) + F(",");
  }
  
  payload += F("\"IPAddress\":\"") + retrievedIP.toString() + F("\"");
  payload += F("}"); //Payload JSON Closure

  http.addHeader(F("Content-Type"), F("application/json"));

  Serial.println(String(payload));

  int httpResponseCode = http.POST(payload);
  Serial.println(httpResponseCode);

  if (httpResponseCode > 0) {
      Serial.printf("HTTP Response Code: %d\n", httpResponseCode);
  } else {
      Serial.printf("HTTP Request failed: %s\n", http.errorToString(httpResponseCode).c_str());
  }
  
  if (httpResponseCode >= 200 && httpResponseCode < 300) {
      String response = http.getString();
      
      StaticJsonDocument<200> doc;
      DeserializationError error = deserializeJson(doc, response);

      if (!error) {
        int returned_deviceId = doc["device_id"];
        int returned_defaultWatchdogInterval = doc["default_watchdog_interval"];

        EEPROM.put(DEVICE_ID, returned_deviceId); 
  
        if (watchdogIntervalMinutes < 1)
        {
            EEPROM.put(WATCHDOG_INTERVAL_MINUTES, returned_defaultWatchdogInterval);   
        }
        
        isRegistered = true; 
        EEPROM.put(IS_REGISTERED, isRegistered);
        EEPROM.commit();

        Serial.println(F("Device registered successfully: ") + response);

        CreateSPIFFSFile();
        if (restartDevice)
        {
            RestartDevice(); 
        }
        
      } else {
        Serial.println(F("Failed to parse JSON response"));
        return;
      }
    } else {
      //logMessage("ERROR: Unable to register device");
      digitalWrite(ERROR_LED_PIN, HIGH);
      //addToQueue("POST", String(hostURL) + updateDeviceURL, payload); // Queue the request if failed
      Serial.print(F("Error on sending POST: "));
      Serial.println(httpResponseCode);

      ForceReset(isInsert);
    }

    http.end();
}

bool SentOnRestartHeartbeat()
{
  WiFiClient client;
  HTTPClient http;

  http.setTimeout(20000);
  
  Serial.println(String(hostURL) + HEARTBEAT_DEVICE_URL);
  http.begin(client, String(hostURL) + HEARTBEAT_DEVICE_URL); 

  bool onRestart = true;

  String payload = F("{"); //Payload JSON Opening
  payload += F("\"DeviceID\":") + String(deviceId) + F(",");
  payload += F("\"SerialNumber\":\"") + String(SERIAL_NUMBER) + F("\",");
  payload += F("\"OnRestart\":") + String(onRestart ? "true" : "false");
  payload += F("}"); //Payload JSON Closure

  http.addHeader(F("Content-Type"), F("application/json"));
  http.addHeader("Accept", "application/json");

  Serial.println(String(payload));

  int httpResponseCode = http.POST(payload);
  Serial.println(httpResponseCode);
  http.end();
  
  if (httpResponseCode != 200)
  {
    disconnectCounter++;
    return false;
  }
  else
  {
    disconnectCounter = 0;
    return true;
  }
}

void SendRegularHeartbeat()
{
  WiFiClient client;
  HTTPClient http;
  
  Serial.println(String(hostURL) + HEARTBEAT_DEVICE_URL);
  http.begin(client, String(hostURL) + HEARTBEAT_DEVICE_URL); 

  bool onRestart = false;

  String payload = F("{"); //Payload JSON Opening
  payload += F("\"DeviceID\":") + String(deviceId) + F(",");
  payload += F("\"SerialNumber\":\"") + String(SERIAL_NUMBER) + F("\",");
  // payload += F("\"OnRestart\":\"") + String(onRestart) + F("\"");
  payload += F("\"OnRestart\":") + String(onRestart ? "true" : "false");
  payload += F("}"); //Payload JSON Closure

  http.addHeader(F("Content-Type"), F("application/json"));

  Serial.println(String(payload));

  int httpResponseCode = http.POST(payload);
  Serial.println(httpResponseCode);

  if (httpResponseCode != 200)
  {
    disconnectCounter++;
  }
  else
  {
    disconnectCounter = 0;
  }
  
  http.end();
}

void InitializeOTA()
{
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = F("sketch");
    } else { 
      type = F("filesystem");
    }
    Serial.println(F("Start updating ") + type);
  });

  ArduinoOTA.onEnd([]() {
    Serial.println(F("\nEnd"));
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println(F("Auth Failed"));
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println(F("Begin Failed"));
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println(F("Connect Failed"));
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println(F("Receive Failed"));
    } else if (error == OTA_END_ERROR) {
      Serial.println(F("End Failed"));
    }
  });

  ArduinoOTA.begin();
}

void ConfigureRelayStateOnStartUp()
{
  Serial.print("Light state on startup: ");
  if (isLEDOn)
  {
    digitalWrite(RELAY_PIN, HIGH);
    Serial.println("ON");
  }
  else {
    digitalWrite(RELAY_PIN, LOW);
    Serial.println("OFF");
  }
}

void ConnectToWiFi() {
    Serial.println("ConnectToWiFi(): Connecting to WiFi...");
    Serial.println("Device ID: " + String(deviceId));

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    WiFi.setSleepMode(WIFI_NONE_SLEEP); 

    // Set WiFiManager timeout
    wifiManager.setConfigPortalTimeout(WIFI_AP_CONFIG_PORTAL_TIMEOUT);
    wifiManager.setDebugOutput(true);

    // Attempt to connect to WiFi or start AP mode
    Serial.println("Attempting to connect to Wi-Fi...");
    digitalWrite(AP_LED_PIN, HIGH);
    
    if (!wifiManager.autoConnect(APSSID.c_str())) {
        Serial.println("Failed to connect. Entering AP mode...");
        WiFi.mode(WIFI_AP);
        WiFi.softAP(APSSID.c_str());
        Serial.println("AP mode started. IP address:");
        Serial.println(WiFi.softAPIP());
        while (true) {
            delay(1000); // Prevent infinite loop lock
        }
    } else {
        Serial.println(F("Connected to WiFi!"));
        Serial.println("WiFi SSID: " + WiFi.SSID());
        Serial.println("Signal Strength: " + String(WiFi.RSSI()));
        Serial.println("IP Address: " + WiFi.localIP().toString());

        // Set fixed hostname
        strncpy(deviceHostname, DEVICE_HOSTNAME, sizeof(deviceHostname) - 1);
        deviceHostname[sizeof(deviceHostname) - 1] = '\0';

        // Update LEDs and clear logs
        digitalWrite(LED_BUILTIN, LOW);
        digitalWrite(PROCESSING_LED_PIN, HIGH);
        digitalWrite(AP_LED_PIN, LOW);

        if (SPIFFS.exists("/logs.txt")) {
            if (SPIFFS.remove("/logs.txt")) {
                Serial.println("Log file cleared successfully.");
            } else {
                Serial.println("Failed to clear log file.");
            }
        }
    }

    Serial.println("Connected to Wi-Fi successfully!");
}

void LoadConfig() {
    // Ensure buffers are clear before reading
    memset(ipString, 0, sizeof(ipString));
    memset(deviceHostname, 0, sizeof(deviceHostname));
  
    // Read data from EEPROM
    EEPROM.get(IP_STRING, ipString);
    EEPROM.get(DEVICE_ID, deviceId);
    EEPROM.get(WATCHDOG_INTERVAL_MINUTES, watchdogIntervalMinutes);
    EEPROM.get(STORED_TIME_IN_SECONDS, storedTimeInSeconds);
    EEPROM.get(LAST_MILLIS, lastMillis);
    EEPROM.get(IS_REGISTERED, isRegistered);
    EEPROM.get(IS_PAUSED, isPaused);
    EEPROM.get(IS_FREE, isFree);
    EEPROM.get(IS_OPEN_TIME, isOpenTime);
    EEPROM.get(IS_LED_ON, isLEDOn);
    EEPROM.get(START_DATE_TIME, startDateTime);
    EEPROM.get(THREAD, thread);
    EEPROM.get(SPIFFS_WRITE_INTERVAL, writeInterval);

    // Null-terminate ipString from EEPROM
    ipString[sizeof(ipString) - 1] = '\0';

    // Use constant hostname
    strncpy(deviceHostname, DEVICE_HOSTNAME, sizeof(deviceHostname) - 1);
    deviceHostname[sizeof(deviceHostname) - 1] = '\0';

    snprintf(hostURL, sizeof(hostURL), "http://%s:%d", deviceHostname, SERVER_PORT);
    Serial.print("Host URL: ");
    Serial.println(String(hostURL));
}


void InitializeSerial() {
    Serial.begin(115200);
    while (!Serial) {
        ; // Wait for Serial to initialize
    }
    Serial.println("Serial interface initialized.");
}

void InitializeRelayPin()
{
  Serial.println("Initializing relay pin...");
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  Serial.println("Relay pin initialized!");
}

void InitializeSPIFFS()
{
  Serial.println("Initializing SPIFFS...");
  EEPROM.begin(512);
  if (!SPIFFS.begin()) {
    Serial.println(F("SPIFFS: Failed to mount file system"));
    return;
  }
  Serial.println("SPIFFS initialized!");
}

void InitializeComponents()
{
  Serial.println("Initializing Components...");
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  pinMode(AP_LED_PIN, OUTPUT);
  digitalWrite(AP_LED_PIN, LOW);
  pinMode(PROCESSING_LED_PIN, OUTPUT);  
  digitalWrite(PROCESSING_LED_PIN, LOW);
  pinMode(PUSH_BUTTON_PIN, INPUT_PULLUP);
  pinMode(AP_BUTTON_PIN, INPUT_PULLUP);
  pinMode(ERROR_LED_PIN, OUTPUT);
  digitalWrite(ERROR_LED_PIN, LOW);

  APLastButtonState = digitalRead(AP_BUTTON_PIN);
  
  Serial.println("Components initialized!");
}
