/*OFFLINE*/

#include <Arduino.h>

#define SERIAL_NUMBER "225B0-2-3"
#define OTA_PASSWORD "L1ghtN0d3@2024"
#define DEVICE_HOSTNAME "lightmaster.local"
#define SERVER_PORT 80
#define WIFI_AP_CONFIG_PORTAL_TIMEOUT_SECONDS 180 // 3 minute
#define MIN_SPIFFS_WRITE_INTERVAL 10 // Minimum interval in seconds
#define MAX_SPIFFS_WRITES_COUNTS 20 // Maximum writes for longer durations
#define MAX_DISCONNECT_COUNTS 4 // Maximum failed heartbeat before device restart
#define GRACE_PERIOD_SECONDS 3 // Added seconds every start of rated time
#define AP_BUTTON_HOLD_DURATION_MILLISECONDS 1000 //The duration the AP button must be pressed for AP to take effect

//API URLs
#define REGISTER_DEVICE_URL "/api/device/insert"
#define UPDATE_DEVICE_URL "/api/device/update"
#define STOP_DEVICE_URL "/api/device-time/end"
#define PAUSE_DEVICE_URL "/api/device-time/pause"
#define HEARTBEAT_DEVICE_URL "/api/device/heartbeat"
#define DEVICE_DELETE_RESPONSE_URL "/api/device/response/delete"


//Files
#define TIME_FILE_PATH "/time.txt"
#define ERROR_FILE_PATH "/error.txt"

//Components
#define RELAY_PIN D1
#define AP_BUTTON_PIN D2
#define EMERGENCY_BUTTON_PIN D3
#define AP_LED_PIN D4
#define ERROR_LED_PIN D7
#define PROCESSING_LED_PIN D8

// Memory assignment
#define IP_STRING 0                 // 16 bytes
#define EMERGENCYPASSKEY 17                 // 50 bytes
#define DEVICE_ID 67                // 5 bytes
#define WATCHDOG_INTERVAL_MINUTES 72 // 5 bytes
#define STORED_TIME_IN_SECONDS 77   // 5 bytes
#define LAST_MILLIS 82              // 5 bytes
#define IS_REGISTERED 87            // 1 byte
#define IS_PAUSED 88                // 1 byte (shifted down from 89)
#define IS_FREE 89                  // 1 byte (shifted down from 90)
#define IS_OPEN_TIME 90             // 1 byte (shifted down from 91)
#define IS_LED_ON 91                // 1 byte (shifted down from 92)
#define START_DATE_TIME 92          // 20 bytes (shifted down from 93, DateTime string "YYYY-MM-DD HH:MM:SS")
#define THREAD 112                  // 4 bytes (shifted down from 113, Integer)
#define SPIFFS_WRITE_INTERVAL 116   // 4 bytes (shifted down from 117, Integer)
#define IS_MANUAL_MODE 120          // 1 byte

