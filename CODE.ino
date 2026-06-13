#include <WiFi.h>
#include <WiFiMulti.h> 
#include <HTTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <Preferences.h>

// =================== CẤU HÌNH CLOUD ============================
const String DISCORD_HOOK  = "";

// ĐIỀN TOKEN VÀ ID GROUP TELEGRAM VÀO ĐÂY:
const String TELEGRAM_TOKEN   = "";
const String TELEGRAM_CHAT_ID = ""; 

// =================== DANH SÁCH THẺ VIP =========================
const String ALLOWED_UIDS[] = {
  "C2147A2D",
  "C2FDE22D",
  "429AFA29",
  "90794220"
};
const int UID_COUNT = sizeof(ALLOWED_UIDS) / sizeof(ALLOWED_UIDS[0]);

// =================== HẰNG SỐ HỆ THỐNG ==========================
#define NUM_SLOTS          4      
#define SLOT_CONFIRM_MS    5000   
#define BARRIER_TIMEOUT_MS 10000  
#define FIRE_LOCK_MS       10000  
#define BARRIER_DELAY_MS   800    
#define WIFI_RETRY_MS      5000   

#define SERVO_OPEN_ANGLE   0
#define SERVO_CLOSE_ANGLE  180

// =================== CHÂN PHẦN CỨNG ============================
const int FIRE_SIGNAL_PIN_1 = 34; // MQ số 1 (VÀO)
const int FIRE_SIGNAL_PIN_2 = 32; // MQ số 2 (RA)
const int BUZZER_PIN        = 2;  // Còi hú Active
const int PUMP_RELAY_PIN    = 33; // Relay Bơm

// 4 Mắt IR Ô đỗ (Tương ứng: 35, VP, VN, 25)
const int IR_SLOTS[NUM_SLOTS] = {35, 36, 39, 25}; 
const int IR_GATE_IN  = 27; // Chống kẹt VÀO
const int IR_GATE_OUT = 14; // Chống kẹt RA

#define SS_IN        5
#define RST_IN       4
#define SS_OUT       16
#define RST_OUT      17
#define SERVO_IN_PIN 13
#define SERVO_OUT_PIN 26

// =================== ĐỐI TƯỢNG TOÀN CỤC ========================
WiFiMulti wifiMulti;
LiquidCrystal_I2C lcd(0x27, 16, 2); 
MFRC522 rfidIn(SS_IN,   RST_IN);
MFRC522 rfidOut(SS_OUT, RST_OUT);
Servo servoIn;
Servo servoOut;
Preferences prefs;                   
QueueHandle_t notifyQueue;          

// =================== BIẾN TRẠNG THÁI ===========================
unsigned long fireLockUntil = 0;
bool prevFireMode = false; 
bool isFireActive = false; 

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

// =================== MULTI-TASK THÔNG BÁO ======================
void notifyTask(void* param) {
  char* msg;
  for (;;) {
    if (xQueueReceive(notifyQueue, &msg, portMAX_DELAY) == pdTRUE) {
      if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        
        // Discord
        http.begin(DISCORD_HOOK);
        http.addHeader("Content-Type", "application/json");
        String discordPayload = "{\"content\": \"";
        discordPayload += String(msg);
        discordPayload += "\"}";
        http.POST(discordPayload);
        http.end();

        // Telegram
        String teleUrl = "https://api.telegram.org/bot" + TELEGRAM_TOKEN + "/sendMessage";
        http.begin(teleUrl);
        http.addHeader("Content-Type", "application/json");
        String telePayload = "{\"chat_id\": \"" + TELEGRAM_CHAT_ID + "\", \"text\": \"";
        telePayload += String(msg);
        telePayload += "\"}";
        http.POST(telePayload);
        http.end();
      }
      free(msg);
    }
  }
}

void sendNotify(const String& message) {
  char* buf = (char*)malloc(message.length() + 1);
  if (buf) {
    strcpy(buf, message.c_str());
    if (xQueueSend(notifyQueue, &buf, 0) != pdTRUE) {
      free(buf);
    }
  }
}

// =================== HÀM TIỆN ÍCH ==============================
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
    Serial.println("[WIFI] Mat ket noi! Dang thu lai...");
    WiFi.reconnect(); 
  }
}

// =================== XỬ LÝ RÀO CHẮN ============================
void handleBarrierIn() {
  if (isFireActive) return; 
  if (millis() < fireLockUntil) return;
  if (!isBarrierInOpen) return;

  bool irLow = (digitalRead(IR_GATE_IN) == LOW);
  if (irLow) {
    if (!carCrossingIn) Serial.println("[IR_IN] Xe dang qua rao...");
    carCrossingIn = true;
  }

  if (carCrossingIn && !irLow) {
    Serial.println("[SERVO_IN] Xe da qua. Dong rao!");
    delay(BARRIER_DELAY_MS);
    servoIn.write(SERVO_CLOSE_ANGLE);
    isBarrierInOpen = false;
    carCrossingIn   = false;
    return;
  }

  if (millis() - barrierInOpenAt > BARRIER_TIMEOUT_MS) {
    Serial.println("[SERVO_IN] Timeout. Tu dong dong rao!");
    servoIn.write(SERVO_CLOSE_ANGLE);
    isBarrierInOpen = false;
    carCrossingIn   = false;
  }
}

void handleBarrierOut() {
  if (isFireActive) return; 
  if (millis() < fireLockUntil) return;
  if (!isBarrierOutOpen) return;

  bool irLow = (digitalRead(IR_GATE_OUT) == LOW);
  if (irLow) {
    if (!carCrossingOut) Serial.println("[IR_OUT] Xe dang qua rao...");
    carCrossingOut = true;
  }

  if (carCrossingOut && !irLow) {
    Serial.println("[SERVO_OUT] Xe da qua. Dong rao!");
    delay(BARRIER_DELAY_MS);
    servoOut.write(SERVO_CLOSE_ANGLE);
    isBarrierOutOpen = false;
    carCrossingOut   = false;
    return;
  }

  if (millis() - barrierOutOpenAt > BARRIER_TIMEOUT_MS) {
    Serial.println("[SERVO_OUT] Timeout. Tu dong dong rao!");
    servoOut.write(SERVO_CLOSE_ANGLE);
    isBarrierOutOpen = false;
    carCrossingOut   = false;
  }
}

// =================== SETUP =====================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=======================================");
  Serial.println("[SYSTEM] === KHOI DONG NHAGUIXE V3.2.1 ===");
  Serial.println("=======================================");

  Wire.begin(21, 22); 
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("   NHAGUIXE     ");
  lcd.setCursor(0, 1); lcd.print("Dang khoi dong..");
    
  pinMode(FIRE_SIGNAL_PIN_1, INPUT); 
  pinMode(FIRE_SIGNAL_PIN_2, INPUT); 
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW); 
  digitalWrite(PUMP_RELAY_PIN, LOW); 
  
  pinMode(35, INPUT);
  pinMode(36, INPUT); 
  pinMode(39, INPUT); 
  pinMode(25, INPUT);
  pinMode(IR_GATE_IN,  INPUT_PULLUP);
  pinMode(IR_GATE_OUT, INPUT_PULLUP);

  pinMode(SS_IN, OUTPUT);
  digitalWrite(SS_IN, HIGH);
  pinMode(SS_OUT, OUTPUT);
  digitalWrite(SS_OUT, HIGH);
  
  SPI.begin();
  rfidIn.PCD_Init();
  rfidOut.PCD_Init();
  Serial.println("[SYSTEM] Da khoi tao SPI & RFID.");

  servoIn.attach(SERVO_IN_PIN,  500, 2400);
  servoOut.attach(SERVO_OUT_PIN, 500, 2400);
  servoIn.write(SERVO_CLOSE_ANGLE);
  servoOut.write(SERVO_CLOSE_ANGLE);
  Serial.println("[SYSTEM] Da khoi tao Servo.");

  prefs.begin("parking", false);
  int restored = 0;
  for (int i = 0; i < NUM_SLOTS; i++) {
    String key = "s" + String(i);
    slotState[i] = prefs.getBool(key.c_str(), false);
    if (slotState[i]) restored++;
  }
  availableSlots = NUM_SLOTS - restored;
  Serial.println("[SYSTEM] Da khoi phuc du lieu O do.");

  notifyQueue = xQueueCreate(10, sizeof(char*));
  xTaskCreatePinnedToCore(notifyTask, "NotifyTask", 8192, NULL, 1, NULL, 0);

  Serial.print("[WIFI] Dang ket noi WiFi...");
  wifiMulti.addAP("sv.uneti.edu.vn", "sv.uneti.edu.vn");
  wifiMulti.addAP("uneti.edu.vn", "uneti.edu.vn");
  wifiMulti.addAP("vietanh3006-5G", "0982248677VA");
  wifiMulti.addAP("Ph██no█ K██sl█n█", "33550336");

  unsigned long wStart = millis();
  while (wifiMulti.run() != WL_CONNECTED && millis() - wStart < 5000) {
    Serial.print(".");
    delay(300);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Ket noi THANH CONG! IP: " + WiFi.localIP().toString());
    sendNotify("✅ **HỆ THỐNG ONLINE:** Bãi đỗ xe  sẵn sàng phục vụ!");
  } else {
    Serial.println("\n[WIFI] Ket noi THAT BAI! Se thu lai sau.");
  }
  lcd.clear();
  Serial.println("[SYSTEM] BOOT HOAN TAT. SAN SANG HOAT DONG!\n");
}

// =================== LOOP CHÍNH ================================
void loop() {
  checkWifi();

  // 1. PCCC ĐA ĐIỂM
  bool fire1 = (digitalRead(FIRE_SIGNAL_PIN_1) == LOW);
  bool fire2 = (digitalRead(FIRE_SIGNAL_PIN_2) == LOW);
  bool currentFireState = (fire1 || fire2); 
  
  if (currentFireState && !isFireActive) {
    isFireActive = true;
    Serial.println("\n[FIRE] !!! BAO DONG DO !!! Phat hien khoi/gas.");
    Serial.println("[FIRE] -> Bat Bom + Coi hu.");
    Serial.println("[FIRE] -> Khoa rao Vao, Mo toang rao Ra!");
    digitalWrite(BUZZER_PIN, HIGH);
    digitalWrite(PUMP_RELAY_PIN, HIGH); 

    servoIn.write(SERVO_CLOSE_ANGLE);   
    servoOut.write(SERVO_OPEN_ANGLE);   
    isBarrierOutOpen  = true;
    barrierOutOpenAt  = millis();
    fireLockUntil     = millis() + FIRE_LOCK_MS; 
    
    updateLCD(0, true);
    sendNotify("🔥 **BÁO ĐỘNG ĐỎ:** Phát hiện hỏa hoạn tại bãi xe! Kích hoạt hệ thống phun nước dập lửa và mở rào thoát hiểm khẩn cấp!");
  } 
  else if (!currentFireState && isFireActive) {
    isFireActive = false;
    Serial.println("\n[FIRE] Da dap tat lua. He thong an toan tro lai.");
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(PUMP_RELAY_PIN, LOW); 
    sendNotify("💧 **AN TOÀN:** Hỏa hoạn đã được khống chế thành công. Hệ thống quay lại trạng thái giám sát.");
  }

  bool inFireMode = (millis() < fireLockUntil);
  if (inFireMode) {
    if (!prevFireMode) updateLCD(0, true); 
  } else {
    if (prevFireMode) needLcdUpdate = true; 
  }
  prevFireMode = inFireMode;

  // 2. CẬP NHẬT TRẠNG THÁI Ô ĐỖ
  int currentAvailable = 0;
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (digitalRead(IR_SLOTS[i]) == LOW) {
      if (millis() - slotTimer[i] > SLOT_CONFIRM_MS) {
        if (!slotState[i]) {
          slotState[i] = true;
          prefs.putBool(("s" + String(i)).c_str(), true);
          needLcdUpdate = true; 
          Serial.println("[SLOT] O do so " + String(i+1) + " -> CO XE");
        }
      }
    } else {
      slotTimer[i] = millis();
      if (slotState[i]) {
        slotState[i] = false;
        prefs.putBool(("s" + String(i)).c_str(), false);
        needLcdUpdate = true; 
        Serial.println("[SLOT] O do so " + String(i+1) + " -> TRONG");
      }
    }
    if (!slotState[i]) currentAvailable++;
  }
  availableSlots = currentAvailable;

  // 3. HIỂN THỊ LCD
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
      Serial.println("\n[RFID_IN] The quet vao: " + uid);

      if (!isAllowed(uid)) {
        Serial.println("[RFID_IN] -> TU CHOI! The khong hop le.");
        sendNotify("⛔ **CHẶN CỬA:** Thẻ lạ `" + uid + "` xâm nhập bãi xe Bloomvers! Yêu cầu quay đầu!");
        lcd.setCursor(0, 0); lcd.print(" THE KHONG HOP! ");
        lcd.setCursor(0, 1); lcd.print(" YEU CAU LUI XE ");
        delay(2000); 
        needLcdUpdate = true; 
      } else if (recSlot > 0 && availableSlots > 0) {
        Serial.println("[RFID_IN] -> DONG Y! Mo rao Vao. Goi y slot: " + String(recSlot));
        servoIn.write(SERVO_OPEN_ANGLE);
        isBarrierInOpen = true;
        barrierInOpenAt = millis();
        carCrossingIn   = false;
        sendNotify("📥 **XE VÀO:** Thẻ VIP `" + uid + "` check-in thành công. Gợi ý đỗ tại Slot: " + String(recSlot));
      } else {
        Serial.println("[RFID_IN] -> BAI FULL! Khong the nhan them xe.");
        sendNotify("⚠️ **BÃI ĐẦY:** Thẻ VIP `" + uid + "` yêu cầu vào nhưng hệ thống hết chỗ đỗ trống!");
        lcd.setCursor(0, 0); lcd.print("  BAI DA FULL!  ");
        lcd.setCursor(0, 1); lcd.print(" YEU CAU LUI XE ");
        delay(2000);
        needLcdUpdate = true;
      }
      rfidIn.PICC_HaltA();
      rfidIn.PCD_StopCrypto1();
    }
  }

  // 5. RFID LÀN RA
  if (!isBarrierOutOpen) {
    if (rfidOut.PICC_IsNewCardPresent() && rfidOut.PICC_ReadCardSerial()) {
      String uid = getUID(rfidOut);
      Serial.println("\n[RFID_OUT] The quet ra: " + uid);
      
      if (!isAllowed(uid)) {
         Serial.println("[RFID_OUT] -> CANH BAO! The la quet ra.");
         sendNotify("⚠️ **CẢNH BÁO:** Phát hiện thẻ lạ `" + uid + "` quẹt ở đầu ra!");
      } else {
         Serial.println("[RFID_OUT] -> DONG Y! Mo rao Ra.");
      }
      
      servoOut.write(SERVO_OPEN_ANGLE);
      isBarrierOutOpen = true;
      barrierOutOpenAt = millis();
      carCrossingOut   = false;
      sendNotify("📤 **XE RA:** Thẻ VIP `" + uid + "` check-out thành công. Hẹn gặp lại!");

      rfidOut.PICC_HaltA();
      rfidOut.PCD_StopCrypto1();
    }
  }

  // 6. XỬ LÝ TỰ ĐỘNG ĐÓNG RÀO CHẮN
  handleBarrierIn();
  handleBarrierOut();

  delay(30);
}
