#include <WiFi.h>
#include <FirebaseESP32.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <time.h>
#include <vector>
#include <algorithm>
#include <TinyGPS++.h>
#include <SoftwareSerial.h>
// WiFi credentials
#define WIFI_SSID "narzo"
#define WIFI_PASSWORD "x4p9rvij"

// Firebase credentials
#define FIREBASE_HOST "ecspro-268aa-default-rtdb.firebaseio.com"
#define FIREBASE_AUTH "JvXU6zjfGmSZSuthheCINQCV6k"
// GPS Settings
#define GPS_RX 16
#define GPS_TX 17
TinyGPSPlus gps;
SoftwareSerial gpsSerial(GPS_RX, GPS_TX);

// OLED display settings
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// MPU6050 sensor
Adafruit_MPU6050 mpu;

// Firebase objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
// Buzzer settings
const int BUZZER_PIN = 5;


const unsigned long REMINDER_CHECK_INTERVAL = 60000; // Check for reminders every minute
const unsigned long DATABASE_CHECK_INTERVAL = 60000; // Check database every 5 minutes
unsigned long lastReminderCheck = 0;
unsigned long lastDatabaseCheck = 0;
std::vector<std::pair<String, String>> reminders; // Store time and message pairs
size_t currentReminderIndex = 0;

// Fall detection variables
const float FALL_THRESHOLD = 50.0;  // Adjust this threshold as necessary
const int FALL_DURATION = 2000;      // 2 seconds for fall confirmation
bool fallDetected = false;
bool fallReported = false;
unsigned long fallStartTime = 0;

// Define the user IDs
const String FALL_USER_ID = "u0ihwAGudWXYo3yQoTEdNHUkgp82";
const String REMINDER_USER_ID = "u0ihwAGudWXYo3yQoTEdNHUkgp82";

// Reminder variables
String reminderMessage = "";
time_t reminderTime = 0;

// NTP Settings
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800;    // GMT+5:30 for India
const int daylightOffset_sec = 0;    // India doesn't use daylight saving time

// Function prototypes
bool syncTime();
void displayTime();
void updateFirebase(bool fallStatus);
void fetchReminderData();
time_t convertToUnixTime(String timeStr);
void activateReminder();
void displayMessage(String message);
void updateGPSLocation();
void setup() {
    Serial.begin(115200);
    Wire.begin();

    pinMode(BUZZER_PIN, OUTPUT);

    // Initialize OLED display
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println(F("SSD1306 allocation failed"));
        for (;;);
    }
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(F("Initializing..."));
    display.display();

    // Initialize MPU6050
    if (!mpu.begin()) {
        Serial.println("Failed to find MPU6050 chip");
        while (1) {
            delay(10);
        }
    }

    // Connect to Wi-Fi
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    display.println(F("Connecting to WiFi..."));
    display.display();
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
    }
    display.println(F("WiFi Connected!"));
    display.display();
  // Initialize GPS
    gpsSerial.begin(9600);

    // Sync time with NTP server
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    if (!syncTime()) {
        Serial.println("Failed to sync time. Restarting...");
        ESP.restart();
    }

    // Configure Firebase
    config.host = FIREBASE_HOST;
    config.signer.tokens.legacy_token = FIREBASE_AUTH;
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);

    // Fetch initial reminder data
    fetchReminderData();

}

void loop() {
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);

     // Calculate total acceleration for fall detection
    float acceleration = sqrt(pow(a.acceleration.x, 2) + 
                              pow(a.acceleration.y, 2) + 
                              pow(a.acceleration.z, 2));

    // Fall detection logic
    if (acceleration > FALL_THRESHOLD) {
        if (!fallDetected && !fallReported) {
            fallDetected = true;
            fallStartTime = millis();
            Serial.println("Potential fall detected!");
            displayMessage("Potential fall detected!");
        }
    } else {
        // Reset fall detection if acceleration drops below threshold
        fallDetected = false;
    }

    // Confirm the fall if acceleration stays above the threshold for FALL_DURATION
    if (fallDetected && !fallReported && (millis() - fallStartTime > FALL_DURATION)) {
        Serial.println("Fall confirmed!");
        displayMessage("Fall confirmed!");
        updateFirebase(true);
        fallReported = true;
    }
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    // Update GPS location
    updateGPSLocation();

    // Check for reminders
    if (millis() - lastReminderCheck >= REMINDER_CHECK_INTERVAL) {
        checkAndActivateReminder(timeinfo);
        lastReminderCheck = millis();
    }

    // Check for database updates
    if (millis() - lastDatabaseCheck >= DATABASE_CHECK_INTERVAL) {
        fetchReminderData();
        lastDatabaseCheck = millis();
    }

    delay(100);  // Slight delay for smoother operation
}

bool syncTime() {
    int retry = 0;
    const int maxRetries = 5;
    while (time(nullptr) < 1000000000 && retry < maxRetries) {
        Serial.println("Waiting for time sync...");
        delay(1000);
        retry++;
    }
    if (retry >= maxRetries) {
        return false;
    }
    displayTime(); // Display the synced time
    return true;
}
void updateGPSLocation() {
    while (gpsSerial.available() > 0) {
        if (gps.encode(gpsSerial.read())) {
            if (gps.location.isValid()) {
                float latitude = gps.location.lat();
                float longitude = gps.location.lng();

                // Update Firebase with new GPS coordinates
                String path = "/users/" + FALL_USER_ID + "/location";
                FirebaseJson json;
                json.set("latitude", String(latitude, 6));
                json.set("longitude", String(longitude, 6));

                if (Firebase.setJSON(fbdo, path, json)) {
                    Serial.println("GPS location updated in Firebase");
                    displayMessage("GPS Updated");
                } else {
                    Serial.println("Failed to update GPS location");
                    Serial.println("Reason: " + fbdo.errorReason());
                    displayMessage("GPS Update Failed");
                }
            }
        }
    }

    // If no GPS data is received for 5 seconds, display an error
    if (millis() > 5000 && gps.charsProcessed() < 10) {
        Serial.println("No GPS detected");
    }
}
void displayTime() {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char timeString[20];
    strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &timeinfo);
    Serial.println(timeString);
    displayMessage(timeString);
}

void updateFirebase(bool fallStatus) {
    if (fallStatus) {  // Only update Firebase if a fall is detected
        String path = "/users/" + FALL_USER_ID + "/fallDetected";
        if (Firebase.setBool(fbdo, path, true)) {
            Serial.println("Fall detected and updated in Firebase");
        } else {
            Serial.println("Failed to update fall status");
            Serial.println("Reason: " + fbdo.errorReason());
        }
    }
}
void fetchReminderData() {
    String path = "/users/" + REMINDER_USER_ID + "/reminders";

    if (Firebase.getJSON(fbdo, path)) {
        FirebaseJson &json = fbdo.jsonObject();
        FirebaseJsonData result;

        std::vector<std::pair<String, String>> newReminders;

        size_t count = json.iteratorBegin();
        String key, value;
        int type = 0;
        for (size_t i = 0; i < count; i++) {
            json.iteratorGet(i, type, key, value);
            if (type == FirebaseJson::JSON_OBJECT) {
                FirebaseJson reminderJson;
                reminderJson.setJsonData(value);

                String timeStr, message;
                reminderJson.get(result, "time");
                if (result.success) {
                    timeStr = result.stringValue.substring(11, 16); // Extract HH:MM
                }
                reminderJson.get(result, "message");
                if (result.success) {
                    message = result.stringValue;
                }

                if (!timeStr.isEmpty() && !message.isEmpty()) {
                    newReminders.push_back(std::make_pair(timeStr, message));
                }
            }
        }
        json.iteratorEnd();

        // Sort new reminders by time
        std::sort(newReminders.begin(), newReminders.end());

        // Check if there are any changes
        if (newReminders != reminders) {
            reminders = newReminders;
            updateNextReminder();
        }

        if (reminders.empty()) {
            Serial.println("No reminders found");
            displayMessage("No reminders found");
        }
    } else {
        Serial.println("Failed to fetch reminders");
        Serial.println("Reason: " + fbdo.errorReason());
        displayMessage("Failed to fetch reminders");
    }
}
void updateNextReminder() {
    if (reminders.empty()) {
        return;
    }

    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char currentTimeStr[6];
    strftime(currentTimeStr, sizeof(currentTimeStr), "%H:%M", &timeinfo);
    String currentTime = String(currentTimeStr);

    // Find the next reminder
    for (size_t i = 0; i < reminders.size(); i++) {
        if (reminders[i].first > currentTime) {
            currentReminderIndex = i;
            break;
        }
    }
    // If all reminders are before current time, set to first reminder for next day
    if (reminders[currentReminderIndex].first <= currentTime) {
        currentReminderIndex = 0;
    }

    String nextReminderTime = reminders[currentReminderIndex].first;
    Serial.println("Next reminder at: " + nextReminderTime);
    displayMessage("Next reminder at: " + nextReminderTime);
}

time_t convertToUnixTime(String timeStr) {
    // Assuming the time string is in format: "YYYY-MM-DDTHH:MM:SS"
    int year = timeStr.substring(0, 4).toInt();
    int month = timeStr.substring(5, 7).toInt();
    int day = timeStr.substring(8, 10).toInt();
    int hour = timeStr.substring(11, 13).toInt();
    int minute = timeStr.substring(14, 16).toInt();
    int second = 0; // Default to 0 seconds

    struct tm timeinfo = {0};
    timeinfo.tm_year = year - 1900; // Years since 1900
    timeinfo.tm_mon = month - 1;    // Months since January (0-11)
    timeinfo.tm_mday = day;
    timeinfo.tm_hour = hour;
    timeinfo.tm_min = minute;
    timeinfo.tm_sec = second;

    time_t timestamp = mktime(&timeinfo);
    return timestamp;
}

void activateReminder(const String& message) {
    Serial.println("Reminder activated!");
    Serial.println("Message: " + message);

    // Display the reminder message on the OLED
    displayMessage(message);

    // Activate buzzer continuously for 10 seconds at full volume
    unsigned long startTime = millis();
    while (millis() - startTime < 10000) {  // 10 seconds duration
        digitalWrite(BUZZER_PIN, HIGH);  // Turn the buzzer ON
        delay(100);
        digitalWrite(BUZZER_PIN, LOW);   // Turn the buzzer OFF briefly for 100ms to make a pulsing sound
        delay(100);
    }
    
    digitalWrite(BUZZER_PIN, LOW);  // Turn off the buzzer completely after 10 seconds
}
void updateNextReminderDisplay() {
    if (!reminders.empty()) {
        String nextReminderTime = reminders[currentReminderIndex].first;
        Serial.println("Next reminder at: " + nextReminderTime);
        displayMessage("Next reminder at: " + nextReminderTime);
    }
}
void checkAndActivateReminder(struct tm& currentTime) {
    if (reminders.empty()) {
        return;
    }

    char currentTimeStr[6];
    strftime(currentTimeStr, sizeof(currentTimeStr), "%H:%M", &currentTime);
    String currentTimeString = String(currentTimeStr);

    if (currentTimeString == reminders[currentReminderIndex].first) {
        activateReminder(reminders[currentReminderIndex].second);
        currentReminderIndex = (currentReminderIndex + 1) % reminders.size();
        updateNextReminder();
    }
}
void displayMessage(String message) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(message);
    display.display();
}

