#include <WiFi.h>
#include <WiFiMulti.h> // Tích hợp Đa WiFi dự phòng
#include <HTTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <Preferences.h>

// =================== CẤU HÌNH NGƯỜI DÙNG =======================
const String DISCORD_HOOK  = "https://discord.com/api/webhooks/1508623853226819584/m7ERgR_wr6Kyc-zI93CCgF0QBhZ4ialKap9xKGNjzF-MrgB0nr4JrmftIpYtrUw6ZZA1";

// Whitelist UID thẻ RFID
const String ALLOWED_UIDS[] = {
  "A1B2C3D4",
  "11223344",
  "AABBCCDD"
};
const int UID_COUNT = sizeof(ALLOWED_UIDS) / sizeof(ALLOWED_UIDS[0]);

// =================== HẰNG SỐ HỆ THỐNG ==========================
#define NUM_SLOTS          4      
#define SLOT_CONFIRM_MS    5000   
#define BARRIER_TIMEOUT_MS 10000  
#define FIRE_LOCK_MS       10000  
#define BARRIER_DELAY_MS   800    
#define WIFI_RETRY_MS      5000   
#define SERVO_OPEN_ANGLE   90
#define SERVO_CLOSE_ANGLE  0

// =================== CHÂN PHẦN CỨNG ============================
const int FIRE_SIGNAL_PIN = 34;           
const int IR_SLOTS[NUM_SLOTS] = {32, 33, 25, 26};
const int IR_GATE_IN  = 27;
const int IR_GATE_OUT = 14;

#define SS_IN        5
#define RST_IN       4
#define SS_OUT       16
#define RST_OUT      17
#define SERVO_IN_PIN 13
#define SERVO_OUT_PIN 12

// =================== ĐỐI TƯỢNG TOÀN CỤC ========================
WiFiMulti wifiMulti;
LiquidCrystal_I2C lcd(0x27, 16, 2);
MFRC522 rfidIn(SS_IN,   RST_IN);
MFRC522 rfidOut(SS_OUT, RST_OUT);
Servo servoIn;
Servo servoOut;
Preferences prefs;                   
QueueHandle_t discordQueue;          

// =================== BIẾN TRẠNG THÁI ===========================
volatile bool isFireDetected = false;
portMUX_TYPE fireMux = portMUX_INITIALIZER_UNLOCKED;
unsigned long fireLockUntil = 0;
bool prevFireMode = false; 

unsigned long slotTimer[NUM_SLOTS] = {0};
bool          slotState[NUM_SLOTS] = {false};
int           availableSlots = NUM_SLOTS;

bool          isBarrierInOpen   = false;
bool          carCrossingIn     = false;
unsigned long barrierInOpenAt  = 0;

bool          isBarrierOutOpen  = false;
bool          carCrossingOut    = false;
unsigned long barrierOutOpenAt = 0;

unsigned long lastWifiCheck = 0;
bool          needLcdUpdate = true; 

// =================== ISR BÁO CHÁY ==============================
void IRAM_ATTR fireISR() {
  static unsigned long lastFire = 0;
  unsigned long now = millis();
  if (now - lastFire > 200) { 
    portENTER_CRITICAL_ISR(&fireMux);
    isFireDetected = true;
    portEXIT_CRITICAL_ISR(&fireMux);
    lastFire = now;
  }
}

// =================== DISCORD TASK (CORE 0) =====================
void discordTask(void* param) {
  char* msg;
  for (;;) {
    if (xQueueReceive(discordQueue, &msg, portMAX_DELAY) == pdTRUE) {
      if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(DISCORD_HOOK);
        http.addHeader("Content-Type", "application/json");
        String payload = "{\"content\": \"";
        payload += String(msg);
        payload += "\"}";
        http.POST(payload);
        http.end();
      }
      free(msg);
    }
  }
}

void sendDiscord(const String& message) {
  char* buf = (char*)malloc(message.length() + 1);
  if (buf) {
    strcpy(buf, message.c_str());
    if (xQueueSend(discordQueue, &buf, 0) != pdTRUE) {
      free(buf);
    }
  }
}

// =================== HÀM TIỆN ÍCH LOGIC ========================
bool isAllowed(const String& uid) {
  for (int i = 0; i < UID_COUNT; i++) {
    if (ALLOWED_UIDS[i] == uid) return true;
  }
  return false;
}

String getUID(MFRC522& rfid) {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

int findAvailableSlot() {
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (!slotState[i]) return i + 1;
  }
  return 0;
}

void updateLCD(int recSlot, bool fire) {
  if (fire) {
    lcd.setCursor(0, 0); lcd.print("!!! BAO CHAY !!!");
    lcd.setCursor(0, 1); lcd.print("IN:CLOSE OUT:OPN"); 
    return;
  }

  lcd.setCursor(0, 0);
  if (recSlot > 0 && availableSlots > 0) {
    lcd.print("Goi y: Slot ");
    lcd.print(recSlot);
    lcd.print("   "); 
  } else {
    lcd.print("Bai xe da FULL! "); 
  }

  lcd.setCursor(0, 1);
  for (int i = 0; i < NUM_SLOTS; i++) {
    lcd.print(i + 1);
    lcd.print(slotState[i] ? ":F " : ":E ");
  }
}

void checkWifi() {
  if (millis() - lastWifiCheck < WIFI_RETRY_MS) return;
  lastWifiCheck = millis();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Mat mang! Dang quet tim WiFi du phong...");
    wifiMulti.run();
  }
}

// =================== LOGIC PCCC & RÀO CHẮN =====================
void handleBarrierIn() {
  if (millis() < fireLockUntil) return;
  if (!isBarrierInOpen) return;

  bool irLow = (digitalRead(IR_GATE_IN) == LOW);
  if (irLow) carCrossingIn = true;

  if (carCrossingIn && !irLow) {
    delay(BARRIER_DELAY_MS);
    servoIn.write(SERVO_CLOSE_ANGLE);
    isBarrierInOpen = false;
    carCrossingIn   = false;
    return;
  }

  if (millis() - barrierInOpenAt > BARRIER_TIMEOUT_MS) {
    servoIn.write(SERVO_CLOSE_ANGLE);
    isBarrierInOpen = false;
    carCrossingIn   = false;
    sendDiscord("⚠️ Rao VAO tu dong dong do timeout (khong co xe qua)!");
  }
}

void handleBarrierOut() {
  if (millis() < fireLockUntil) return;
  if (!isBarrierOutOpen) return;

  bool irLow = (digitalRead(IR_GATE_OUT) == LOW);
  if (irLow) carCrossingOut = true;

  if (carCrossingOut && !irLow) {
    delay(BARRIER_DELAY_MS);
    servoOut.write(SERVO_CLOSE_ANGLE);
    isBarrierOutOpen = false;
    carCrossingOut   = false;
    return;
  }

  if (millis() - barrierOutOpenAt > BARRIER_TIMEOUT_MS) {
    servoOut.write(SERVO_CLOSE_ANGLE);
    isBarrierOutOpen = false;
    carCrossingOut   = false;
    sendDiscord("⚠️ Rao RA tu dong dong do timeout (khong co xe qua)!");
  }
}

// =================== SETUP =====================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== BAI DO XE ===");

  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("XIN CHAO");
  lcd.setCursor(0, 1); lcd.print("Dang khoi dong..");
    
  pinMode(FIRE_SIGNAL_PIN, INPUT);       
  pinMode(IR_GATE_IN,  INPUT_PULLUP);
  pinMode(IR_GATE_OUT, INPUT_PULLUP);
  for (int i = 0; i < NUM_SLOTS; i++) {
    pinMode(IR_SLOTS[i], INPUT_PULLUP);
  }

  SPI.begin();
  rfidIn.PCD_Init();
  rfidOut.PCD_Init();
  delay(50);

  servoIn.attach(SERVO_IN_PIN,  500, 2400);
  servoOut.attach(SERVO_OUT_PIN, 500, 2400);
  servoIn.write(SERVO_CLOSE_ANGLE);
  servoOut.write(SERVO_CLOSE_ANGLE);

  prefs.begin("parking", false);
  int restored = 0;
  for (int i = 0; i < NUM_SLOTS; i++) {
    String key = "s" + String(i);
    slotState[i] = prefs.getBool(key.c_str(), false);
    if (slotState[i]) restored++;
  }
  availableSlots = NUM_SLOTS - restored;

  attachInterrupt(digitalPinToInterrupt(FIRE_SIGNAL_PIN), fireISR, RISING);

  discordQueue = xQueueCreate(10, sizeof(char*));
  xTaskCreatePinnedToCore(discordTask, "DiscordTask", 8192, NULL, 1, NULL, 0);

  // ================= TÍCH HỢP ĐA WIFI BACK-UP =================
  lcd.setCursor(0, 1); lcd.print("Ket noi WiFi... ");
  
  // NẠP DANH SÁCH WIFI
  wifiMulti.addAP("sv.uneti.edu.vn", NULL);
  wifiMulti.addAP("sv.uneti.edu.vn", "sv.uneti.edu.vn");
  wifiMulti.addAP("uneti.edu.vn", "uneti.edu.vn");
  wifiMulti.addAP("vietanh3006-5G", "0982248677VA");
  wifiMulti.addAP("\"Ph██no█ K██sl█n█\"", "33550336");

  unsigned long wStart = millis();
  while (wifiMulti.run() != WL_CONNECTED && millis() - wStart < 10000) {
    delay(300);
  }

  if (WiFi.status() == WL_CONNECTED) {
    sendDiscord("✅ **HỆ THỐNG ONLINE:** Bãi đỗ xe sẵn sàng! Mạng: `" + WiFi.SSID() + "` | IP: " + WiFi.localIP().toString());
  }
  lcd.clear();
}

// =================== LOOP CHÍNH ================================
void loop() {
  checkWifi();

  // 1. ĐỌC TRẠNG THÁI CHÁY AN TOÀN
  bool fire = false;
  portENTER_CRITICAL(&fireMux);
  fire = isFireDetected;
  if (isFireDetected) isFireDetected = false;
  portEXIT_CRITICAL(&fireMux);

  if (fire) {
    servoIn.write(SERVO_CLOSE_ANGLE);   
    servoOut.write(SERVO_OPEN_ANGLE);   
    isBarrierOutOpen  = true;
    barrierOutOpenAt  = millis();
    fireLockUntil     = millis() + FIRE_LOCK_MS; 
    
    updateLCD(0, true);
    sendDiscord("🔥 **BÁO ĐỘNG ĐỎ:** Phát hiện hỏa hoạn! Đóng cổng VÀO, mở cổng RA thoát hiểm!");
  }

  bool inFireMode = (millis() < fireLockUntil);
  
  if (inFireMode) {
    if (!prevFireMode) updateLCD(0, true); 
  } else {
    if (prevFireMode) {
      needLcdUpdate = true; 
    }
  }
  prevFireMode = inFireMode;

  // 2. CẬP NHẬT 4 Ô ĐỖ
  int currentAvailable = 0;
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (digitalRead(IR_SLOTS[i]) == LOW) {
      if (millis() - slotTimer[i] > SLOT_CONFIRM_MS) {
        if (!slotState[i]) {
          slotState[i] = true;
          prefs.putBool(("s" + String(i)).c_str(), true);
          needLcdUpdate = true; 
        }
      }
    } else {
      slotTimer[i] = millis();
      if (slotState[i]) {
        slotState[i] = false;
        prefs.putBool(("s" + String(i)).c_str(), false);
        needLcdUpdate = true; 
      }
    }
    if (!slotState[i]) currentAvailable++;
  }
  availableSlots = currentAvailable;

  // 3. CẬP NHẬT LCD (Event-Driven)
  if (!inFireMode && needLcdUpdate) {
    int recSlot = findAvailableSlot();
    updateLCD(recSlot, false);
    needLcdUpdate = false; 
  }

  // 4. RFID LÀN VÀO
  if (!inFireMode && !isBarrierInOpen) {
    if (rfidIn.PICC_IsNewCardPresent() && rfidIn.PICC_ReadCardSerial()) {
      String uid = getUID(rfidIn);
      int recSlot = findAvailableSlot();

      if (!isAllowed(uid)) {
        sendDiscord("⛔ **THẺ LẠ:** UID `" + uid + "` không có trong danh sách!");
      } else if (recSlot > 0 && availableSlots > 0) {
        servoIn.write(SERVO_OPEN_ANGLE);
        isBarrierInOpen = true;
        barrierInOpenAt = millis();
        carCrossingIn   = false;
        sendDiscord("📥 **XE VÀO:** Thẻ `" + uid + "` — Gợi ý: **Slot " + String(recSlot) + "**");
      } else {
        sendDiscord("❌ **TỪ CHỐI:** Thẻ `" + uid + "` — Bãi đã FULL!");
      }
      rfidIn.PICC_HaltA();
      rfidIn.PCD_StopCrypto1();
    }
  }

  // 5. RFID LÀN RA
  if (!isBarrierOutOpen) {
    if (rfidOut.PICC_IsNewCardPresent() && rfidOut.PICC_ReadCardSerial()) {
      String uid = getUID(rfidOut);

      if (!isAllowed(uid)) sendDiscord("⚠️ **CẢNH BÁO:** Thẻ lạ `" + uid + "` quẹt ở làn RA!");

      servoOut.write(SERVO_OPEN_ANGLE);
      isBarrierOutOpen = true;
      barrierOutOpenAt = millis();
      carCrossingOut   = false;
      sendDiscord("📤 **XE RA:** Thẻ `" + uid + "` vừa rời bãi Bloomvers.");

      rfidOut.PICC_HaltA();
      rfidOut.PCD_StopCrypto1();
    }
  }

  // 6. XỬ LÝ ĐÓNG RÀO
  handleBarrierIn();
  handleBarrierOut();

  delay(30);
}
