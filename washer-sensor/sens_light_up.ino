#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <MPU6050_light.h>

// -------------------------------------------------
// FILL THESE IN before flashing
// -------------------------------------------------
const char* SSID      = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* HA_HOST   = "http://192.168.X.X:8123";  // Home Assistant IP

const char* WEBHOOK_STARTED = "/api/webhook/washer_started";
const char* WEBHOOK_DONE    = "/api/webhook/washer_done";
// -------------------------------------------------

MPU6050 mpu(Wire);

// -----------------------------------------------
// ADJUST THIS VALUE to change shake sensitivity
// Higher number = needs a harder shake to trigger
// Lower number  = triggers from a gentler shake
// Start at 0.3 and tune from there
// -----------------------------------------------
const float THRESHOLD = 0.3;

// At 50ms per loop tick:
const int SHAKE_CONFIRM_TICKS = 100;   // 5s of shaking  → WASHING
const int QUIET_CONFIRM_TICKS = 1200;  // 60s of quiet   → DONE

enum MachineState { IDLE, WASHING };
MachineState state = IDLE;
int shakingFor = 0;
int quietFor   = 0;

float prevX = 0, prevY = 0, prevZ = 0;

void sendWebhook(const char* path) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = String(HA_HOST) + path;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST("{}");
  Serial.print("Webhook sent: ");
  Serial.print(url);
  Serial.print(" -> HTTP ");
  Serial.println(code);
  http.end();
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  delay(3000);

  Serial.println("Connecting to MPU6050...");

  byte status = mpu.begin();
  if (status != 0) {
    Serial.println("MPU6050 not found — check wiring!");
    while (1);
  }

  Serial.println("Found! Calibrating — keep the sensor still...");
  mpu.calcOffsets();
  Serial.println("Done. Reading accelerometer...");
  Serial.print("Shake threshold set to: ");
  Serial.println(THRESHOLD);
  Serial.println("-----------------------------------");

  Serial.print("Connecting to WiFi");
  WiFi.begin(SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.print("\nWiFi connected — IP: ");
  Serial.println(WiFi.localIP());
  Serial.println("-----------------------------------");

  delay(1000);
}

void loop() {
  mpu.update();

  float ax = mpu.getAccX();
  float ay = mpu.getAccY();
  float az = mpu.getAccZ();

  // how much each axis changed since last reading
  float dx = abs(ax - prevX);
  float dy = abs(ay - prevY);
  float dz = abs(az - prevZ);

  // is any axis changing faster than the threshold?
  bool shaking = (dx > THRESHOLD || dy > THRESHOLD || dz > THRESHOLD);

  if (shaking) {
    shakingFor++;
    quietFor = 0;
  } else {
    quietFor++;
    shakingFor = 0;
  }

  if (state == IDLE && shakingFor >= SHAKE_CONFIRM_TICKS) {
    state = WASHING;
    shakingFor = 0;
    Serial.println(">>> Washer STARTED — sending webhook");
    sendWebhook(WEBHOOK_STARTED);
  }

  if (state == WASHING && quietFor >= QUIET_CONFIRM_TICKS) {
    state = IDLE;
    quietFor = 0;
    Serial.println(">>> Washer DONE — sending webhook");
    sendWebhook(WEBHOOK_DONE);
  }

  // print readouts to Serial Monitor
  Serial.print("X: ");
  Serial.print(ax, 2);
  Serial.print("g  Y: ");
  Serial.print(ay, 2);
  Serial.print("g  Z: ");
  Serial.print(az, 2);
  Serial.print("g  |  Change — dX: ");
  Serial.print(dx, 2);
  Serial.print("  dY: ");
  Serial.print(dy, 2);
  Serial.print("  dZ: ");
  Serial.print(dz, 2);
  Serial.print("  |  Shaking: ");
  Serial.print(shaking ? "yes" : "no");
  Serial.print("  |  State: ");
  Serial.println(state == IDLE ? "IDLE" : "WASHING");

  prevX = ax;
  prevY = ay;
  prevZ = az;

  delay(50);
}
