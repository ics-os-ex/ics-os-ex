/*
 * ICS-OS serial bridge / boot coprocessor
 * Board : Waveshare ESP32-S3-LCD-1.47 (ESP32-S3R8N16, native USB CDC)
 *
 * Captures the target machine's ICS-OS COM1 (115200 8N1) boot log on the
 * onboard UART0 (reserved header: RXD=GPIO44, TXD=GPIO43) and mirrors it to:
 *   - the host PC over native USB CDC (Serial),
 *   - the 172x320 ST7789 LCD (live scrolling console),
 *   - a TF/SD card in the slot (persistent capture, /sdcard/icsos-<id>.log).
 * Host keystrokes on the CDC port are forwarded to the target COM1 RX (GPIO43)
 * so the GRUB serial console stays reachable.
 *
 * Soldering prerequisite: the reserved UART header needs R6 (1K, RXD) and
 * R7 (499R, TXD) populated - see README.md.
 */

#include <Arduino.h>
#include <SPI.h>
#include <SD_MMC.h>
#include "font5x7.h"
#include "bridge_console.h"             // line-buffer logic (TAP-tested host-side)

// ---------------------------------------------------------------------------
// Pin map (verified from the Waveshare schematic + Waveshare demo firmware)
// ---------------------------------------------------------------------------
// Native USB (Type-A port J1): GPIO19=D-, GPIO20=D+ - host link, CDC serial.
#define UART0_RX_PIN  44   // reserved header RXD -> target COM1 TX  (R6, 1K)
#define UART0_TX_PIN  43   // reserved header TXD -> target COM1 RX  (R7, 499R)
#define BRIDGE_BAUD   115200UL   // must match ICS-OS serial.c / GRUB

// ST7789 172x320 SPI LCD (demo: Display_ST7789.h)
#define LCD_MISO      -1
#define LCD_MOSI      45
#define LCD_SCLK      40
#define LCD_CS        42
#define LCD_DC        41
#define LCD_RST       39
#define LCD_BL        48
#define LCD_OFFSET_X  34       // panel is 240 wide, active window offset (demo)
#define LCD_OFFSET_Y  0

// TF card, SD_MMC (demo: SD_Card.h)
#define SD_CLK_PIN    14
#define SD_CMD_PIN    15
#define SD_D0_PIN     16
#define SD_D1_PIN     18
#define SD_D2_PIN     17
#define SD_D3_PIN     21

// ---------------------------------------------------------------------------
// Console geometry: 6x8 px cells on the 172x320 portrait panel
// ---------------------------------------------------------------------------
#define CON_W        172
#define CON_H        320
#define CELL_W       6
#define CELL_H       8
#define COLS         (CON_W / CELL_W)      // 28
#define ROWS         (CON_H / CELL_H)      // 40
#define LOG_LINES    (ROWS - 1)            // top row = status bar
#define STATUS_ROWS  1

// Colors (RGB565)
#define C_BLACK  0x0000
#define C_GREEN  0x07E0
#define C_CYAN   0x07FF
#define C_WHITE  0xFFFF
#define C_RED    0xF800

// SD capture
#define LOG_ROTATE_BYTES  (512UL * 1024UL)

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static SPIClass lcdSpi(FSPI);
static uint8_t lbuf[4];                    // scratch for SPI writes
static uint16_t *frame;                    // one console row in RGB565 (CON_W px)

static bool lcd_ok = false;
static bool sd_ok = false;
static File logFile;
static uint32_t logBytes = 0;

// Console text (screen order: line 0 = just under the status bar,
// line LOG_LINES-1 = bottom row) -- logic in bridge_console.c (TAP-tested
// host-side via `make test-bridge-console-unit`).
static bridge_console_t cons;
static uint8_t statusDirty = 0xFF;         // bit0=status bar, bit1=log area

static uint32_t lastFlush = 0;
static uint32_t lastHostActivity = 0;

static char hostEcho[COLS + 1];
static int hostPos = 0;

// ---------------------------------------------------------------------------
// ST7789 driver (init sequence copied verbatim from the Waveshare demo)
// ---------------------------------------------------------------------------
static inline void lcdCmd(uint8_t cmd) {
  lcdSpi.beginTransaction(SPISettings(80000000, MSBFIRST, SPI_MODE0));
  digitalWrite(LCD_CS, LOW);
  digitalWrite(LCD_DC, LOW);
  lcdSpi.transfer(cmd);
  digitalWrite(LCD_CS, HIGH);
  lcdSpi.endTransaction();
}

static inline void lcdData(const uint8_t *d, uint32_t n) {
  lcdSpi.beginTransaction(SPISettings(80000000, MSBFIRST, SPI_MODE0));
  digitalWrite(LCD_CS, LOW);
  digitalWrite(LCD_DC, HIGH);
  lcdSpi.writeBytes(d, n);
  digitalWrite(LCD_CS, HIGH);
  lcdSpi.endTransaction();
}

static void lcdSetWin(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  // Portrait (VERTICAL) mapping, as in the demo: X->0x2B row, Y->0x2A col
  lcdCmd(0x2A);
  lbuf[0] = y0 >> 8; lbuf[1] = y0 + LCD_OFFSET_Y;
  lbuf[2] = y1 >> 8; lbuf[3] = y1 + LCD_OFFSET_Y;
  lcdData(lbuf, 4);
  lcdCmd(0x2B);
  lbuf[0] = x0 >> 8; lbuf[1] = x0 + LCD_OFFSET_X;
  lbuf[2] = x1 >> 8; lbuf[3] = x1 + LCD_OFFSET_X;
  lcdData(lbuf, 4);
  lcdCmd(0x2C);
}

static void lcdInit() {
  static const uint8_t gE0[] = {0xF0,0x00,0x04,0x04,0x04,0x05,0x29,0x33,0x3E,0x38,0x12,0x12,0x28,0x30};
  static const uint8_t gE1[] = {0xF0,0x07,0x0A,0x0D,0x0B,0x07,0x28,0x33,0x3E,0x36,0x14,0x14,0x29,0x32};
  static const uint8_t gB2[] = {0x0C,0x0C,0x00,0x33,0x33};
  static const uint8_t gD0[] = {0xA4, 0xA1};

  pinMode(LCD_CS, OUTPUT);
  pinMode(LCD_DC, OUTPUT);
  pinMode(LCD_RST, OUTPUT);
  digitalWrite(LCD_CS, HIGH);
  lcdSpi.begin(LCD_SCLK, LCD_MISO, LCD_MOSI);

  ledcSetup(0, 1000, 8);                   // 8-bit duty
  ledcAttachPin(LCD_BL, 0);
  ledcWrite(0, 128);                       // ~50% backlight

  digitalWrite(LCD_CS, LOW);
  delay(50);
  digitalWrite(LCD_RST, LOW);
  delay(50);
  digitalWrite(LCD_RST, HIGH);
  delay(50);
  digitalWrite(LCD_CS, HIGH);

  lcdCmd(0x11); delay(120);
  lcdCmd(0x36); lbuf[0] = 0x70; lcdData(lbuf, 1);   // portrait
  lcdCmd(0x3A); lbuf[0] = 0x05; lcdData(lbuf, 1);   // 16-bit pixels
  lcdCmd(0xB0); lbuf[0] = 0x00; lbuf[1] = 0xE8; lcdData(lbuf, 2);
  lcdCmd(0xB2); lcdData(gB2, 5);
  lcdCmd(0xB7); lbuf[0] = 0x35; lcdData(lbuf, 1);
  lcdCmd(0xBB); lbuf[0] = 0x35; lcdData(lbuf, 1);
  lcdCmd(0xC0); lbuf[0] = 0x2C; lcdData(lbuf, 1);
  lcdCmd(0xC2); lbuf[0] = 0x01; lcdData(lbuf, 1);
  lcdCmd(0xC3); lbuf[0] = 0x13; lcdData(lbuf, 1);
  lcdCmd(0xC4); lbuf[0] = 0x20; lcdData(lbuf, 1);
  lcdCmd(0xC6); lbuf[0] = 0x0F; lcdData(lbuf, 1);
  lcdCmd(0xD0); lcdData(gD0, 2);
  lcdCmd(0xD6); lbuf[0] = 0xA1; lcdData(lbuf, 1);
  lcdCmd(0xE0); lcdData(gE0, sizeof gE0);
  lcdCmd(0xE1); lcdData(gE1, sizeof gE1);
  lcdCmd(0x21);
  lcdCmd(0x11); delay(120);
  lcdCmd(0x29);
  lcd_ok = true;
}

// ---------------------------------------------------------------------------
// 5x7 font rendering
// ---------------------------------------------------------------------------
static void putGlyph(uint16_t *dst, uint16_t fg, uint16_t bg, char c) {
  if ((unsigned char)c < 0x20 || c == 0x7F) c = ' ';
  const uint8_t *g = &font5x7[(c - 0x20) * 5];
  for (uint8_t row = 0; row < 7; row++) {
    for (uint8_t col = 0; col < 5; col++) {
      *dst++ = ((g[col] >> row) & 1) ? fg : bg;
    }
    *dst++ = bg;                           // 6th px of the cell (1 px gutter)
  }
  *dst++ = bg;                             // 8th row of the cell
}

static void renderRow(int i, uint16_t fg, uint16_t bg) {
  // Log line i (0-based) is drawn at screen row (i + STATUS_ROWS).
  const char *s = bridge_console_line(&cons, i);
  uint32_t n = strlen(s);
  if (n > COLS) n = COLS;
  for (uint16_t x = 0; x < CON_W; x++) frame[x] = bg;
  for (uint32_t k = 0; k < n; k++) putGlyph(&frame[k * CELL_W], fg, bg, s[k]);
  int sy = (i + STATUS_ROWS) * CELL_H;
  lcdSetWin(0, sy, CON_W - 1, sy + CELL_H - 1);
  lcdData((uint8_t *)frame, CON_W * CELL_H * 2);
}

static void renderStatus(const char *s, uint16_t fg) {
  for (uint16_t x = 0; x < CON_W; x++) frame[x] = C_BLACK;
  uint32_t n = strlen(s);
  if (n > COLS) n = COLS;
  for (uint32_t k = 0; k < n; k++) putGlyph(&frame[k * CELL_W], fg, C_BLACK, s[k]);
  lcdSetWin(0, 0, CON_W - 1, CELL_H - 1);
  lcdData((uint8_t *)frame, CON_W * CELL_H * 2);
}

static void renderAll() {
  if (!lcd_ok) return;
  for (int i = 0; i < LOG_LINES; i++) {
    renderRow(i, C_GREEN, C_BLACK);
    bridge_console_row_rendered(&cons, i);
  }
  char s[COLS + 1];
  snprintf(s, sizeof s, "ICS-OS BRIDGE  SD:%s", sd_ok ? "ok" : "NO");
  renderStatus(s, sd_ok ? C_GREEN : C_RED);
  statusDirty = 0;
}

/* Render every dirty row in one pass. A full 39-row refresh is ~11 ms of
   80 MHz SPI (39 x 172 x 8 x 2 bytes); the UART RX ring buffer (~1 KB,
   ~8.7 ms at 115200) plus the 64-byte 16550 FIFO on the target side absorb
   the brief stall, so no throttling is needed. */
static void renderDirty() {
  if (!lcd_ok || statusDirty == 0) return;
  if (statusDirty & 1) {
    char s[COLS + 1];
    snprintf(s, sizeof s, "ICS-OS BRIDGE  SD:%s", sd_ok ? "ok" : "NO");
    renderStatus(s, sd_ok ? C_GREEN : C_RED);
    statusDirty &= ~1;
  }
  if (statusDirty & 2) {
    for (int i = 0; i < LOG_LINES; i++) {
      if (bridge_console_dirty(&cons, i)) {
        renderRow(i, C_GREEN, C_BLACK);
        bridge_console_row_rendered(&cons, i);
      }
    }
    statusDirty &= ~2;
  }
}

// ---------------------------------------------------------------------------
// Console text handling (line-buffer logic shared with the host TAP test)
// ---------------------------------------------------------------------------
static void consoleChar(char c) {
  bridge_console_putc(&cons, c);
  /* Cheap: renderDirty() only repaints rows whose dirty bit is set, so
     raising the log-area flag for every byte costs nothing when nothing
     changed. */
  if ((unsigned char)c >= 0x20 || c == '\b' || c == '\t' || c == '\n')
    statusDirty |= 2;
}

// ---------------------------------------------------------------------------
// SD capture
// ---------------------------------------------------------------------------
static void sdInit() {
  if (!SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN)) {
    sd_ok = false;
    return;
  }
  // 1-bit mode + format-on-mount-failure, exactly as the Waveshare demo
  // (the card ships unformatted; the demo is the reference for this slot).
  if (!SD_MMC.begin("/sdcard", true, true)) {
    sd_ok = false;
    return;
  }
  sd_ok = (SD_MMC.cardType() != CARD_NONE);
  if (!sd_ok) return;

  // New log file per boot; rotate away from any previous capture.
  String path = String("/sdcard/icsos-") + String((uint32_t)(millis() + esp_random())) + ".log";
  if (SD_MMC.exists(path.c_str())) SD_MMC.remove(path.c_str());
  logFile = SD_MMC.open(path.c_str(), FILE_WRITE);
  if (!logFile) { sd_ok = false; return; }
  char hdr[160];
  int n = snprintf(hdr, sizeof hdr,
    "=== ICS-OS boot capture, %llu MB card, %llu MB free ===\n",
    SD_MMC.totalBytes() / 1048576,
    (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / 1048576);
  logFile.write((const uint8_t *)hdr, n);
  logFile.flush();
}

static void sdWrite(const uint8_t *d, uint32_t n) {
  if (!sd_ok || !logFile) return;
  if (logBytes + n > LOG_ROTATE_BYTES) {   // keep only the last 512 KB
    logFile.close();
    logFile = SD_MMC.open(logFile.name(), FILE_WRITE);
    if (!logFile) { sd_ok = false; return; }
    logBytes = 0;
  }
  logFile.write(d, n);
  logBytes += n;
  uint32_t now = millis();
  if (now - lastFlush > 500) { logFile.flush(); lastFlush = now; }
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(BRIDGE_BAUD);            // native USB CDC (baud ignored)
  Serial0.begin(BRIDGE_BAUD, SERIAL_8N1, UART0_RX_PIN, UART0_TX_PIN);

  // Wait briefly for USB enumeration so the host monitor can see the banner.
  unsigned long t0 = millis();
  while (millis() - t0 < 300 && !Serial) { delay(5); }

  frame = (uint16_t *)malloc(CON_W * CELL_H);
  bridge_console_init(&cons);
  lcdInit();
  sdInit();
  renderAll();

  // Self-test banner on the host link.
  Serial.println();
  Serial.println("=================================================");
  Serial.println(" ICS-OS serial bridge (ESP32-S3-LCD-1.47)");
  Serial.printf(" UART0 %lu 8N1  TX=GPIO%d RX=GPIO%d\n",
                BRIDGE_BAUD, UART0_TX_PIN, UART0_RX_PIN);
  Serial.printf(" LCD %dx%d %s | SD card: %s\n", CON_W, CON_H,
                lcd_ok ? "ok" : "FAIL", sd_ok ? "ok" : "NO");
  Serial.println(" Target COM1 output will stream here.");
  Serial.println(" Keystrokes are forwarded to target COM1 RX.");
  Serial.println("=================================================");
  lastHostActivity = millis();
}

void loop() {
  // 1) Target -> host/LCD/SD
  int n = Serial0.available();
  while (n-- > 0) {
    uint8_t c = Serial0.read();
    Serial.write(c);
    sdWrite(&c, 1);
    consoleChar((char)c);
  }

  // 2) Host -> target (GRUB / IPL serial input)
  while (Serial.available()) {
    uint8_t c = Serial.read();
    Serial0.write(c);
    lastHostActivity = millis();
    if (c >= 0x20 && hostPos < COLS - 1) {
      hostEcho[hostPos++] = c;
    } else if (c == '\n' || c == '\r') {
      hostEcho[0] = 0; hostPos = 0;
    }
    if (hostPos > 0) {
      char s[COLS + 1];
      snprintf(s, sizeof s, "> %s", hostEcho);
      renderStatus(s, C_CYAN);
    }
  }

  // 3) Periodic housekeeping
  uint32_t now = millis();
  if (hostPos > 0 && now - lastHostActivity > 2000) {
    hostEcho[0] = 0; hostPos = 0;
    char s[COLS + 1];
    snprintf(s, sizeof s, "ICS-OS BRIDGE  SD:%s", sd_ok ? "ok" : "NO");
    renderStatus(s, sd_ok ? C_GREEN : C_RED);
  }
  renderDirty();
}
