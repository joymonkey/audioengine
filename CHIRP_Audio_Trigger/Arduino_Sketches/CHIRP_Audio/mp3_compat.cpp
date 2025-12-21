#include "config.h"

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
