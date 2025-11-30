/*
 * CHIRP Audio Engine
 * Version: 20251128
 * 
 * Description:
 * Multi-stream audio playback engine for RP2350 (Pimoroni Pico Plus 2).
 * Supports simultaneous playback of up to 3 streams (WAV or MP3).
 * 
 * Features:
 * - 3 Independent Audio Streams
 * - Dual MP3 Decoders (Helix) running on Core 0
 * - Ring Buffers in PSRAM (512KB per stream) for glitch-free playback
 * - Automatic mixing on Core 1
 * - Dynamic resource allocation for decoders
 * - Robust WAV/MP3 file handling with auto-stop
 */

#include "config.h"
#include <CRC32.h> // For checksum


// ===================================
// Button Configuration
// ===================================
#define PIN_BTN_NAV 16 // Start/Stop
#define PIN_BTN_FWD 17 // Next
#define PIN_BTN_REV 18 // Prev

// ===================================
// Global Variable Definitions
// (ALL MOVED to serial_commands.cpp / audio_playback.cpp)
// ===================================


// ===================================
// NEW FUNCTION: Calculate Checksum
// ===================================
void calculateGlobalChecksum() {
    Serial.print("Calculating filename checksum... ");
    CRC32 crc;

    // 1. Checksum Bank 1 (Flash) variant filenames
    for (int i = 0; i < bank1SoundCount; i++) {
        for (int v = 0; v < bank1Sounds[i].variantCount; v++) {
            crc.update(bank1Sounds[i].variants[v], strlen(bank1Sounds[i].variants[v]));
        }
    }

    // 2. Checksum Banks 2-6 (SD) filenames
    for (int i = 0; i < sdBankCount; i++) {
        for (int f = 0; f < sdBanks[i].fileCount; f++) {
            crc.update(sdBanks[i].files[f], strlen(sdBanks[i].files[f]));
        }
    }

    globalFilenameChecksum = crc.finalize();
    Serial.println(globalFilenameChecksum);
}

// ===================================
// SETUP (Core 0)
// ===================================
void setup() {
    // Initialize SPI1 FIRST
    SPI1.setRX(SD_MISO);
    SPI1.setTX(SD_MOSI);
    SPI1.setSCK(SD_SCK);
    
    // Serial
    Serial.begin(115200);   // USB serial
    // (Serial2 "UART" fix)
    Serial2.setTX(UART_TX);
    Serial2.setRX(UART_RX);
    Serial2.begin(9600);
    delay(200);
    
    Serial.println("\n╔═══════════════════════════════════════╗");
    Serial.printf(  "║  CHIRP Audio Engine v%s          ║\n", VERSION_STRING);
    Serial.println("╚═══════════════════════════════════════╝");
    Serial.println();
    Serial.println("Stream 0: Bank 1 WAV from Flash (overlay)");
    Serial.println("Stream 1: Banks 2-6 MP3 from SD (primary)");
    Serial.printf("PSRAM: %d KB free\n\n", rp2040.getFreePSRAMHeap() / 1024);

    // LED
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    
    // Buttons
    pinMode(PIN_BTN_NAV, INPUT_PULLUP);
    pinMode(PIN_BTN_FWD, INPUT_PULLUP);
    pinMode(PIN_BTN_REV, INPUT_PULLUP);

    // Initialize Audio System (Streams, Buffers, Flags)
    initAudioSystem();
    Serial.println("Audio System Initialized (3 Streams, 2 MP3 Decoders)");
    
    // Initialize Compat Layer (Moved to serial_commands.cpp, no setup needed)
    // setupMp3TrigCompat(); 

    // Allocate MP3 decoders in PSRAM
    Serial.print("Allocating MP3 decoders in PSRAM... ");
    for (int i = 0; i < MAX_MP3_DECODERS; i++) {
        mp3Decoders[i] = new (pmalloc(sizeof(MP3DecoderHelix))) MP3DecoderHelix(mp3DataCallback);
        if (!mp3Decoders[i]) {
            Serial.printf("Decoder %d FAILED! ", i);
        } else {
            Serial.printf("Decoder %d OK. ", i);
        }
    }
    Serial.println();
    
    // Initialize SD Card
    Serial.print("\nInitializing SD Card... ");
    SdSpiConfig sdConfig(SD_CS, DEDICATED_SPI, SD_SCK_MHZ(25), &SPI1);
    if (!sd.begin(sdConfig)) {
        Serial.println("FAILED at 25MHz, trying 4MHz...");
        SdSpiConfig slowConfig(SD_CS, DEDICATED_SPI, SD_SCK_MHZ(4), &SPI1);
        if (!sd.begin(slowConfig)) {
            Serial.println("FAILED!");
            sd.initErrorHalt(&Serial);
            while (1) { delay(1000); }
        }
        Serial.println("OK (4MHz)");
    } else {
        Serial.println("OK (25MHz)");
    }
    
    // Initialize Flash (LittleFS)
    Serial.print("Initializing Flash... ");
    if (FORMAT_FLASH) {
        Serial.println("\n  FORMATTING FLASH (this will take a minute)...");
        if (!LittleFS.format()) {
            Serial.println("  Format FAILED!");
            while (1) { delay(1000); }
        }
        Serial.println("  Format complete");
    }
    
    if (!LittleFS.begin()) {
        Serial.println("FAILED!");
        Serial.println("ERROR: Check Arduino IDE setting: '2MB Sketch, 14MB FS'");
        Serial.println("Or try setting FORMAT_FLASH to true and re-upload");
        while (1) { delay(1000); }
    }
    Serial.println("OK");
    
    FSInfo fsInfo;
    LittleFS.info(fsInfo);
    Serial.printf("  Total: %d KB, Used: %d KB, Free: %d KB\n",
                  fsInfo.totalBytes / 1024,
                  fsInfo.usedBytes / 1024,
                  (fsInfo.totalBytes - fsInfo.usedBytes) / 1024);

    // Parse INI file *before* scanning banks
    Serial.println("\n=== Reading CHIRP.INI ===");
    parseIniFile();
    Serial.printf("Active Bank 1 Variant set to: %c\n", activeBank1Variant);
                  
    // Scan Bank 1 on SD
    Serial.println("\n=== Scanning Bank 1 (SD Card) ===");
    scanBank1();
    Serial.printf("Found %d sounds in Bank 1\n", bank1SoundCount);
    
    // Sync Bank 1 to Flash
    Serial.println("\n=== Syncing Bank 1 to Flash ===");
    if (!syncBank1ToFlash()) {
        Serial.println("WARNING: Bank 1 sync incomplete");
    }
    
    // Re-check flash usage
    LittleFS.info(fsInfo);
    Serial.printf("  Flash Used: %d KB / %d KB (%.1f%%)\n",
                  fsInfo.usedBytes / 1024,
                  fsInfo.totalBytes / 1024,
                  (fsInfo.usedBytes * 100.0) / fsInfo.totalBytes);

    // Scan SD banks (2-6)
    Serial.println("\n=== Scanning Banks 2-6 (SD Card) ===");
    scanSDBanks();
    Serial.printf("Found %d bank directories\n", sdBankCount);
    
    for (int i = 0; i < sdBankCount; i++) {
        Serial.printf("  Bank %d%c: %s (%d files)\n",
                     sdBanks[i].bankNum,
                     sdBanks[i].variant ? sdBanks[i].variant : ' ',
                     sdBanks[i].dirName,
                     sdBanks[i].fileCount);
    }
    
    // Calculate checksum *after* all banks are scanned
    calculateGlobalChecksum();
    
    // Scan Root Tracks for Legacy Compatibility
    Serial.println("\n=== Scanning Root Tracks (Legacy) ===");
    scanRootTracks();
    
    Serial.println("\n=== System Ready ===");
    Serial.println("Serial Commands (9600 baud):");
    Serial.println("  PLAY:0,1,,5,75   Play Bank 1, Sound 5, Stream 0, Vol 75");
    Serial.println("  PLAY:1,2,A,1,80  Play Bank 2A, Track 1, Stream 1, Vol 80");
    Serial.println("  STOP:0           Stop stream 0");
    Serial.println("  STOP:* Stop all streams");
    Serial.println("  VOLU:1,50        Set stream 1 volume to 50");
    Serial.println("  LIST             List all banks");
    Serial.println("  TONE             Toggle 440Hz test tone");
    Serial.println();
    
    digitalWrite(LED_PIN, HIGH);
}

// ===================================
// LOOP (Core 0)
// ===================================
void loop() {
    static uint32_t maxLoopTime = 0;
    uint32_t startMicros = micros();
    
    // Handle serial commands
    processSerialCommands(Serial);   // USB debug
    processSerialCommands(Serial2);  // ESP32 communication
    
    // --- Button Handling ---
    static unsigned long lastBtnCheck = 0;
    static bool lastNavState = HIGH;
    static bool lastFwdState = HIGH;
    static bool lastRevState = HIGH;
    
    if (millis() - lastBtnCheck > 50) {
        lastBtnCheck = millis();
        
        bool navState = digitalRead(PIN_BTN_NAV);
        bool fwdState = digitalRead(PIN_BTN_FWD);
        bool revState = digitalRead(PIN_BTN_REV);
        
        // Active LOW (Pressed = 0)
        if (lastNavState == HIGH && navState == LOW) action_togglePlayPause();
        if (lastFwdState == HIGH && fwdState == LOW) action_playNext();
        if (lastRevState == HIGH && revState == LOW) action_playPrev();
        
        lastNavState = navState;
        lastFwdState = fwdState;
        lastRevState = revState;
    }
    
    // --- Main Audio Task ---
    // Reads from files and fills ring buffers for all active streams
    fillStreamBuffers();
    
    // Debug: Monitor Buffer Status (every 1s)
    #ifdef DEBUG
    static uint32_t lastDebugTime = 0;
    if (millis() - lastDebugTime > 1000) {
        lastDebugTime = millis();
        for (int i = 0; i < MAX_STREAMS; i++) {
            if (streams[i].active) {
                int avail = streams[i].ringBuffer->availableForWrite();
                int used = STREAM_BUFFER_SIZE - 1 - avail;
                Serial.printf("STRM:%d Used:%d/%d (%.1f%%) R:%d W:%d\n", 
                    i, used, STREAM_BUFFER_SIZE, (float)used*100.0/STREAM_BUFFER_SIZE,
                    streams[i].ringBuffer->readPos, streams[i].ringBuffer->writePos);
            }
        }
    }
    #endif
    
    // Check for stop requests (auto-stop)
    for (int i = 0; i < MAX_STREAMS; i++) {
        // 1. Explicit stop request
        if (streams[i].stopRequested) {
            stopStream(i);
            streams[i].stopRequested = false;
        }
        
        // 2. Auto-stop when file finished AND buffer empty
        if (streams[i].active && streams[i].fileFinished) {
            if (streams[i].ringBuffer->availableForRead() == 0) {
                stopStream(i);
            }
        }
    }
    
    // Debug: System Stats (every 5s)
    #ifdef DEBUG
    static uint32_t lastStatsTime = 0;
    if (millis() - lastStatsTime > 5000) {
        lastStatsTime = millis();
        Serial.printf("STATS: RAM: %d KB, PSRAM: %d KB, Core0 Max Loop: %d us\n", 
            rp2040.getFreeHeap() / 1024, 
            rp2040.getFreePSRAMHeap() / 1024,
            maxLoopTime);
        maxLoopTime = 0; // Reset max
    }
    
    uint32_t loopDuration = micros() - startMicros;
    if (loopDuration > maxLoopTime) maxLoopTime = loopDuration;
    #endif
}

// ===================================
// MP3 Trigger Compatibility Actions
// ===================================
// (Moved to serial_commands.cpp)