/**
 * Autoterm Air 4D — UART Protocol Test Script
 *
 * Tests bidirectionnels du protocole Autoterm sans ESPHome.
 * Utile pour débugger le câblage et comprendre les trames.
 *
 * Matériel: ESP32 DevKit v1
 * Câblage:
 *   GPIO16 (RX) → Display TX (panneau)
 *   GPIO17 (TX) → Display RX (panneau)
 *   GPIO22 (RX) → Heater TX (chaudière)
 *   GPIO23 (TX) → Heater RX (chaudière)
 *
 * Outil série: Arduino Serial Monitor à 115200 baud
 * Protocole Autoterm: 9600 baud, 8N1
 *
 * Version: 1.0
 * License: MIT
 */

// === Configuration ===
#define BAUD_DEBUG     115200   // Vitesse du moniteur série
#define BAUD_AUTOTERM  9600     // Vitesse du protocole Autoterm

// Pins Display (panneau)
#define PIN_DISP_RX 16
#define PIN_DISP_TX 17

// Pins Heater (chaudière)
#define PIN_HEAT_RX 22
#define PIN_HEAT_TX 23

// Protocole
#define FRAME_HEADER    0xAA
#define DEVICE_DISPLAY  0x03
#define DEVICE_HEATER   0x04
#define FUNC_STATUS     0x0F
#define FUNC_SETTINGS   0x02
#define FUNC_POWER      0x01
#define FUNC_STANDBY    0x03
#define FUNC_PANEL_TEMP 0x11
#define FUNC_FAN_ONLY   0x23
// Extended protocol
#define FUNC_VERSION    0x06
#define FUNC_DIAGNOSTIC 0x07
#define FUNC_REPORT     0x0B
#define FUNC_UNLOCK     0x0D
#define FUNC_PRIME      0x13

// Buffer
#define MAX_FRAME_SIZE 64

// === Hardware Serial ===
// Serial0: USB debug (115200)
// Serial2: Display (9600) — RX=16, TX=17
// Serial1: Heater (9600) — RX=22, TX=23

// === CRC16 Modbus ===
uint16_t crc16_modbus(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t pos = 0; pos < length; pos++) {
    crc ^= data[pos];
    for (int i = 0; i < 8; i++) {
      if (crc & 0x0001)
        crc = (crc >> 1) ^ 0xA001;
      else
        crc >>= 1;
    }
  }
  return crc;
}

// === Utilitaires ===
void printHex(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (data[i] < 0x10) Serial.print("0");
    Serial.print(data[i], HEX);
    Serial.print(" ");
  }
}

const char* statusText(uint16_t code) {
  switch (code) {
    case 0x0001: return "Standby";
    case 0x0100: return "Flame sensor cools";
    case 0x0101: return "Ventilation";
    case 0x0200: return "Preparing heating";
    case 0x0201: return "Glow plug heats";
    case 0x0202: return "Ignition 1";
    case 0x0203: return "Ignition 2";
    case 0x0204: return "Combustion chamber heating";
    case 0x0300: return "Heating";
    case 0x0323: return "Fans only";
    case 0x0304: return "Cooling down";
    case 0x0305: return "Idle ventilation";
    case 0x0400: return "Shutdown";
    default: return "Unknown";
  }
}

// === Analyse des trames ===
void analyzeFrame(const uint8_t *frame, size_t len, const char *tag) {
  if (len < 5) return;

  // Vérifier header
  if (frame[0] != FRAME_HEADER) return;

  uint8_t device = frame[1];
  uint8_t payloadLen = frame[2];
  uint8_t funcCode = frame[4];

  Serial.printf("\n=== [%s] Trame (%u bytes) ===\n", tag, len);
  Serial.printf("  Header: 0x%02X | Device: 0x%02X | Func: 0x%02X\n",
                frame[0], device, funcCode);

  // Vérifier CRC
  if (len >= 5) {
    uint16_t expected = crc16_modbus(frame, len - 2);
    uint16_t received = (frame[len-2] << 8) | frame[len-1];
    Serial.printf("  CRC: %s (reçu=0x%04X, attendu=0x%04X)\n",
                  expected == received ? "OK" : "ERREUR",
                  received, expected);
  }

  // Analyser selon le type
  if (funcCode == FUNC_STATUS && device == DEVICE_HEATER && len >= 24) {
    const uint8_t *p = &frame[5];
    uint16_t status = (p[0] << 8) | p[1];
    float voltage = p[6] / 10.0f;
    uint16_t heaterTemp = (p[7] << 8) | p[8];
    float fanSet = p[11] * 60.0f;
    float fanActual = p[12] * 60.0f;
    float pumpFreq = p[14] / 100.0f;
    float internalTemp = (p[3] > 127 ? p[3] - 255 : p[3]);
    float externalTemp = (p[4] > 127 ? p[4] - 255 : p[4]);

    Serial.printf("  STATUS: %s (0x%04X)\n", statusText(status), status);
    Serial.printf("  T_interne: %.1f°C | T_externe: %.1f°C\n", internalTemp, externalTemp);
    Serial.printf("  T_chauffage: %u (%.0f°C)\n", heaterTemp, (heaterTemp - 0x100) / 2.0f);
    Serial.printf("  Tension: %.1fV\n", voltage);
    Serial.printf("  Ventilateur: %.0f/%.0f rpm\n", fanActual, fanSet);
    Serial.printf("  Pompe: %.2f Hz\n", pumpFreq);
  }
  else if (funcCode == FUNC_SETTINGS && device == DEVICE_HEATER && len >= 11) {
    const uint8_t *p = &frame[5];
    Serial.printf("  SETTINGS: work_time=%d temp_src=%d set_temp=%d wait_mode=%d level=%d\n",
                  p[0], p[2], p[3], p[4], p[5]);
  }
  else if (funcCode == FUNC_PANEL_TEMP) {
    Serial.printf("  PANEL_TEMP: %u°C\n", frame[5]);
  }
  else if (funcCode == FUNC_VERSION && device == DEVICE_HEATER && len >= 12) {
    const uint8_t *p = &frame[5];
    Serial.printf("  VERSION: %u.%u.%u.%u (boot:%u)\n", p[0], p[1], p[2], p[3], p[4]);
  }
  else if (funcCode == FUNC_REPORT && device == DEVICE_HEATER && len >= 12) {
    const uint8_t *p = &frame[5];
    uint16_t hours = (p[0] << 8) | p[1];
    uint16_t starts = (p[2] << 8) | p[3];
    Serial.printf("  HISTORY: %u hours, %u starts, errors=[%u,%u,%u]\n",
                  hours, starts, p[4], p[5], p[6]);
  }
  else if (funcCode == 0x01 && device == 0x02 && len >= 77) {
    // Diagnostic telemetry (72 bytes)
    const uint8_t *p = &frame[5];
    float chamberTemp = ((p[18] << 8) | p[19]) - 273.15f;
    float flameTemp = ((p[20] << 8) | p[21]) - 273.15f;
    float boardTemp = (int8_t)p[25];
    uint16_t voltageRaw = (p[26] << 8) | p[27];
    Serial.printf("  DIAGNOSTIC: chamber=%.1f°C flame=%.1f°C board=%.1f°C U=%.1fV fault=%u\n",
                  chamberTemp, flameTemp, boardTemp, voltageRaw / 10.0f, p[28]);
  }
  else {
    Serial.printf("  Payload: ");
    for (int i = 5; i < len - 2; i++) {
      Serial.printf("%02X ", frame[i]);
    }
    Serial.println();
  }
}

// === Envoi de commandes ===
void sendCommand(HardwareSerial &serial, uint8_t func, const uint8_t *payload, uint8_t payloadLen, const char *label) {
  uint8_t frame[16];
  frame[0] = FRAME_HEADER;
  frame[1] = DEVICE_DISPLAY;
  frame[2] = payloadLen;
  frame[3] = 0x00;
  frame[4] = func;
  for (int i = 0; i < payloadLen; i++) {
    frame[5 + i] = payload[i];
  }
  uint16_t crc = crc16_modbus(frame, 5 + payloadLen);
  frame[5 + payloadLen] = (crc >> 8) & 0xFF;
  frame[6 + payloadLen] = crc & 0xFF;

  serial.write(frame, 7 + payloadLen);
  Serial.printf("\n>> [%s] Commande envoyée: %s (func=0x%02X)\n", label, label, func);
}

void sendStandby(HardwareSerial &serial) {
  sendCommand(serial, FUNC_STANDBY, NULL, 0, "STANDBY");
}

void sendPowerMode(HardwareSerial &serial, uint8_t level) {
  uint8_t payload[] = {0xFF, 0xFF, 0x04, 0xFF, 0x02, level};
  sendCommand(serial, FUNC_POWER, payload, 6, "POWER_MODE");
}

void sendFanOnly(HardwareSerial &serial, uint8_t level) {
  uint8_t payload[] = {0xFF, 0xFF, level, 0xFF};
  sendCommand(serial, FUNC_FAN_ONLY, payload, 4, "FAN_ONLY");
}

void sendStatusRequest(HardwareSerial &serial) {
  uint16_t crc = crc16_modbus((uint8_t[]){FRAME_HEADER, DEVICE_DISPLAY, 0x00, 0x00, FUNC_STATUS}, 5);
  uint8_t frame[] = {FRAME_HEADER, DEVICE_DISPLAY, 0x00, 0x00, FUNC_STATUS, (uint8_t)(crc >> 8), (uint8_t)(crc & 0xFF)};
  serial.write(frame, 7);
  Serial.printf("\n>> [REQUEST] Status request sent\n");
}

// Extended protocol commands
void sendVersionRequest(HardwareSerial &serial) {
  sendCommand(serial, FUNC_VERSION, NULL, 0, "VERSION");
}

void sendUnlock(HardwareSerial &serial) {
  sendCommand(serial, FUNC_UNLOCK, NULL, 0, "UNLOCK");
}

void sendPrimePump(HardwareSerial &serial, uint8_t freq) {
  uint8_t payload[] = {freq};
  sendCommand(serial, FUNC_PRIME, payload, 1, "PRIME_PUMP");
}

void sendDiagnosticMode(HardwareSerial &serial, bool enable) {
  uint8_t payload[] = {enable ? 0x01 : 0x00};
  sendCommand(serial, FUNC_DIAGNOSTIC, payload, 1, enable ? "DIAG_ENABLE" : "DIAG_DISABLE");
}

void sendReportRequest(HardwareSerial &serial) {
  sendCommand(serial, FUNC_REPORT, NULL, 0, "REPORT");
}

// === Lecture de trames ===
uint8_t rxBuffer[MAX_FRAME_SIZE];
size_t rxLen = 0;
uint32_t lastByteMs = 0;

bool readFrame(HardwareSerial &serial, const char *tag) {
  while (serial.available()) {
    uint8_t b = serial.read();

    // Timeout: si plus de 200ms entre deux bytes, reset
    if (rxLen > 0 && (millis() - lastByteMs) > 200) {
      Serial.printf("\n⚠️ [%s] Timeout trame (%u bytes ignorés)\n", tag, rxLen);
      rxLen = 0;
    }
    lastByteMs = millis();

    // Attendre header
    if (rxLen == 0 && b != FRAME_HEADER) {
      // Byte avant header — log si intéressant
      if (b != 0x00 && b != 0xFF) {
        Serial.printf("[%s] Byte hors trame: 0x%02X\n", tag, b);
      }
      continue;
    }

    rxBuffer[rxLen++] = b;

    // Vérifier si trame complète
    if (rxLen >= 5) {
      uint8_t payloadLen = rxBuffer[2];
      size_t totalLen = 5 + payloadLen + 2;  // header + 3 + payload + 2 CRC

      if (rxLen >= totalLen) {
        analyzeFrame(rxBuffer, totalLen, tag);
        rxLen = 0;
        return true;
      }
    }

    // Overflow
    if (rxLen >= MAX_FRAME_SIZE) {
      Serial.printf("\n⚠️ [%s] Buffer overflow (%u bytes), flush\n", tag, rxLen);
      rxLen = 0;
    }
  }
  return false;
}

// === Commandes série ===
void handleSerialCommand() {
  if (!Serial.available()) return;

  char cmd = Serial.read();
  switch (cmd) {
    case '1':
      Serial.println("\n🚀 Démarrage: Power Mode Level 4");
      sendPowerMode(Serial2, 4);
      break;
    case '2':
      Serial.println("\n🚀 Démarrage: Power Mode Level 8");
      sendPowerMode(Serial2, 8);
      break;
    case '3':
      Serial.println("\n💨 Fan Only Level 5");
      sendFanOnly(Serial2, 5);
      break;
    case '0':
      Serial.println("\n⏹ Standby");
      sendStandby(Serial2);
      break;
    case 'r':
      Serial.println("\n📊 Demande status...");
      sendStatusRequest(Serial2);
      break;
    case 'v':
      Serial.println("\n📋 Demande version firmware...");
      sendVersionRequest(Serial2);
      break;
    case 'u':
      Serial.println("\n🔓 Unlock (error 37)...");
      sendUnlock(Serial2);
      break;
    case 'p':
      Serial.println("\n⛽ Prime pump 1Hz...");
      sendPrimePump(Serial2, 1);
      break;
    case 'd':
      Serial.println("\n🔬 Toggle diagnostic mode...");
      sendDiagnosticMode(Serial2, true);
      break;
    case 'i':
      Serial.println("\n📊 Demande historique...");
      sendReportRequest(Serial2);
      break;
    case 'h':
      Serial.println("\n=== COMMANDES ===");
      Serial.println("  1 = Power Mode Level 4");
      Serial.println("  2 = Power Mode Level 8");
      Serial.println("  3 = Fan Only Level 5");
      Serial.println("  0 = Standby");
      Serial.println("  r = Request Status");
      Serial.println("  v = Version Firmware");
      Serial.println("  u = Unlock (error 37)");
      Serial.println("  p = Prime Pump 1Hz");
      Serial.println("  d = Diagnostic Mode");
      Serial.println("  i = History/Report");
      Serial.println("  h = Aide");
      break;
    default:
      // Ignorer newline, etc.
      break;
  }
}

// === Setup ===
void setup() {
  // Debug USB
  Serial.begin(BAUD_DEBUG);
  delay(1000);
  Serial.println("\n\n========================================");
  Serial.println("  Autoterm Air 4D — UART Protocol Test");
  Serial.println("========================================");
  Serial.printf("  Debug: %d baud | Autoterm: %d baud\n", BAUD_DEBUG, BAUD_AUTOTERM);
  Serial.printf("  Display: RX=GPIO%d TX=GPIO%d\n", PIN_DISP_RX, PIN_DISP_TX);
  Serial.printf("  Heater:  RX=GPIO%d TX=GPIO%d\n", PIN_HEAT_RX, PIN_HEAT_TX);
  Serial.println("========================================");
  Serial.println("  Appuyez 'h' pour l'aide des commandes");
  Serial.println("========================================\n");

  // UART Display (panneau)
  Serial2.begin(BAUD_AUTOTERM, SERIAL_8N1, PIN_DISP_RX, PIN_DISP_TX);
  // Serial1: RX=22 TX=23 pour la chaudière
  Serial1.begin(BAUD_AUTOTERM, SERIAL_8N1, PIN_HEAT_RX, PIN_HEAT_TX);

  Serial.println("✅ UART initialisés. En attente de trames...\n");
}

// === Loop ===
void loop() {
  // Lire des trames depuis la chaudière
  readFrame(Serial1, "HEATER→ESP");

  // Lire des trames depuis le panneau
  readFrame(Serial2, "DISPLAY→ESP");

  // Gérer les commandes du moniteur série
  handleSerialCommand();
}
