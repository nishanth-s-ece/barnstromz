#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_Sensor.h>

// ==========================================
// PIN DEFINITIONS (ESP32 DevKit V1)
// ==========================================
#define TFT_CS         5
#define TFT_RST        4
#define TFT_DC         2
// TFT MOSI (SDA) -> GPIO 23
// TFT SCK (CLK)  -> GPIO 18

#define MQ2_PIN       34    // Analog input (ADC1)
#define SOS_PIN       13    // Tactile push button
#define BUZZER_PIN    12    // Active buzzer
#define MITIGATION_PIN 14   // Active mitigation signal (LED/Servo/Relay)

#define LED_RED       25    // Alert LED
#define LED_YELLOW    26    // Warning LED
#define LED_GREEN     27    // Safe LED

// ==========================================
// THRESHOLDS & CONSTANTS
// ==========================================
#define GAS_WARN_THRESH   1000  // MQ-2 raw ADC warning level
#define GAS_ALARM_THRESH  1800  // MQ-2 raw ADC dangerous level
#define FALL_HIGH_G       2.8   // Spike impact (g)
#define FALL_LOW_G        0.3   // Freefall state (g)
#define ALTITUDE_DROP_MIN 0.8   // Minimum altitude drop in meters to validate fall

// ==========================================
// SYSTEM STATES
// ==========================================
enum SystemState {
  STATE_NORMAL,
  STATE_WARNING,
  STATE_EMERGENCY
};

SystemState currentState = STATE_NORMAL;
SystemState previousState = STATE_NORMAL; // Track state changes for clean redraws
String emergencyReason = "";

// Hardware Objects
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
Adafruit_MPU6050 mpu;
Adafruit_BMP280 bmp; // Shared I2C Bus (0x76)
WebServer server(80);

// Timing Variables
unsigned long lastSensorRead = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastBuzzerToggle = 0;
bool buzzerState = false;

// Telemetry Buffers
float currentG = 1.0;
int currentGas = 0;
float currentAltitude = 0.0;
float baselineAltitude = 0.0;
float currentPressure = 0.0;

// ==========================================
// WEB SERVER HTML TEMPLATE
// ==========================================
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <title>VanguardGuard Dashboard</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta http-equiv="refresh" content="3">
  <style>
    body { font-family: Arial, sans-serif; background-color: #121212; color: #FFFFFF; text-align: center; margin:0; padding:20px; }
    .card { background-color: #1E1E1E; padding: 20px; border-radius: 10px; box-shadow: 0 4px 8px rgba(0,0,0,0.5); max-width: 400px; margin: auto; }
    h1 { color: #00E676; margin-bottom: 5px; }
    h3 { color: #888; margin-top: 0; }
    .status { font-size: 22px; font-weight: bold; padding: 10px; border-radius: 5px; margin: 15px 0; }
    .NORMAL { background-color: #1B5E20; color: #A5D6A7; }
    .WARNING { background-color: #F57F17; color: #FFFDE7; }
    .EMERGENCY { background-color: #B71C1C; color: #FFCDD2; animation: blink 1s infinite; }
    .data { font-size: 16px; margin: 8px 0; text-align: left; }
    .btn { background-color: #D32F2F; color: white; border: none; padding: 12px 24px; font-size: 16px; border-radius: 5px; cursor: pointer; text-decoration: none; display: inline-block; margin-top: 15px; }
    @keyframes blink { 50% { opacity: 0.5; } }
  </style>
</head>
<body>
  <div class="card">
    <h1>VanguardGuard</h1>
    <h3>Active Safety Node</h3>
    <div class="status %STATE_CLASS%">STATUS: %STATE_TEXT%</div>
    <div class="data"><b>G-Force Vector:</b> %G_FORCE% g</div>
    <div class="data"><b>Altitude:</b> %ALTITUDE% m</div>
    <div class="data"><b>Baro Pressure:</b> %PRESSURE% hPa</div>
    <div class="data"><b>Gas ADC Level:</b> %GAS_LEVEL%</div>
    <div class="data"><b>Active Hazard:</b> %HAZARD%</div>
    <a href="/reset" class="btn">CLEAR ALARM</a>
  </div>
</body>
</html>
)rawliteral";

// ==========================================
// FUNCTION PROTOTYPES
// ==========================================
void handleRoot();
void handleReset();
void renderSplashScreen();
void updateDisplay();
void processSensors();
void updateOutputs();

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(115200);

  // Pin Configuration
  pinMode(SOS_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(MITIGATION_PIN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);

  digitalWrite(MITIGATION_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  // Initialize Display
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1); // Landscape mode
  tft.fillScreen(ST7735_BLACK);

  // Splash Screen
  renderSplashScreen();

  // Initialize I2C Bus (GPIO 21 SDA, GPIO 22 SCL)
  Wire.begin(21, 22);

  if (!mpu.begin()) {
    Serial.println("Failed to find MPU6050 chip!");
  } else {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  }

  if (!bmp.begin(0x76)) {
    Serial.println("Failed to find BMP280 chip!");
  } else {
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                   Adafruit_BMP280::SAMPLING_X2,
                   Adafruit_BMP280::SAMPLING_X16,
                   Adafruit_BMP280::FILTER_X16,
                   Adafruit_BMP280::STANDBY_MS_500);
    baselineAltitude = bmp.readAltitude(1013.25);
  }

  // Setup Wi-Fi Access Point
  WiFi.softAP("VanguardGuard-Node", "safetyfirst");

  // Web Server Routes
  server.on("/", handleRoot);
  server.on("/reset", handleReset);
  server.begin();

  delay(3000); // Read splash screen
  tft.fillScreen(ST7735_BLACK);
}

// ==========================================
// MAIN LOOP
// ==========================================
void loop() {
  server.handleClient();

  unsigned long currentMillis = millis();

  // Poll Sensors every 100ms
  if (currentMillis - lastSensorRead >= 100) {
    lastSensorRead = currentMillis;
    processSensors();
  }

  // Refresh Screen & Logic every 250ms
  if (currentMillis - lastDisplayUpdate >= 250) {
    lastDisplayUpdate = currentMillis;
    updateOutputs();
    updateDisplay();
  }
}

// ==========================================
// SENSOR PROCESSING & TRIAGE LOGIC
// ==========================================
void processSensors() {
  // Read SOS Button
  if (digitalRead(SOS_PIN) == LOW) {
    currentState = STATE_EMERGENCY;
    emergencyReason = "SOS PRESSED!";
    return;
  }

  // Read MPU6050 Accelerometer
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  float ax = a.acceleration.x / 9.81;
  float ay = a.acceleration.y / 9.81;
  float az = a.acceleration.z / 9.81;
  currentG = sqrt(ax * ax + ay * ay + az * az);

  // Read BMP280 Altitude & Pressure
  currentPressure = bmp.readPressure() / 100.0F; // hPa
  currentAltitude = bmp.readAltitude(1013.25);

  // Sensor Fusion Fall Logic (G-force spike + Altitude drop)
  float altitudeDifference = baselineAltitude - currentAltitude;
  if ((currentG > FALL_HIGH_G || currentG < FALL_LOW_G) && altitudeDifference >= ALTITUDE_DROP_MIN) {
    currentState = STATE_EMERGENCY;
    emergencyReason = "FALL DETECTED!";
    return;
  }

  // Read MQ-2 Gas Sensor
  currentGas = analogRead(MQ2_PIN);

  if (currentGas >= GAS_ALARM_THRESH) {
    currentState = STATE_EMERGENCY;
    emergencyReason = "GAS SPIKE!";
    return;
  } 
  
  if (currentGas >= GAS_WARN_THRESH && currentState != STATE_EMERGENCY) {
    currentState = STATE_WARNING;
    emergencyReason = "GAS ELEVATED";
    return;
  }

  // Return to normal if no persistent hazards exist
  if (currentState == STATE_WARNING && currentGas < GAS_WARN_THRESH) {
    currentState = STATE_NORMAL;
    emergencyReason = "NONE";
  }
}

// ==========================================
// OUTPUT ACTUATION
// ==========================================
void updateOutputs() {
  switch (currentState) {
    case STATE_NORMAL:
      digitalWrite(LED_GREEN, HIGH);
      digitalWrite(LED_YELLOW, LOW);
      digitalWrite(LED_RED, LOW);
      digitalWrite(BUZZER_PIN, LOW);
      digitalWrite(MITIGATION_PIN, LOW);
      break;

    case STATE_WARNING:
      digitalWrite(LED_GREEN, LOW);
      digitalWrite(LED_YELLOW, HIGH);
      digitalWrite(LED_RED, LOW);
      digitalWrite(MITIGATION_PIN, LOW);
      
      if (millis() - lastBuzzerToggle >= 500) {
        lastBuzzerToggle = millis();
        buzzerState = !buzzerState;
        digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
      }
      break;

    case STATE_EMERGENCY:
      digitalWrite(LED_GREEN, LOW);
      digitalWrite(LED_YELLOW, LOW);
      digitalWrite(LED_RED, HIGH);
      digitalWrite(MITIGATION_PIN, HIGH);

      if (millis() - lastBuzzerToggle >= 100) {
        lastBuzzerToggle = millis();
        buzzerState = !buzzerState;
        digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
      }
      break;
  }
}

// ==========================================
// NON-FLICKERING DISPLAY RENDERING
// ==========================================
void renderSplashScreen() {
  tft.fillScreen(ST7735_BLACK);
  tft.drawRect(2, 2, 156, 124, ST7735_GREEN);
  
  tft.setTextColor(ST7735_WHITE);
  tft.setTextSize(1);
  tft.setCursor(22, 12);
  tft.println("VANGUARDGUARD");

  tft.fillRect(10, 32, 140, 26, ST7735_GREEN);
  tft.setTextColor(ST7735_BLACK);
  tft.setTextSize(2);
  tft.setCursor(18, 38);
  tft.println("ACTIVE NODE");

  tft.setTextColor(ST7735_CYAN);
  tft.setTextSize(1);
  tft.setCursor(12, 70);
  tft.println("Industry 6.0 Active");
  tft.setCursor(24, 82);
  tft.println("Mitigation Node");

  tft.setTextColor(ST7735_YELLOW);
  tft.setCursor(30, 104);
  tft.println("System Booting...");
}

void updateDisplay() {
  // Wipe screen only if transitioning into or out of Emergency mode
  if (currentState != previousState) {
    tft.fillScreen(ST7735_BLACK);
    previousState = currentState;
  }

  if (currentState == STATE_EMERGENCY) {
    tft.fillRect(0, 0, 160, 24, ST7735_RED);
    tft.setTextColor(ST7735_WHITE, ST7735_RED);
    tft.setTextSize(2);
    tft.setCursor(10, 4);
    tft.println("EMERGENCY!");
    
    tft.setTextColor(ST7735_WHITE, ST7735_BLACK);
    tft.setTextSize(1);
    tft.setCursor(5, 32);
    tft.print("HAZARD  : ");
    tft.print(emergencyReason);
    tft.print("        ");

    tft.setCursor(5, 48);
    tft.print("G-Force : ");
    tft.print(currentG, 1);
    tft.print(" g   ");

    tft.setCursor(5, 64);
    tft.print("Altitude: ");
    tft.print(currentAltitude, 1);
    tft.print(" m   ");

    tft.setCursor(5, 80);
    tft.print("Gas ADC : ");
    tft.print(currentGas);
    tft.print("    ");

    tft.fillRect(5, 98, 150, 18, ST7735_WHITE);
    tft.setTextColor(ST7735_RED, ST7735_WHITE);
    tft.setCursor(12, 103);
    tft.println("MITIGATION ACTIVE");
  } 
  else {
    // Top Bar (Static overlay)
    tft.fillRect(0, 0, 160, 16, (currentState == STATE_WARNING) ? ST7735_YELLOW : ST7735_BLUE);
    tft.setTextColor((currentState == STATE_WARNING) ? ST7735_BLACK : ST7735_WHITE, 
                     (currentState == STATE_WARNING) ? ST7735_YELLOW : ST7735_BLUE);
    tft.setTextSize(1);
    tft.setCursor(5, 4);
    tft.println("VANGUARDGUARD NODE");

    // Overwrite values directly using background-fill colors to eliminate flicker
    tft.setTextColor(ST7735_WHITE, ST7735_BLACK);
    tft.setCursor(5, 24);
    tft.print("Status : ");
    if (currentState == STATE_NORMAL) {
      tft.setTextColor(ST7735_GREEN, ST7735_BLACK);
      tft.print("SAFE     ");
    } else {
      tft.setTextColor(ST7735_YELLOW, ST7735_BLACK);
      tft.print("WARNING  ");
    }

    tft.setTextColor(ST7735_WHITE, ST7735_BLACK);
    tft.setCursor(5, 40);
    tft.print("G-Force: ");
    tft.print(currentG, 2);
    tft.print(" g   ");

    tft.setCursor(5, 56);
    tft.print("Alt    : ");
    tft.print(currentAltitude, 1);
    tft.print(" m   ");

    tft.setCursor(5, 72);
    tft.print("Gas    : ");
    tft.print(currentGas);
    tft.print("    ");

    tft.setCursor(5, 88);
    tft.print("Hazard : ");
    tft.print(emergencyReason.length() > 0 ? emergencyReason : "NONE");
    tft.print("       ");

    tft.drawFastHLine(0, 104, 160, ST7735_WHITE);
    tft.setCursor(5, 110);
    tft.setTextColor(ST7735_CYAN, ST7735_BLACK);
    tft.println("HITECH ECE");
  }
}

// ==========================================
// WEB SERVER HANDLERS
// ==========================================
void handleRoot() {
  String page = HTML_PAGE;
  
  if (currentState == STATE_NORMAL) {
    page.replace("%STATE_CLASS%", "NORMAL");
    page.replace("%STATE_TEXT%", "SAFE");
  } else if (currentState == STATE_WARNING) {
    page.replace("%STATE_CLASS%", "WARNING");
    page.replace("%STATE_TEXT%", "WARNING");
  } else {
    page.replace("%STATE_CLASS%", "EMERGENCY");
    page.replace("%STATE_TEXT%", "CRITICAL EMERGENCY");
  }

  page.replace("%G_FORCE%", String(currentG, 2));
  page.replace("%ALTITUDE%", String(currentAltitude, 1));
  page.replace("%PRESSURE%", String(currentPressure, 1));
  page.replace("%GAS_LEVEL%", String(currentGas));
  page.replace("%HAZARD%", emergencyReason.length() > 0 ? emergencyReason : "NONE");

  server.send(200, "text/html", page);
}

void handleReset() {
  currentState = STATE_NORMAL;
  emergencyReason = "NONE";
  baselineAltitude = currentAltitude;
  server.sendHeader("Location", "/");
  server.send(303);
}
