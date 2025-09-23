// Pico MP3 Trigger - v20250921_28
/*
 * A fully-featured, dual-core audio player for the Raspberry Pi Pico,
 * designed to emulate and extend the functionality of the SparkFun MP3 Trigger.
 *
 * --- Functional Summary ---
 * - MP3 & WAV Playback: Plays standard MP3 files and both mono and stereo 16-bit WAV files.
 * - Advanced Audio Mixing: Can mix a WAV over an MP3. Stopping the MP3 will
 * not interrupt the WAV, which will continue playing over a silent background.
 * - Flexible Filename Matching: Triggers files that START WITH a 3-digit number (e.g., "005_sound.mp3").
 * - Self-Sufficient: Automatically creates a valid "000SILENCE.MP3" file on
 * the SD card if it is missing, using a frame-by-frame generation method.
 * - MP3TRIGR.INI Compatibility: Parses the INI file using the '#' prefix
 * and space delimiters, exactly like the original MP3 Trigger.
 * - Dual-Core Operation: Plays MP3s on Core 1, handles mixing/UI on Core 0 for smooth performance.
 * - SD Card File Caching: Scans and caches all track filenames on startup for instant track triggering.
 * - Self-Healing INI File: Reads 'MP3TRIGR.INI' for settings. Creates the file with defaults
 * and adds any missing keys if needed, preserving comments and structure.
 * - Smart Attenuation: Automatically reduces WAV volume when mixing over a real MP3 to prevent
 * clipping, and applies a master headroom reduction to the MP3 signal.
 * - Status LED: Onboard LED is solid ON for a single track, and blinks when a WAV is mixed over a real MP3.
 * - Serial Control: Responds to a comprehensive set of ASCII and binary serial commands.
 * - Pushbutton Control: Supports physical buttons for Forward, Rewind, and Start/Stop.
 * - Finishes Playback Message: Sends an 'X' character upon track completion.
 * - Command Toggling: Sending a trigger command for a currently playing track will stop that specific track.
 *
 * Implemented Serial Commands:
 * - 't' + byte:    Binary Trigger. Interrupts playback to play track (0-255).
 * - 'T' + ASCII:   ASCII Trigger. Interrupts playback to play track (e.g., "T5"). Sends a "P!" response.
 * - 'p' + byte:    Binary Play. Plays track (0-255) ONLY if player is silent.
 * - 'P' + ASCII:   ASCII Play. Plays track (e.g., "P12") ONLY if player is silent. Sends a "P!" response.
 * - 'v' + byte/ASCII: Set Volume (0=Max, 255=Mute). Accepts binary byte in Mode 1, ASCII in Mode 2.
 * - 'O':           Start/Stop. Toggles playback of the current track.
 * - 'F':           Forward. Plays the next track in the file list.
 * - 'R':           Rewind. Plays the previous track in the file list.
 *
 * Pushbutton Controls:
 * - FWD_PIN:       Plays the next track.
 * - REV_PIN:       Plays the previous track.
 * - PLAY_PIN:      Toggles Start/Stop for the current track.
 */

#include "MP3DecoderHelix.h"
#include <SdFat.h>
#include <I2S.h>
#include "pico/mutex.h"

#define VERSION_STRING "v20250921_28"

using namespace libhelix;

// ===================================
// ==      Configuration        ==
// ===================================
// Default values, will be overridden by MP3TRIGR.INI if present.
long serialBaud = 115200;
int masterAttenuation = 97; // 1-100%, prevents clipping
int boardMode = 1; // 1 = MP3 Trigger (Binary Vol), 2 = Developer (ASCII Vol)

// Create a mutex to protect SD card access
auto_init_mutex(sd_mutex);
// Create a mutex to protect the Serial port from concurrent access
auto_init_mutex(log_mutex);

// Pin definitions
#define LED_PIN 25
#define SD_CS 13
#define SD_MISO 12
#define SD_MOSI 15
#define SD_SCK 14
#define I2S_BCLK 9
#define I2S_LRCK 10
#define I2S_DATA 11
#define FWD_PIN 28
#define REV_PIN 27
#define PLAY_PIN 26

// Audio parameters
#define SAMPLE_RATE 44100
#define MAX_FILENAME_LEN 32 
#define MAX_TRACKS 100

// ============================================
// == VOLUME CONTROL (0 = max, 255 = mute) ==
// ============================================
byte mp3Volume = 0;
byte wavVolume = 0;
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

// I2S and SD card objects
I2S i2s(OUTPUT, I2S_BCLK, I2S_DATA);
SdFat sd;
FsFile mp3File;
FsFile wavFile;

// WAV buffer for overlay
#define WAV_BUFFER_SIZE 4096 // Optimized buffer size
int16_t wavBuffer[WAV_BUFFER_SIZE];
int wavBufferPos = 0;
int wavBufferLevel = 0;

// Track list for navigation
char trackList[MAX_TRACKS][MAX_FILENAME_LEN];
int trackCount = 0;
int currentTrackIndex = -1;
char core1PlayingFile[MAX_FILENAME_LEN];

// Playback state
volatile bool isPlayingMP3 = false;
volatile bool isPlayingWAV = false;
volatile int mp3TrackToPlay = -1;
volatile int currentWavTrackNumber = -1;
volatile int currentMp3TrackNumber = -1;
volatile int currentWavChannels = 0;
volatile bool playbackStartedSuccessfully = false;
volatile bool stopMp3Request = false;
volatile bool loopSilentTrack = false;
bool mp3WasPlaying = false;
bool wavWasPlaying = false;
bool responseRequested = false;

// Button state
unsigned long lastFwdTime = 0;
unsigned long lastRevTime = 0;
unsigned long lastPlayTime = 0;
const unsigned long debounceDelay = 200;

// LED Blink state
unsigned long lastBlinkTime = 0;
const unsigned int blinkInterval = 200; 

// Logging helpers to add timestamps, now thread-safe
void log_message(String msg) {
  if (boardMode == 2) {
    mutex_enter_blocking(&log_mutex);
    Serial.println("[" + String(millis()) + "] " + msg);
    mutex_exit(&log_mutex);
  }
}

void send_serial_response(const char* msg) {
  mutex_enter_blocking(&log_mutex);
  Serial.println(msg);
  mutex_exit(&log_mutex);
}

// Custom implementation of strcasestr to ensure portability
const char* my_strcasestr(const char* haystack, const char* needle) {
  if (!needle || !*needle) return haystack;
  for (; *haystack; ++haystack) {
    const char* h = haystack;
    const char* n = needle;
    while (*h && *n && (tolower((unsigned char)*h) == tolower((unsigned char)*n))) {
      h++;
      n++;
    }
    if (!*n) return haystack;
  }
  return nullptr;
}


// MP3 decoder callback
void mp3DataCallback(MP3FrameInfo &info, int16_t *pcm_buffer, size_t len, void* ref) {
  for (size_t i = 0; i < len; i += info.nChans) {
    int16_t mp3Left = pcm_buffer[i];
    int16_t mp3Right = (info.nChans == 2) ? pcm_buffer[i + 1] : pcm_buffer[i];
    
    mp3Left = (int16_t)(((int32_t)mp3Left * masterAttenuation) / 100);
    mp3Right = (int16_t)(((int32_t)mp3Right * masterAttenuation) / 100);

    int32_t mixedLeft = ((int32_t)mp3Left * (255 - mp3Volume)) / 255;
    int32_t mixedRight = ((int32_t)mp3Right * (255 - mp3Volume)) / 255;
    
    if (isPlayingWAV && wavBufferLevel > 0) {
      int32_t scaledWavSampleL = 0;
      int32_t scaledWavSampleR = 0;

      if (currentWavChannels == 1 && wavBufferLevel >= 1) {
        int16_t wavSample = wavBuffer[wavBufferPos];
        wavBufferPos = (wavBufferPos + 1) % WAV_BUFFER_SIZE;
        wavBufferLevel--;
        scaledWavSampleL = scaledWavSampleR = ((int32_t)wavSample * (255 - wavVolume)) / 255;
      } else if (currentWavChannels == 2 && wavBufferLevel >= 2) {
        int16_t wavLeft = wavBuffer[wavBufferPos];
        int16_t wavRight = wavBuffer[(wavBufferPos + 1) % WAV_BUFFER_SIZE];
        wavBufferPos = (wavBufferPos + 2) % WAV_BUFFER_SIZE;
        wavBufferLevel -= 2;
        scaledWavSampleL = ((int32_t)wavLeft * (255 - wavVolume)) / 255;
        scaledWavSampleR = ((int32_t)wavRight * (255 - wavVolume)) / 255;
      }
      
      if (strcmp(core1PlayingFile, "000SILENCE.MP3") != 0) {
        scaledWavSampleL /= 2;
        scaledWavSampleR /= 2;
      }
      
      mixedLeft += scaledWavSampleL;
      mixedRight += scaledWavSampleR;
    }

    i2s.write16(constrain(mixedLeft, -32768, 32767), constrain(mixedRight, -32768, 32767));
  }
}

MP3DecoderHelix mp3Decoder(mp3DataCallback);

// OPTIMIZED: This function now searches the cached trackList in RAM.
bool findTrack(int trackNumber, char* filenameBuffer, int bufferSize) {
  char search_prefix[4];
  snprintf(search_prefix, sizeof(search_prefix), "%03d", trackNumber);
  
  for (int i = 0; i < trackCount; i++) {
    if (strncmp(search_prefix, trackList[i], 3) == 0) {
      strncpy(filenameBuffer, trackList[i], bufferSize - 1);
      filenameBuffer[bufferSize - 1] = '\0';
      return true;
    }
  }
  
  return false;
}


// ===================================
// == Code for the Second Core (Core1)
// ===================================
void setup1() {}

void loop1() {
  if (stopMp3Request) {
    isPlayingMP3 = false;
    loopSilentTrack = false;
    stopMp3Request = false;
    currentMp3TrackNumber = -1;
  }

  if (mp3TrackToPlay != -1) {
    int track = mp3TrackToPlay;
    mp3TrackToPlay = -1;
    
    if (isPlayingMP3) {
      isPlayingMP3 = false;
      mutex_enter_blocking(&sd_mutex);
      if (mp3File) mp3File.close();
      mutex_exit(&sd_mutex);
      delay(5);
    }

    char filename[MAX_FILENAME_LEN];
    bool found = false;
    if (track == 0) {
      strcpy(filename, "000SILENCE.MP3");
      mutex_enter_blocking(&sd_mutex);
      found = sd.exists(filename);
      mutex_exit(&sd_mutex);
    } else {
      found = findTrack(track, filename, sizeof(filename));
    }
    
    if (found && my_strcasestr(filename, ".mp3")) {
      log_message("Core 1: Playing MP3 " + String(filename));
      
      mutex_enter_blocking(&sd_mutex);
      mp3File = sd.open(filename, FILE_READ);
      mutex_exit(&sd_mutex);

      if (mp3File) {
        strcpy(core1PlayingFile, filename);
        currentMp3TrackNumber = track;
        mp3Decoder.begin();
        isPlayingMP3 = true;
        playbackStartedSuccessfully = true;
        if (track > 0) {
          for (int i = 0; i < trackCount; i++) {
            if (strcmp(trackList[i], filename) == 0) {
              currentTrackIndex = i;
              break;
            }
          }
        }
      } else {
        log_message("Core 1: Found file but failed to open " + String(filename));
        isPlayingMP3 = false;
      }
    } else {
      isPlayingMP3 = false;
      if (found && !my_strcasestr(filename, ".mp3")) {
        log_message("Core 1: Track " + String(track) + " is not an MP3.");
      } else if (!found) {
        log_message("Core 1: Could not find track " + String(track));
      }
    }
  }

  if (isPlayingMP3) {
    uint8_t buffer[512];
    int bytesRead = 0;
    
    mutex_enter_blocking(&sd_mutex);
    if (mp3File) {
        bytesRead = mp3File.read(buffer, sizeof(buffer));
    }
    mutex_exit(&sd_mutex);

    if (bytesRead > 0) {
      mp3Decoder.write(buffer, bytesRead);
    } else {
      if (loopSilentTrack && strcmp(core1PlayingFile, "000SILENCE.MP3") == 0) {
        mutex_enter_blocking(&sd_mutex);
        mp3File.close();
        mp3File = sd.open("000SILENCE.MP3", FILE_READ);
        mutex_exit(&sd_mutex);
        if (mp3File) {
          mp3Decoder.begin();
        } else {
          log_message("Core 1: ERROR - Failed to re-open silent track.");
          isPlayingMP3 = false;
        }
      } else {
        isPlayingMP3 = false;
      }
    }
  } else {
    core1PlayingFile[0] = '\0';
    currentMp3TrackNumber = -1;
    mutex_enter_blocking(&sd_mutex);
    if (mp3File) mp3File.close();
    mutex_exit(&sd_mutex);
    delay(1);
  }
}

// ===================================
// == Code for the Main Core (Core0)
// ===================================

void scanTracks() {
  log_message("Scanning SD card for tracks...");
  trackCount = 0;
  FsFile root;
  FsFile file;
  mutex_enter_blocking(&sd_mutex);
  if (root.open("/")) {
    while (file.openNext(&root, O_RDONLY) && trackCount < MAX_TRACKS) {
      char tempName[MAX_FILENAME_LEN];
      file.getName(tempName, sizeof(tempName));
      if (!file.isDirectory() && (my_strcasestr(tempName, ".mp3") || my_strcasestr(tempName, ".wav"))) {
        if (strncmp(tempName, "000", 3) != 0) {
          strcpy(trackList[trackCount], tempName);
          trackCount++;
        }
      }
      file.close();
    }
    root.close();
  }
  mutex_exit(&sd_mutex);

  for (int i = 0; i < trackCount - 1; i++) {
    for (int j = 0; j < trackCount - i - 1; j++) {
      if (strcmp(trackList[j], trackList[j + 1]) > 0) {
        char temp[MAX_FILENAME_LEN];
        strcpy(temp, trackList[j]);
        strcpy(trackList[j], trackList[j + 1]);
        strcpy(trackList[j + 1], temp);
      }
    }
  }

  if (boardMode == 2) {
    log_message("Found " + String(trackCount) + " navigable tracks.");
    for (int i=0; i < trackCount; i++) {
      mutex_enter_blocking(&log_mutex);
      Serial.println("  " + String(i+1) + ": " + String(trackList[i]));
      mutex_exit(&log_mutex);
    }
  }
}

void parseIniFile() {
  bool baudFound = false;
  bool attenFound = false;
  bool modeFound = false;
  char iniBuffer[1025]; // Increased buffer size
  char newSettings[200] = "";
  
  mutex_enter_blocking(&sd_mutex);
  FsFile iniFile = sd.open("MP3TRIGR.INI", FILE_READ);
  
  if (iniFile) {
    if (boardMode == 2) log_message("Reading MP3TRIGR.INI...");
    int bytesRead = iniFile.read(iniBuffer, 1024);
    iniBuffer[bytesRead] = '\0';
    iniFile.close();
    
    char* line = strtok(iniBuffer, "\r\n");
    while(line != NULL) {
      if (line[0] == '*') break;

      if (line[0] == '#') {
        char* command = line + 1;
        while (*command == ' ') command++;
        
        if (strncasecmp(command, "BAUD", 4) == 0) {
          serialBaud = atol(command + 4);
          baudFound = true;
        } else if (strncasecmp(command, "ATTEN", 5) == 0) {
          masterAttenuation = atoi(command + 5);
          if (masterAttenuation < 1 || masterAttenuation > 100) masterAttenuation = 97;
          attenFound = true;
        } else if (strncasecmp(command, "MODE", 4) == 0) {
          boardMode = atoi(command + 4);
          if (boardMode < 1 || boardMode > 2) boardMode = 1;
          modeFound = true;
        }
      }
      line = strtok(NULL, "\r\n");
    }

    if (!baudFound || !attenFound || !modeFound) {
      if (boardMode == 2) log_message("One or more settings missing, updating INI file...");
      if (!baudFound) strcat(newSettings, "#BAUD 115200\r\n");
      if (!attenFound) strcat(newSettings, "#ATTEN 97\r\n");
      if (!modeFound) strcat(newSettings, "#MODE 1\r\n");
      
      iniFile = sd.open("MP3TRIGR.INI", FILE_READ);
      bytesRead = iniFile.read(iniBuffer, 1024);
      iniBuffer[bytesRead] = '\0';
      iniFile.close();
      
      strcat(newSettings, iniBuffer);

      iniFile = sd.open("MP3TRIGR.INI", FILE_WRITE | O_TRUNC);
      if(iniFile) {
        iniFile.print(newSettings);
        iniFile.close();
      }
    }

  } else {
    Serial.begin(serialBaud);
    log_message("MP3TRIGR.INI not found. Creating with default settings.");
    iniFile = sd.open("MP3TRIGR.INI", FILE_WRITE);
    if (iniFile) {
      iniFile.println("#BAUD 115200");
      iniFile.println("#ATTEN 97");
      iniFile.println("#MODE 1");
      iniFile.println("******************** ALL INIT COMMANDS ABOVE THIS LINE *********************");
      iniFile.close();
    }
  }
  
  mutex_exit(&sd_mutex);
}

int validateAndSkipWAVHeader(FsFile& file) {
  WAVHeader header;
  if (file.readBytes((char*)&header, sizeof(WAVHeader)) != sizeof(WAVHeader)) return 0;
  if (strncmp(header.riff, "RIFF", 4) != 0 || strncmp(header.wave, "WAVE", 4) != 0 || header.audioFormat != 1) return 0;
  return header.numChannels;
}

void stopWAVPlayback() {
  isPlayingWAV = false;
  currentWavTrackNumber = -1;
  currentWavChannels = 0;
  loopSilentTrack = false;
  mutex_enter_blocking(&sd_mutex);
  if (wavFile) wavFile.close();
  mutex_exit(&sd_mutex);
}

void startWAVPlayback(const char* filename, int trackNumber) {
  mutex_enter_blocking(&sd_mutex);
  if (isPlayingWAV) {
    wavFile.close();
  }
  wavFile = sd.open(filename, FILE_READ);
  int channels = 0;
  if (wavFile) {
    channels = validateAndSkipWAVHeader(wavFile);
  }
  mutex_exit(&sd_mutex);
  
  if (channels > 0) {
    log_message("Core 0: Playing " + String(channels) + "-channel WAV " + String(filename));
    currentWavTrackNumber = trackNumber;
    currentWavChannels = channels;
    wavBufferLevel = 0;
    wavBufferPos = 0;
    isPlayingWAV = true;
  } else {
    log_message("Core 0: Failed to open or validate " + String(filename));
    isPlayingWAV = false;
  }
}

bool fillWAVBuffer() {
  bool wavFinished = false;
  mutex_enter_blocking(&sd_mutex);
  while (wavBufferLevel < (WAV_BUFFER_SIZE - 1024) && wavFile && wavFile.available()) {
    uint8_t tempBuffer[1024];
    int bytesToRead = min(sizeof(tempBuffer), (unsigned int)((WAV_BUFFER_SIZE - wavBufferLevel) * 2));
    int bytesRead = wavFile.read(tempBuffer, bytesToRead);
    int samplesRead = bytesRead / 2;
    
    if (bytesRead > 0) {
      int writeStart = (wavBufferPos + wavBufferLevel) % WAV_BUFFER_SIZE;
      for (int i = 0; i < samplesRead; i++) {
        int writePos = (writeStart + i) % WAV_BUFFER_SIZE;
        wavBuffer[writePos] = (int16_t)(tempBuffer[i * 2] | (tempBuffer[i * 2 + 1] << 8));
      }
      wavBufferLevel += samplesRead;
    } else {
      break;
    }
  }

  if (wavFile && !wavFile.available() && wavBufferLevel < currentWavChannels) {
    wavFinished = true;
  }
  mutex_exit(&sd_mutex);
  return wavFinished;
}

void triggerTrack(int trackNumber, bool shouldInterrupt, bool needsResponse) {
  char filename[MAX_FILENAME_LEN];
  if (!findTrack(trackNumber, filename, sizeof(filename))) {
    log_message("CMD: No track found for " + String(trackNumber));
    return;
  }

  if (my_strcasestr(filename, ".mp3")) {
    if (isPlayingMP3 && trackNumber == currentMp3TrackNumber) {
      log_message("CMD: Stopping MP3 track " + String(trackNumber));
      if (isPlayingWAV) {
        log_message("...WAV is active, switching to silent MP3 background.");
        mp3TrackToPlay = 0;
        loopSilentTrack = true;
      } else {
        stopMp3Request = true;
      }
    } else {
      if (!shouldInterrupt && isPlayingMP3) {
        log_message("CMD: Play ignored, a track is already playing.");
        return;
      }
      log_message("CMD: Triggering MP3 track " + String(trackNumber));
      if (needsResponse) responseRequested = true;
      loopSilentTrack = false;
      mp3TrackToPlay = trackNumber;
    }
  } else if (my_strcasestr(filename, ".wav")) {
    if (isPlayingWAV && trackNumber == currentWavTrackNumber) {
      log_message("CMD: Stopping WAV track " + String(trackNumber));
      stopWAVPlayback();
    } else {
      if (!shouldInterrupt && isPlayingWAV) {
          log_message("CMD: Play ignored, a track is already playing.");
          return;
      }
      log_message("CMD: Triggering WAV track " + String(trackNumber));
      if (needsResponse) responseRequested = true;
      if (!isPlayingMP3) {
        log_message("CMD: Starting silent track for WAV playback.");
        mp3TrackToPlay = 0;
        loopSilentTrack = true;
      }
      startWAVPlayback(filename, trackNumber);
    }
  }
}

// Function to create the silent MP3 if it's missing
bool createMinimalSilenceMP3(const char* filename) {
  FsFile file;
  mutex_enter_blocking(&sd_mutex);
  bool success = file.open(filename, O_WRONLY | O_CREAT | O_TRUNC);
  mutex_exit(&sd_mutex);

  if (!success) {
    log_message("Failed to open file for writing: " + String(filename));
    return false;
  }
  
  log_message("Creating file: " + String(filename));
  
  // MP3 frame for 32kbps mono at 44.1kHz
  // Frame size calculation: 144 * bitrate / sample_rate = 144 * 32000 / 44100 ≈ 104 bytes per frame
  const uint8_t mp3FrameHeader[] = { 0xFF, 0xFA, 0x40, 0x00 };
  
  // Create a ~1 second file for robustness
  const int framesToCreate = 43;  // ~1 second worth of frames
  const int frameSize = 104;         
  const int dataSize = frameSize - 4;
  
  for (int frame = 0; frame < framesToCreate; frame++) {
    mutex_enter_blocking(&sd_mutex);
    file.write(mp3FrameHeader, 4);
    for (int i = 0; i < dataSize; i++) {
      file.write((uint8_t)0x00);
    }
    mutex_exit(&sd_mutex);
  }
  
  mutex_enter_blocking(&sd_mutex);
  file.close();
  mutex_exit(&sd_mutex);

  log_message("Successfully created " + String(filename));
  return true;
}

void ensureSilenceFileExists() {
  mutex_enter_blocking(&sd_mutex);
  bool exists = sd.exists("000SILENCE.MP3");
  mutex_exit(&sd_mutex);

  if (!exists) {
    log_message("000SILENCE.MP3 not found, creating it...");
    if (!createMinimalSilenceMP3("000SILENCE.MP3")) {
      log_message("FATAL ERROR: Could not create 000SILENCE.MP3!");
      while(true) { // Halt
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        delay(100);
      }
    }
  }
}

void setup() {
  delay(100);
  pinMode(LED_PIN, OUTPUT);
  pinMode(FWD_PIN, INPUT_PULLUP);
  pinMode(REV_PIN, INPUT_PULLUP);
  pinMode(PLAY_PIN, INPUT_PULLUP);
  
  SPI1.setRX(SD_MISO);
  SPI1.setTX(SD_MOSI);
  SPI1.setSCK(SD_SCK);
  SdSpiConfig sdConfig(SD_CS, DEDICATED_SPI, 25000000, &SPI1);
  if (!sd.begin(sdConfig)) {
    Serial.begin(115200);
    log_message("SD card initialization failed!");
    digitalWrite(LED_PIN, HIGH);
    sd.initErrorHalt(&Serial);
    while (1);
  }

  Serial.begin(serialBaud); 
  long initialBaud = serialBaud;
  
  // This must be run before parsing INI in case developer mode is on
  parseIniFile();
  
  if (serialBaud != initialBaud) {
    Serial.end();
    Serial.begin(serialBaud);
  }

  if (boardMode == 2) {
    while(!Serial && millis() < 5000);
  }
  
  ensureSilenceFileExists();
  
  if (boardMode == 2) {
    Serial.println("\n--- Pico MP3 Trigger " VERSION_STRING " ---");
    log_message("Serial port started at " + String(serialBaud) + " baud.");
    log_message("Master attenuation set to " + String(masterAttenuation) + "%.");
    log_message("Mode set to " + String(boardMode) + " (1=Compat, 2=Dev)");
  }
  
  scanTracks();
  if (!i2s.begin(SAMPLE_RATE)) {
    log_message("I2S initialization failed!");
    while (1);
  }
  if (boardMode == 2) log_message("I2S initialized at " + String(SAMPLE_RATE) + "Hz.");
  mp3Decoder.begin();
  if (boardMode == 2) log_message("System ready.");
}

void processSerialCommands() {
  if (Serial.available()) {
    char command = Serial.read();
    
    switch(command) {
      case 'T': {
        int trackNumber = Serial.parseInt();
        if (trackNumber > 0) triggerTrack(trackNumber, true, true);
        break;
      }
      case 't': {
        if (Serial.available()) triggerTrack(Serial.read(), true, false);
        break;
      }
      case 'P': {
        int trackNumber = Serial.parseInt();
        if (trackNumber > 0) triggerTrack(trackNumber, false, true);
        break;
      }
      case 'p': {
        if (Serial.available()) triggerTrack(Serial.read(), false, false);
        break;
      }
      case 'v': {
        int volume = -1;
        if (boardMode == 2) { // Developer mode uses ASCII
          volume = Serial.parseInt();
        } else { // MP3 Trigger Compatibility mode uses Binary
          if (Serial.available()) {
            volume = Serial.read();
          }
        }
        if (volume >= 0 && volume <= 255) {
          mp3Volume = wavVolume = volume;
          log_message("CMD: Set volume to " + String(volume) + " (0=max, 255=mute)");
        }
        break;
      }
      case 'O': {
        if (isPlayingMP3 || isPlayingWAV) {
          log_message("CMD: Stop playback");
          stopMp3Request = true;
          stopWAVPlayback();
        } else if (currentTrackIndex != -1) {
          log_message("CMD: Start playback");
          triggerTrack(atoi(trackList[currentTrackIndex]), true, false);
        } else {
          log_message("CMD: No track selected to play.");
        }
        break;
      }
      case 'F':
      case 'R': {
        if (trackCount > 0) {
          currentTrackIndex += (command == 'F' ? 1 : -1);
          if (currentTrackIndex >= trackCount) currentTrackIndex = 0;
          if (currentTrackIndex < 0) currentTrackIndex = trackCount - 1;
          log_message("CMD: " + String(command == 'F' ? "Forward" : "Rewind"));
          triggerTrack(atoi(trackList[currentTrackIndex]), true, false);
        }
        break;
      }
    }
    
    while(Serial.available() && (Serial.peek() == '\n' || Serial.peek() == '\r')) Serial.read();
  }
}

void handleButtons() {
  unsigned long currentTime = millis();
  if (digitalRead(FWD_PIN) == LOW && (currentTime - lastFwdTime > debounceDelay)) {
    lastFwdTime = currentTime;
    if (trackCount > 0) {
      currentTrackIndex++;
      if (currentTrackIndex >= trackCount) currentTrackIndex = 0;
      log_message("BTN: Forward");
      triggerTrack(atoi(trackList[currentTrackIndex]), true, false);
    }
  }
  if (digitalRead(REV_PIN) == LOW && (currentTime - lastRevTime > debounceDelay)) {
    lastRevTime = currentTime;
    if (trackCount > 0) {
      currentTrackIndex--;
      if (currentTrackIndex < 0) currentTrackIndex = trackCount - 1;
      log_message("BTN: Rewind");
      triggerTrack(atoi(trackList[currentTrackIndex]), true, false);
    }
  }
  if (digitalRead(PLAY_PIN) == LOW && (currentTime - lastPlayTime > debounceDelay)) {
    lastPlayTime = currentTime;
    if (isPlayingMP3 || isPlayingWAV) {
      log_message("BTN: Stop playback");
      stopMp3Request = true; 
      stopWAVPlayback();
    } else if (currentTrackIndex != -1) {
      log_message("BTN: Start playback");
      triggerTrack(atoi(trackList[currentTrackIndex]), true, false);
    }
  }
}

void loop() {
  processSerialCommands();
  handleButtons();
  
  if (isPlayingWAV) {
    if (fillWAVBuffer()) { // This now returns true if the WAV finished
      stopWAVPlayback();
    }
  }

  if (responseRequested && playbackStartedSuccessfully) {
    send_serial_response("P!");
    responseRequested = false;
    playbackStartedSuccessfully = false;
  }
  
  if (mp3WasPlaying && !isPlayingMP3) {
    log_message("Core 0: Detected MP3 has finished.");
    send_serial_response("X");
  }
  if (wavWasPlaying && !isPlayingWAV) {
    log_message("Core 0: Detected WAV has finished.");
    send_serial_response("X");
  }
  mp3WasPlaying = isPlayingMP3;
  wavWasPlaying = isPlayingWAV;

  // Handle LED status
  bool isMixingRealFiles = isPlayingMP3 && isPlayingWAV && (strcmp(core1PlayingFile, "000SILENCE.MP3") != 0);
  if (isMixingRealFiles) { // Both "real" files playing, blink
    if (millis() - lastBlinkTime > blinkInterval) {
      lastBlinkTime = millis();
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    }
  } else if (isPlayingMP3 || isPlayingWAV) { // Any single file (or WAV + silent) playing, solid on
    digitalWrite(LED_PIN, HIGH);
  } else { // None playing, off
    digitalWrite(LED_PIN, LOW);
  }
}
