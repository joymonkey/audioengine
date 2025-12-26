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
#define VERSION_STRING "20251223"
//#define DEBUG // Comment out to disable debug logging

// Hardware Configuration
#define LED_PIN 25 //LED pin of the Pimoroni Pico Plus 2
#define SD_CS   13
#define SD_MISO 12
#define SD_MOSI 15
#define SD_SCK  14
#define I2S_BCLK 9
#define I2S_LRCK 10
#define I2S_DATA 11
#define NEOPIXEL_PIN 19 //CHIRP Audio Trigger PCB has 3 neopixels on pin 19

// UART Pins (Serial2)
#define UART_TX 4
#define UART_RX 5

// Button Configuration
#define PIN_BTN_NAV 17 // Start/Stop
#define PIN_BTN_FWD 16 // Next
#define PIN_BTN_REV 18 // Prev

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

// Outgoing Serial Message Queue
#define SERIAL2_QUEUE_SIZE 16
#define SERIAL2_MSG_MAX_LENGTH 128

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
    char page;
    char dirName[32];
    char files[MAX_FILES_PER_BANK][64];
    int fileCount;
};

struct SerialMessage {
    char buffer[SERIAL2_MSG_MAX_LENGTH];
    uint8_t length;
};

struct SerialQueue {
    SerialMessage messages[SERIAL2_QUEUE_SIZE];
    volatile int readPos;
    volatile int writePos;
    uint32_t messagesSent;
    uint32_t messagesDropped;
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
extern char activeBank1Page;
extern SDBank sdBanks[MAX_SD_BANKS];
extern int sdBankCount;

// Root Tracks (Legacy Compatibility)
#define MAX_ROOT_TRACKS 255
extern char rootTracks[MAX_ROOT_TRACKS][16]; // "NNN.MP3"
extern int rootTrackCount;

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

// Outgoing Serial Message Queue
extern SerialQueue serial2Queue;

// Control I2S Hardware State from Core 0
extern volatile bool g_allowAudio;

// ===================================
// NEW: Flexible Audio Architecture
// ===================================

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
    uint32_t sampleRate; // Source sample rate (e.g. 44100 or 22050)
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
bool parseIniFile();
void scanBank1();
bool syncBank1ToFlash(bool fwUpdated);
void scanSDBanks();
void scanRootTracks();
SDBank* findSDBank(uint8_t bank, char page);
const char* getSDFile(uint8_t bank, char page, int index);

// from audio_playback.cpp
void mp3DataCallback(MP3FrameInfo &info, int16_t *pcm_buffer, size_t len, void* ref);
bool startStream(int streamIdx, const char* filename);
void stopStream(int streamIdx);
void fillStreamBuffers(); // Main loop task
void initAudioSystem();
// NEW: Prototype for the Chirp function
void playChirp(int startFreq, int endFreq, int durationMs, uint8_t vol);

// from serial_commands.cpp (MP3 Trigger Compat)
void action_togglePlayPause();
void action_playNext();
void action_playPrev();
void action_playTrackById(int trackNum);
void action_playTrackByIndex(int trackIndex);
void action_setSparkfunVolume(uint8_t sfVol);
bool checkAndHandleMp3Command(Stream &s, uint8_t firstByte);

// from serial_queue.cpp
void initSerial2Queue();
bool queueSerial2Message(const char* msg);
void trySendQueuedMessages(int maxMessages);
bool isCpuBusy();
int getQueuedMessageCount();

// from blinkies.cpp
void initBlinkies();
void playStartupSequence();
void playErrorSequence();
void updateSyncLEDs(bool fileTransferEvent = false);
void updateRuntimeLEDs();


#endif // CONFIG_H