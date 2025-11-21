#ifndef CONFIG_H
#define CONFIG_H

#include <SdFat.h>
#include <LittleFS.h>
#include <I2S.h>
#include "pico/mutex.h"
#include "MP3DecoderHelix.h"

using namespace libhelix;

// ===================================
// Constants
// ===================================
#define VERSION_STRING "20251119"
#define DEBUG // Comment out to disable debug logging

// Hardware Configuration
#define LED_PIN 25
#define SD_CS   13
#define SD_MISO 12
#define SD_MOSI 15
#define SD_SCK  14
#define I2S_BCLK 9
#define I2S_LRCK 10
#define I2S_DATA 11

// UART Pins (Serial2)
#define UART_TX 4
#define UART_RX 5

// Development Mode
#define DEV_MODE true
#define DEV_SYNC_LIMIT 100
#define FORMAT_FLASH false

// Audio Configuration
#define SAMPLE_RATE 44100
#define WAV_BUFFER_SIZE 8192
// #define STREAM_WAV_BUFFER_SIZE 4096 // Stream 2 removed

// Bank/File Limits
#define MAX_SOUNDS 100
#define MAX_SD_BANKS 20
#define MAX_FILES_PER_BANK 100

// ===================================
// Struct Definitions
// ===================================
struct WAVHeader {
    char riff[4];
    uint32_t fileSize;
    char wave[4];
    char fmt[4];
    uint32_t fmtSize;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char data[4];
    uint32_t dataSize;
};

struct SoundFile {
    char basename[16];
    char variants[25][32];
    int variantCount;
    int lastVariantPlayed; // For non-repeating random
};

struct SDBank {
    uint8_t bankNum;
    char variant;
    char dirName[32];
    char files[MAX_FILES_PER_BANK][64];
    int fileCount;
};

// ===================================
// Extern Global Variables
// ===================================

// File Systems
extern SdFat sd;
extern I2S i2s;

// Thread Safety
extern mutex_t sd_mutex;
extern mutex_t flash_mutex;
extern mutex_t log_mutex;
// --- LOCK-FREE: wav_buffer_mutex removed ---

// --- Legacy Stream Variables Removed (Replaced by AudioStream streams[]) ---

// Bank File Lists
extern SoundFile bank1Sounds[MAX_SOUNDS];
extern int bank1SoundCount;
extern char bank1DirName[64]; 
extern char activeBank1Variant;
extern SDBank sdBanks[MAX_SD_BANKS];
extern int sdBankCount;

// Test Tone State
extern volatile bool testToneActive;
extern volatile uint32_t testTonePhase;
// --- OPTIMIZATION: Use pre-computed multipliers ---
extern volatile int16_t masterAttenMultiplier;
#define TEST_TONE_FREQ 440
#define PHASE_INCREMENT ((uint32_t)TEST_TONE_FREQ << 16) / SAMPLE_RATE

// MP3 Decoder
extern MP3DecoderHelix* mp3Decoder; // "Option 2" PSRAM fix

// Filename Checksum
extern uint32_t globalFilenameChecksum;

// ===================================
// NEW: Flexible Audio Architecture
// ===================================

#define MAX_STREAMS 3
#define MAX_MP3_DECODERS 2
#define MAX_STREAMS 3
#define MAX_MP3_DECODERS 2
#define STREAM_BUFFER_SIZE (256 * 1024) // 256K samples = 512KB per stream (PSRAM)

enum StreamType {
    STREAM_TYPE_INACTIVE = 0,
    STREAM_TYPE_WAV_FLASH, // Legacy optimized path (optional, or treat as generic)
    STREAM_TYPE_WAV_SD,
    STREAM_TYPE_MP3_SD
};

struct RingBuffer {
    int16_t* buffer; // Pointer to PSRAM
    volatile int readPos;
    volatile int writePos;
    
    // Helper to get available write space
    int availableForWrite() {
        if (!buffer) return 0;
        int currentLevel = (writePos - readPos + STREAM_BUFFER_SIZE) % STREAM_BUFFER_SIZE;
        return (STREAM_BUFFER_SIZE - 1) - currentLevel;
    }
    
    // Helper to get available samples to read
    int availableForRead() {
        if (!buffer) return 0;
        return (writePos - readPos + STREAM_BUFFER_SIZE) % STREAM_BUFFER_SIZE;
    }
    
    bool push(int16_t sample) {
        if (!buffer) return false;
        
        int nextWrite = (writePos + 1) % STREAM_BUFFER_SIZE;
        if (nextWrite == readPos) {
            // Buffer Full - Drop sample
            return false;
        }
        
        buffer[writePos] = sample;
        writePos = nextWrite;
        return true;
    }
    
    int16_t pop() {
        if (!buffer) return 0;
        int16_t sample = buffer[readPos];
        readPos = (readPos + 1) % STREAM_BUFFER_SIZE;
        return sample;
    }
    
    void clear() {
        readPos = 0;
        writePos = 0;
        if (buffer) {
            // Optional: memset(buffer, 0, STREAM_BUFFER_SIZE * sizeof(int16_t));
            // But clearing pointers is enough for ring buffer logic usually.
            // For safety against noise:
            // memset(buffer, 0, STREAM_BUFFER_SIZE * sizeof(int16_t)); 
            // (memset on 512KB might be slow, so maybe skip or do partial?)
        }
    }
};

struct AudioStream {
    bool active;
    StreamType type;
    float volume; // 0.0 to 1.0
    int decoderIndex; // -1 if not using MP3 decoder
    
    // File Handles
    File flashFile; // For LittleFS
    FsFile sdFile;  // For SdFat
    
    // Buffer
    RingBuffer* ringBuffer;
    
    // State
    char filename[64];
    bool stopRequested;
    bool fileFinished;
    uint8_t channels; // 1 = Mono, 2 = Stereo
    uint32_t startTime; // Debug timestamp
};

extern AudioStream streams[MAX_STREAMS];
extern RingBuffer streamBuffers[MAX_STREAMS];
extern MP3DecoderHelix* mp3Decoders[MAX_MP3_DECODERS];
extern bool mp3DecoderInUse[MAX_MP3_DECODERS];

// ===================================
// Function Prototypes
// ===================================

// from serial_commands.cpp
void log_message(const String& msg);
void processSerialCommands(Stream &serial); // Dual-buffer fix

// from file_management.cpp
void parseIniFile();
void scanBank1();
bool syncBank1ToFlash();
void scanSDBanks();
SDBank* findSDBank(uint8_t bank, char variant);
const char* getSDFile(uint8_t bank, char variant, int index);

// from audio_playback.cpp
// from audio_playback.cpp
void mp3DataCallback(MP3FrameInfo &info, int16_t *pcm_buffer, size_t len, void* ref);
bool startStream(int streamIdx, const char* filename);
void stopStream(int streamIdx);
void fillStreamBuffers(); // Main loop task
void initAudioSystem();
// NEW: Prototype for the Chirp function
void playChirp(int startFreq, int endFreq, int durationMs, uint8_t vol);

#endif // CONFIG_H