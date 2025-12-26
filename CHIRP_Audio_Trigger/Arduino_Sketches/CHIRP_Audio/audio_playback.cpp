#include "config.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

// =================================================================================
//  UNIFIED MIXER LOGIC + CHIRP GENERATOR
// =================================================================================

// Mixer configuration
#define STREAM0_FADE_MS      5     
#ifndef SAMPLE_RATE
#define SAMPLE_RATE 44100
#endif
#define STREAM0_FADE_SAMPLES ((SAMPLE_RATE * STREAM0_FADE_MS) / 1000)

// --- SINE LOOKUP TABLE (Optimization) ---
// A full 256-value sine wave (0..255 corresponds to 0..360 degrees)
// Values are signed 8-bit (-127 to 127) to save space, scaled up during mixing.
const int8_t SINE_LUT[256] = {
    0, 3, 6, 9, 12, 15, 18, 21, 24, 27, 30, 33, 36, 39, 42, 45,
    48, 51, 54, 57, 60, 63, 65, 68, 71, 73, 76, 78, 81, 83, 85, 88,
    90, 92, 94, 96, 98, 100, 102, 104, 106, 107, 109, 110, 112, 113, 115, 116,
    117, 118, 120, 121, 122, 122, 123, 124, 125, 125, 126, 126, 126, 127, 127, 127,
    127, 127, 127, 127, 126, 126, 126, 125, 125, 124, 123, 122, 122, 121, 120, 118,
    117, 116, 115, 113, 112, 110, 109, 107, 106, 104, 102, 100, 98, 96, 94, 92,
    90, 88, 85, 83, 81, 78, 76, 73, 71, 68, 65, 63, 60, 57, 54, 51,
    48, 45, 42, 39, 36, 33, 30, 27, 24, 21, 18, 15, 12, 9, 6, 3,
    0, -3, -6, -9, -12, -15, -18, -21, -24, -27, -30, -33, -36, -39, -42, -45,
    -48, -51, -54, -57, -60, -63, -65, -68, -71, -73, -76, -78, -81, -83, -85, -88,
    -90, -92, -94, -96, -98, -100, -102, -104, -106, -107, -109, -110, -112, -113, -115, -116,
    -117, -118, -120, -121, -122, -122, -123, -124, -125, -125, -126, -126, -126, -127, -127, -127,
    -127, -127, -127, -127, -126, -126, -126, -125, -125, -124, -123, -122, -122, -121, -120, -118,
    -117, -116, -115, -113, -112, -110, -109, -107, -106, -104, -102, -100, -98, -96, -94, -92,
    -90, -88, -85, -83, -81, -78, -76, -73, -71, -68, -65, -63, -60, -57, -54, -51,
    -48, -45, -42, -39, -36, -33, -30, -27, -24, -21, -18, -15, -12, -9, -6, -3
};

// --- CHIRP STATE ---
struct ChirpState {
    bool active;
    uint32_t phase;         // Current phase accumulator
    uint32_t phaseInc;      // Current frequency increment per sample
    uint32_t targetInc;     // Target frequency increment
    int32_t sweepStep;      // How much to change phaseInc per sample
    uint32_t samplesLeft;   // Duration counter
    uint8_t volume;         // 0..255
};

// Global Chirp Instance (volatile for thread safety between Core0/1)
volatile ChirpState chirp = {false, 0, 0, 0, 0, 0, 0};

// ===================================
// Global Audio Objects
// ===================================
AudioStream streams[MAX_STREAMS];
RingBuffer streamBuffers[MAX_STREAMS];
MP3DecoderHelix* mp3Decoders[MAX_MP3_DECODERS];
bool mp3DecoderInUse[MAX_MP3_DECODERS];

// Context for the callback (since library doesn't pass user data through write)
volatile int currentDecodingStream = -1;

// ===================================
// Initialize Audio System
// ===================================
void initAudioSystem() {
    // Initialize Streams
    for (int i = 0; i < MAX_STREAMS; i++) {
        streams[i].active = false;
        streams[i].type = STREAM_TYPE_INACTIVE;
        streams[i].volume = 1.0f;
        streams[i].decoderIndex = -1;
        streams[i].ringBuffer = &streamBuffers[i];
        streams[i].stopRequested = false;
        streams[i].fileFinished = false;
        
        // Allocate Buffer in PSRAM
        // 256K samples * 2 bytes = 512KB
        streams[i].ringBuffer->buffer = (int16_t*)pmalloc(STREAM_BUFFER_SIZE * sizeof(int16_t));
        
        if (streams[i].ringBuffer->buffer) {
            // Success
            streams[i].ringBuffer->clear();
            #ifdef DEBUG
            Serial.printf("Stream %d: Buffer allocated in PSRAM (512KB)\n", i);
            #endif
        } else {
            // Allocation Failed!
            Serial.printf("Stream %d: ERROR - PSRAM Allocation Failed!\n", i);
        }
    }
    
    // Initialize Decoder Pool Flags
    for (int i = 0; i < MAX_MP3_DECODERS; i++) {
        mp3DecoderInUse[i] = false;
        // Note: mp3Decoders[i] are allocated in setup()
    }
}

// Simple inline helpers
static inline int32_t i16_to_i32(int16_t s) { return (int32_t)s; }
static inline int16_t i32_to_i16(int32_t v) { if (v > 32767) return 32767; if (v < -32768) return -32768; return (int16_t)v; }

// ===================================
// Fill Stream Buffers (Core 0)
// ===================================
// This function iterates through all active streams, reads data from their
// respective files (Flash or SD), decodes it (if MP3), and pushes it into
// the stream's Ring Buffer.
void fillStreamBuffers() {
    for (int i = 0; i < MAX_STREAMS; i++) {
        AudioStream* s = &streams[i];
        
        if (!s->active || s->fileFinished) continue;
        
        // Check if buffer needs data
        int available = s->ringBuffer->availableForWrite();
        
        if (s->type == STREAM_TYPE_MP3_SD) {
            // --- MP3 (SD) ---
            // MP3 frames can be large. Low bitrate frames can be many samples per byte.
            // Safety margin increased to 16384 to be absolutely sure.
            if (available > 16384) {
                uint8_t mp3Buf[512]; 
                int bytesRead = 0;
                
                mutex_enter_blocking(&sd_mutex);
                if (s->sdFile) {
                    bytesRead = s->sdFile.read(mp3Buf, sizeof(mp3Buf));
                    if (bytesRead == 0) {
                        if (!s->sdFile.available()) {
                            s->fileFinished = true;
                            #ifdef DEBUG
                            log_message(String("Stream ") + i + ": MP3 EOF detected (read 0)");
                            #endif
                        } else {
                             #ifdef DEBUG
                             log_message(String("Stream ") + i + ": MP3 read 0 but available!");
                             #endif
                        }
                    }
                }
                mutex_exit(&sd_mutex);
                
                if (bytesRead > 0 && s->decoderIndex != -1) {
                    // Set global context before writing
                    currentDecodingStream = i;
                    mp3Decoders[s->decoderIndex]->write(mp3Buf, bytesRead);
                    currentDecodingStream = -1;
                }
            }
            
        } else if (s->type == STREAM_TYPE_WAV_SD || s->type == STREAM_TYPE_WAV_FLASH) {
            // --- WAV (SD or Flash) ---
            // WAV is simpler, we read small chunks.
            // We need enough space for the expanded data.
            // Worst case: Mono 22.05kHz -> Stereo 44.1kHz = 4x expansion.
            // 512 bytes read = 256 samples input -> 1024 samples output.
            // To be safe and avoid any boundary issues, we check for 2048 samples.
            if (available > 2048) {
                uint8_t wavBuf[512];
                int bytesRead = 0;
                
                if (s->type == STREAM_TYPE_WAV_SD) {
                    mutex_enter_blocking(&sd_mutex);
                    if (s->sdFile) {
                        bytesRead = s->sdFile.read(wavBuf, sizeof(wavBuf));
                        if (bytesRead == 0) { 
                            s->fileFinished = true;
                            #ifdef DEBUG
                            log_message(String("Stream ") + i + ": WAV (SD) EOF detected");
                            #endif
                        }
                    }
                    mutex_exit(&sd_mutex);
                } else {
                    mutex_enter_blocking(&flash_mutex);
                    if (s->flashFile) {
                        bytesRead = s->flashFile.read(wavBuf, sizeof(wavBuf));
                        if (bytesRead == 0) { 
                            s->fileFinished = true;
                            #ifdef DEBUG
                            log_message(String("Stream ") + i + ": WAV (Flash) EOF detected");
                            #endif
                        }
                    }
                    mutex_exit(&flash_mutex);
                }
                
                if (bytesRead > 0) {
                    int samples = bytesRead / 2;
                    
                    // Handle 22.05kHz upsampling (duplicate samples)
                    if (s->sampleRate == 22050) {
                         if (s->channels == 2) {
                              // STEREO -> UPSAMPLE (Duplicate frame)
                              // Process in pairs (L+R) and duplicate each frame
                              // Input: L0, R0, L1, R1...
                              // Output: L0, R0, L0, R0, L1, R1, L1, R1...
                              for (int k = 0; k < samples; k += 2) {
                                  if (k + 1 >= samples) break; // Should not happen if aligned
                                  
                                  int16_t left = (int16_t)(wavBuf[k*2] | (wavBuf[k*2+1] << 8));
                                  int16_t right = (int16_t)(wavBuf[(k+1)*2] | (wavBuf[(k+1)*2+1] << 8));
                                  
                                  // Frame 1
                                  if (!s->ringBuffer->push(left)) break;
                                  if (!s->ringBuffer->push(right)) break;
                                  // Frame 2 (Duplicate)
                                  if (!s->ringBuffer->push(left)) break;
                                  if (!s->ringBuffer->push(right)) break;
                              }
                         } else {
                             // MONO -> STEREO (Duplicate) -> UPSAMPLE (Duplicate again)
                             // Total 4 pushes per input sample
                             for (int k = 0; k < samples; k++) {
                                 int16_t sample = (int16_t)(wavBuf[k*2] | (wavBuf[k*2+1] << 8));
                                 if (!s->ringBuffer->push(sample)) break; // L1
                                 if (!s->ringBuffer->push(sample)) break; // R1
                                 if (!s->ringBuffer->push(sample)) break; // L2
                                 if (!s->ringBuffer->push(sample)) break; // R2
                             }
                         }
                         
                    } else {
                        // Normal 44.1kHz handling
                        for (int k = 0; k < samples; k++) {
                            int16_t sample = (int16_t)(wavBuf[k*2] | (wavBuf[k*2+1] << 8));
                            
                            if (s->channels == 1) {
                                // MONO -> STEREO (Duplicate)
                                s->ringBuffer->push(sample); // Left
                                s->ringBuffer->push(sample); // Right
                            } else {
                                // STEREO (Pass through)
                                s->ringBuffer->push(sample);
                            }
                        }
                    }
                }
            }
        }
        
        // Auto-stop if finished and buffer empty
        if (s->fileFinished && s->ringBuffer->availableForRead() == 0) {
            s->stopRequested = true;
        }
    }
}

// OPTIMIZED: Integer-only Hard Limiter (Much faster than float version)
static inline void applyFastLimiter(int32_t &l, int32_t &r) {
    if (l > 32767) l = 32767;
    else if (l < -32768) l = -32768;

    if (r > 32767) r = 32767;
    else if (r < -32768) r = -32768;
}

namespace Mixer {
    // ===================================
    // Mixer (Core 1)
    // ===================================
    // Mixes samples from all active streams and sends to I2S.
    inline void processSample() {
        int32_t mixedLeft = 0;
        int32_t mixedRight = 0;
        bool activeAudio = false;

        // 1. Mix Streams
        for (int i = 0; i < MAX_STREAMS; i++) {
            if (streams[i].active && streams[i].ringBuffer->availableForRead() >= 2) {
                // Pop stereo samples (L, R)
                int16_t l = streams[i].ringBuffer->pop();
                int16_t r = streams[i].ringBuffer->pop();
                
                // Apply Volume
                // Gain is 0..256 (volume * master)
                // We'll do a simple float multiply for flexibility first, then optimize later if needed.
                // Or stick to int math: volume is float 0..1.0
                
                int32_t volFixed = (int32_t)(streams[i].volume * 256.0f);
                
                // Ramp Up (Fade In) over 50ms to prevent pops
                uint32_t elapsed = millis() - streams[i].startTime;
                if (elapsed < 50) {
                     int32_t ramp = (elapsed * 256) / 50;
                     if (ramp > 256) ramp = 256;
                     volFixed = (volFixed * ramp) >> 8;
                }

                int32_t gain = (volFixed * masterAttenMultiplier) >> 8; // Result 0..256 approx
                
                mixedLeft += ((int32_t)l * gain) >> 8;
                mixedRight += ((int32_t)r * gain) >> 8;
                activeAudio = true;
            }
        }

        // --- CHIRP / TONE GENERATOR ---
        if (chirp.active) {
            if (chirp.samplesLeft > 0) {
                // 1. Get Sine Value from LUT (index 0-255)
                uint8_t index = (chirp.phase >> 24); 
                int32_t sample = (int32_t)SINE_LUT[index];

                // 2. Scale up: LUT is +/- 127. 
                // We want standard audio levels (+/- 32000ish).
                // Shift << 8 gives +/- 32512.
                sample = sample << 8;

                // 3. Apply Volume (0-255)
                // sample * vol >> 8
                sample = (sample * chirp.volume) >> 8;

                // 4. Mix
                mixedLeft += sample;
                mixedRight += sample;

                // 5. Advance Phase
                chirp.phase += chirp.phaseInc;

                // 6. Sweep Frequency
                if (chirp.sweepStep != 0) {
                    chirp.phaseInc += chirp.sweepStep;
                    if (chirp.sweepStep > 0 && chirp.phaseInc > chirp.targetInc) chirp.phaseInc = chirp.targetInc;
                    if (chirp.sweepStep < 0 && chirp.phaseInc < chirp.targetInc) chirp.phaseInc = chirp.targetInc;
                }

                chirp.samplesLeft--;
            } else {
                chirp.active = false;
            }
        }

        // --- OPTIMIZATION: Fast Limiter ---
        applyFastLimiter(mixedLeft, mixedRight);

        i2s.write16(i32_to_i16(mixedLeft), i32_to_i16(mixedRight));
    }
} 


// ===================================
// HELPER: Trigger a Chirp
// ===================================
// startFreq: Start Frequency in Hz
// endFreq:   End Frequency in Hz
// durationMs: Duration in Milliseconds
// vol:        Volume (0-255)
void playChirp(int startFreq, int endFreq, int durationMs, uint8_t vol = 128) {
    if (durationMs <= 0) return;
    
    double incPerHz = 4294967296.0 / (double)SAMPLE_RATE;
    uint32_t startInc = (uint32_t)(startFreq * incPerHz);
    uint32_t endInc = (uint32_t)(endFreq * incPerHz);
    
    uint32_t totalSamples = (durationMs * SAMPLE_RATE) / 1000;
    
    int32_t step = 0;
    if (totalSamples > 0) {
        step = (int32_t)((double)((int64_t)endInc - (int64_t)startInc) / (double)totalSamples);
    }

    chirp.active = false; // Pause
    chirp.phase = 0;
    chirp.phaseInc = startInc;
    chirp.targetInc = endInc;
    chirp.sweepStep = step;
    chirp.samplesLeft = totalSamples;
    chirp.volume = vol;
    chirp.active = true; // Go
}


// ===================================
// MP3 Decoder Callback
// ===================================
void mp3DataCallback(MP3FrameInfo &info, int16_t *pcm_buffer, size_t len, void* ref) {
    // Use global context since library doesn't pass user data through write() correctly
    int streamIdx = currentDecodingStream;
    if (streamIdx < 0 || streamIdx >= MAX_STREAMS) return;
    
    RingBuffer* rb = streams[streamIdx].ringBuffer;
    
    // Check channels from decoder info
    int channels = info.nChans;
    
    // Debug: Check sample rate mismatch
    static int lastSampleRate = 0;
    if (info.samprate != lastSampleRate && info.samprate != 0) {
        lastSampleRate = info.samprate;
    }
    if (streams[streamIdx].sampleRate == 0 && info.samprate != 0) {
        streams[streamIdx].sampleRate = info.samprate;
    }
    // Handle 22.05kHz upsampling vs Normal 44.1kHz
    if (info.samprate == 22050) {
        // --- 22.05kHz Handling ---
        if (channels == 2) {
             // Stereo: Process in pairs (L, R) and duplicate the frame
             for (size_t i = 0; i < len; i += 2) {
                 if (i + 1 >= len) break;
                 
                 int16_t left = pcm_buffer[i];
                 int16_t right = pcm_buffer[i+1];
                 
                 // Frame 1
                 if (!rb->push(left)) break;
                 if (!rb->push(right)) break;
                 // Frame 2 (Duplicate)
                 if (!rb->push(left)) break;
                 if (!rb->push(right)) break;
             }
        } else {
            // Mono: Duplicate sample 4 times (L1, R1, L2, R2)
             for (size_t i = 0; i < len; i++) {
                int16_t sample = pcm_buffer[i];
                if (!rb->push(sample)) break; // L1
                if (!rb->push(sample)) break; // R1
                if (!rb->push(sample)) break; // L2
                if (!rb->push(sample)) break; // R2
             }
        }
    } else {
        // --- Normal 44.1kHz Handling ---
        for (size_t i = 0; i < len; i++) {
            if (channels == 1) {
                // MONO -> STEREO (Duplicate)
                if (!rb->push(pcm_buffer[i])) break; // Left
                if (!rb->push(pcm_buffer[i])) break; // Right
            } else {
                // STEREO (Pass through)
                rb->push(pcm_buffer[i]);
            }
        }
    }
}


// ===================================
// SETUP1 (Core 1)
// ===================================
void setup1() {
    // Core 1 setup
}


// ===================================
// LOOP1 - Audio Processing (Core 1)
// ===================================
void loop1() {
    bool isRunning = false;

    while (true) {
        if (g_allowAudio) {
            if (!isRunning) {
                i2s.begin(SAMPLE_RATE);
                isRunning = true;
            }
            Mixer::processSample();
        } else {
            if (isRunning) {
                i2s.end();
                isRunning = false;
            }
            delay(1);
        }
    }
}


// ===================================
// Start Stream Playback
// ===================================
bool startStream(int streamIdx, const char* filename) {
    if (streamIdx < 0 || streamIdx >= MAX_STREAMS) return false;
    
    stopStream(streamIdx); // Ensure stopped first
    
    AudioStream* s = &streams[streamIdx];
    
    // Determine file type and location
    // Convention: "/flash/..." is Flash, otherwise SD
    bool isFlash = (strncmp(filename, "/flash/", 7) == 0);
    const char* ext = strrchr(filename, '.');
    bool isMP3 = (ext && strcasecmp(ext, ".mp3") == 0);
    
    if (isFlash) {
        // --- WAV from Flash ---
        mutex_enter_blocking(&flash_mutex);
        s->flashFile = LittleFS.open(filename, "r");
        if (!s->flashFile) {
            log_message(String("Stream ") + streamIdx + ": ERROR - Could not open flash file");
            mutex_exit(&flash_mutex);
            return false;
        }
        
        // Read Header & Find Data Chunk
        WAVHeader header;
        s->flashFile.read((uint8_t*)&header, sizeof(WAVHeader));
        
        // Check for "data" chunk (basic check)
        // If not "data", we might need to skip chunks.
        // For now, do a simple search for "data"
        if (strncmp(header.data, "data", 4) != 0) {
            // Header is likely larger or has extra chunks.
            // Reset to 12 (after RIFF/WAVE) and search
            s->flashFile.seek(12);
            
            char chunkID[4];
            uint32_t chunkSize;
            
            while (s->flashFile.available()) {
                s->flashFile.read((uint8_t*)chunkID, 4);
                s->flashFile.read((uint8_t*)&chunkSize, 4);
                
                if (strncmp(chunkID, "data", 4) == 0) {
                    // Found data!
                    break; 
                } else {
                    // Skip this chunk
                    s->flashFile.seek(s->flashFile.position() + chunkSize);
                }
            }
        }
        
        s->channels = header.numChannels;
        s->sampleRate = header.sampleRate;
        if (s->channels < 1 || s->channels > 2) s->channels = 2; 
        
        s->type = STREAM_TYPE_WAV_FLASH;
        mutex_exit(&flash_mutex);
        
    } else {
        // --- SD Card File ---
        mutex_enter_blocking(&sd_mutex);
        s->sdFile = sd.open(filename, FILE_READ);
        if (!s->sdFile) {
            log_message(String("Stream ") + streamIdx + ": ERROR - Could not open SD file");
            mutex_exit(&sd_mutex);
            return false;
        }
        
        if (isMP3) {
            // --- MP3 Setup ---
            // Find free decoder
            int decoderIdx = -1;
            for (int i = 0; i < MAX_MP3_DECODERS; i++) {
                if (!mp3DecoderInUse[i]) {
                    decoderIdx = i;
                    mp3DecoderInUse[i] = true;
                    break;
                }
            }
            
            if (decoderIdx == -1) {
                log_message(String("Stream ") + streamIdx + ": ERROR - No MP3 decoders available");
                s->sdFile.close();
                mutex_exit(&sd_mutex);
                return false;
            }
            
            s->decoderIndex = decoderIdx;
            s->decoderIndex = decoderIdx;
            s->type = STREAM_TYPE_MP3_SD;
            s->channels = 2; 
            s->sampleRate = 0; // Unknown until first frame decoded 
            
            // Initialize Decoder
            if (mp3Decoders[decoderIdx]) {
                mp3Decoders[decoderIdx]->begin();
            }
            
        } else {
            // --- WAV from SD ---
            // Read Header & Find Data Chunk
            WAVHeader header;
            s->sdFile.read((uint8_t*)&header, sizeof(WAVHeader));
            
            if (strncmp(header.data, "data", 4) != 0) {
                s->sdFile.seek(12);
                char chunkID[4];
                uint32_t chunkSize;
                
                while (s->sdFile.available()) {
                    s->sdFile.read((uint8_t*)chunkID, 4);
                    s->sdFile.read((uint8_t*)&chunkSize, 4);
                    
                    if (strncmp(chunkID, "data", 4) == 0) {
                        break; 
                    } else {
                        s->sdFile.seek(s->sdFile.position() + chunkSize);
                    }
                }
            }
            
            s->channels = header.numChannels;
            s->sampleRate = header.sampleRate;
            if (s->channels < 1 || s->channels > 2) s->channels = 2;
            
            s->type = STREAM_TYPE_WAV_SD;
            s->decoderIndex = -1;
        }
        mutex_exit(&sd_mutex);
    }
    
    strncpy(s->filename, filename, sizeof(s->filename) - 1);
    s->ringBuffer->clear();
    s->active = true;
    s->fileFinished = false;
    s->startTime = millis(); // Log start time
    
    log_message(String("Stream ") + streamIdx + ": Playing " + filename + " (Start: " + s->startTime + "ms)");
    
    if (isMP3) {
        log_message(String("  Format: MP3, Rate: ") + (s->sampleRate > 0 ? String(s->sampleRate) : "Unknown") + "Hz, Ch: " + s->channels);
    } else {
        // Read details for WAV debugging
        uint16_t bits = 0;
        uint16_t align = 0;
        if (s->type == STREAM_TYPE_WAV_SD) {
             mutex_enter_blocking(&sd_mutex);
             if (s->sdFile) {
                 s->sdFile.seek(34); // bitsPerSample
                 s->sdFile.read(&bits, 2);
                 s->sdFile.seek(32); // blockAlign
                 s->sdFile.read(&align, 2);
                 s->sdFile.seek(44);
                 uint32_t pos = s->sdFile.position();
                 s->sdFile.seek(34); s->sdFile.read(&bits, 2);
                 s->sdFile.seek(32); s->sdFile.read(&align, 2);
                 s->sdFile.seek(pos);
             }
             mutex_exit(&sd_mutex);
        } else if (s->type == STREAM_TYPE_WAV_FLASH) {
             mutex_enter_blocking(&flash_mutex);
             if (s->flashFile) {
                 uint32_t pos = s->flashFile.position();
                 s->flashFile.seek(34); s->flashFile.read((uint8_t*)&bits, 2);
                 s->flashFile.seek(32); s->flashFile.read((uint8_t*)&align, 2);
                 s->flashFile.seek(pos);
             }
             mutex_exit(&flash_mutex);
        }
        log_message(String("  Format: WAV, Rate: ") + s->sampleRate + "Hz, Ch: " + s->channels + ", Bits: " + bits + ", Align: " + align);
    }
    return true;
}


// ===================================
// Stop Stream Playback
// ===================================
void stopStream(int streamIdx) {
    if (streamIdx < 0 || streamIdx >= MAX_STREAMS) return;
    AudioStream* s = &streams[streamIdx];
    
    if (!s->active && s->type == STREAM_TYPE_INACTIVE) return;
    
    s->active = false;
    
    // Release Decoder
    if (s->type == STREAM_TYPE_MP3_SD && s->decoderIndex != -1) {
        if (mp3Decoders[s->decoderIndex]) {
            mp3Decoders[s->decoderIndex]->end();
        }
        mp3DecoderInUse[s->decoderIndex] = false;
        s->decoderIndex = -1;
    }
    
    // Close Files
    if (s->type == STREAM_TYPE_WAV_FLASH) {
        mutex_enter_blocking(&flash_mutex);
        if (s->flashFile) s->flashFile.close();
        mutex_exit(&flash_mutex);
    } else if (s->type == STREAM_TYPE_WAV_SD || s->type == STREAM_TYPE_MP3_SD) {
        mutex_enter_blocking(&sd_mutex);
        if (s->sdFile) s->sdFile.close();
        mutex_exit(&sd_mutex);
    }
    
    s->type = STREAM_TYPE_INACTIVE;
    s->ringBuffer->clear();
    
    uint32_t duration = millis() - s->startTime;
    log_message(String("Stream ") + streamIdx + ": Stopped (Duration: " + duration + "ms)");
}