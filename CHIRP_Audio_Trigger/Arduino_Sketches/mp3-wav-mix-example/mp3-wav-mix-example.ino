// MP3 + WAV Player using arduino-libhelix - v20250902_007
/*
 * This Arduino sketch is for a Raspberry Pi Pico 2 with SD card module and PCM5102A I2S module
 * Using the Arduino IDE and most recent arduino-pico core by earlephilhower (v5.0.0).
 * Using the latest libhelix library (v0.9.1) for MP3 decoding.
 * At a button press the Pico 2 begins to play a long MP3 music track.
 * On the second press the Pico 2 mixes a second WAV over the MP3.
 * The MP3 file (test1.mp3) is stereo 44100Hz 128kb/s constant bitrate.
 * The WAV file (test2.wav) is mono 44100Hz 16-bit.
 */

#include "MP3DecoderHelix.h"
#include <SdFat.h>
#include <I2S.h>
#include "pico/mutex.h"

using namespace libhelix;

// Create a mutex to protect SD card access
auto_init_mutex(sd_mutex);

// Pin definitions
#define SD_CS 13
#define SD_MISO 12
#define SD_MOSI 15
#define SD_SCK 14
#define I2S_BCLK 9
#define I2S_LRCK 10
#define I2S_DATA 11
#define INPUT_BUTTON 27

// Audio parameters
#define SAMPLE_RATE 44100

// ============================================
// == VOLUME CONTROL (0 = silent, 255 = max) ==
// ============================================
byte mp3Volume = 50;  // Set volume for the MP3 track
byte wavVolume = 255; // Set volume for the WAV overlay
// ============================================


// WAV header structure
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

// Create I2S instance
I2S i2s(OUTPUT, I2S_BCLK, I2S_DATA);

// SdFat objects
SdFat sd;
FsFile mp3File;
FsFile wavFile;

// WAV buffer for overlay
#define WAV_BUFFER_SIZE 2048
int16_t wavBuffer[WAV_BUFFER_SIZE];
int wavBufferPos = 0;
int wavBufferLevel = 0;

// Playback state - volatile because it's accessed by both cores
volatile bool isPlayingMP3 = false;
volatile bool isPlayingWAV = false;
volatile bool startMp3Request = false; // Core 0 sets this to ask Core 1 to start
bool mp3WasPlaying = false; // For detecting when MP3 finishes
bool buttonPressed = false;
unsigned long lastButtonTime = 0;
const unsigned long debounceDelay = 200;

// Debug variables
unsigned long lastDebugTime = 0;
unsigned long debugInterval = 2000; // Print debug info every 2 seconds
unsigned long mp3CallbackCount = 0;
unsigned long wavSamplesConsumed = 0;
unsigned long wavBufferUnderruns = 0;
unsigned long lastWavFillTime = 0;
int minBufferLevel = WAV_BUFFER_SIZE;
int maxBufferLevel = 0;

// MP3 decoder callback - directly output to I2S with WAV mixing and volume control
void mp3DataCallback(MP3FrameInfo &info, int16_t *pcm_buffer, size_t len, void* ref) {
  mp3CallbackCount++;
  
  static bool infoPrinted = false;
  if (!infoPrinted) {
    Serial.println("=== MP3 DECODER INFO (from Core " + String(get_core_num()) + ") ===");
    Serial.println("Sample Rate: " + String(info.samprate) + "Hz");
    Serial.println("Channels: " + String(info.nChans));
    Serial.println("Samples per frame: " + String(len));
    infoPrinted = true;
  }
  
  for (size_t i = 0; i < len; i += info.nChans) {
    int16_t mp3Left = pcm_buffer[i];
    int16_t mp3Right = (info.nChans == 2) ? pcm_buffer[i + 1] : pcm_buffer[i];
    
    int32_t mixedLeft = ((int32_t)mp3Left * mp3Volume) / 255;
    int32_t mixedRight = ((int32_t)mp3Right * mp3Volume) / 255;

    if (isPlayingWAV && wavBufferLevel > 0) {
      int16_t wavSample = wavBuffer[wavBufferPos];
      wavBufferPos = (wavBufferPos + 1) % WAV_BUFFER_SIZE;
      wavBufferLevel--;
      wavSamplesConsumed++;
      
      if (wavBufferLevel < minBufferLevel) minBufferLevel = wavBufferLevel;
      if (wavBufferLevel > maxBufferLevel) maxBufferLevel = wavBufferLevel;
      
      int32_t scaledWavSample = ((int32_t)wavSample * wavVolume) / 255;
      mixedLeft += scaledWavSample;
      mixedRight += scaledWavSample;
      
    } else if (isPlayingWAV && wavBufferLevel == 0) {
      wavBufferUnderruns++;
    }
    
    int16_t finalLeft = constrain(mixedLeft, -32768, 32767);
    int16_t finalRight = constrain(mixedRight, -32768, 32767);

    i2s.write16(finalLeft, finalRight);
  }
}

// Create MP3 decoder with callback
MP3DecoderHelix mp3Decoder(mp3DataCallback);

// ===================================
// == Code for the Second Core (Core1)
// ===================================
void setup1() {
  // This core's main job is to feed the MP3 decoder
}

void loop1() {
  // Check for a start request from Core 0
  if (startMp3Request) {
    startMp3Request = false; // Acknowledge request
    
    // Ensure we are not already playing
    if (!isPlayingMP3) {
      Serial.println("Core 1: Start request received. Opening MP3 file...");
      mutex_enter_blocking(&sd_mutex);
      mp3File = sd.open("test1.mp3", FILE_READ);
      mutex_exit(&sd_mutex);

      if (mp3File) {
        mp3CallbackCount = 0;
        mp3Decoder.begin();
        isPlayingMP3 = true; // Set the official playing state
        Serial.println("Core 1: MP3 playback started successfully.");
      } else {
        Serial.println("Core 1: Failed to open test1.mp3.");
        isPlayingMP3 = false; // Make sure state is false if open fails
      }
    }
  }

  if (isPlayingMP3) {
    mutex_enter_blocking(&sd_mutex);
    // Make sure the file handle is still valid before checking available()
    bool available = mp3File && mp3File.available();
    mutex_exit(&sd_mutex);

    if (available) {
      uint8_t buffer[512];
      
      mutex_enter_blocking(&sd_mutex);
      int bytesRead = mp3File.read(buffer, 512);
      mutex_exit(&sd_mutex);

      if (bytesRead > 0) {
        mp3Decoder.write(buffer, bytesRead);
      } else {
        isPlayingMP3 = false; // End of file, signal Core 0
      }
    } else {
        isPlayingMP3 = false; // File not available or closed, signal Core 0
    }
  } else {
    // If not playing, ensure the file handle is safely closed.
    mutex_enter_blocking(&sd_mutex);
    if (mp3File) {
      mp3File.close();
    }
    mutex_exit(&sd_mutex);
    delay(1); // Don't spin too fast when idle
  }
}

// ===================================
// == Code for the Main Core (Core0)
// ===================================
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000);
  
  Serial.println("MP3 + WAV Player Initializing (Core Safe Version)...");
  
  pinMode(INPUT_BUTTON, INPUT_PULLUP);
  
  Serial.print("Initializing SD card...");
  SPI1.setRX(SD_MISO);
  SPI1.setTX(SD_MOSI);
  SPI1.setSCK(SD_SCK);
  
  SdSpiConfig sdConfig(SD_CS, DEDICATED_SPI, 20000000, &SPI1);
  
  if (!sd.begin(sdConfig)) {
    Serial.println("SD card initialization failed!");
    sd.initErrorHalt(&Serial);
    while (1);
  }
  Serial.println("SD card initialized.");
  
  if (!sd.exists("test1.mp3")) Serial.println("Warning: test1.mp3 not found!");
  if (!sd.exists("test2.wav")) Serial.println("Warning: test2.wav not found!");

  Serial.print("Initializing I2S...");
  if (!i2s.begin(SAMPLE_RATE)) {
    Serial.println("I2S initialization failed!");
    while (1);
  }
  Serial.println("I2S initialized at " + String(SAMPLE_RATE) + "Hz.");
  
  mp3Decoder.begin();
  
  Serial.println("System ready!");
}

void loop() {
  handleButton();

  if (isPlayingWAV) {
    fillWAVBuffer();
  }

  // Check if Core 1 has finished with the MP3
  if (mp3WasPlaying && !isPlayingMP3) {
      Serial.println("Core 0: Detected MP3 has finished.");
      if (!isPlayingWAV) {
        Serial.println("All playback finished.");
      }
  }
  mp3WasPlaying = isPlayingMP3; // Update state for next loop

  if (millis() - lastDebugTime > debugInterval) {
    printDebugInfo();
    lastDebugTime = millis();
  }
}

void handleButton() {
  if (digitalRead(INPUT_BUTTON) == LOW && !buttonPressed && 
      (millis() - lastButtonTime > debounceDelay)) {
    buttonPressed = true;
    lastButtonTime = millis();
    
    if (!isPlayingMP3 && !isPlayingWAV) {
      startMP3Playback();
    } else if (isPlayingMP3 && !isPlayingWAV) {
      startWAVPlayback();
    } else {
      stopAllPlayback();
    }
  }
  
  if (digitalRead(INPUT_BUTTON) == HIGH && buttonPressed) {
    buttonPressed = false;
  }
}

void startMP3Playback() {
  // This function now only signals Core 1 to start.
  // Core 1 will handle opening the file.
  Serial.println("Core 0: Requesting MP3 playback...");
  if (!isPlayingMP3) {
      startMp3Request = true;
      mp3WasPlaying = true; // Set this here to track that we started
  }
}

void startWAVPlayback() {
  Serial.println("Starting WAV overlay...");
  
  bool success;
  mutex_enter_blocking(&sd_mutex);
  wavFile = sd.open("test2.wav", FILE_READ);
  success = wavFile;
  if (success) {
    success = validateAndSkipWAVHeader(wavFile); // This also reads from the file
  }
  mutex_exit(&sd_mutex);
  
  if (!success) {
    Serial.println("Failed to open or validate test2.wav");
    mutex_enter_blocking(&sd_mutex);
    if(wavFile) wavFile.close();
    mutex_exit(&sd_mutex);
    return;
  }
  
  wavBufferLevel = 0;
  wavBufferPos = 0;
  wavSamplesConsumed = 0;
  wavBufferUnderruns = 0;
  minBufferLevel = WAV_BUFFER_SIZE;
  maxBufferLevel = 0;
  
  isPlayingWAV = true;
  Serial.println("WAV overlay started");
}

void fillWAVBuffer() {
  unsigned long fillStartTime = millis();
  
  mutex_enter_blocking(&sd_mutex);
  // Only try to read if the file handle is valid
  while (wavBufferLevel < (WAV_BUFFER_SIZE - 512) && wavFile && wavFile.available()) {
    uint8_t tempBuffer[1024 * 2];
    int maxSamplesToRead = WAV_BUFFER_SIZE - wavBufferLevel;
    int bytesToRead = min(min(sizeof(tempBuffer), (unsigned int)(maxSamplesToRead * 2)), wavFile.available());
    int bytesRead = wavFile.read(tempBuffer, bytesToRead);
    int samplesRead = bytesRead / 2;
    
    if (bytesRead > 0) {
      int writeStart = (wavBufferPos + wavBufferLevel) % WAV_BUFFER_SIZE;
      for (int i = 0; i < samplesRead; i++) {
        int writePos = (writeStart + i) % WAV_BUFFER_SIZE;
        wavBuffer[writePos] = (int16_t)(tempBuffer[i * 2] | (tempBuffer[i * 2 + 1] << 8));
      }
      wavBufferLevel += samplesRead;
      if (wavBufferLevel > maxBufferLevel) maxBufferLevel = wavBufferLevel;
    } else {
      break; // Read failed
    }
  }

  bool wavStillGoing = true;
  if (wavFile && !wavFile.available() && wavBufferLevel == 0) {
    Serial.println("WAV file finished");
    wavFile.close();
    wavStillGoing = false;
  }
  mutex_exit(&sd_mutex);

  if(!wavStillGoing){
    isPlayingWAV = false;
  }
  
  lastWavFillTime = millis() - fillStartTime;
}

bool validateAndSkipWAVHeader(FsFile& file) {
  WAVHeader header;
  
  if (file.readBytes((char*)&header, sizeof(WAVHeader)) != sizeof(WAVHeader)) {
    Serial.println("Failed to read WAV header");
    return false;
  }
  
  if (strncmp(header.riff, "RIFF", 4) != 0 || 
      strncmp(header.wave, "WAVE", 4) != 0 ||
      header.audioFormat != 1) {
    Serial.println("Invalid WAV format");
    return false;
  }
  
  Serial.println("=== WAV FILE INFO ===");
  Serial.println("Sample Rate: " + String(header.sampleRate) + "Hz");
  Serial.println("Channels: " + String(header.numChannels));
  Serial.println("Bits per Sample: " + String(header.bitsPerSample));
  Serial.println("Data Size: " + String(header.dataSize) + " bytes");
  Serial.println("====================");
  
  return true;
}

void printDebugInfo() {
  if (isPlayingMP3 || isPlayingWAV) {
    Serial.println("=== DEBUG INFO (Core " + String(get_core_num()) + ") ===");
    Serial.println("Time: " + String(millis()/1000) + "s");
    
    if (isPlayingMP3) {
      mutex_enter_blocking(&sd_mutex);
      unsigned long pos = mp3File ? mp3File.position() : 0;
      size_t rem = mp3File ? mp3File.available() : 0;
      mutex_exit(&sd_mutex);
      Serial.println("MP3: Callbacks=" + String(mp3CallbackCount) + 
                     " FilePos=" + String(pos) + 
                     " Remaining=" + String(rem));
    }
    
    if (isPlayingWAV) {
      unsigned long currentMillis = millis();
      float elapsedTime = (currentMillis > 0) ? currentMillis / 1000.0 : 1.0;
      float consumptionRate = (elapsedTime > 0) ? (wavSamplesConsumed / elapsedTime) : 0;
      Serial.println("WAV: Level=" + String(wavBufferLevel) + 
                     "/" + String(WAV_BUFFER_SIZE) + 
                     " Underruns=" + String(wavBufferUnderruns));
      Serial.println("     Rate=" + String(consumptionRate, 1) + "sps" +
                     " Expected=" + String(SAMPLE_RATE) + "sps");
      Serial.println("     BufferRange=" + String(minBufferLevel) + 
                     "-" + String(maxBufferLevel) +
                     " LastFillTime=" + String(lastWavFillTime) + "ms");
                     
      minBufferLevel = wavBufferLevel;
      maxBufferLevel = wavBufferLevel;
    }
    Serial.println("==================");
  }
}

void stopAllPlayback() {
  Serial.println("Stopping all playback");
  
  isPlayingMP3 = false; // Signal Core 1 to stop and clean up its file.
  isPlayingWAV = false; 
  
  // Clean up WAV file immediately on Core 0
  mutex_enter_blocking(&sd_mutex);
  if(wavFile) wavFile.close();
  mutex_exit(&sd_mutex);
  wavBufferLevel = 0; 
  
  delay(10); // Give loops time to react and close files.
  Serial.println("All playback stopped");
}
