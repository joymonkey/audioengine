#include "config.h"

// ===================================
// Global Variable Definitions
// ===================================

// File Systems
SdFat sd;
I2S i2s(OUTPUT, I2S_BCLK, I2S_DATA, I2S_LRCK);

// Audio Configuration
// We pre-calculate (attenuation_0_100 * 256 / 100) -> 0-256
volatile int16_t masterAttenMultiplier = (97 * 256) / 100; // Default 97%

// Bank 1 File List (Flash)
SoundFile bank1Sounds[MAX_SOUNDS];
int bank1SoundCount = 0;
char bank1DirName[64] = "";
char activeBank1Page = 'A'; 

// SD Banks Structure (Banks 2-6)
SDBank sdBanks[MAX_SD_BANKS];
int sdBankCount = 0;

// Root Tracks (Legacy Compatibility)
char rootTracks[MAX_ROOT_TRACKS][16];
int rootTrackCount = 0;

// Test Tone State
volatile bool testToneActive = false;
volatile uint32_t testTonePhase = 0;

// Filename Checksum
uint32_t globalFilenameChecksum = 0;


// ===================================
// Global Mutex Definitions
// ===================================
__attribute__((section(".mutex_array"))) mutex_t sd_mutex;
__attribute__((section(".mutex_array"))) mutex_t flash_mutex;
__attribute__((section(".mutex_array"))) mutex_t log_mutex;

// ===================================
// Logging Helper
// ===================================
void log_message(const String& msg) {
    mutex_enter_blocking(&log_mutex);
    Serial.println(msg);
    mutex_exit(&log_mutex);
}
