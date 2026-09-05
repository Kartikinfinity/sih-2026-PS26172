// ESP32-S3-N16R8 hardware verification.
//
// Purpose: confirm the board boots, the toolchain is correct, and that the
// flash/PSRAM the firmware actually detects matches what platformio.ini
// declares (16 MB flash + 8 MB octal PSRAM).
//
// This sketch touches no peripherals. The INMP441 is deliberately not
// initialised or referenced here.

#include <Arduino.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

namespace {

const char *chipModelName(esp_chip_model_t model) {
  switch (model) {
    case CHIP_ESP32:   return "ESP32";
    case CHIP_ESP32S2: return "ESP32-S2";
    case CHIP_ESP32S3: return "ESP32-S3";
    case CHIP_ESP32C3: return "ESP32-C3";
    default:           return "unknown";
  }
}

// Bytes -> a human-sized string, e.g. "16 MB" / "8192 KB".
void printSize(const char *label, uint64_t bytes) {
  Serial.printf("  %-22s %llu bytes", label, (unsigned long long)bytes);
  if (bytes >= (1024ULL * 1024ULL)) {
    Serial.printf("  (%.2f MB)", (double)bytes / (1024.0 * 1024.0));
  } else if (bytes >= 1024ULL) {
    Serial.printf("  (%.2f KB)", (double)bytes / 1024.0);
  }
  Serial.println();
}

void printChipInfo() {
  esp_chip_info_t info;
  esp_chip_info(&info);

  Serial.println("[ CHIP ]");
  Serial.printf("  %-22s %s\n",   "model", chipModelName(info.model));
  Serial.printf("  %-22s %d\n",   "revision", info.revision);
  Serial.printf("  %-22s %d\n",   "cores", info.cores);
  Serial.printf("  %-22s %u MHz\n", "cpu frequency", (unsigned)getCpuFrequencyMhz());
  Serial.printf("  %-22s %s\n",   "sdk version", ESP.getSdkVersion());
  Serial.printf("  %-22s %012llx\n", "efuse mac", ESP.getEfuseMac());
  Serial.printf("  %-22s %s%s%s%s\n", "features",
                (info.features & CHIP_FEATURE_WIFI_BGN) ? "wifi " : "",
                (info.features & CHIP_FEATURE_BT)       ? "bt "   : "",
                (info.features & CHIP_FEATURE_BLE)      ? "ble "  : "",
                (info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded-flash" : "");
  Serial.println();
}

void printFlashInfo() {
  Serial.println("[ FLASH ]");

  // What the running firmware/bootloader image believes.
  printSize("configured size", (uint64_t)ESP.getFlashChipSize());

  // What the flash chip itself reports over SPI -- the real hardware answer.
  uint32_t detected = 0;
  if (esp_flash_get_size(NULL, &detected) == ESP_OK) {
    printSize("detected size", (uint64_t)detected);
  } else {
    Serial.println("  detected size          <query failed>");
  }

  Serial.printf("  %-22s %u Hz\n", "speed", (unsigned)ESP.getFlashChipSpeed());
  Serial.printf("  %-22s %d\n",    "mode", (int)ESP.getFlashChipMode());
  Serial.printf("  %-22s %u bytes\n", "sketch size", (unsigned)ESP.getSketchSize());
  Serial.println();
}

void printPsramInfo() {
  Serial.println("[ PSRAM ]");

  if (!psramFound()) {
    Serial.println("  NOT FOUND");
    Serial.println("  -> Expected 8 MB. Check board_build.arduino.memory_type");
    Serial.println("     in platformio.ini (qio_opi vs opi_opi).");
    Serial.println();
    return;
  }

  printSize("total", (uint64_t)ESP.getPsramSize());
  printSize("free",  (uint64_t)ESP.getFreePsram());
  printSize("largest free block",
            (uint64_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
  Serial.println();
}

void printInternalHeap() {
  Serial.println("[ INTERNAL RAM ]");
  printSize("heap size", (uint64_t)ESP.getHeapSize());
  printSize("free heap", (uint64_t)ESP.getFreeHeap());
  Serial.println();
}

// Compare what we found against what an N16R8 module should have.
void printExpectationCheck() {
  const uint64_t kExpectedFlash = 16ULL * 1024 * 1024;
  const uint64_t kExpectedPsram = 8ULL * 1024 * 1024;

  const uint64_t flash = (uint64_t)ESP.getFlashChipSize();
  const uint64_t psram = psramFound() ? (uint64_t)ESP.getPsramSize() : 0;

  // PSRAM is reported slightly under 8 MB once the allocator takes its cut,
  // so allow a small margin rather than demanding an exact match.
  const bool flashOk = (flash >= kExpectedFlash);
  const bool psramOk = (psram >= (kExpectedPsram - (512ULL * 1024)));

  Serial.println("[ N16R8 EXPECTATION CHECK ]");
  Serial.printf("  %-22s %s\n", "16 MB flash", flashOk ? "PASS" : "FAIL");
  Serial.printf("  %-22s %s\n", "8 MB psram",  psramOk ? "PASS" : "FAIL");
  Serial.println();
  Serial.println(flashOk && psramOk
                     ? ">>> HARDWARE MATCHES ESP32-S3-N16R8"
                     : ">>> MISMATCH - see sections above");
  Serial.println();
}

}  // namespace

void setup() {
  Serial.begin(921600);

  // Give the USB-serial bridge a moment so the banner is not truncated.
  delay(500);

  Serial.println();
  Serial.println("========================================================");
  Serial.println(" ESP32-S3-N16R8  |  HARDWARE VERIFICATION");
  Serial.println(" boot OK - no peripherals initialised");
  Serial.println("========================================================");
  Serial.println();

  printChipInfo();
  printFlashInfo();
  printPsramInfo();
  printInternalHeap();
  printExpectationCheck();

  Serial.println("========================================================");
  Serial.println(" end of report - idling");
  Serial.println("========================================================");
}

void loop() {
  // Slow heartbeat so it is obvious the board is alive and not resetting.
  delay(5000);
  Serial.printf("alive  uptime=%lus  free_heap=%u  free_psram=%u\n",
                (unsigned long)(millis() / 1000),
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getFreePsram());
}
