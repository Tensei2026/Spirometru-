/*
 * ╔══════════════════════════════════════════════════════════════════╗
 * ║  SPIROMETRU ELECTRONIC PORTABIL — v12                           ║
 * ║  ESP32-C3 SuperMini                                             ║
 * ╠══════════════════════════════════════════════════════════════════╣
 * ║  Manevra FVC completa:                                          ║
 * ║    1. REPAUS    → calibrare ADC_ref                             ║
 * ║    2. INSPIR_1  → inspiratie maxima (VIR, FIV1, PIF)           ║
 * ║    3. EXPIR     → expiratie fortata (PEF, FEV1, FEV_total)     ║
 * ║    4. INSPIR_2  → inspiratie fortata inapoi (FVC complet)      ║
 * ║    5. RESULT    → trimite toti parametrii prin BLE              ║
 * ╠══════════════════════════════════════════════════════════════════╣
 * ║  Parametri calculati:                                           ║
 * ║    PEF       — debit expirator de varf (L/min)                 ║
 * ║    FEV1      — volum expirat prima secunda (L)                  ║
 * ║    FEV_total — volum expirat total (L)                          ║
 * ║    PIF       — debit inspirator de varf (L/min)                 ║
 * ║    FIV1      — volum inspirat prima secunda (L)                 ║
 * ║    VIR       — volum inspirator total (L)                       ║
 * ║    FVC       — FEV_total + VIR (L)                             ║
 * ║    IT        — indice Tiffeneau FEV1/FVC × 100 (%)             ║
 * ╠══════════════════════════════════════════════════════════════════╣
 * ║  Detectie inspiratie — Metoda 2 (fereastra temporala):          ║
 * ║    Dupa varf expirator, ADC < ADC_ref - INSPIR_THR             ║
 * ║    Prag relativ fata de manevra curenta, nu absolut             ║
 * ║  Detectie inspiratie — Metoda 1 (schimbare semn):               ║
 * ║    ADC < ADC_ref → flux inversat → inspiratie                   ║
 * ║    Calculata in paralel pentru comparatie                       ║
 * ╠══════════════════════════════════════════════════════════════════╣
 * ║  Pini:                                                          ║
 * ║    GPIO1  — buton (scurt=cal, lung 2s=toggle BLE)              ║
 * ║    GPIO2  — ADC semnal senzor (ESP_ADC)                        ║
 * ║    GPIO3  — ADC baterie (BAT_ADC)                              ║
 * ║    GPIO5  — AUX_PWR                                            ║
 * ║    GPIO6  — OLED SDA                                           ║
 * ║    GPIO7  — OLED SCL                                           ║
 * ║    GPIO8  — LED (active LOW)                                   ║
 * ║    GPIO10 — PERIPH_PWR / step-up                               ║
 * ╠══════════════════════════════════════════════════════════════════╣
 * ║  BLE — nume dispozitiv: "Spirometru"                            ║
 * ║  Protocol:                                                       ║
 * ║    Streaming READY: "R:XXXX" la 200Hz                          ║
 * ║    Dupa manevra: MEAS_START → R:XXXX×N → PEF:x FEV1:x         ║
 * ║                  PIF:x FIV1:x VIR:x FVC:x IT:x → MEAS_END     ║
 * ║    Comenzi: CAL, RST                                            ║
 * ╚══════════════════════════════════════════════════════════════════╝
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "esp_sleep.h"
#include "driver/gpio.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ══════════════════════════════════════════════════════════════════
// CONFIGURARE
// ══════════════════════════════════════════════════════════════════

#define SMOOTH_SIZE       30
#define CAL_SAMPLES      500
#define CAL_DELAY_MS     300

// Praguri expiratie
#define BLOW_START_THR   100    // ADC peste ref pentru detectie expiratie
#define PEAK_HOLD_MS    1000    // ms dupa care varf nu mai creste → final expir

// Praguri inspiratie
#define INSPIR_THR        60    // ADC sub ref pentru detectie inspiratie
                                // (mai mic ca BLOW_START_THR — inspiratia e mai slaba)
#define INSPIR_HOLD_MS   800    // ms dupa care semnalul inspirator nu mai scade → final inspir
#define INSPIR_WAIT_MS  2000    // timp maxim de asteptare inspiratie dupa expiratie

// Inregistrare
#define REC_HZ           200
#define REC_MAX_S         25
#define REC_SIZE (REC_HZ * REC_MAX_S)

// Sistem
#define AWAKE_MS        30000
#define LIPO_READ_MS     5000
#define BLE_HOLD_MS      2000
#define BLE_STREAM_MS       5
#define BLE_TX_MS           5
#define GRAPH_SCALE       300
#define GRAPH_SCROLL_MS    50

#define LIPO_MAX  4.20f
#define LIPO_MIN  3.00f

// Calibrare PEF polinomiala (N=30, R²=0.9965)
// PEF [L/min] = A*ADC² + B*ADC + C
#define CAL_A   -0.00008658f
#define CAL_B    0.659186f
#define CAL_C  -579.461f

// ══════════════════════════════════════════════════════════════════
// PINI
// ══════════════════════════════════════════════════════════════════

#define WAKE_PIN    GPIO_NUM_1
#define SIG_PIN     2
#define ADC_PIN     3
#define AUX_PWR     GPIO_NUM_5
#define PERIPH_PWR  GPIO_NUM_10
#define LED_PIN     GPIO_NUM_8
#define SDA_PIN     6
#define SCL_PIN     7

// ══════════════════════════════════════════════════════════════════
// DISPLAY
// ══════════════════════════════════════════════════════════════════

#define SCREEN_W  128
#define SCREEN_H   64
#define OLED_ADDR 0x3C
#define GRAPH_W   128
#define GRAPH_Y    45
#define GRAPH_H    18
#define GRAPH_CY   54

// ══════════════════════════════════════════════════════════════════
// BLE
// ══════════════════════════════════════════════════════════════════

#define BLE_DEVICE_NAME  "Spirometru"
#define SERVICE_UUID     "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_UUID        "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CMD_UUID         "2c7c3e4a-19b8-4f5a-9f45-c8e4d2b3a1f6"

// ══════════════════════════════════════════════════════════════════
// STATE MACHINE
// ══════════════════════════════════════════════════════════════════

enum DevState {
  ST_READY,
  ST_CALIBRATING,
  ST_INSPIR_1,     // inspiratie maxima initiala
  ST_EXPIR,        // expiratie fortata
  ST_INSPIR_2,     // inspiratie fortata finala
  ST_RESULT
};
DevState devState = ST_READY;

// ══════════════════════════════════════════════════════════════════
// VARIABILE MASURARE
// ══════════════════════════════════════════════════════════════════

// Calibrare
uint16_t sigCenterRaw  = 2048;
uint32_t calSum = 0, calCount = 0;
uint32_t calDelayStart = 0;
bool     calSampling   = false;
uint16_t calMin = 0xFFFF, calMax = 0;

// Rolling mean
uint16_t smoothBuf[SMOOTH_SIZE];
uint8_t  smoothHead = 0;
uint32_t smoothSum  = 0;
uint16_t smoothADC  = 0;

// Graf
int8_t   graphBuf[GRAPH_W] = {0};
uint32_t lastGraphScroll   = 0;

// ── Expiratie ─────────────────────────────────────────────────────
uint16_t peakExpir     = 0;    // ADC maxim in expiratie
uint32_t lastPeakExpir = 0;    // timestamp ultimei actualizari varf expir
float    pefValue      = 0.0f;
float    fev1Value     = 0.0f;
float    fevTotalValue = 0.0f;

// ── Inspiratie initiala (INSPIR_1) ────────────────────────────────
uint16_t peakInspir1    = 0;   // ADC minim in inspiratie 1 (cel mai mic = debit max)
uint32_t lastPeakInspir1= 0;
float    pifValue1      = 0.0f;
float    fiv1Value1     = 0.0f;
float    virValue1      = 0.0f;

// ── Inspiratie finala (INSPIR_2) ──────────────────────────────────
uint16_t peakInspir2    = 0;
uint32_t lastPeakInspir2= 0;
float    pifValue2      = 0.0f;
float    fiv1Value2     = 0.0f;
float    virValue2      = 0.0f;

// ── Rezultate finale ──────────────────────────────────────────────
float    fvcValue      = 0.0f;
float    itValue       = 0.0f;  // indice Tiffeneau %

// ── Inregistrare ──────────────────────────────────────────────────
uint16_t recBuf[REC_SIZE];
uint16_t recIdx        = 0;
uint32_t recStartMs    = 0;
uint32_t lastRecSample = 0;

// Marcaje in buffer pentru segmentele manevrei
uint16_t recIdxExpirStart  = 0;
uint16_t recIdxExpirEnd    = 0;
uint16_t recIdxInspir1Start= 0;
uint16_t recIdxInspir1End  = 0;
uint16_t recIdxInspir2Start= 0;

// ── BLE TX ────────────────────────────────────────────────────────
uint8_t  txPhase = 6;
uint16_t txIdx   = 0;
uint32_t lastTxMs= 0;

// ══════════════════════════════════════════════════════════════════
// RTC
// ══════════════════════════════════════════════════════════════════

RTC_DATA_ATTR bool bleEnabledRTC = false;

// ══════════════════════════════════════════════════════════════════
// GLOBALE
// ══════════════════════════════════════════════════════════════════

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
BLEServer*         pServer = nullptr;
BLECharacteristic* pNotify = nullptr;
BLECharacteristic* pCmd    = nullptr;
bool deviceConnected  = false;
bool bleEnabled       = false;
bool bleInitialised   = false;
volatile bool cmdCal  = false;
volatile bool cmdRst  = false;

uint16_t rawADC   = 0;
float    lipoV    = 0.0f;
uint32_t lastLipo = 0;

// ══════════════════════════════════════════════════════════════════
// BLE CALLBACKS
// ══════════════════════════════════════════════════════════════════

class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    deviceConnected = true;
    Serial.println("[BLE] conectat");
  }
  void onDisconnect(BLEServer* s) override {
    deviceConnected = false;
    s->startAdvertising();
    Serial.println("[BLE] deconectat → re-advertising");
  }
};

class CmdCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String v = c->getValue();
    if (v == "CAL" || v == "cal") cmdCal = true;
    else if (v == "RST" || v == "rst") cmdRst = true;
    Serial.printf("[CMD] %s\n", v.c_str());
  }
};

void bleStart() {
  if (!bleInitialised) {
    BLEDevice::init(BLE_DEVICE_NAME);
    BLEDevice::setMTU(185);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new ServerCB());
    BLEService* svc = pServer->createService(SERVICE_UUID);
    pNotify = svc->createCharacteristic(CHAR_UUID,
                BLECharacteristic::PROPERTY_READ |
                BLECharacteristic::PROPERTY_NOTIFY);
    pNotify->addDescriptor(new BLE2902());
    pNotify->setValue("idle");
    pCmd = svc->createCharacteristic(CMD_UUID,
               BLECharacteristic::PROPERTY_WRITE |
               BLECharacteristic::PROPERTY_WRITE_NR);
    pCmd->setCallbacks(new CmdCB());
    svc->start();
    bleInitialised = true;
  }
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  bleEnabled = true; bleEnabledRTC = true;
  Serial.println("[BLE] advertising ca 'Spirometru'");
}

void bleStopInternal() {
  BLEDevice::stopAdvertising();
  deviceConnected = false; bleEnabled = false;
}

void bleStopUser() {
  BLEDevice::stopAdvertising();
  deviceConnected = false; bleEnabled = false; bleEnabledRTC = false;
}

// ══════════════════════════════════════════════════════════════════
// UTILITARE
// ══════════════════════════════════════════════════════════════════

static inline void led(bool on) { digitalWrite(LED_PIN, on ? LOW : HIGH); }

float readLipo() {
  uint32_t s = 0;
  for (int i = 0; i < 16; i++) s += analogReadMilliVolts(ADC_PIN);
  return (s / 16000.0f) * 2.0f;
}
int battPct(float v) {
  return constrain((int)((v - LIPO_MIN) / (LIPO_MAX - LIPO_MIN) * 100.0f), 0, 100);
}

void initSmooth(uint16_t fill) {
  for (int i = 0; i < SMOOTH_SIZE; i++) smoothBuf[i] = fill;
  smoothSum = (uint32_t)fill * SMOOTH_SIZE;
  smoothHead = 0; smoothADC = fill;
}

void updateSmooth(uint16_t raw) {
  smoothSum -= smoothBuf[smoothHead];
  smoothBuf[smoothHead] = raw;
  smoothSum += raw;
  smoothHead = (smoothHead + 1) % SMOOTH_SIZE;
  smoothADC  = (uint16_t)(smoothSum / SMOOTH_SIZE);
}

// ══════════════════════════════════════════════════════════════════
// FORMULE CALIBRARE
// ══════════════════════════════════════════════════════════════════

// ADC → PEF (L/min) — formula polinomiala din datele reale
float adcToPef(uint16_t adc) {
  if (adc <= sigCenterRaw) return 0.0f;
  float a = (float)adc;
  float pef = CAL_A * a * a + CAL_B * a + CAL_C;
  return max(0.0f, pef);
}

// Diferenta ADC → PIF (L/min) — aceeasi curba aplicata pe diferenta
// Inspiratia produce o diferenta de presiune inversa dar de acelasi tip
float adcDiffToPif(uint16_t adcMin) {
  if (adcMin >= sigCenterRaw) return 0.0f;
  uint16_t diff = sigCenterRaw - adcMin;
  // Aplicam formula pe un ADC virtual centrat
  uint16_t adcVirtual = sigCenterRaw + diff;
  float pif = CAL_A * (float)adcVirtual * adcVirtual + CAL_B * adcVirtual + CAL_C;
  return max(0.0f, pif);
}

// Integrare trapezoidala pe buffer intre doua indecsi
// Converteste ADC → debit Q(t) [L/s] si integreaza
float integrateBuffer(uint16_t* buf, uint16_t startIdx, uint16_t endIdx,
                      bool isInspir, uint16_t maxSamples) {
  if (endIdx <= startIdx) return 0.0f;
  uint16_t n = min((uint16_t)(endIdx - startIdx), maxSamples);
  float dt   = 1.0f / REC_HZ;  // interval intre esantioane [s]
  float sum  = 0.0f;

  for (uint16_t i = startIdx; i < startIdx + n - 1; i++) {
    float q1, q2;
    if (!isInspir) {
      // Expiratie: ADC > ref
      q1 = adcToPef(buf[i])     / 60.0f;  // L/min → L/s
      q2 = adcToPef(buf[i + 1]) / 60.0f;
    } else {
      // Inspiratie: ADC < ref, folosim diferenta fata de ref
      q1 = adcDiffToPif(buf[i])     / 60.0f;
      q2 = adcDiffToPif(buf[i + 1]) / 60.0f;
    }
    sum += (q1 + q2) * 0.5f * dt;
  }
  return max(0.0f, sum);
}

// ══════════════════════════════════════════════════════════════════
// CALCUL REZULTATE
// ══════════════════════════════════════════════════════════════════

void computeResults() {
  // ── PEF ──────────────────────────────────────────────────────
  pefValue = adcToPef(peakExpir);

  // ── FEV1 — integrare trapezoidala prima secunda expiratie ─────
  uint16_t fev1Samples = min((uint16_t)REC_HZ, (uint16_t)(recIdxExpirEnd - recIdxExpirStart));
  fev1Value = integrateBuffer(recBuf, recIdxExpirStart, recIdxExpirStart + fev1Samples, false, REC_HZ);

  // ── FEV_total — integrare pe toata expiratia ──────────────────
  fevTotalValue = integrateBuffer(recBuf, recIdxExpirStart, recIdxExpirEnd, false, REC_SIZE);

  // ── PIF Metoda 2 (fereastra temporala) — inspiratie finala ────
  pifValue2  = adcDiffToPif(peakInspir2);
  uint16_t fiv1Samples2 = min((uint16_t)REC_HZ, (uint16_t)(recIdx - recIdxInspir2Start));
  fiv1Value2 = integrateBuffer(recBuf, recIdxInspir2Start, recIdxInspir2Start + fiv1Samples2, true, REC_HZ);
  virValue2  = integrateBuffer(recBuf, recIdxInspir2Start, recIdx, true, REC_SIZE);

  // ── PIF Metoda 1 (schimbare semn) — inspiratie initiala ───────
  pifValue1  = adcDiffToPif(peakInspir1);
  uint16_t fiv1Samples1 = min((uint16_t)REC_HZ, (uint16_t)(recIdxInspir1End - recIdxInspir1Start));
  fiv1Value1 = integrateBuffer(recBuf, recIdxInspir1Start, recIdxInspir1Start + fiv1Samples1, true, REC_HZ);
  virValue1  = integrateBuffer(recBuf, recIdxInspir1Start, recIdxInspir1End, true, REC_SIZE);

  // ── FVC si Indice Tiffeneau ───────────────────────────────────
  // FVC = FEV_total + VIR din inspiratia initiala (Metoda 1)
  // Folosim media celor doua volume inspiratorii pentru robustete
  float virMedie = (virValue1 + virValue2) / 2.0f;
  fvcValue = fevTotalValue + virMedie;

  if (fvcValue > 0.1f)
    itValue = (fev1Value / fvcValue) * 100.0f;
  else
    itValue = 0.0f;

  Serial.printf("[RESULT] PEF=%.1f FEV1=%.2f FEVtot=%.2f\n",
                pefValue, fev1Value, fevTotalValue);
  Serial.printf("         PIF1=%.1f FIV1_1=%.2f VIR1=%.2f (Metoda1)\n",
                pifValue1, fiv1Value1, virValue1);
  Serial.printf("         PIF2=%.1f FIV1_2=%.2f VIR2=%.2f (Metoda2)\n",
                pifValue2, fiv1Value2, virValue2);
  Serial.printf("         FVC=%.2f IT=%.1f%%\n", fvcValue, itValue);
}

// ══════════════════════════════════════════════════════════════════
// CALIBRARE
// ══════════════════════════════════════════════════════════════════

void calibrateBlocking() {
  uint32_t s = 0; uint16_t mn = 0xFFFF, mx = 0;
  for (int i = 0; i < 256; i++) {
    uint16_t r = (uint16_t)analogRead(SIG_PIN);
    s += r;
    if (r < mn) mn = r;
    if (r > mx) mx = r;
    delay(1);
  }
  sigCenterRaw = (uint16_t)(s / 256);
  calMin = mn; calMax = mx;
  initSmooth(sigCenterRaw);
  Serial.printf("[CAL] boot centre=%u noise=%u expirThr=%d inspirThr=%d\n",
                sigCenterRaw, mx-mn, BLOW_START_THR, INSPIR_THR);
}

void startCalibration(uint32_t now) {
  devState = ST_CALIBRATING;
  calSum = 0; calCount = 0; calMin = 0xFFFF; calMax = 0;
  calDelayStart = now; calSampling = false;
  // Reset toti parametrii
  pefValue = fev1Value = fevTotalValue = 0.0f;
  pifValue1 = fiv1Value1 = virValue1 = 0.0f;
  pifValue2 = fiv1Value2 = virValue2 = 0.0f;
  fvcValue = itValue = 0.0f;
  peakExpir = peakInspir1 = peakInspir2 = 0;
  lastPeakExpir = lastPeakInspir1 = lastPeakInspir2 = 0;
  txPhase = 6; txIdx = 0;
}

void enterResult() {
  computeResults();
  devState = ST_RESULT;
  txPhase  = bleEnabledRTC ? 0 : 6;
  txIdx    = 0;
  if (bleEnabledRTC) bleStart();
}

// ══════════════════════════════════════════════════════════════════
// GRAPH
// ══════════════════════════════════════════════════════════════════

void scrollGraph() {
  int dev   = (int)smoothADC - (int)sigCenterRaw;
  int halfH = GRAPH_H / 2 - 1;
  int pix   = (GRAPH_SCALE > 0) ? dev * halfH / GRAPH_SCALE : 0;
  pix = constrain(pix, -halfH, halfH);
  memmove(graphBuf, graphBuf + 1, GRAPH_W - 1);
  graphBuf[GRAPH_W - 1] = (int8_t)pix;
}

void drawGraph() {
  display.drawRect(0, 44, SCREEN_W, 20, SSD1306_WHITE);

  // Linie prag expiratie (sus)
  int thrExpPix = constrain(BLOW_START_THR * (GRAPH_H/2-1) / GRAPH_SCALE, 0, GRAPH_H/2-1);
  for (int x = 0; x < GRAPH_W; x += 4)
    display.drawPixel(x, GRAPH_CY - thrExpPix, SSD1306_WHITE);

  // Linie prag inspiratie (jos)
  int thrInsPix = constrain(INSPIR_THR * (GRAPH_H/2-1) / GRAPH_SCALE, 0, GRAPH_H/2-1);
  for (int x = 2; x < GRAPH_W; x += 4)
    display.drawPixel(x, GRAPH_CY + thrInsPix, SSD1306_WHITE);

  // Linie zero
  for (int x = 0; x < GRAPH_W; x += 6)
    display.drawPixel(x, GRAPH_CY, SSD1306_WHITE);

  // Semnal
  for (int x = 0; x < GRAPH_W - 1; x++) {
    int y1 = constrain(GRAPH_CY - (int)graphBuf[x],     GRAPH_Y, GRAPH_Y+GRAPH_H-1);
    int y2 = constrain(GRAPH_CY - (int)graphBuf[x + 1], GRAPH_Y, GRAPH_Y+GRAPH_H-1);
    display.drawLine(x, y1, x+1, y2, SSD1306_WHITE);
  }
}

// ══════════════════════════════════════════════════════════════════
// ANIMATII
// ══════════════════════════════════════════════════════════════════

void wakeAnimation() {
  for (int s = 1; s <= 6; s++) {
    display.clearDisplay();
    for (int i = s; i >= 1; i--) {
      int w = i*22, h = i*11;
      display.drawRect(max(0,(SCREEN_W-w)/2), max(0,(SCREEN_H-h)/2),
                       min(SCREEN_W,w), min(SCREEN_H,h), SSD1306_WHITE);
    }
    display.display(); delay(70);
  }
  display.clearDisplay(); display.display(); delay(40);
}

void sleepAnimation() {
  for (int y = 0; y <= 31; y += 3) {
    display.clearDisplay();
    display.drawFastHLine(0, y,      SCREEN_W, SSD1306_WHITE);
    display.drawFastHLine(0, 63 - y, SCREEN_W, SSD1306_WHITE);
    display.display(); delay(18);
  }
  display.clearDisplay(); display.display();
}

// ══════════════════════════════════════════════════════════════════
// DISPLAY PRINCIPAL
// ══════════════════════════════════════════════════════════════════

void drawBluetooth(int x, int y, uint16_t col) {
  display.drawLine(x+3,y,   x+3,y+9, col);
  display.drawLine(x+3,y,   x+6,y+2, col);
  display.drawLine(x+6,y+2, x+3,y+5, col);
  display.drawLine(x+3,y+5, x+6,y+7, col);
  display.drawLine(x+6,y+7, x+3,y+9, col);
}

void drawBattery(int x, int y, int pct, uint16_t col) {
  display.drawRect(x, y, 18, 7, col);
  display.fillRect(x+18, y+2, 2, 3, col);
  int fw = constrain(16*pct/100, 0, 16);
  if (fw > 0) display.fillRect(x+1, y+1, fw, 5, col);
}

void drawInterface(uint32_t remainingMs, bool pressed, uint32_t holdMs) {
  uint32_t now = millis();
  display.clearDisplay();

  // Header inversat
  display.fillRect(0, 0, SCREEN_W, 10, SSD1306_WHITE);
  if (bleEnabled) {
    drawBluetooth(1, 0, SSD1306_BLACK);
    if (deviceConnected) display.fillCircle(9, 1, 1, SSD1306_BLACK);
  }

  // Label stare
  const char* lbl;
  switch (devState) {
    case ST_READY:       lbl = "READY";    break;
    case ST_CALIBRATING: lbl = calSampling ? "CAL..." : "HOLD.."; break;
    case ST_INSPIR_1:    lbl = ((now/300)%2==0) ? "INSPIRA" : "       "; break;
    case ST_EXPIR:       lbl = ((now/300)%2==0) ? "EXPIRA"  : "      "; break;
    case ST_INSPIR_2:    lbl = ((now/300)%2==0) ? "INSPIRA" : "       "; break;
    case ST_RESULT:      lbl = (txPhase<6) ? "TRIMIT" : "RESULT"; break;
    default:             lbl = "---"; break;
  }
  int lx = bleEnabled ? 10 : 0;
  int cx = lx + (108 - lx - (int)strlen(lbl)*6) / 2;
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(max(lx, cx), 1);
  display.print(lbl);
  drawBattery(109, 2, battPct(lipoV), SSD1306_BLACK);
  display.setTextColor(SSD1306_WHITE);
  display.drawFastHLine(0, 10, SCREEN_W, SSD1306_WHITE);

  // Corp
  display.setTextSize(1);

  if (devState == ST_CALIBRATING) {
    display.setCursor(0, 13);
    display.print(calSampling ? "Calibrare..." : "Stai nemiscat...");
    if (calSampling) {
      int pw = (int)((uint32_t)124 * calCount / CAL_SAMPLES);
      display.drawRect(2, 23, 124, 7, SSD1306_WHITE);
      if (pw > 0) display.fillRect(2, 23, pw, 7, SSD1306_WHITE);
    }
  } else {
    // PEF principal
    display.setCursor(0, 12);
    display.print("PEF");
    display.setTextSize(2);
    display.setCursor(0, 20);
    if (pefValue > 0.0f) display.print((int)pefValue);
    else                  display.print("---");
    display.setTextSize(1);

    // FEV1 si FVC pe aceeasi linie
    display.setCursor(0, 36);
    display.print("L/min");
    display.setCursor(52, 36);
    if (fev1Value > 0.0f) {
      display.print("F1:");
      display.print(fev1Value, 1);
      display.print("L");
    }
    display.setCursor(96, 36);
    if (itValue > 0.0f) {
      display.print("IT:");
      display.print((int)itValue);
      display.print("%");
    }

    // Bara de progres contextuala
    if (pressed && holdMs > 100) {
      int hw = (int)((uint32_t)68 * min(holdMs,(uint32_t)BLE_HOLD_MS) / BLE_HOLD_MS);
      display.drawRect(0, 43, 72, 1, SSD1306_WHITE);
      if (hw > 0) display.fillRect(0, 43, hw, 1, SSD1306_WHITE);
      display.setCursor(74, 43);
      display.print(bleEnabled ? "BLE OFF" : "BLE ON ");
    } else if (devState == ST_EXPIR && lastPeakExpir > 0) {
      uint32_t sinceP = now - lastPeakExpir;
      if (sinceP < (uint32_t)PEAK_HOLD_MS) {
        int bw = (int)((uint32_t)SCREEN_W * (PEAK_HOLD_MS - sinceP) / PEAK_HOLD_MS);
        display.drawFastHLine(0, 43, bw, SSD1306_WHITE);
      }
    } else if ((devState == ST_INSPIR_1 || devState == ST_INSPIR_2) && lastPeakInspir2 > 0) {
      uint32_t sinceP = now - lastPeakInspir2;
      if (sinceP < (uint32_t)INSPIR_HOLD_MS) {
        int bw = (int)((uint32_t)SCREEN_W * (INSPIR_HOLD_MS - sinceP) / INSPIR_HOLD_MS);
        display.drawFastHLine(0, 43, bw, SSD1306_WHITE);
      }
    } else if (devState == ST_READY) {
      int tw = (int)((uint32_t)(SCREEN_W-2) * remainingMs / AWAKE_MS);
      if (tw > 0) display.drawFastHLine(1, 43, tw, SSD1306_WHITE);
    }
  }

  drawGraph();
  display.display();
}

// ══════════════════════════════════════════════════════════════════
// SETUP
// ══════════════════════════════════════════════════════════════════

void setup() {
  gpio_hold_dis(PERIPH_PWR); gpio_hold_dis(AUX_PWR);
  gpio_deep_sleep_hold_dis();
  pinMode(PERIPH_PWR, OUTPUT); digitalWrite(PERIPH_PWR, HIGH);
  pinMode(AUX_PWR,    OUTPUT); digitalWrite(AUX_PWR,    HIGH);
  delay(100);

  analogSetPinAttenuation(SIG_PIN, ADC_11db);
  analogSetPinAttenuation(ADC_PIN, ADC_11db);
  pinMode(LED_PIN,  OUTPUT); led(false);
  pinMode(WAKE_PIN, INPUT_PULLUP);

  Serial.begin(115200); delay(100);
  bool wokeByButton = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO);
  Serial.printf("\n=== Spirometru v12 — %s bleRTC=%d ===\n",
                wokeByButton ? "trezit de buton" : "pornire", bleEnabledRTC);

  calibrateBlocking();

  Wire.begin(SDA_PIN, SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))
    Serial.println("[OLED] negasit");
  display.clearDisplay(); display.display();

  lipoV = readLipo(); lastLipo = millis();
  if (bleEnabledRTC) bleStart();
  wakeAnimation();
  if (wokeByButton)
    for (int i = 0; i < 2; i++) { led(true); delay(80); led(false); delay(80); }
  led(true);

  // ════════════════════════════════════════════════════════════
  // LOOP PRINCIPAL
  // ════════════════════════════════════════════════════════════

  uint32_t idleStart   = millis();
  uint32_t lastDisplay = 0;
  uint32_t lastBLEStr  = 0;
  uint32_t pressStart  = 0;
  bool     holdFired   = false;
  bool     wasPressed  = false;
  uint32_t inspir2WaitStart = 0;  // cand am intrat in asteptare INSPIR_2

  while (true) {
    uint32_t now = millis();

    if (bleEnabled && deviceConnected) idleStart = now;

    uint32_t elapsed   = now - idleStart;
    uint32_t remaining = (elapsed >= AWAKE_MS) ? 0 : (AWAKE_MS - elapsed);

    if (remaining == 0 &&
        devState != ST_INSPIR_1 &&
        devState != ST_EXPIR &&
        devState != ST_INSPIR_2 &&
        devState != ST_RESULT) break;

    bool pressed = (digitalRead(WAKE_PIN) == LOW);

    // ── Buton apasare ────────────────────────────────────────
    if (pressed && !wasPressed) {
      pressStart = now; holdFired = false; idleStart = now;
      switch (devState) {
        case ST_READY:
        case ST_RESULT:
          startCalibration(now);
          break;
        case ST_INSPIR_1:
        case ST_EXPIR:
        case ST_INSPIR_2:
          // Anulare manevra
          recIdx = 0;
          if (bleEnabledRTC && !bleEnabled) bleStart();
          startCalibration(now);
          Serial.println("[MANEVRA] anulata");
          break;
        case ST_CALIBRATING:
          break;
      }
    }
    if (!pressed) holdFired = false;
    uint32_t holdMs = pressed ? (now - pressStart) : 0;

    // Hold 2s → toggle BLE
    if (pressed && !holdFired && holdMs >= BLE_HOLD_MS) {
      holdFired = true;
      if (bleEnabled) bleStopUser(); else bleStart();
    }
    wasPressed = pressed;

    // Comenzi BLE remote
    if (cmdCal) {
      cmdCal = false;
      if (devState == ST_READY || devState == ST_RESULT) startCalibration(now);
    }
    if (cmdRst) {
      cmdRst = false; devState = ST_READY;
      recIdx = 0; txPhase = 6; txIdx = 0;
      pefValue = fev1Value = fevTotalValue = 0.0f;
      pifValue1 = fiv1Value1 = virValue1 = 0.0f;
      pifValue2 = fiv1Value2 = virValue2 = 0.0f;
      fvcValue = itValue = 0.0f;
    }

    // ── ADC + smooth ─────────────────────────────────────────
    rawADC = (uint16_t)analogRead(SIG_PIN);
    updateSmooth(rawADC);

    // ── STATE MACHINE ─────────────────────────────────────────

    switch (devState) {

      // ── READY ──────────────────────────────────────────────
      case ST_READY:
        // Detectie start inspiratie initiala (ADC scade sub ref)
        if ((int)smoothADC < (int)sigCenterRaw - INSPIR_THR) {
          devState          = ST_INSPIR_1;
          recIdx            = 0;
          recStartMs        = now;
          lastRecSample     = now;
          peakInspir1       = smoothADC;  // cel mai mic ADC = debit max inspir
          lastPeakInspir1   = now;
          recIdxInspir1Start= 0;
          idleStart         = now;
          if (bleEnabled) bleStopInternal();
          Serial.printf("[INSPIR_1] start smooth=%u ref=%u\n", smoothADC, sigCenterRaw);
        }
        // Detectie start expiratie directa (pacient a omis inspiratia)
        else if ((int)smoothADC > (int)sigCenterRaw + BLOW_START_THR) {
          devState          = ST_EXPIR;
          recIdx            = 0;
          recStartMs        = now;
          lastRecSample     = now;
          peakExpir         = smoothADC;
          lastPeakExpir     = now;
          recIdxExpirStart  = 0;
          idleStart         = now;
          if (bleEnabled) bleStopInternal();
          Serial.printf("[EXPIR] start direct smooth=%u ref=%u\n", smoothADC, sigCenterRaw);
        }
        break;

      // ── INSPIRATIE 1 ───────────────────────────────────────
      case ST_INSPIR_1: {
        // Urmareste minimul (debit inspirator maxim)
        if (smoothADC < peakInspir1) {
          peakInspir1     = smoothADC;
          lastPeakInspir1 = now;
        }

        // Inregistrare
        if (recIdx < REC_SIZE && (now - lastRecSample) >= (1000UL/REC_HZ)) {
          recBuf[recIdx++] = rawADC;
          lastRecSample    = now;
        }

        // Detectie sfarsit inspiratie 1:
        // Semnalul revine spre ref sau trece peste ref (incepe expiratie)
        bool inspirDone = (lastPeakInspir1 > 0 &&
                           (now - lastPeakInspir1) >= (uint32_t)INSPIR_HOLD_MS &&
                           smoothADC > (uint16_t)((int)sigCenterRaw - INSPIR_THR/2));
        bool expirStarted = ((int)smoothADC > (int)sigCenterRaw + BLOW_START_THR);

        if (inspirDone || expirStarted) {
          recIdxInspir1End = recIdx;
          devState         = ST_EXPIR;
          peakExpir        = smoothADC;
          lastPeakExpir    = now;
          recIdxExpirStart = recIdx;
          Serial.printf("[INSPIR_1] done  peak=%u  samples=%u → EXPIR\n",
                        peakInspir1, recIdxInspir1End);
        }

        // Timeout siguranta
        if ((now - recStartMs) >= 8000UL) {
          recIdxInspir1End = recIdx;
          devState         = ST_EXPIR;
          recIdxExpirStart = recIdx;
          peakExpir        = sigCenterRaw;
          lastPeakExpir    = now;
          Serial.println("[INSPIR_1] timeout → EXPIR");
        }
        break;
      }

      // ── EXPIRATIE ──────────────────────────────────────────
      case ST_EXPIR: {
        if (smoothADC > peakExpir) {
          peakExpir     = smoothADC;
          lastPeakExpir = now;
          pefValue      = adcToPef(peakExpir);
        }

        if (recIdx < REC_SIZE && (now - lastRecSample) >= (1000UL/REC_HZ)) {
          recBuf[recIdx++] = rawADC;
          lastRecSample    = now;
        }

        bool expirDone = (lastPeakExpir > 0 &&
                          (now - lastPeakExpir) >= (uint32_t)PEAK_HOLD_MS &&
                          smoothADC < (uint16_t)((int)sigCenterRaw + BLOW_START_THR/2));

        if (expirDone) {
          recIdxExpirEnd    = recIdx;
          devState          = ST_INSPIR_2;
          peakInspir2       = sigCenterRaw;
          lastPeakInspir2   = 0;
          recIdxInspir2Start= recIdx;
          inspir2WaitStart  = now;
          Serial.printf("[EXPIR] done  peak=%u PEF=%.1f samples=%u → INSPIR_2\n",
                        peakExpir, pefValue, recIdxExpirEnd - recIdxExpirStart);
        }

        if ((now - recStartMs) >= (uint32_t)(REC_MAX_S * 1000UL)) {
          recIdxExpirEnd = recIdx;
          enterResult();
          idleStart = now;
          Serial.println("[EXPIR] timeout → RESULT");
        }
        break;
      }

      // ── INSPIRATIE 2 ───────────────────────────────────────
      case ST_INSPIR_2: {
        // Detectie start inspiratie 2
        if ((int)smoothADC < (int)sigCenterRaw - INSPIR_THR) {
          if (lastPeakInspir2 == 0) {
            lastPeakInspir2 = now;
            peakInspir2     = smoothADC;
          }
          if (smoothADC < peakInspir2) {
            peakInspir2     = smoothADC;
            lastPeakInspir2 = now;
          }
        }

        if (recIdx < REC_SIZE && (now - lastRecSample) >= (1000UL/REC_HZ)) {
          recBuf[recIdx++] = rawADC;
          lastRecSample    = now;
        }

        // Sfarsit inspiratie 2
        bool inspir2Done = (lastPeakInspir2 > 0 &&
                            (now - lastPeakInspir2) >= (uint32_t)INSPIR_HOLD_MS);

        // Timeout asteptare inspiratie 2
        bool inspir2Timeout = ((now - inspir2WaitStart) >= (uint32_t)INSPIR_WAIT_MS &&
                               lastPeakInspir2 == 0);

        if (inspir2Done || inspir2Timeout) {
          if (inspir2Timeout) {
            Serial.println("[INSPIR_2] timeout (pacient nu a inspirat)");
            // Rezultat calculat fara inspiratia finala
            virValue2  = 0.0f;
            pifValue2  = 0.0f;
            fiv1Value2 = 0.0f;
          }
          enterResult();
          idleStart = now;
          Serial.printf("[INSPIR_2] done  peak=%u PIF=%.1f\n",
                        peakInspir2, adcDiffToPif(peakInspir2));
        }
        break;
      }

      // ── RESULT ─────────────────────────────────────────────
      case ST_RESULT:
        break;
    }

    // ── Graf scroll ──────────────────────────────────────────
    if (now - lastGraphScroll >= GRAPH_SCROLL_MS) {
      lastGraphScroll = now;
      scrollGraph();
    }

    // ── Baterie ──────────────────────────────────────────────
    if (now - lastLipo >= LIPO_READ_MS) {
      lastLipo = now; lipoV = readLipo();
    }

    // ════════════════════════════════════════════════════════
    // BLE DATA
    // ════════════════════════════════════════════════════════

    if (bleEnabled && deviceConnected) {
      // Streaming READY
      if (devState == ST_READY || devState == ST_CALIBRATING) {
        if (now - lastBLEStr >= BLE_STREAM_MS) {
          lastBLEStr = now;
          char buf[14];
          snprintf(buf, sizeof(buf), "R:%u", rawADC);
          pNotify->setValue(buf); pNotify->notify();
        }
      }

      // Trimitere rezultate
      else if (devState == ST_RESULT && txPhase < 6) {
        if (now - lastTxMs >= BLE_TX_MS) {
          lastTxMs = now;
          char b[64];
          switch (txPhase) {
            case 0:
              snprintf(b, sizeof(b), "MEAS_START:N=%u,RATE=%d", recIdx, REC_HZ);
              pNotify->setValue(b); pNotify->notify();
              txPhase = 1; txIdx = 0;
              break;
            case 1:
              if (txIdx < recIdx) {
                snprintf(b, sizeof(b), "R:%u", recBuf[txIdx++]);
                pNotify->setValue(b); pNotify->notify();
              } else { txPhase = 2; }
              break;
            case 2:
              snprintf(b, sizeof(b), "PEF:%.1f", pefValue);
              pNotify->setValue(b); pNotify->notify(); txPhase = 3; break;
            case 3:
              snprintf(b, sizeof(b), "FEV1:%.2f", fev1Value);
              pNotify->setValue(b); pNotify->notify(); txPhase = 4; break;
            case 4:
              snprintf(b, sizeof(b), "FEVT:%.2f", fevTotalValue);
              pNotify->setValue(b); pNotify->notify(); txPhase = 5; break;
            case 5:
              snprintf(b, sizeof(b), "PIF1:%.1f", pifValue1);
              pNotify->setValue(b); pNotify->notify(); txPhase = 7; break;
            case 7:
              snprintf(b, sizeof(b), "FIV1:%.2f", fiv1Value2);
              pNotify->setValue(b); pNotify->notify(); txPhase = 8; break;
            case 8:
              snprintf(b, sizeof(b), "VIR:%.2f", (virValue1+virValue2)/2.0f);
              pNotify->setValue(b); pNotify->notify(); txPhase = 9; break;
            case 9:
              snprintf(b, sizeof(b), "FVC:%.2f", fvcValue);
              pNotify->setValue(b); pNotify->notify(); txPhase = 10; break;
            case 10:
              snprintf(b, sizeof(b), "IT:%.1f", itValue);
              pNotify->setValue(b); pNotify->notify(); txPhase = 11; break;
            case 11:
              pNotify->setValue("MEAS_END"); pNotify->notify();
              txPhase = 12; break;
            case 12:
              txPhase = 6; devState = ST_READY; idleStart = millis();
              Serial.println("[TX] complet → READY");
              break;
          }
        }
      }
    } else if (!bleEnabled && devState == ST_RESULT && txPhase == 0) {
      txPhase = 6;
    }

    // ── Display 20Hz ────────────────────────────────────────
    if (now - lastDisplay >= 50) {
      lastDisplay = now;
      if (devState == ST_EXPIR || devState == ST_INSPIR_1 || devState == ST_INSPIR_2)
        led((now/150) % 2);
      else if (devState == ST_CALIBRATING)
        led((now/400) % 2);
      else
        led(!pressed);
      drawInterface(remaining, pressed, holdMs);
    }

    delay(1);
  }

  // ── Sleep ────────────────────────────────────────────────
  led(false);
  if (bleInitialised) { BLEDevice::stopAdvertising(); BLEDevice::deinit(true); }
  sleepAnimation();
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  Wire.end();
  while (digitalRead(WAKE_PIN) == LOW) delay(10);
  delay(50);
  digitalWrite(PERIPH_PWR, LOW); digitalWrite(AUX_PWR, LOW); delay(20);
  gpio_hold_en(PERIPH_PWR); gpio_hold_en(AUX_PWR);
  gpio_deep_sleep_hold_en();
  esp_deep_sleep_enable_gpio_wakeup(1ULL << WAKE_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  Serial.println("[SYS] sleep..."); Serial.flush();
  esp_deep_sleep_start();
}

void loop() {}
