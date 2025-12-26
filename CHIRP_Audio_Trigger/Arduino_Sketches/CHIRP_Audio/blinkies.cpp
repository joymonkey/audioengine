#include "config.h"
#include <Adafruit_NeoPixel.h>

// ===================================
// Blinkies (LEDs) Control - Adafruit_NeoPixel
// ===================================

#define NUM_LEDS 3
#define LED_BRIGHTNESS 20 // Low brightness

// Initialize NeoPixel strip
// Using standard constructor. Adafruit library typically handles PIO on RP2040/RP2350 automatically if supported/enabled in core.
// If direct bit-banging causes issues, we might need adjustments, but default usually works best.
Adafruit_NeoPixel blinkies(NUM_LEDS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// State Cache
uint32_t currentColors[NUM_LEDS];
bool needsUpdate = false;

// ===================================
// Helpers
// ===================================

void showBlinkies() {
    // Check CPU busy? 
    // Adafruit NeoPixel disables interrupts for bitbanging (if not PIO).
    // If it uses PIO, it should be fine.
    // We'll limit updates anyway.
    
    if (needsUpdate) {
        if (!isCpuBusy()) {
            blinkies.show();
            needsUpdate = false;
        }
    }
}

void setPixel(int index, uint8_t r, uint8_t g, uint8_t b) {
    uint32_t color = blinkies.Color(r, g, b);
    if (currentColors[index] != color) {
        currentColors[index] = color;
        blinkies.setPixelColor(index, color);
        needsUpdate = true;
    }
}

void clearBlinkies() {
    for(int i=0; i<NUM_LEDS; i++) {
        setPixel(i, 0, 0, 0);
    }
    // Force show for clear, bypassing cpu check
    blinkies.show(); 
    needsUpdate = false;
}

// ===================================
// Init
// ===================================
void initBlinkies() {
    blinkies.begin();
    blinkies.setBrightness(LED_BRIGHTNESS);
    clearBlinkies();
    // showBlinkies(); // Removed redundant call
}

// ===================================
// Startup Sequence
// ===================================
void playStartupSequence() {
    // Off -> Each turns Green
    delay(100);
    for(int i=0; i<NUM_LEDS; i++) {
        blinkies.setPixelColor(i, blinkies.Color(0, 255, 0)); // Green
        blinkies.show();
        delay(100);
    }
    delay(500);
    clearBlinkies();
}

// ===================================
// SD Error Sequence
// ===================================
void playErrorSequence() {
    g_allowAudio = true; // Ensure audio is enabled for chirps
    int count = 0;
    while(true) {
        // Red
        for(int i=0; i<NUM_LEDS; i++) {
            blinkies.setPixelColor(i, blinkies.Color(255, 0, 0));
        }
        blinkies.show(); // Force show
        
        // Chirp first 3 times
        if (count < 3) {
            playChirp(1000, 750, 750, 100);
        }
        
        delay(750);
        
        // Off
        for(int i=0; i<NUM_LEDS; i++) {
            blinkies.setPixelColor(i, 0);
        }
        blinkies.show(); // Force show
        
        delay(750);
        count++;
    }
}

// ===================================
// Sync Logic
// ===================================
unsigned long lastSyncBlinkTime = 0;
bool syncLed2State = false;

void updateSyncLEDs(bool fileTransferEvent) {
    // During Sync, audio shouldn't be critical (muted usually), so we can be a bit looser.
    
    unsigned long now = millis();
    
    // LED 2 Blink (Heartbeat) - 500ms
    if (now - lastSyncBlinkTime > 500) {
        lastSyncBlinkTime = now;
        syncLed2State = !syncLed2State;
        if (syncLed2State) setPixel(2, 0, 255, 0);
        else setPixel(2, 0, 0, 0);
    }
    
    // File Transfer Event
    if (fileTransferEvent) {
        // Blink LED 0
        blinkies.setPixelColor(0, blinkies.Color(0, 255, 0));
        blinkies.show();
        delay(20);
        blinkies.setPixelColor(0, 0);
        
        // Blink LED 1
        blinkies.setPixelColor(1, blinkies.Color(0, 255, 0));
        blinkies.show();
        delay(20);
        blinkies.setPixelColor(1, 0);
        
        needsUpdate = true; // Ensure state is synced back eventually
    }
    
    showBlinkies();
}

// ===================================
// Runtime Logic (Loop)
// ===================================

void updateRuntimeLEDs() {
    bool isIdle = true;
    for (int i=0; i<MAX_STREAMS; i++) {
        if (streams[i].active && !streams[i].stopRequested) {
            isIdle = false;
            break;
        }
    }

    // Rate Limit Config
    unsigned long interval = isIdle ? 30 : 100; // 33Hz for smooth idle, 10Hz for playback
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck < interval) return;
    lastCheck = millis();

    // Skip if busy (crucial for audio stability)
    if (isCpuBusy()) return;
    
    if (!isIdle) {
        // --- Playback Mode ---
        // Solid Colors: Blue (WAV), Green (MP3)
        for (int i=0; i<NUM_LEDS; i++) {
            if (i < MAX_STREAMS && streams[i].active && !streams[i].stopRequested) {
                if (streams[i].type == STREAM_TYPE_MP3_SD) {
                    setPixel(i, 0, 255, 0); // Green
                } else {
                    setPixel(i, 0, 0, 255); // Blue
                }
            } else {
                setPixel(i, 0, 0, 0); // Off
            }
        }
    } else {
        // --- Idle Mode: Cylon Scanner + Color Fade ---
        // Cycle: Blue -> Purple -> Pink -> Blue ...
        // Color transition speed
        static float colorPhase = 0;
        colorPhase += 0.05f; 
        if (colorPhase >= 6.28f) colorPhase -= 6.28f; // 2*PI wrap
        
        // Cylon Position (0 to NUM_LEDS-1 and back)
        static float pos = 0;
        static float dir = 0.15f;
        pos += dir;
        
        if (pos >= NUM_LEDS - 0.5f) {
            pos = NUM_LEDS - 0.5f;
            dir = -dir;
        } else if (pos <= -0.5f) {
            pos = -0.5f;
            dir = -dir;
        }
        
        // Calculate Base Color (Blue-centric w/ Purple/Pink)
        // Simple RGB mix based on sine
        // Blue is dominant. Red comes in for Purple/Pink. Green stays low/zero.
        // Blue: 0,0,255
        // Purple: 128,0,128
        // Pink: 255,0,128
        
        // Let's sinusoidal oscillate Red component 0->255?
        // sin(-1..1) -> 0..1 -> 0..255
        uint8_t redAmt = (uint8_t)((sin(colorPhase) + 1.0f) * 127.5f);
        uint8_t blueAmt = 255; // Keep blue high? Or fade it slightly?
        // Maybe dip blue slightly when Red is high (Pink)
        // Pink is R~255, B~128. Purple R~128, B~128.
        
        if (redAmt > 200) blueAmt = 150; // shift to Pink
        
        for(int i=0; i<NUM_LEDS; i++) {
            // Brightness based on distance from 'pos'
            float dist = abs(pos - i);
            float bright = 1.0f - dist; // 1.0 at center, 0 at 1 unit away
            if (bright < 0) bright = 0;
            // enhance peak
            bright = pow(bright, 2.0f); 
            
            // Apply color
            uint8_t r = (uint8_t)(redAmt * bright);
            uint8_t b = (uint8_t)(blueAmt * bright);
            
            setPixel(i, r, 0, b);
        }
    }
    
    showBlinkies();
}
