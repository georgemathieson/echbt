#include "Arduino.h"
#include "heltec.h"
#include "BLEDevice.h"
#include "icons.h"
#include "device.h"
#include "power.h"

static boolean connected = false;

static BLERemoteCharacteristic* writeCharacteristic;
static BLERemoteCharacteristic* sensorCharacteristic;
static BLEAdvertisedDevice* device;
static BLEClient* client;
static BLEScan* scanner;

static int cadence = 0;
static int resistance = 0;
static int mph = 0;
static int power = 0;
static unsigned long runtime = 0;
static unsigned long last_millis = 0;

#define debug 1
#define maxResistance 32

// Called when device sends update notification
static void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* data, size_t length, bool isNotify) {
  switch (data[1]) {
    // Cadence notification
    case 0xD1:
      //runtime = int((data[7] << 8) + data[8]); // This runtime has massive drift
      cadence = int((data[9] << 8) + data[10]);
      power = getPower(cadence, resistance);
      mph = getMilesPerHour(cadence);
      break;
    // Resistance notification
    case 0xD2:
      resistance = int(data[3]);
      power = getPower(cadence, resistance);
      break;
  }

  if (debug) {
    Serial.print("CALLBACK(");
    Serial.print(pBLERemoteCharacteristic->getUUID().toString().c_str());
    Serial.print(":");
    Serial.print(length);
    Serial.print("):");
    for (int x = 0; x < length; x++) {
      if (data[x] < 16) {
        Serial.print("0");
      }
      Serial.print(data[x], HEX);
    }

    Serial.println();
  }
}

// Called on connect or disconnect
class ClientCallback : public BLEClientCallbacks {
    void onConnect(BLEClient* pclient) {
      digitalWrite(LED, LOW);
      Serial.println("Connected!");
    }
    void onDisconnect(BLEClient* pclient) {
      connected = false;
      delete(client);
      client = nullptr;
      digitalWrite(LED, HIGH);
      Serial.println("Disconnected!");
    }
};

class AdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
      Serial.print("BLE Advertised Device found: ");
      Serial.println(advertisedDevice.toString().c_str());
      if (advertisedDevice.getName().size() > 0) {
        BLEAdvertisedDevice * d = new BLEAdvertisedDevice;
        *d = advertisedDevice;
        addDevice(d);
      }
    }
};

void updateDisplay() {
  Heltec.display->clear();

  // Runtime
  Heltec.display->setFont(ArialMT_Plain_24);
  char buf[5];
  const int minutes = int(runtime / 60000);
  itoa(minutes, buf, 10);
  Heltec.display->setTextAlignment(TEXT_ALIGN_RIGHT);
  Heltec.display->drawString(48, 0, buf);
  Heltec.display->setTextAlignment(TEXT_ALIGN_LEFT);
  Heltec.display->drawXbm(0, 4, clock_icon_width, clock_icon_height, clock_icon);
  Heltec.display->drawString(48, 0, ":");
  const int seconds = int(runtime % 60000) / 1000;
  if (seconds < 10) {
    buf[0] = '0';
    itoa(seconds, &buf[1], 10);
  } else {
    itoa(seconds, buf, 10);
  }
  Heltec.display->drawString(54, 0, buf);

  // Cadence
  Heltec.display->drawXbm(0, 26, cadence_icon_width, cadence_icon_height, cadence_icon);
  itoa(cadence, buf, 10);
  Heltec.display->drawString(22, 22, buf);

  // Power
  Heltec.display->drawXbm(66, 26, power_icon_width, power_icon_height, power_icon);
  itoa(power, buf, 10);
  Heltec.display->drawString(86, 22, buf);

  // Resistance
  Heltec.display->setTextAlignment(TEXT_ALIGN_CENTER);
  itoa(getPeletonResistance(resistance), buf, 10);
  Heltec.display->drawString(116, 42, buf);
  Heltec.display->drawXbm(0, 52, resistance_icon_width, resistance_icon_height, resistance_icon);
  Heltec.display->drawProgressBar(23, 49, 78, 14, uint8_t((100 * resistance) / maxResistance));

  // MPH
  itoa(mph, buf, 10);
  Heltec.display->drawString(116, 0, buf);

  Heltec.display->display();
}

bool connectToServer() {
  Serial.print("Connecting to ");
  Serial.println(device->getName().c_str());

  Heltec.display->clear();
  Heltec.display->setLogBuffer(10, 50);
  Heltec.display->setFont(ArialMT_Plain_10);
  Heltec.display->println("Connecting to:");
  Heltec.display->println(device->getName().c_str());
  Heltec.display->drawLogBuffer(0, 0);
  Heltec.display->display();

  client = BLEDevice::createClient();
  client->setClientCallbacks(new ClientCallback());
  client->connect(device);

  // Sometimes it immediately disconnects - client will be null if so
  delay(200);
  if (client == nullptr) {
    return false;
  }

  BLERemoteService* remoteService = client->getService(connectUUID);
  if (remoteService == nullptr) {
    Serial.print("Failed to find service UUID: ");
    Serial.println(connectUUID.toString().c_str());
    client->disconnect();
    return false;
  }
  Serial.println("Found device.");

  // Look for the sensor
  sensorCharacteristic = remoteService->getCharacteristic(sensorUUID);
  if (sensorCharacteristic == nullptr) {
    Serial.print("Failed to find sensor characteristic UUID: ");
    Serial.println(sensorUUID.toString().c_str());
    client->disconnect();
    return false;
  }
  sensorCharacteristic->registerForNotify(notifyCallback);
  Serial.println("Enabled sensor notifications.");

  // Look for the write service
  writeCharacteristic = remoteService->getCharacteristic(writeUUID);
  if (writeCharacteristic == nullptr) {
    Serial.print("Failed to find write characteristic UUID: ");
    Serial.println(writeUUID.toString().c_str());
    client->disconnect();
    return false;
  }
  // Enable device notifications
  byte message[] = {0xF0, 0xB0, 0x01, 0x01, 0xA2};
  writeCharacteristic->writeValue(message, 5);
  Serial.println("Activated status callbacks.");

  return true;
}

void setup() {
  Serial.begin(115200);
  Serial.flush();
  delay(50);

  Heltec.display->init();

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  pinMode(KEY_BUILTIN, OUTPUT);
  digitalWrite(KEY_BUILTIN, HIGH);

  BLEDevice::init("");
  scanner = BLEDevice::getScan();
  scanner->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks());
  scanner->setInterval(1349);
  scanner->setWindow(449);
  scanner->setActiveScan(true);
}

void loop() {
  // Start scan
  if (!connected) {
    Serial.println("Start Scan!");
    Heltec.display->clear();
    Heltec.display->drawXbm(64, 0, mountain_icon_width, mountain_icon_height, mountain_icon);
    Heltec.display->setLogBuffer(10, 50);
    Heltec.display->setFont(ArialMT_Plain_10);
    Heltec.display->println("Starting Scan..");
    Heltec.display->drawLogBuffer(0, 0);
    Heltec.display->display();
    scanner->start(6, false); // Scan for 5 seconds
    BLEDevice::getScan()->stop();

    device = selectDevice(); // Pick a device
    if (device != nullptr) {
      connected = connectToServer();
      if (!connected) {
        Serial.println("Failed to connect...");
        Heltec.display->println("Failed to");
        Heltec.display->println("connect...");
        Heltec.display->drawLogBuffer(0, 0);
        Heltec.display->display();
        delay(1100);
        return;
      }
    } else {
      Serial.println("No device found...");
      Heltec.display->println("No device");
      Heltec.display->println("found...");
      Heltec.display->drawLogBuffer(0, 0);
      Heltec.display->display();
      delay(1100);
      return;
    }
  }

  // Update timer if cadence is rolling
  if (cadence > 0) {
    unsigned long now = millis();
    runtime += now - last_millis;
    last_millis = now;
  } else {
    last_millis = millis();
  }

  delay(200); // Delay 200ms between loops.
  updateDisplay();
}
