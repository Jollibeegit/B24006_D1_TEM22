#include <ESP8266WiFi.h>
#include <ModbusMaster.h>
#include <SoftwareSerial.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>

#define RESET_BUTTON_PIN D3  // 리셋 버튼 핀
#define PIN_OUT D4           // Modbus TX/RX 전환 핀
#define LED_PIN D4           // 내장 LED (GPIO2)

WiFiManager wifiManager;
unsigned long buttonPressTime = 0;
bool buttonPressed = false;
unsigned long lastConnectionAttempt = 0;
const unsigned long retryInterval = 300000;  // 5분

unsigned long previousMillis = 0;
const unsigned long interval = 300000;  // 5분

#define NUM_SLAVES 22

ModbusMaster nodes[NUM_SLAVES];
SoftwareSerial modbusSerial(D1, D2); // D1: RX, D2: TX

String pco = "B24006"; // 업체 코드
const char* server = "mndsystem.co.kr";

// 🔄 LED 하트비트 변수
unsigned long lastBlinkTime = 0;
const unsigned long blinkInterval = 10000;  // 20초
bool ledBlinking = false;

void preTransmission() { digitalWrite(PIN_OUT, 1); }
void postTransmission() { digitalWrite(PIN_OUT, 0); }

int16_t getSignedValue(uint16_t value) {
  return (value > 32767) ? value - 65536 : value;
}


void checkForUpdate() {
  WiFiClient client;
  t_httpUpdate_return ret = ESPhttpUpdate.update(client, "https://b24006-d1-tem22.onrender.com/firmware.bin");

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("❌ Update failed. Error (%d): %s\n", 
        ESPhttpUpdate.getLastError(), 
        ESPhttpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("🔄 No updates.");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("✅ Update successful!");
      break;
  }
}


void setup() {
  pinMode(PIN_OUT, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // LED OFF

  Serial.begin(9600);  
  modbusSerial.begin(9600);

  for (int i = 0; i < NUM_SLAVES; i++) {
    nodes[i].begin(i + 1, modbusSerial);
    nodes[i].preTransmission(preTransmission);
    nodes[i].postTransmission(postTransmission);
  }

  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

  if (WiFi.SSID() != "") {
    Serial.print("Connecting to Wi-Fi: ");
    Serial.println(WiFi.SSID());

    WiFi.begin();
    lastConnectionAttempt = millis();

    if (WiFi.waitForConnectResult() == WL_CONNECTED) {
      Serial.println("Wi-Fi Connected!");
      Serial.print("Local IP: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println("Wi-Fi Connection Failed");
    }
  } else {
    Serial.println("No saved Wi-Fi credentials found");
  }
  ArduinoOTA.begin();
  ESP.wdtDisable();
  ESP.wdtEnable(WDTO_8S);

}

void loop() {
  
  ESP.wdtFeed();
  checkForUpdate();

  // ✅ 1분마다 LED 하트비트 깜빡
  if (millis() - lastBlinkTime >= blinkInterval && !ledBlinking) {
    digitalWrite(LED_PIN, LOW);  // LED ON
    ledBlinking = true;
    lastBlinkTime = millis();
  }
  if (ledBlinking && millis() - lastBlinkTime >= 200) {
    digitalWrite(LED_PIN, HIGH);  // LED OFF
    ledBlinking = false;
  }

  // 📶 WiFi 재접속
  if (WiFi.status() != WL_CONNECTED && millis() - lastConnectionAttempt >= retryInterval) {
    Serial.println("Reconnecting Wi-Fi...");
    WiFi.disconnect();
    delay(1000);
    WiFi.reconnect();
    lastConnectionAttempt = millis();
  }

  // 🔘 리셋 버튼 처리
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    if (!buttonPressed) {
      buttonPressed = true;
      buttonPressTime = millis();
    }
    if (buttonPressed && (millis() - buttonPressTime >= 5000)) {
      Serial.println("Button held for 5s, entering AP mode...");
      WiFi.disconnect();
      delay(1000);
      wifiManager.startConfigPortal("MNDSYSTEM", "123456789");
      Serial.println("AP mode started.");
      buttonPressed = false;
    }
  } else {
    buttonPressed = false;
  }

  // 📡 Modbus → 서버 전송
  if (WiFi.status() == WL_CONNECTED) {
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= interval) {
      previousMillis = currentMillis;

      for (int i = 0; i < NUM_SLAVES; i++) {
        char slaveID[6];
        sprintf(slaveID, "A%03d", i + 1);
        String postData = "pco=" + pco + "&mac=" + String(slaveID) + "&tem=";

        int16_t rawValue;
        uint8_t result = nodes[i].readInputRegisters(0x03E8, 1);  // 주소 1001

        if (result == nodes[i].ku8MBSuccess) {
          rawValue = getSignedValue(nodes[i].getResponseBuffer(0));
          float temperature = rawValue / 10.0;
          postData += String(temperature, 1);

          Serial.print("Slave ");
          Serial.print(i + 1);
          Serial.print(" (ID: ");
          Serial.print(slaveID);
          Serial.print(") Temperature: ");
          Serial.println(temperature, 1);
        } else {
          Serial.print("❌ Modbus 실패! Slave ");
          Serial.print(i + 1);
          Serial.print(" (ID: ");
          Serial.print(slaveID);
          Serial.print(") 오류 코드: ");
          Serial.println(result);
          continue;
        }

        WiFiClient client;
        if (client.connect(server, 80)) {
          client.println("POST /2024/tem/save_data.php HTTP/1.1");
          client.println("Host: mndsystem.co.kr");
          client.println("Content-Type: application/x-www-form-urlencoded");
          client.print("Content-Length: ");
          client.println(postData.length());
          client.println();
          client.print(postData);

          Serial.println("📡 Data sent: " + postData);

          while (client.available()) {
            String response = client.readString();
            Serial.println("✅ Server Response: " + response);
          }
          client.stop();
        } else {
          Serial.println("❌ Failed to connect to server.");
        }

        delay(200);
      }
    }
  }
  ArduinoOTA.handle();
}
