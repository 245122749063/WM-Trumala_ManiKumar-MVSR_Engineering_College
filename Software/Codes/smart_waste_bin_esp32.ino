#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <esp_sleep.h>
#include <ESP32Servo.h>

// Ultrasonic sensor
#define TRIG_PIN 5
#define ECHO_PIN 18

// Segregation sensors / actuators
#define PIR_PIN        26   // PIR OUT
#define MOIST_PIN      15   // Potentiometer wiper (ADC)
#define SERVO_DRY_PIN  14   // Dry bin servo signal
#define SERVO_WET_PIN  27   // Wet bin servo signal


const char* ssid = "Wokwi-GUEST";
const char* password = "";

const char* mqtt_server       = "broker.hivemq.com";
const int   mqtt_port         = 1883;
const char* mqtt_topic        = "waste/bin01/level";
const char* mqtt_status_topic = "waste/bin01/status";
const char* mqtt_alert_topic  = "waste/alerts";

int binDepth = 30;          // cm
int fillLevel = 0;          // %
bool alertSent = false;
unsigned long lastReading = 0;
const unsigned long readingInterval = 5000;  // 5s for testing


WiFiClient espClient;
PubSubClient mqttClient(espClient);
WebServer server(80);

Servo servoDry;
Servo servoWet;


void reconnectMQTT();
void readSensor();
void processAlert();
void handleRoot();
void handleSegregation();


void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  pinMode(PIR_PIN, INPUT);

  servoDry.attach(SERVO_DRY_PIN);
  servoWet.attach(SERVO_WET_PIN);
  servoDry.write(0);  // lids closed
  servoWet.write(0);

  // WiFi
  Serial.println("Connecting WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  // MQTT
  mqttClient.setServer(mqtt_server, mqtt_port);
  reconnectMQTT();

  // HTTP server
  server.on("/", handleRoot);
  server.begin();

  Serial.println("Smart Waste Bin v3.2 - MQTT + Segregation Ready");
  Serial.print("Web: http://");
  Serial.println(WiFi.localIP());
}


void loop() {
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();
  server.handleClient();

  // Fill-level measurement + MQTT
  if (millis() - lastReading > readingInterval) {
    readSensor();
    processAlert();
    lastReading = millis();
  }

  // Dry / Wet segregation
  handleSegregation();
}


void reconnectMQTT() {
  if (mqttClient.connected()) return;

  Serial.print("Connecting MQTT...");
  if (mqttClient.connect("Bin01_ESP32")) {
    Serial.println("connected!");
    mqttClient.publish(mqtt_status_topic, "ONLINE");
  } else {
    Serial.print("failed, rc=");
    Serial.print(mqttClient.state());
    Serial.println(" retrying in 5s");
    delay(5000);
  }
}


void readSensor() {
  long duration;
  int distance;
  int distances[3];

  for (int i = 0; i < 3; i++) {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    duration = pulseIn(ECHO_PIN, HIGH, 30000);
    distances[i] = duration * 0.034 / 2;
    delay(50);
  }

  distance = (distances[0] + distances[1] + distances[2]) / 3;

  fillLevel = ((binDepth - distance) * 100) / binDepth;
  fillLevel = constrain(fillLevel, 0, 100);

  Serial.print("Distance: ");
  Serial.print(distance);
  Serial.print("cm, Fill Level: ");
  Serial.print(fillLevel);
  Serial.println("%");

  if (mqttClient.connected()) {
    String payload = String(fillLevel);
    mqttClient.publish(mqtt_topic, payload.c_str());
  }
}

void processAlert() {
  if (!mqttClient.connected()) return;

  if (fillLevel >= 80 && !alertSent) {
    Serial.println("ALERT: Bin 80%+ full, NEEDS_COLLECTION");
    mqttClient.publish(mqtt_alert_topic, "Bin01: NEEDS_COLLECTION");
    mqttClient.publish(mqtt_status_topic, "HIGH");
    alertSent = true;
  } else if (fillLevel < 70 && alertSent) {
    Serial.println("STATUS: Bin back to NORMAL");
    mqttClient.publish(mqtt_status_topic, "NORMAL");
    alertSent = false;
  }
}


void handleSegregation() {
  static bool binOpen = false;

  int pirState = digitalRead(PIR_PIN);

  // If no motion and bin was open → close it
  if (pirState == LOW && binOpen) {
    servoDry.write(0);
    servoWet.write(0);
    binOpen = false;
    Serial.println("Bins closed");
    return;
  }

  // If bin already open, do nothing
  if (binOpen) return;

  // Motion detected → classify once
  if (pirState == HIGH) {
    int moist = analogRead(MOIST_PIN);
    Serial.print("Object detected, moisture value: ");
    Serial.println(moist);

    int threshold = 2000;

    if (moist > threshold) {
      Serial.println("Classified: WET -> Open WET bin");
      servoWet.write(90);
    } else {
      Serial.println("Classified: DRY -> Open DRY bin");
      servoDry.write(90);
    }

    binOpen = true;
  }
}

void handleRoot() {
  String statusText;
  if (fillLevel >= 80)      statusText = "HIGH PRIORITY";
  else if (fillLevel >= 60) statusText = "MEDIUM";
  else                      statusText = "NORMAL";

  String html = "<!DOCTYPE html><html><head>";
  html += "<title>Smart Waste Bin v3.2</title>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<style>body{font-family:Arial;padding:20px;background:#f0f8ff}";
  html += "h1{color:#2c3e50}.card{padding:20px;border-radius:10px;";
  html += "background:#fff;box-shadow:0 2px 6px rgba(0,0,0,0.15)}";
  html += ".high{border-left:10px solid #e74c3c}";
  html += ".medium{border-left:10px solid #f39c12}";
  html += ".normal{border-left:10px solid #27ae60}";
  html += "</style></head><body>";

  String cls = (fillLevel>=80) ? "high" : (fillLevel>=60) ? "medium" : "normal";

  html += "<h1>🗑️ Smart Waste Bin Monitor</h1>";
  html += "<div class='card " + cls + "'>";
  html += "<p><b>Bin ID:</b> BIN_01</p>";
  html += "<p><b>Fill Level:</b> " + String(fillLevel) + "%</p>";
  html += "<p><b>Status:</b> " + statusText + "</p>";
  html += "<p><b>MQTT Level Topic:</b> waste/bin01/level</p>";
  html += "<p><b>MQTT Alert Topic:</b> waste/alerts</p>";
  html += "<p><b>Segregation:</b> PIR + Potentiometer (dry/wet)</p>";
  html += "<p><b>Device IP:</b> " + WiFi.localIP().toString() + "</p>";
  html += "</div></body></html>";

  server.send(200, "text/html", html);
}
