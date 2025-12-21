#include "config.h"

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
                
                // Debug Logging: Echo commands from Serial2 (UART) to Serial (USB)
                if (&serial == &Serial2) {
                    Serial.printf("RX [UART]: %s\n", cmdBuffer);
                }

                // PLAY Command
                if (strncmp(cmdBuffer, "PLAY:", 5) == 0) {
                    int stream, bank, volume, index;
                    char page;
                    
                    char* ptr = cmdBuffer + 5;
                    
                    // NEW FORMAT: PLAY:index,bank,page,volume
                    // Defaults
                    bank = 1;
                    page = 'A';
                    volume = -1; // Use current volume
                    
                    // Auto-select stream
                    stream = getNextAvailableStream();
                    
                    // 1. Index (Required)
                    if (*ptr == '\0' || *ptr == '\r' || *ptr == '\n') goto play_error;
                    index = atoi(ptr);
                    
                    // Check for next parameter
                    ptr = strchr(ptr, ',');
                    if (ptr) {
                        ptr++; // Skip comma
                        
                        // 2. Bank (Optional)
                        if (*ptr != ',' && *ptr != '\0' && *ptr != '\r' && *ptr != '\n') {
                            bank = atoi(ptr);
                        }
                        
                        // Check for next parameter
                        ptr = strchr(ptr, ',');
                        if (ptr) {
                            ptr++; // Skip comma
                            
                            // 3. Page (Optional)
                            if (*ptr != ',' && *ptr != '\0' && *ptr != '\r' && *ptr != '\n') {
                                if (*ptr >= 'A' && *ptr <= 'Z') {
                                    page = *ptr;
                                } else if (*ptr >= 'a' && *ptr <= 'z') {
                                    page = *ptr - 32; // Uppercase
                                } else if (*ptr == '0') {
                                    page = 0; // Explicitly 0 for non-paged banks
                                }
                            }
                            
                            // Check for next parameter
                            ptr = strchr(ptr, ',');
                            if (ptr) {
                                ptr++; // Skip comma
                                
                                // 4. Volume (Optional)
                                if (*ptr != '\0' && *ptr != '\r' && *ptr != '\n') {
                                    volume = atoi(ptr);
                                }
                            }
                        }
                    }
                    
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
                                if (volume >= 0) {
                                    if (volume > 99) volume = 99;
                                    streams[stream].volume = (float)volume / 99.0f;
                                }
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
                                if (volume >= 0) {
                                    if (volume > 99) volume = 99;
                                    streams[stream].volume = (float)volume / 99.0f;
                                }
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
                        serial.println("ERR:PARAM - Format: PLAY:index,bank,page,volume");
                    }
                }
                
                // STOP Command  
                else if (strcmp(cmdBuffer, "STOP") == 0 || strncmp(cmdBuffer, "STOP:", 5) == 0) {
                    if (strcmp(cmdBuffer, "STOP") == 0 || cmdBuffer[5] == '*') {
                        // Stop all streams if just "STOP" or "STOP:*"
                        for (int i = 0; i < MAX_STREAMS; i++) {
                            stopStream(i);
                            sendSerialResponse(serial, "PACK:STOP");
                            sendSerialResponseF(serial, "S:%d,idle,,0", i);
                        }
                    } else {
                        int stream = cmdBuffer[5] - '0';
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
                else if (strncmp(cmdBuffer, "VOL:", 4) == 0) {
                    char* comma = strchr(cmdBuffer + 4, ',');
                    
                    if (comma) {
                        // Comma exists: Set specific stream volume
                        int stream = atoi(cmdBuffer + 4);
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
                        int volume = atoi(cmdBuffer + 4);
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
                    // Send full directory name for Bank 1 (e.g. "1A_R2D2")
                    sendSerialResponseF(serial, "BANK:1,%s,%d", 
                                  bank1DirName, 
                                  bank1SoundCount);

                    for (int i = 0; i < sdBankCount; i++) {
                        // Send full directory name (e.g. "1A_R2D2") instead of just page char
                        sendSerialResponseF(serial, "BANK:%d,%s,%d",
                                     sdBanks[i].bankNum,
                                     sdBanks[i].dirName,
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
                
                // CCRC Command: Clear Flash (Force Re-sync)
                else if (strcmp(cmdBuffer, "CCRC") == 0) {
                    serial.println("CMD: CCRC - Clearing Flash...");
                    
                    // Stop any active playback to avoid file access conflicts
                    for (int i = 0; i < MAX_STREAMS; i++) {
                        stopStream(i);
                    }
                    
                    int count = 0;
                    Dir dir = LittleFS.openDir("/flash");
                    while (dir.next()) {
                        if (!dir.isDirectory()) {
                            String fullPath = "/flash/" + dir.fileName();
                            if (LittleFS.remove(fullPath)) {
                                count++;
                            }
                        }
                    }
                    
                    serial.printf("Deleted %d files from /flash.\n", count);
                    serial.println("Please REBOOT the board to re-sync files.");
                    sendSerialResponse(serial, "PACK:CCRC");
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
            } else {
                 // Clean up empty lines (cmdPos == 0) silently
                 // This handles the flush command from Configurator/Core
            }
        }
        else if (cmdPos < (sizeof(usbCmdBuffer) - 1)) {
            cmdBuffer[cmdPos++] = c;
        }
    }
}