/*
 * CHIRP Audio Trigger
 * 
 * Description:
 * Multi-stream audio playback engine for RP2350.
 * Supports simultaneous playback of up to 3 streams (WAV or MP3).
 * Sound file manifest handling; so your droid knows what sounds are available.
 * 
 * Features:
 * - 3 Independent Audio Streams
 * - Supports new CHIRP serial commands and legacy MP3 Trigger commands
 * - Dual MP3 Decoders (Helix) running on Core 0
 * - Ring Buffers in PSRAM (512KB per stream) for glitch-free playback
 * - Automatic mixing on Core 1
 * - Dynamic resource allocation for decoders
 * - Robust WAV/MP3 file handling with auto-stop
 *
 * File Format Notes:
 * The system expects sound files to have a 44.1 kHz sample rate (CD quality).
 * Most files you come across will be at this rate, but 48 kHz files have become more
 * common of late. I recommend resampling any 48 kHz files prior to trying to play them
 * with the CHIRP Audio Trigger. It will play 48kHz, but then will be slowed ~92% and not
 * sound good.
 * To keep filesizes down, it's recommended to use mono WAV files. Stereo MP3's are fine.
 *
 * SD Card Structure for Droid Use:
 * Files can be stored similarly to Padawan/MP3 Trigger, but to take full advantage
 * of the CHIRP Droid Control system can be structed into Sound Bank folders. This allows
 * droid operators to easily update sounds on the droid by re-arranging the SD card,
 * without a need to adjust any code.
 * The system supports up to 6 sound banks, each can have numerous "pages" of sounds.
 * Sound Bank 1 is for the droids primary vocals. These should all be WAV files, and total
 * no more than 14MB. Files starting with similar characters but ending with consecutive
 * numbers will be considered as a sound variant group, and when triggered a single variant
 * will be randomly chosen from the group.
 * For example these 6 files are considered to be only 3 sounds...
 *   SD:/1A_R2D2/beep_01.wav
 *   SD:/1A_R2D2/beep_02.wav
 *   SD:/1A_R2D2/beep_03.wav
 *   SD:/1A_R2D2/disagree.wav
 *   SD:/1A_R2D2/happy_01.wav
 *   SD:/1A_R2D2/happy_02.wav
 * Sound Bank 1 files are transfered from the SD card to flash memory at startup, allowing
 * these sounds to always be available with minimal system overhead needed.
 * Sound Banks 2-6 have looser rules. Files can still be grouped by ending similar sounds 
 * with variant numbers, but the files can be MP3 or WAV format and of any filesize.
 * Different pages of sounds are defined by the letter in the folder name following the
 * Sound Bank number. File names should be kept short as possible while keeping them
 * identifiable to the user. For example...
 *   SD:/2A_SW-Music/ImpMarch.mp3
 *   SD:/2A_SW-Music/RebelsMix.mp3
 *   SD:/2B_SW-Clips/LeiaShort.wav
 *   SD:/2B_SW-Clips/LeiaLong.mp3
 *   SD:/3A_Effects/persicope01.mp3
 *   SD:/3A_Effects/persicope02.mp3
 *   SD:/3A_Effects/servo01.mp3
 *   SD:/3A_Effects/servo02.mp3
 * 
 * CHIRP Serial Commands:
 * PLAY : play a sound
 * STOP : stop a stream or all streams
 * VOL  : set volume from 0 (silent) to 99 (max)
 * CHRP : play a basic sound chirp
 * GMAN : Get Manifest of sound banks
 * LIST : Get a list of Sound Banks and Pages
 * GNME : Get Name of a sound in a provided sound bank and page
 * STAT : display the Status of each stream
 *
 * Legacy MP3 Trigger Serial Commands:
 * T : Trigger by sound file number (ASCII)
 * t : Trigger by sound file number (binary)
 * p : Trigger by file index (binary)
 * v : Set volume by number (binary), 255=silent, 0=max
 * O : Start/Stop currently selected track in the SD card root
 * F : Play next track in the SD card root
 * R : Play previous track in the SD card root
 *
 */

#include "config.h"
#include <CRC32.h> // For checksum
#include <Adafruit_NeoPixel.h>

volatile bool g_allowAudio = false; // Start muted (for startup sync)

// ===================================
// NEW FUNCTION: Calculate Checksum
// ===================================
/*void calculateGlobalChecksum() {
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
}*/

// ===================================
// SETUP (Core 0)
// ===================================
void setup() {
    // LED
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    
    // Initialize Blinkies
    initBlinkies();
    playStartupSequence();
    //delay(1750);

    // Initialize SPI1 FIRST
    SPI1.setRX(SD_MISO);
    SPI1.setTX(SD_MOSI);
    SPI1.setSCK(SD_SCK);
    
    // USB Serial (mainly used for development)
    Serial.begin(115200);
    // Serial2 receives commands and sends acknowledgements
    Serial2.setTX(UART_TX);
    Serial2.setRX(UART_RX);
    Serial2.begin(115200);
    
    
    Serial.println("\n╔═══════════════════════════════════════╗");
    Serial.printf(  "║  CHIRP Audio Trigger v%s        ║\n", VERSION_STRING);
    Serial.println("╚═══════════════════════════════════════╝");
    Serial.println();
    Serial.printf("PSRAM: %d KB free\n\n", rp2040.getFreePSRAMHeap() / 1024);
    
    // Buttons
    pinMode(PIN_BTN_NAV, INPUT_PULLUP);
    pinMode(PIN_BTN_FWD, INPUT_PULLUP);
    pinMode(PIN_BTN_REV, INPUT_PULLUP);

    // Initialize Audio System (Streams, Buffers, Flags)
    initAudioSystem();
    Serial.println("Audio System Initialized (3 Streams, 2 MP3 Decoders)");
    
    // Initialize Serial2 Message Queue
    initSerial2Queue();
    Serial.println("Serial2 Message Queue Initialized");

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
            sd.initErrorPrint(&Serial);
            playErrorSequence();
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
    bool fwUpdated = parseIniFile();
    Serial.printf("Active Bank 1 Page set to: %c\n", activeBank1Page);
                  
    // Scan Bank 1 on SD
    Serial.println("\n=== Scanning Bank 1 (SD Card) ===");
    scanBank1();
    Serial.printf("Found %d sounds in Bank 1\n", bank1SoundCount);
    
    // Sync Bank 1 to Flash
    Serial.println("\n=== Syncing Bank 1 to Flash ===");
    if (!syncBank1ToFlash(fwUpdated)) {
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
                     sdBanks[i].page ? sdBanks[i].page : ' ',
                     sdBanks[i].dirName,
                     sdBanks[i].fileCount);
    }
    
    // Calculate checksum *after* all banks are scanned
    //calculateGlobalChecksum();
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
    
    // Scan Root Tracks for Legacy Compatibility
    Serial.println("\n=== Scanning Root Tracks (Legacy) ===");
    scanRootTracks();
    
    // Enable Audio Output (Unmute)
    g_allowAudio = true;
    delay(100);

    Serial.println("\n=== System Ready ===");
    Serial.println("Serial Commands (115200 baud):");
    Serial.println("  PLAY:5         Play Bank 1, Sound 5");
    Serial.println("  PLAY:1,2,B,80  Play Bank 2, Page B, Sound 1, Vol 80");
    Serial.println("  STOP:0           Stop stream 0");
    Serial.println("  STOP:* Stop all streams");
    Serial.println("  VOL:1,50         Set stream 1 volume to 50");
    Serial.println("  LIST             List all banks");
    Serial.println("  CHRP:500,100,500,50"); //CHRP:StartHz,EndHz,DurationMs,Volume
    Serial.println("  CCRC             Clear sounds from flash ram"); //CHRP:StartHz,EndHz,DurationMs,Volume

    Serial.println();
    
    digitalWrite(LED_PIN, HIGH);
}

// ===================================
// LOOP (Core 0)
// ===================================
void loop() {
    #ifdef DEBUG
    static uint32_t maxLoopTime = 0;
    uint32_t startMicros = micros();
    #endif
    
    // Handle serial commands
    processSerialCommands(Serial);   // USB debug
    processSerialCommands(Serial2);  // ESP32 communication
    
    // Update Blinkies
    updateRuntimeLEDs();
    
    // Try to send queued Serial2 messages (up to 5 per loop iteration)
    // Only sends when CPU is not busy with MP3 decoding
    trySendQueuedMessages(5);
    
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
