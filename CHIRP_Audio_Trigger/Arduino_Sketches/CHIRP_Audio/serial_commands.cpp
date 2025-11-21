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
char activeBank1Variant = 'A'; 

// SD Banks Structure (Banks 2-6)
SDBank sdBanks[MAX_SD_BANKS];
int sdBankCount = 0;

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
        if (c == '\n' || c == '\r') {
            if (cmdPos > 0) {
                cmdBuffer[cmdPos] = '\0';
                
                // PLAY Command
                if (strncmp(cmdBuffer, "PLAY:", 5) == 0) {
                    int stream, bank, volume, index;
                    char variant;
                    
                    char* ptr = cmdBuffer + 5;
                    
                    stream = atoi(ptr);
                    ptr = strchr(ptr, ',');
                    if (!ptr) goto play_error;
                    ptr++;
                    
                    bank = atoi(ptr);
                    ptr = strchr(ptr, ',');
                    if (!ptr) goto play_error;
                    ptr++;
                    
                    if (*ptr == ',') {
                        variant = 0; 
                    } else if (*ptr >= 'A' && *ptr <= 'Z') {
                        variant = *ptr;
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
                            
                            if (startStream(stream, fullPath)) {
                                streams[stream].volume = (float)volume / 99.0f;
                                serial.println("PACK:PLAY");
                                serial.printf("STRM:%d,playing,%s,%d\n", 
                                             stream, bank1Sounds[index - 1].basename, volume);
                            } else {
                                serial.println("ERR:NOFILE");
                            }
                        } else {
                            serial.println("ERR:PARAM - Invalid sound index");
                        }
                    }
                    else if (bank >= 2 && bank <= 6) {
                        const char* filename = getSDFile(bank, variant, index);
                        if (filename) {
                            SDBank* sdBank = findSDBank(bank, variant);
                            char fullPath[128];
                            snprintf(fullPath, sizeof(fullPath), "/%s/%s", 
                                    sdBank->dirName, filename);
                            
                            if (startStream(stream, fullPath)) {
                                streams[stream].volume = (float)volume / 99.0f;
                                serial.println("PACK:PLAY");
                                serial.printf("STRM:%d,playing,%s,%d\n", stream, filename, volume);
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
                        serial.println("ERR:PARAM - Format: PLAY:stream,bank,variant,index,volume");
                    }
                }
                
                // STOP Command
                else if (strncmp(cmdBuffer, "STOP:", 5) == 0) {
                    char target = cmdBuffer[5];
                    if (target == '*') {
                        for (int i = 0; i < MAX_STREAMS; i++) {
                            stopStream(i);
                            serial.println("PACK:STOP");
                            serial.printf("STRM:%d,idle,,0\n", i);
                        }
                    } else {
                        int stream = target - '0';
                        if (stream >= 0 && stream < MAX_STREAMS) {
                            stopStream(stream);
                            serial.println("PACK:STOP");
                            serial.printf("STRM:%d,idle,,0\n", stream);
                        } else {
                            serial.println("ERR:PARAM - Invalid stream");
                        }
                    }
                }

                // CHIRP Command (NEW - FIXED)
                else if (strncmp(cmdBuffer, "CHIRP:", 6) == 0) {
                    // Format: CHIRP:StartHz,EndHz,DurationMs,Volume
                    
                    // FIX: Declare all variables at the TOP of the block
                    // to avoid jumping over initializers.
                    char* ptr = cmdBuffer + 6;
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
                    serial.println("PACK:CHIRP");
                    
                    if (false) {
                        chirp_error:
                        serial.println("ERR:PARAM - Format: CHIRP:start,end,ms,vol");
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
                            serial.println("PACK:SVOL");
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
                        
                        serial.println("PACK:SVOL");
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
                                     sdBanks[i].variant ? sdBanks[i].variant : ' ',
                                     sdBanks[i].dirName,
                                     sdBanks[i].fileCount);
                    }
                    serial.println();
                }
                
                // GMAN Command (with MSUM)
                else if (strcmp(cmdBuffer, "GMAN") == 0) {
                    serial.printf("MDAT:%d\n", sdBankCount + 1);
                    serial.printf("BANK:1,,%d\n", bank1SoundCount);
                    
                    for (int i = 0; i < sdBankCount; i++) {
                        serial.printf("BANK:%d,%c,%d\n",
                                     sdBanks[i].bankNum,
                                     sdBanks[i].variant ? sdBanks[i].variant : ',',
                                     sdBanks[i].fileCount);
                    }
                    
                    serial.printf("MSUM:%lu\n", globalFilenameChecksum);
                    serial.println("MEND");
                }
                
                // GNME Command (with new parser and .wav fix)
                else if (strncmp(cmdBuffer, "GNME:", 5) == 0) {
                    int bank, index;
                    char variant = 0;
                    char* ptr = cmdBuffer + 5; // e.g., "2,A,5" or "1,,1" or "1,A,1"

                    // 1. Get Bank
                    bank = atoi(ptr);
                    ptr = strchr(ptr, ',');
                    if (!ptr) goto gnme_error;
                    ptr++; // ptr is now "A,5" or ",1"

                    // 2. Get Variant
                    if (*ptr == ',') {
                        variant = 0; // Empty variant
                    } else {
                        variant = *ptr;
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
                        serial.printf("NAME:1,,%d,%s.wav\n",
                                      index,
                                      bank1Sounds[index - 1].basename);
                    }
                    else if (bank >= 2 && bank <= 6 && index >= 1) {
                        // For Banks 2-6, we send the *full filename*
                        const char* filename = getSDFile(bank, variant, index);
                        if (filename) {
                            serial.printf("NAME:%d,%c,%d,%s\n",
                                         bank, variant == 0 ? ',' : variant, 
                                         index, filename);
                        } else {
                            serial.printf("NAME:%d,%c,%d,INVALID\n",
                                         bank, variant == 0 ? ',' : variant, index);
                        }
                    }
                    else {
                        goto gnme_error;
                    }
                    
                    if (false) {
                    gnme_error:
                         serial.println("ERR:PARAM - Format: GNME:bank,variant,index");
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
                
                // TONE Command
                else if (strcmp(cmdBuffer, "TONE") == 0) {
                    if (!testToneActive) {
                         testToneActive = true;
                         testTonePhase = 0;
                        serial.println("Test tone ON (440Hz)");
                    } else {
                        testToneActive = false;
                        serial.println("Test tone OFF");
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