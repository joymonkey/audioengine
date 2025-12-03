#include "config.h"


// ===================================
// Global Variable Definitions
// (Moved from .ino file to fix linker errors)
// ===================================

// File Systems
SdFat sd;
I2S i2s(OUTPUT, I2S_BCLK, I2S_DATA, I2S_LRCK);

// --- OPTIMIZATION: Use pre-computed multipliers ---
// Audio Configuration
// We pre-calculate (attenuation_0_100 * 256 / 100) -> 0-256
volatile int16_t masterAttenMultiplier = (97 * 256) / 100; // Default 97%

// --- Legacy Stream Globals Removed (Replaced by AudioStream streams[]) ---

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
// (TEST_TONE_FREQ and PHASE_INCREMENT are now #defined in config.h)

// --- Legacy MP3 Decoder Removed (Replaced by mp3Decoders[]) ---

// Filename Checksum
uint32_t globalFilenameChecksum = 0;


// ===================================
// Global Mutex Definitions
// ===================================
__attribute__((section(".mutex_array"))) mutex_t sd_mutex;
__attribute__((section(".mutex_array"))) mutex_t flash_mutex;
__attribute__((section(".mutex_array"))) mutex_t log_mutex;
// --- LOCK-FREE: wav_buffer_mutex removed ---

// ===================================
// Logging Helper
// ===================================
void log_message(const String& msg) {
    mutex_enter_blocking(&log_mutex);
    Serial.println(msg);
    mutex_exit(&log_mutex);
}

// ===================================
// Serial Output Helper
// ===================================
// Sends to USB Serial immediately, queues for Serial2
void sendSerialResponse(Stream &serial, const char* msg) {
    if (&serial == &Serial) {
        // USB Serial: Send immediately for debugging
        serial.println(msg);
    } else if (&serial == &Serial2) {
        // Serial2: Queue the message
        queueSerial2Message(msg);
    }
}

void sendSerialResponseF(Stream &serial, const char* format, ...) {
    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    sendSerialResponse(serial, buffer);
}

// ===================================
// MP3 Trigger Compatibility Actions
// ===================================

// State Tracking
int lastPlayedRootIndex = 0; // 0-based index in rootTracks array

// Helper to play a root track by index
void playRootTrack(int index) {
    if (rootTrackCount == 0) return;
    
    // Wrap or Clamp? 
    // Sparkfun "Next" wraps.
    if (index < 0) index = rootTrackCount - 1;
    if (index >= rootTrackCount) index = 0;
    
    const char* filename = rootTracks[index];
    
    // Construct path
    char fullPath[128];
    snprintf(fullPath, sizeof(fullPath), "/%s", filename);
    
    // Stop Stream 1 (SD Stream) and Play
    stopStream(1);
    if (startStream(1, fullPath)) {
        lastPlayedRootIndex = index;
        Serial.printf("COMPAT: Playing Root Track %d/%d (%s)\n", index + 1, rootTrackCount, filename);
    }
}

void action_togglePlayPause() {
    if (streams[1].active) {
        stopStream(1);
        Serial.println("COMPAT: Stop");
    } else {
        // Play last played root track
        playRootTrack(lastPlayedRootIndex);
    }
}

void action_playNext() {
    playRootTrack(lastPlayedRootIndex + 1);
}

void action_playPrev() {
    playRootTrack(lastPlayedRootIndex - 1);
}

void action_playTrackById(int trackNum) {
    // trackNum is 1-based (e.g. 1 -> "001.MP3")
    // We need to find the file that starts with "001" or "1" or matches "001.mp3"
    // Sparkfun is strict about "NNN.MP3" usually.
    // But since we indexed ALL files, we can search our index.
    
    char prefix[8];
    snprintf(prefix, sizeof(prefix), "%03d", trackNum); // "001"
    
    // Search for file starting with "001"
    for (int i = 0; i < rootTrackCount; i++) {
        if (strncmp(rootTracks[i], prefix, 3) == 0) {
            playRootTrack(i);
            return;
        }
    }
    
    // Fallback: Try strict number match if filename is just "1.mp3"
    snprintf(prefix, sizeof(prefix), "%d.", trackNum); // "1."
    for (int i = 0; i < rootTrackCount; i++) {
        if (strncmp(rootTracks[i], prefix, strlen(prefix)) == 0) {
            playRootTrack(i);
            return;
        }
    }
    
    Serial.printf("COMPAT: Track ID %d not found in root.\n", trackNum);
}

void action_playTrackByIndex(int trackIndex) {
    // trackIndex is raw 1-based index from 'p' command
    // Just play the file at that index in our sorted list
    if (trackIndex >= 1 && trackIndex <= rootTrackCount) {
        playRootTrack(trackIndex - 1);
    }
}

void action_setSparkfunVolume(uint8_t sfVol) {
    // 0 = Loud, 255 = Silent
    float vol = 1.0f - ((float)sfVol / 255.0f);
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    
    // Apply to ALL streams for global volume control effect
    for (int i = 0; i < MAX_STREAMS; i++) {
        streams[i].volume = vol;
    }
    Serial.printf("COMPAT: Volume set to %.2f\n", vol);
}

// ===================================
// MP3 Trigger Compatibility Parser
// ===================================

// Helper to wait for the second byte of a 2-byte command
// Returns true if byte received, false if timed out.
bool waitForByte(Stream &s, uint8_t &data, unsigned long timeout = 10) {
    unsigned long start = millis();
    while (s.available() == 0) {
        if (millis() - start > timeout) return false;
    }
    data = s.read();
    return true;
}

bool checkAndHandleMp3Command(Stream &s, uint8_t firstByte) {
    uint8_t arg = 0;

    switch (firstByte) {
        // ----------------------------------------------------------
        // 1-Byte Navigation Commands
        // ----------------------------------------------------------
        case 'O': // Navigation Center (Start/Stop)
            action_togglePlayPause();
            return true;

        case 'F': // Navigation Forward (Next)
            action_playNext();
            return true;

        case 'R': // Navigation Reverse (Prev)
            action_playPrev();
            return true;

        // ----------------------------------------------------------
        // 2-Byte Trigger/Control Commands
        // ----------------------------------------------------------
        
        case 'T': // Trigger (ASCII): 'T' + '1'-'9'
            if (waitForByte(s, arg)) {
                if (arg >= '0' && arg <= '9') {
                    action_playTrackById(arg - '0'); 
                    return true;
                }
            }
            return false;

        case 't': // Trigger (Binary): 't' + 0-255
            if (waitForByte(s, arg)) {
                action_playTrackById((int)arg);
                return true;
            }
            break;

        case 'v': // Set Volume (Binary): 'v' + 0-255
            if (waitForByte(s, arg)) {
                action_setSparkfunVolume(arg);
                return true;
            }
            break;

        case 'p': // Play (Binary): 'p' + 0-255 (Play by Index)
            if (waitForByte(s, arg)) {
                action_playTrackByIndex((int)arg);
                return true;
            }
            break;

        default:
            // Not a compat command, let main parser handle it
            return false;
    }
    
    return false;
}

// Helper to find the next available stream
int getNextAvailableStream() {
    // 1. Try to find an inactive stream
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (!streams[i].active) {
            return i;
        }
    }
    
    // 2. All busy? Steal Stream 0.
    return 0;
}

// ===================================
// Serial Command Processing
// ===================================
void processSerialCommands(Stream &serial) {
    static char usbCmdBuffer[128];
    static int usbCmdPos = 0;
    static char uartCmdBuffer[128];
    static int uartCmdPos = 0;

    char* cmdBuffer;
    int* cmdPosPtr; 

    if (&serial == &Serial) {
        cmdBuffer = usbCmdBuffer;
        cmdPosPtr = &usbCmdPos;
    } else if (&serial == &Serial2) {
        cmdBuffer = uartCmdBuffer;
        cmdPosPtr = &uartCmdPos;
    } else {
        return; 
    }
    
    int& cmdPos = *cmdPosPtr; 
    
    while (serial.available()) {                
        char c = serial.read();
        
        // Try compat layer first (ONLY if we are not already building a command)
        if (cmdPos == 0 && checkAndHandleMp3Command(serial, (uint8_t)c)) {
            continue;
        }

        if (c == '\n' || c == '\r') {
            if (cmdPos > 0) {
                cmdBuffer[cmdPos] = '\0';
                
                // PLAY Command
                if (strncmp(cmdBuffer, "PLAY:", 5) == 0) {
                    int stream, bank, volume, index;
                    char page;
                    
                    char* ptr = cmdBuffer + 5;
                    
                    // NEW FORMAT: PLAY:bank,page,index,volume
                    // Auto-select stream
                    stream = getNextAvailableStream();
                    
                    bank = atoi(ptr);
                    ptr = strchr(ptr, ',');
                    if (!ptr) goto play_error;
                    ptr++;
                    
                    if (*ptr == ',') {
                        page = 0; 
                    } else if (*ptr >= 'A' && *ptr <= 'Z') {
                        page = *ptr;
                        ptr++;
                        if (*ptr != ',') goto play_error;
                    } else {
                        goto play_error;
                    }
                    ptr++;
                    
                    index = atoi(ptr);
                    ptr = strchr(ptr, ',');
                    if (!ptr) goto play_error;
                    ptr++;
                    
                    volume = atoi(ptr);
                    
                    if (volume < 0) volume = 0;
                    if (volume > 99) volume = 99;
                    
                    if (stream < 0 || stream >= MAX_STREAMS) {
                        serial.println("ERR:PARAM - Invalid stream");
                        goto play_done;
                    }

                    if (bank == 1) {
                        if (index >= 1 && index <= bank1SoundCount) {
                            
                            // Pick random variant, avoiding the last-played one
                            SoundFile& sound = bank1Sounds[index - 1];
                            int variantIdx;

                            if (sound.variantCount == 1) {
                                variantIdx = 0; // Only one choice
                            } else {
                                variantIdx = random(sound.variantCount);
                                if (variantIdx == sound.lastVariantPlayed) {
                                    variantIdx = (variantIdx + 1) % sound.variantCount;
                                }
                            }
                            
                            sound.lastVariantPlayed = variantIdx;
                            const char* filename = sound.variants[variantIdx];
                            
                            // Prefix with /flash/ for startStream to know it's flash
                            char fullPath[80];
                            snprintf(fullPath, sizeof(fullPath), "/flash/%s", filename);
                            
                            // Send acknowledgement (queued for Serial2)
                            sendSerialResponse(serial, "PACK:PLAY");
                            sendSerialResponseF(serial, "S:%d,ply,%d", stream, volume);

                            if (startStream(stream, fullPath)) {
                                streams[stream].volume = (float)volume / 99.0f;
                            } else {
                                serial.println("ERR:NOFILE");
                            }
                        } else {
                            serial.println("ERR:PARAM - Invalid sound index");
                        }
                    }
                    else if (bank >= 2 && bank <= 6) {
                        const char* filename = getSDFile(bank, page, index);
                        if (filename) {
                            SDBank* sdBank = findSDBank(bank, page);
                            char fullPath[128];
                            snprintf(fullPath, sizeof(fullPath), "/%s/%s", 
                                    sdBank->dirName, filename);
                            
                            // Send acknowledgement (queued for Serial2)
                            sendSerialResponse(serial, "PACK:PLAY");
                            sendSerialResponseF(serial, "S:%d,ply,%d", stream, volume);

                            if (startStream(stream, fullPath)) {
                                streams[stream].volume = (float)volume / 99.0f;
                            } else {
                                serial.println("ERR:NOFILE");
                            }
                        } else {
                            serial.println("ERR:PARAM - Invalid file index");
                        }
                    }
                    else {
                        serial.println("ERR:PARAM - Invalid bank");
                    }
                    
                    play_done:;
                    
                    if (false) {
                    play_error:
                        serial.println("ERR:PARAM - Format: PLAY:bank,page,index,volume");
                    }
                }
                
                // STOP Command
                else if (strncmp(cmdBuffer, "STOP:", 5) == 0) {
                    char target = cmdBuffer[5];
                    if (target == '*') {
                        for (int i = 0; i < MAX_STREAMS; i++) {
                            stopStream(i);
                            sendSerialResponse(serial, "PACK:STOP");
                            sendSerialResponseF(serial, "S:%d,idle,,0", i);
                        }
                    } else {
                        int stream = target - '0';
                        if (stream >= 0 && stream < MAX_STREAMS) {
                            stopStream(stream);
                            sendSerialResponse(serial, "PACK:STOP");
                            sendSerialResponseF(serial, "S:%d,idle,,0", stream);
                        } else {
                            serial.println("ERR:PARAM - Invalid stream");
                        }
                    }
                }

                // CHRP Command
                else if (strncmp(cmdBuffer, "CHRP:", 5) == 0) {
                    // Format: CHRP:StartHz,EndHz,DurationMs,Volume
                    
                    // FIX: Declare all variables at the TOP of the block
                    // to avoid jumping over initializers.
                    char* ptr = cmdBuffer + 5;
                    int start = atoi(ptr);
                    int end = 0;
                    int ms = 0;
                    int vol = 128; // default mid volume

                    ptr = strchr(ptr, ',');
                    if (!ptr) goto chirp_error;
                    end = atoi(++ptr);
                    
                    ptr = strchr(ptr, ',');
                    if (!ptr) goto chirp_error;
                    ms = atoi(++ptr);
                    
                    ptr = strchr(ptr, ',');
                    if (ptr) {
                        vol = atoi(++ptr);
                    }
                    
                    // Call the function
                    playChirp(start, end, ms, vol);
                    sendSerialResponse(serial, "PACK:CHRP");
                    
                    if (false) {
                        chirp_error:
                        serial.println("ERR:PARAM - Format: CHRP:start,end,ms,vol");
                    }
                }
                
                // VOLU Command
                else if (strncmp(cmdBuffer, "VOLU:", 5) == 0) {
                    char* comma = strchr(cmdBuffer + 5, ',');
                    
                    if (comma) {
                        // Comma exists: Set specific stream volume
                        int stream = atoi(cmdBuffer + 5);
                        int volume = atoi(comma + 1);
                        if (volume < 0) volume = 0;
                        if (volume > 99) volume = 99;
                        
                        if (stream >= 0 && stream < MAX_STREAMS) {
                            streams[stream].volume = (float)volume / 99.0f;
                            sendSerialResponse(serial, "PACK:SVOL");
                        } else {
                            serial.println("ERR:PARAM - Invalid stream");
                        }
                    } else {
                        // No comma: Set ALL stream volumes
                        int volume = atoi(cmdBuffer + 5);
                        if (volume < 0) volume = 0;
                        if (volume > 99) volume = 99;

                        for (int i = 0; i < MAX_STREAMS; i++) {
                            streams[i].volume = (float)volume / 99.0f;
                        }
                        
                        sendSerialResponse(serial, "PACK:SVOL");
                    }
                }
                
                // LIST Command
                else if (strcmp(cmdBuffer, "LIST") == 0) {
                    serial.println("\n=== Bank 1 (Flash) ===");
                    serial.printf("Sounds: %d\n", bank1SoundCount);
                    for (int i = 0; i < bank1SoundCount && i < 10; i++) {
                        serial.printf("  %2d. %s (%d variants)\n",
                                      i + 1,
                                      bank1Sounds[i].basename,
                                      bank1Sounds[i].variantCount);
                    }
                    if (bank1SoundCount > 10) {
                        serial.printf("  ... and %d more\n", bank1SoundCount - 10);
                    }
                    
                    serial.println("\n=== Banks 2-6 (SD) ===");
                    for (int i = 0; i < sdBankCount; i++) {
                        serial.printf("Bank %d%c: %s (%d files)\n",
                                      sdBanks[i].bankNum,
                                      sdBanks[i].page ? sdBanks[i].page : ' ',
                                      sdBanks[i].dirName,
                                      sdBanks[i].fileCount);
                    }
                    serial.println();
                }
                
                // GMAN Command (with MSUM)
                else if (strcmp(cmdBuffer, "GMAN") == 0) {
                    sendSerialResponseF(serial, "MDAT:%d", sdBankCount + 1);
                    sendSerialResponseF(serial, "BANK:1,,%d", bank1SoundCount);

                    for (int i = 0; i < sdBankCount; i++) {
                        sendSerialResponseF(serial, "BANK:%d,%c,%d",
                                     sdBanks[i].bankNum,
                                     sdBanks[i].page ? sdBanks[i].page : ',',
                                     sdBanks[i].fileCount);
                    }
                    
                    sendSerialResponseF(serial, "MSUM:%lu", globalFilenameChecksum);
                    sendSerialResponse(serial, "MEND");
                }
                
                // GNME Command (with new parser and .wav fix)
                else if (strncmp(cmdBuffer, "GNME:", 5) == 0) {
                    int bank, index;
                    char page = 0;
                    char* ptr = cmdBuffer + 5; // e.g., "2,A,5" or "1,,1" or "1,A,1"

                    // 1. Get Bank
                    bank = atoi(ptr);
                    ptr = strchr(ptr, ',');
                    if (!ptr) goto gnme_error;
                    ptr++; // ptr is now "A,5" or ",1"

                    // 2. Get Page
                    if (*ptr == ',') {
                        page = 0; // Empty page
                    } else {
                        page = *ptr;
                        ptr = strchr(ptr, ','); // Find next comma
                        if (!ptr) goto gnme_error;
                    }
                    ptr++; // ptr is now "5" or "1"

                    // 3. Get Index
                    index = atoi(ptr);
                    if (index == 0) goto gnme_error;

                    
                    // Now, handle the request
                    if (bank == 1 && index >= 1 && index <= bank1SoundCount) {
                        // For Bank 1, send the basename + ".wav"
                        sendSerialResponseF(serial, "NAME:1,,%d,%s.wav",
                                      index,
                                      bank1Sounds[index - 1].basename);
                    }
                    else if (bank >= 2 && bank <= 6 && index >= 1) {
                        // For Banks 2-6, we send the *full filename*
                        const char* filename = getSDFile(bank, page, index);
                        if (filename) {
                            sendSerialResponseF(serial, "NAME:%d,%c,%d,%s",
                                         bank, page == 0 ? ',' : page, 
                                         index, filename);
                        } else {
                            sendSerialResponseF(serial, "NAME:%d,%c,%d,INVALID",
                                         bank, page == 0 ? ',' : page, index);
                        }
                    }
                    else {
                        goto gnme_error;
                    }
                    
                    if (false) {
                    gnme_error:
                         serial.println("ERR:PARAM - Format: GNME:bank,page,index");
                    }
                }
                
                // STAT Command
                else if (strncmp(cmdBuffer, "STAT:", 5) == 0) {
                    int stream = atoi(cmdBuffer + 5);
                    if (stream >= 0 && stream < MAX_STREAMS) {
                        if (streams[stream].active) {
                            int vol = (int)(streams[stream].volume * 99.0f);
                            serial.printf("STAT:playing,%s,%d\n",
                                         streams[stream].filename, vol);
                        } else {
                            serial.printf("STAT:idle,,0\n");
                        }
                    } else {
                        serial.println("ERR:PARAM - Invalid stream");
                    }
                }
                
                // Unknown Command
                else {
                    serial.println("ERR:UNKNOWN");
                }
                
                cmdPos = 0; 
            }
        }
        else if (cmdPos < (sizeof(usbCmdBuffer) - 1)) {
            cmdBuffer[cmdPos++] = c;
        }
    }
}