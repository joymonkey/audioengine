#include "config.h"

// ===================================
// Parse CHIRP.INI File
// ===================================
void parseIniFile() {
    bool found = false;
    mutex_enter_blocking(&sd_mutex);
    FsFile iniFile = sd.open("CHIRP.INI", FILE_READ);
    
    if (iniFile) {
        char buffer[128];
        while (iniFile.available()) {
            // Read a line, trim whitespace
            String line = iniFile.readStringUntil('\n');
            line.trim();
            line.toCharArray(buffer, sizeof(buffer));

            // Check if it's a valid setting line
            if (strlen(buffer) > 0 && buffer[0] == '#') {
                char* command = buffer + 1;
                while (*command == ' ') command++; // trim leading space after #

                if (strncasecmp(command, "BANK1_PAGE", 10) == 0) {
                    char* value = strchr(command, ' ');
                    if (value) {
                        while (*(++value) == ' '); // Find first char of value
                        if (*value >= 'A' && *value <= 'Z') {
                            activeBank1Page = *value;
                            found = true;
                        }
                    }
                }
                // Legacy support for BANK1_VARIANT
                else if (strncasecmp(command, "BANK1_VARIANT", 13) == 0) {
                    char* value = strchr(command, ' ');
                    if (value) {
                        while (*(++value) == ' '); // Find first char of value
                        if (*value >= 'A' && *value <= 'Z') {
                            activeBank1Page = *value;
                            found = true;
                        }
                    }
                }
            }
        }
        iniFile.close();
    }

    if (!found) {
        // File not found or key was missing, create/update it
        iniFile = sd.open("CHIRP.INI", FILE_WRITE | O_TRUNC);
        if (iniFile) {
            iniFile.println("# CHIRP Configuration File");
            iniFile.println("# Set the active Bank 1 page (A-Z)");
            iniFile.println("# This selects which '1X_...' directory to sync to flash.");
            iniFile.printf("#BANK1_PAGE %c\n", activeBank1Page); // Write default 'A'
            iniFile.close();
            Serial.println("CHIRP.INI not found or key missing, created with defaults.");
        } else {
            Serial.println("ERROR: Could not create CHIRP.INI!");
        }
    }
    mutex_exit(&sd_mutex);
}


// ===================================
// Scan Bank 1 (Finds dir matching activeBank1Page)
// ===================================
void scanBank1() {
    bank1SoundCount = 0;
    bank1DirName[0] = '\0'; // Clear the name
    
    char targetPrefix[4]; // "1A_"
    snprintf(targetPrefix, sizeof(targetPrefix), "1%c_", activeBank1Page);
    
    mutex_enter_blocking(&sd_mutex);
    FsFile root = sd.open("/");
    
    if (!root || !root.isDirectory()) {
        Serial.println("ERROR: Could not open root directory");
        mutex_exit(&sd_mutex);
        return;
    }

    FsFile bankDir;
    // Loop 1: Find the *specific* Bank 1 Directory
    while (bankDir.openNext(&root, O_RDONLY)) {
        char dirName[64];
        bankDir.getName(dirName, sizeof(dirName));
        
        if (bankDir.isDirectory() && strncmp(dirName, targetPrefix, 3) == 0) {
            strncpy(bank1DirName, dirName, sizeof(bank1DirName) - 1);
            Serial.printf("Found Active Bank 1 Directory: %s\n", bank1DirName);
            
            // Now, scan files inside this directory
            FsFile file;
            while (file.openNext(&bankDir, O_RDONLY)) {
                char filename[64];
                file.getName(filename, sizeof(filename));
                if (!file.isDirectory()) {
                    const char* ext = strrchr(filename, '.');
                    if (ext && (strcasecmp(ext, ".wav") == 0 || strcasecmp(ext, ".mp3") == 0)) {
                        char* underscore = strchr(filename, '_');
                        if (underscore && isdigit(*(underscore + 1))) {
                            char basename[16];
                            int baseLen = underscore - filename;
                            if (baseLen >= sizeof(basename)) baseLen = sizeof(basename) - 1;
                            strncpy(basename, filename, baseLen);
                            basename[baseLen] = '\0';
                            
                            int soundIdx = -1;
                            for (int i = 0; i < bank1SoundCount; i++) {
                                if (strcasecmp(bank1Sounds[i].basename, basename) == 0) {
                                    soundIdx = i;
                                    break;
                                }
                            }
                            
                            if (soundIdx == -1) {
                                if (bank1SoundCount < MAX_SOUNDS) {
                                     soundIdx = bank1SoundCount++;
                                     strncpy(bank1Sounds[soundIdx].basename, basename, sizeof(bank1Sounds[soundIdx].basename) - 1);
                                    bank1Sounds[soundIdx].variantCount = 0;
                                    bank1Sounds[soundIdx].lastVariantPlayed = -1; // Init non-repeat
                                }
                            }
                            
                            if (soundIdx != -1 && bank1Sounds[soundIdx].variantCount < 25) {
                                
                                 strncpy(bank1Sounds[soundIdx].variants[bank1Sounds[soundIdx].variantCount],
                                       filename,
                                       sizeof(bank1Sounds[soundIdx].variants[0]) - 1);
                                 bank1Sounds[soundIdx].variantCount++;
                            }
                        }
                        else {
                            char basename[16];
                            strncpy(basename, filename, sizeof(basename) - 1);
                            char* dot = strrchr(basename, '.');
                            if (dot) *dot = '\0';
                            if (bank1SoundCount < MAX_SOUNDS) {
                                int soundIdx = bank1SoundCount++;
                                strncpy(bank1Sounds[soundIdx].basename, basename, sizeof(bank1Sounds[soundIdx].basename) - 1);
                                strncpy(bank1Sounds[soundIdx].variants[0], filename, sizeof(bank1Sounds[soundIdx].variants[0]) - 1);
                                bank1Sounds[soundIdx].variantCount = 1;
                                bank1Sounds[soundIdx].lastVariantPlayed = -1; // Init non-repeat
                            }
                        }
                    }
                }
                file.close();
            } // end file loop
            
            bankDir.close();
            break; // Found and processed Bank 1, so exit the root loop
        }
        bankDir.close();
    } // end root loop
    
    root.close();
    mutex_exit(&sd_mutex);

    if (bank1DirName[0] == '\0') {
        Serial.printf("WARNING: No Bank 1 directory matching '%s...' found on SD card.\n", targetPrefix);
    }
}


// ===================================
// Sync Bank 1 to Flash
// ===================================
bool syncBank1ToFlash() {
    if (bank1DirName[0] == '\0') {
        Serial.println("  Skipping sync: No active Bank 1 directory found.");
        return false;
    }
    
    if (!LittleFS.exists("/flash")) {
        LittleFS.mkdir("/flash");
    }

    // --- Pruning stale files from flash ---
    Serial.println("  Pruning stale files from flash...");
    int filesDeleted = 0;
    Dir dir = LittleFS.openDir("/flash");
    while (dir.next()) {
        if (!dir.isDirectory()) {
            char flashFilename[64];
            strncpy(flashFilename, dir.fileName().c_str(), sizeof(flashFilename) - 1);
            
            bool foundInMasterList = false;
            // Check if this file exists in the new SD bank (master list)
            for (int i = 0; i < bank1SoundCount; i++) {
                for (int v = 0; v < bank1Sounds[i].variantCount; v++) {
                    if (strcmp(flashFilename, bank1Sounds[i].variants[v]) == 0) {
                        foundInMasterList = true;
                        break;
                    }
                }
                if (foundInMasterList) break;
            }
            
            // If the file was NOT found in the master list, delete it.
            if (!foundInMasterList) {
                char fullFlashPath[80];
                snprintf(fullFlashPath, sizeof(fullFlashPath), "/flash/%s", flashFilename);
                if (LittleFS.remove(fullFlashPath)) {
                    Serial.printf("    - Deleted stale file: %s\n", flashFilename);
                    filesDeleted++;
                } else {
                    Serial.printf("    - ERROR deleting: %s\n", flashFilename);
                }
            }
        }
    }
    if (filesDeleted == 0) {
        Serial.println("    - No stale files found.");
    }
    // --- End of Pruning Feature ---

    
    int totalFiles = 0;
    for (int i = 0; i < bank1SoundCount; i++) {
        totalFiles += bank1Sounds[i].variantCount;
    }
    
    int syncLimit = DEV_MODE ? min(totalFiles, DEV_SYNC_LIMIT) : totalFiles;
    Serial.printf("  Syncing %d files from %s", syncLimit, bank1DirName);
    if (DEV_MODE && totalFiles > syncLimit) {
        Serial.printf(" (DEV MODE: limited to first %d)", DEV_SYNC_LIMIT);
    }
    Serial.println();
    
    int filesCopied = 0;
    int filesSkipped = 0;
    int filesProcessed = 0;
    for (int i = 0; i < bank1SoundCount; i++) {
        for (int v = 0; v < bank1Sounds[i].variantCount; v++) {
            filesProcessed++;
            if (DEV_MODE && filesProcessed > DEV_SYNC_LIMIT) {
                goto sync_complete;
            }
            
            const char* filename = bank1Sounds[i].variants[v];
            Serial.printf("  [%d/%d] ", filesProcessed, syncLimit);
            
            char sdPath[64];
            char flashPath[64];
            snprintf(sdPath, sizeof(sdPath), "/%s/%s", bank1DirName, filename);
            snprintf(flashPath, sizeof(flashPath), "/flash/%s", filename);
            
            // Heartbeat for scanning
            updateSyncLEDs(false);

            bool needsCopy = true;
            mutex_enter_blocking(&sd_mutex);
            FsFile sdFile = sd.open(sdPath, FILE_READ);
            if (sdFile) {
                size_t sdSize = sdFile.size();
                if (LittleFS.exists(flashPath)) {
                    File flashFile = LittleFS.open(flashPath, "r");
                    if (flashFile) {
                        size_t flashSize = flashFile.size();
                        if (flashSize == sdSize) {
                            needsCopy = false;
                            filesSkipped++;
                            Serial.printf("Skipped: %s\n", filename);
                        }
                        
                        flashFile.close();
                    }
                }
                
                if (needsCopy) {
                    // Sync File Transition Feedback
                    updateSyncLEDs(true);
                    
                    sdFile.rewind();
                    File flashFile = LittleFS.open(flashPath, "w");
                    if (flashFile) {
                        const uint16_t CHUNK_SIZE = 512;
                        uint8_t buffer[CHUNK_SIZE];
                        uint32_t remaining = sdSize;
                        bool copySuccess = true;
                        Serial.printf("Copying: %s (%lu KB)... ", 
                                     filename, sdSize / 1024);
                        while (remaining > 0 && copySuccess) {
                            uint16_t toRead = (remaining > CHUNK_SIZE) ?
                                              CHUNK_SIZE : remaining;
                            
                            int bytesRead = sdFile.read(buffer, toRead);
                            
                            // Heartbeat during copy
                            updateSyncLEDs(false);

                            if (bytesRead <= 0) {
                                Serial.println(" READ ERROR!");
                                copySuccess = false;
                                break;
                            }
                            
                            int bytesWritten = flashFile.write(buffer, bytesRead);
                            if (bytesWritten != bytesRead) {
                                Serial.println(" WRITE ERROR!");
                                copySuccess = false;
                                break;
                            }
                            
                            remaining -= bytesRead;
                        }
                        
                        flashFile.close();
                        if (copySuccess) {
                            Serial.println("OK");
                            filesCopied++;
                            
                            // Success Beep!
                            g_allowAudio = true; 
                            delay(5); // Wait for I2S to start
                            playChirp(2000, 500, 60, 50); // fast chirp
                            delay(60);
                            playChirp(2000, 4000, 50, 50); // fast chirp
                            delay(60); // Wait for chirp (blocking Core 0 is fine here)
                            g_allowAudio = false; // Mute again
                            delay(5);
                        }
                    } else {
                        Serial.println(" FAILED to create flash file!");
                    }
                }
                
                sdFile.close();
            } else {
                Serial.printf("ERROR: Could not open %s\n", sdPath);
            }
            
            mutex_exit(&sd_mutex);
        }
    }
    
sync_complete:
    Serial.printf("\n  Summary: %d copied, %d skipped, %d pruned\n", 
                  filesCopied, filesSkipped, filesDeleted);
    return true;
}


// ===================================
// Scan SD Banks (2-6 with optional pages)
// ===================================
void scanSDBanks() {
    sdBankCount = 0;
    mutex_enter_blocking(&sd_mutex);
    FsFile root = sd.open("/");
    
    if (!root || !root.isDirectory()) {
        Serial.println("ERROR: Could not open root directory");
        mutex_exit(&sd_mutex);
        return;
    }
    
    FsFile dir;
    while (dir.openNext(&root, O_RDONLY)) {
        if (dir.isDirectory()) {
            char dirName[64];
            dir.getName(dirName, sizeof(dirName));
            
            // Match pattern: [2-6][A-Z]?_[Name]
            if (strlen(dirName) >= 2 &&
                dirName[0] >= '2' && dirName[0] <= '6') {
                
                uint8_t bankNum = dirName[0] - '0';
                char page = 0;
                
                // Check for page letter
                if (strlen(dirName) >= 3 && 
                    dirName[1] >= 'A' && dirName[1] <= 'Z' &&
                    dirName[2] == '_') {
                    page = dirName[1];
                }
                else if (dirName[1] != '_') {
                    // Invalid format
                    dir.close();
                    continue;
                }
                
                // Create new bank entry
                if (sdBankCount < MAX_SD_BANKS) {
                    SDBank* bank = &sdBanks[sdBankCount];
                    bank->bankNum = bankNum;
                    bank->page = page;
                    strncpy(bank->dirName, dirName, sizeof(bank->dirName) - 1);
                    bank->fileCount = 0;
                    
                    // Scan files in this directory
                    char fullPath[80];
                    snprintf(fullPath, sizeof(fullPath), "/%s", dirName);
                    FsFile bankDir = sd.open(fullPath);
                    
                    if (bankDir && bankDir.isDirectory()) {
                        FsFile file;
                        while (file.openNext(&bankDir, O_RDONLY)) {
                            if (!file.isDirectory() && bank->fileCount < MAX_FILES_PER_BANK) {
                                char filename[64];
                                file.getName(filename, sizeof(filename));
                                
                                const char* ext = strrchr(filename, '.');
                                if (ext && (strcasecmp(ext, ".wav") == 0 ||
                                           strcasecmp(ext, ".mp3") == 0 ||
                                           strcasecmp(ext, ".aac") == 0 ||
                                           strcasecmp(ext, ".m4a") == 0)) {
                                    strncpy(bank->files[bank->fileCount], filename,
                                            sizeof(bank->files[0]) - 1);
                                    bank->fileCount++;
                                }
                            }
                            file.close();
                        }
                        bankDir.close();
                    }
                    
                    sdBankCount++;
                }
            }
        }
        dir.close();
    }
    
    root.close();
    mutex_exit(&sd_mutex);
}


// ===================================
// Find SD Bank by number and page
// ===================================
SDBank* findSDBank(uint8_t bank, char page) {
    for (int i = 0; i < sdBankCount; i++) {
        if (sdBanks[i].bankNum == bank && sdBanks[i].page == page) {
            return &sdBanks[i];
        }
    }
    return nullptr;
}


// ===================================
// Get File from SD Bank
// ===================================
const char* getSDFile(uint8_t bank, char page, int index) {
    SDBank* sdBank = findSDBank(bank, page);
    if (!sdBank) return nullptr;
    
    if (index < 1 || index > sdBank->fileCount) return nullptr;
    
    return sdBank->files[index - 1];
}


// ===================================
// Scan Root Tracks (Legacy Compatibility)
// ===================================
void scanRootTracks() {
    rootTrackCount = 0;
    mutex_enter_blocking(&sd_mutex);
    FsFile root = sd.open("/");
    
    if (!root || !root.isDirectory()) {
        Serial.println("ERROR: Could not open root directory for legacy scan");
        mutex_exit(&sd_mutex);
        return;
    }
    
    FsFile file;
    while (file.openNext(&root, O_RDONLY)) {
        if (!file.isDirectory()) {
            char filename[64];
            file.getName(filename, sizeof(filename));
                       
            // index all valid audio files in SD root
            const char* ext = strrchr(filename, '.');
            if (ext && (strcasecmp(ext, ".wav") == 0 ||
                       strcasecmp(ext, ".mp3") == 0 ||
                       strcasecmp(ext, ".aac") == 0 ||
                       strcasecmp(ext, ".m4a") == 0)) {
                
                if (rootTrackCount < MAX_ROOT_TRACKS) {
                    strncpy(rootTracks[rootTrackCount], filename, sizeof(rootTracks[0]) - 1);
                    rootTrackCount++;
                }
            }
        }
        file.close();
    }
    root.close();
    mutex_exit(&sd_mutex);
    
    // Sort the tracks alphabetically to ensure deterministic order
    // (Bubble sort is fine for < 255 items)
    for (int i = 0; i < rootTrackCount - 1; i++) {
        for (int j = 0; j < rootTrackCount - i - 1; j++) {
            if (strcasecmp(rootTracks[j], rootTracks[j+1]) > 0) {
                char temp[16];
                strncpy(temp, rootTracks[j], sizeof(temp));
                strncpy(rootTracks[j], rootTracks[j+1], sizeof(rootTracks[j]));
                strncpy(rootTracks[j+1], temp, sizeof(temp));
            }
        }
    }
    
    Serial.printf("Found %d root tracks for legacy compatibility.\n", rootTrackCount);
}