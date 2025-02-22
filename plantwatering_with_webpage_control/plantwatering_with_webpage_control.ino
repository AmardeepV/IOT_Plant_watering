#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "time.h"
#include <EEPROM.h>

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_PCD8544.h>

// Wi-Fi credentials
//const char *ssid = "ARTI";
//const char *password = "xxxxxx";

const char *ssid = "Magenta9448572";
const char *password = "b5n7hrj69mss";

// NTP settings
const char *ntpServer1 = "pool.ntp.org";
const char *ntpServer2 = "time.nist.gov";
const long gmtOffset_sec = 3600;
const int daylightOffset_sec = 3600;
const char *time_zone = "CET-1CEST,M3.5.0,M10.5.0/3";  // TimeZone for Europe/Austria

Adafruit_PCD8544 display = Adafruit_PCD8544(18, 23, 4, 15, 2);
int contrastValue = 60; // Default Contrast Value

// ============ Pin & Scheduling Settings ============
const int WATER_PUMP_PIN = 13;
bool waterPumpState = false;      // Current output state (true = ON, false = OFF)

// Scheduled watering settings (default)
int scheduledWday     = 1;    // 0=Sunday, 1=Monday, etc.
int scheduledHour     = 14;
int scheduledMinute   = 0;
const int scheduledSecStart = 0;
const int scheduledSecEnd   = 30;
const unsigned long wateringDuration = 8000;  // Automatic watering lasts 8 seconds

// Flags for scheduled watering and manual override
bool scheduledWateringActive = false;
unsigned long wateringStartMillis = 0;
bool manualOverride = false;      // When true, scheduled watering is disabled

// New flag to ensure one trigger per scheduled minute.
bool scheduledTriggered = false;

// ============ Web Server Setup ============
AsyncWebServer server(80);

// HTML page with placeholders for current state and scheduling.
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html>
  <head>
    <title>Web Watering</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link rel="icon" href="data:,">
    <style>
      html {font-family: Arial; text-align: center; margin: 0 auto;}
      h2 {font-size: 2.5rem;}
      h4, p {font-size: 1.8rem;}
      body {max-width: 600px; margin:0 auto; padding-bottom: 25px;}
      .switch {position: relative; display: inline-block; width: 120px; height: 68px;} 
      .switch input {display: none;}
      .slider {position: absolute; top: 0; left: 0; right: 0; bottom: 0; background-color: #ccc; border-radius: 6px;}
      .slider:before {position: absolute; content: ""; height: 52px; width: 52px; left: 8px; bottom: 8px; background-color: #fff; transition: .4s; border-radius: 3px;}
      input:checked + .slider {background-color: #b30000;}
      input:checked + .slider:before {transform: translateX(52px);}
      form {margin-top: 20px;}
      label {font-size: 1.5rem;}
      input[type=number] {width: 60px; font-size: 1.5rem;}
      select {font-size: 1.5rem; padding: 4px;}
      button {font-size: 1.5rem; padding: 8px 16px; margin-top: 10px;}
    </style>
  </head>
  <body>
    <h2>Web Watering</h2>
    <h4>Water Pump - GPIO 13</h4>
    <p>Current State: <span id="currentState">%CURRENT_STATE%</span></p>
    <label class="switch">
      <input type="checkbox" onchange="toggleCheckbox(this)" id="13" %CHECKED%>
      <span class="slider"></span>
    </label>
    <hr>
    <h4>Set Schedule</h4>
    <form action="/setSchedule" method="GET">
      <label for="day">Day:</label>
      <select name="day" id="day">
        <option value="0">Sunday</option>
        <option value="1">Monday</option>
        <option value="2">Tuesday</option>
        <option value="3">Wednesday</option>
        <option value="4">Thursday</option>
        <option value="5">Friday</option>
        <option value="6">Saturday</option>
      </select><br><br>
      <label for="hour">Hour (0-23):</label>
      <input type="number" name="hour" id="hour" min="0" max="23" value="%HOUR%"><br><br>
      <label for="minute">Minute (0-59):</label>
      <input type="number" name="minute" id="minute" min="0" max="59" value="%MINUTE%"><br><br>
      <button type="submit">Set Schedule</button>
    </form>
    <p>Current Scheduled Watering: <span id="schedule">%SCHEDULE%</span></p>
    <script>
      function toggleCheckbox(element) {
        var xhr = new XMLHttpRequest();
        xhr.open("GET", "/update?output=" + element.id + "&state=" + (element.checked ? "1" : "0"), true);
        xhr.send();
      }
      // Poll the /status endpoint every 2 seconds to update current state and schedule info.
      setInterval(function(){
        var xhr = new XMLHttpRequest();
        xhr.open("GET", "/status", true);
        xhr.onreadystatechange = function() {
          if(xhr.readyState == 4 && xhr.status == 200){
            var data = JSON.parse(xhr.responseText);
            document.getElementById("currentState").innerHTML = data.state;
            // Update the switch if needed:
            document.getElementById("13").checked = (data.state == "ON");
            document.getElementById("schedule").innerHTML = data.schedule;
          }
        }
        xhr.send();
      }, 2000);
    </script>
  </body>
</html>
)rawliteral";

// Processor function to replace placeholders in the HTML.
String processor(const String &var) {
  if(var == "CHECKED"){
    return digitalRead(WATER_PUMP_PIN) ? "checked" : "";
  } else if(var == "CURRENT_STATE"){
    return digitalRead(WATER_PUMP_PIN) ? "ON" : "OFF";
  } else if(var == "SCHEDULE"){
    String dayName;
    switch(scheduledWday){
      case 0: dayName = "Sunday"; break;
      case 1: dayName = "Monday"; break;
      case 2: dayName = "Tuesday"; break;
      case 3: dayName = "Wednesday"; break;
      case 4: dayName = "Thursday"; break;
      case 5: dayName = "Friday"; break;
      case 6: dayName = "Saturday"; break;
      default: dayName = "Unknown";
    }
    char buf[64];
    sprintf(buf, "Every %s at %02d:%02d (active %ds window)", dayName.c_str(), scheduledHour, scheduledMinute, scheduledSecEnd - scheduledSecStart);
    return String(buf);
  } else if(var == "HOUR"){
    char buf[3];
    sprintf(buf, "%02d", scheduledHour);
    return String(buf);
  } else if(var == "MINUTE"){
    char buf[3];
    sprintf(buf, "%02d", scheduledMinute);
    return String(buf);
  }
  return String();
}

// ============ EEPROM Functions ============
void saveSchedule() {
  EEPROM.write(0, scheduledWday);
  EEPROM.write(1, scheduledHour);
  EEPROM.write(2, scheduledMinute);
  EEPROM.commit();  // IMPORTANT: Saves data permanently!
  Serial.println("Schedule saved to EEPROM.");
}

void loadSchedule() {
  scheduledWday = EEPROM.read(0);
  scheduledHour = EEPROM.read(1);
  scheduledMinute = EEPROM.read(2);
  Serial.printf("Loaded schedule from EEPROM: Day=%d, Hour=%d, Minute=%d\n", scheduledWday, scheduledHour, scheduledMinute);
}

// ============ Automatic Watering Function ============
void checkWateringSchedule() {
  // Only run scheduled watering if manual override is NOT active.
  if (manualOverride) return;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Time not available yet.");
    return;
  }

  // For debugging, print the current time.
  char timeStr[64];
  strftime(timeStr, sizeof(timeStr), "%A, %B %d %Y %H:%M:%S", &timeinfo);
  Serial.println(timeStr);

  // Check if current time falls within the scheduled watering window.
  if (!scheduledWateringActive &&
      timeinfo.tm_wday == scheduledWday &&
      timeinfo.tm_hour == scheduledHour &&
      timeinfo.tm_min == scheduledMinute &&
      timeinfo.tm_sec >= scheduledSecStart &&
      timeinfo.tm_sec <= scheduledSecEnd &&
      !scheduledTriggered) {
    Serial.println("Scheduled watering triggered.");
    scheduledWateringActive = true;
    scheduledTriggered = true;  // Prevent re-triggering in the same minute.
    wateringStartMillis = millis();
    digitalWrite(WATER_PUMP_PIN, HIGH);
    waterPumpState = true;
  }
  
  // End scheduled watering if duration has passed.
  if (scheduledWateringActive && (millis() - wateringStartMillis >= wateringDuration)) {
    Serial.println("Scheduled watering ended.");
    scheduledWateringActive = false;
    digitalWrite(WATER_PUMP_PIN, LOW);
    waterPumpState = false;
  }

  // Reset the trigger flag once the scheduled minute has passed.
  if (timeinfo.tm_min != scheduledMinute) {
    scheduledTriggered = false;
  }
}

// ============ Setup ============
void setup(){
  Serial.begin(115200);
  display.begin();
  display.setContrast(contrastValue);
  display.clearDisplay();

  pinMode(WATER_PUMP_PIN, OUTPUT);
  digitalWrite(WATER_PUMP_PIN, LOW);
  waterPumpState = false;
  manualOverride = false;
  scheduledTriggered = false;

  // Initialize EEPROM
  EEPROM.begin(10);  // Set size (10 bytes for schedule data)

  // Load schedule from EEPROM
  loadSchedule();

  // Connect to WiFi
  Serial.printf("Connecting to %s\n", ssid);
  WiFi.begin(ssid, password);
  while(WiFi.status() != WL_CONNECTED){
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("Connected to WiFi");
  Serial.println(WiFi.localIP());

  display.setCursor(15,0);
  display.setTextSize(1);
  display.println("Webserver");
  display.setTextSize(1);
  display.setTextColor(BLACK);
  display.setCursor(10,10);
  display.println(WiFi.localIP());
  display.display();

  // Configure NTP (this will update the system time)
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);
  // Optionally, use: configTzTime(time_zone, ntpServer1, ntpServer2);

  // Web server endpoint for root page.
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", index_html, processor);
  });

  // Endpoint to update GPIO state from web interface (manual toggle).
  server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){
    if(request->hasParam("output") && request->hasParam("state")){
      int output = request->getParam("output")->value().toInt();
      int state = request->getParam("state")->value().toInt();
      if(output == WATER_PUMP_PIN){
        digitalWrite(output, state);
        waterPumpState = state;
        // When user toggles manually, enable manual override.
        manualOverride = (state == 1);
        Serial.printf("Manual override: GPIO %d set to %d\n", output, state);
      }
    }
    request->send(200, "text/plain", "OK");
  });

  // Endpoint to update the watering schedule via the web page.
  server.on("/setSchedule", HTTP_GET, [](AsyncWebServerRequest *request){
    if(request->hasParam("day") && request->hasParam("hour") && request->hasParam("minute")){
      scheduledWday = request->getParam("day")->value().toInt();
      scheduledHour = request->getParam("hour")->value().toInt();
      scheduledMinute = request->getParam("minute")->value().toInt();
      Serial.printf("New schedule set: day=%d, hour=%d, minute=%d\n", scheduledWday, scheduledHour, scheduledMinute);
      
      saveSchedule();  // Save to EEPROM
    }
    // Redirect back to root page.
    request->redirect("/");
  });

  // Endpoint to return current status (as JSON) for AJAX polling.
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "{";
    json += "\"state\":\"";
    json += (digitalRead(WATER_PUMP_PIN) ? "ON" : "OFF");
    json += "\",";
    json += "\"schedule\":\"";
    String dayName;
    switch(scheduledWday){
      case 0: dayName = "Sunday"; break;
      case 1: dayName = "Monday"; break;
      case 2: dayName = "Tuesday"; break;
      case 3: dayName = "Wednesday"; break;
      case 4: dayName = "Thursday"; break;
      case 5: dayName = "Friday"; break;
      case 6: dayName = "Saturday"; break;
      default: dayName = "Unknown";
    }
    char buf[64];
    sprintf(buf, "Every %s at %02d:%02d", dayName.c_str(), scheduledHour, scheduledMinute);
    json += buf;
    json += "\"}";
    request->send(200, "application/json", json);
  });

  // Start the web server.
  server.begin();
}

// ============ Loop ============
void loop(){
  // Check the scheduled watering every second.
  checkWateringSchedule();
  delay(1000);
}
