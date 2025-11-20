// Pico MP3 Trigger - v20251013+Serial1
/*
 * Extended with:
 * - Auto-discovering sound bank system (folder-based organization)
 * - Sound variant support (random selection from _01, _02, _03 files)
 * - Serial command 'L' to list all banks and sounds
 * - Serial command 'B<bank>,<sound>' to play from specific bank
 * - Compatible with ESP32 robot controller via serial
 * - DUAL SERIAL SUPPORT: Accepts commands from both USB Serial and Serial1
 *
 * All original MP3 Trigger functionality preserved!
 */

#include "MP3DecoderHelix.h"
#include <SdFat.h>
#include <I2S.h>
#include "pico/mutex.h"

#define VERSION_STRING "v20251013+Serial1"

using namespace libhelix;

// ===================================
// ==      Configuration        ==
// ===================================
long serialBaud = 115200;
long serial1Baud = 9600;  // Baud rate for Serial1 (hardware UART)
int masterAttenuation = 97;
int boardMode = 1; // 1 = MP3 Trigger (Binary Vol), 2 = Developer (ASCII Vol)

auto_init_mutex(sd_mutex);
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

// Serial1 pins (hardware UART)
#define SERIAL1_TX 0
#define SERIAL1_RX 1

// Audio parameters
#define SAMPLE_RATE 44100
#define MAX_FILENAME_LEN 32 
#define MAX_TRACKS 100

// Sound bank system
#define MAX_BANKS 6
#define MAX_SOUNDS_PER_BANK 100
#define MAX_VARIANTS_PER_SOUND 10

struct SoundVariant {
  char filename[MAX_FILENAME_LEN];
};

struct Sound {
  char displayName[MAX_FILENAME_LEN];
  int variantCount;
  SoundVariant variants[MAX_VARIANTS_PER_SOUND];
};

struct SoundBank {
  char name[16];
  char folder[16];
  int soundCount;
  Sound sounds[MAX_SOUNDS_PER_BANK];
};

SoundBank soundBanks[MAX_BANKS];
int activeBankCount = 0;

// Volume control
byte mp3Volume = 0;
byte wavVolume = 0;

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

// WAV buffer
#define WAV_BUFFER_SIZE 4096
int16_t wavBuffer[WAV_BUFFER_SIZE];
int wavBufferPos = 0;
int wavBufferLevel = 0;

// Track list for legacy commands
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

// LED state
unsigned long lastBlinkTime = 0;
const unsigned int blinkInterval = 200;

// Forward declaration of mp3DataCallback
void mp3DataCallback(MP3FrameInfo &info, int16_t *pcm_buffer, size_t len, void* ref);

// MP3 decoder object (must be declared before any functions that use it)
MP3DecoderHelix mp3Decoder(mp3DataCallback);

// Logging helpers
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

// Custom strcasestr
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

// Helper: Extract base name (remove _01, _02, etc.)
void getBaseName(const char* filename, char* baseName, int bufferSize) {
  strncpy(baseName, filename, bufferSize - 1);
  baseName[bufferSize - 1] = '\0';
  
  // Remove extension
  char* ext = strrchr(baseName, '.');
  if (ext) *ext = '\0';
  
  // Remove variant suffix _XX
  int len = strlen(baseName);
  if (len > 3 && baseName[len-3] == '_' && 
      isdigit(baseName[len-2]) && isdigit(baseName[len-1])) {
    baseName[len-3] = '\0';
  }
  
  // Remove number prefix (001_, 002_, etc.)
  if (len > 4 && isdigit(baseName[0]) && isdigit(baseName[1]) && 
      isdigit(baseName[2]) && baseName[3] == '_') {
    memmove(baseName, baseName + 4, strlen(baseName + 4) + 1);
  }
}

// Auto-discover sound banks from SD card folders
void initSoundBanks() {
  log_message("Auto-discovering sound banks from SD card...");
  activeBankCount = 0;
  
  FsFile root;
  FsFile entry;
  
  mutex_enter_blocking(&sd_mutex);
  
  if (!root.open("/")) {
    log_message("ERROR: Could not open SD card root");
    mutex_exit(&sd_mutex);
    return;
  }
  
  // Scan root directory for folders
  int bankIndex = 0;
  while (entry.openNext(&root, O_RDONLY) && bankIndex < MAX_BANKS) {
    if (entry.isDirectory()) {
      char folderName[MAX_FILENAME_LEN];
      entry.getName(folderName, sizeof(folderName));
      
      // Skip system/hidden folders
      if (folderName[0] != '.' && 
          strcmp(folderName, "SYSTEM~1") != 0 &&
          strcmp(folderName, "System Volume Information") != 0) {
        
        strncpy(soundBanks[bankIndex].name, folderName, 15);
        soundBanks[bankIndex].name[15] = '\0';
        strncpy(soundBanks[bankIndex].folder, folderName, 15);
        soundBanks[bankIndex].folder[15] = '\0';
        
        log_message("Found folder: " + String(folderName));
        bankIndex++;
      }
    }
    entry.close();
  }
  
  root.close();
  mutex_exit(&sd_mutex);
  
  // Sort banks alphabetically
  for (int i = 0; i < bankIndex - 1; i++) {
    for (int j = 0; j < bankIndex - i - 1; j++) {
      if (strcmp(soundBanks[j].name, soundBanks[j + 1].name) > 0) {
        SoundBank temp = soundBanks[j];
        soundBanks[j] = soundBanks[j + 1];
        soundBanks[j + 1] = temp;
      }
    }
  }
  
  // Scan each bank for sounds
  for (int b = 0; b < bankIndex; b++) {
    scanSoundBank(b);
  }
  
  log_message("Sound bank discovery complete. Active banks: " + String(activeBankCount));
}

void scanSoundBank(int bankIndex) {
  if (bankIndex < 0 || bankIndex >= MAX_BANKS) return;
  
  SoundBank* bank = &soundBanks[bankIndex];
  bank->soundCount = 0;
  
  char folderPath[32];
  snprintf(folderPath, sizeof(folderPath), "/%s", bank->folder);
  
  // Collect all files
  char allFiles[MAX_SOUNDS_PER_BANK * MAX_VARIANTS_PER_SOUND][MAX_FILENAME_LEN];
  int fileCount = 0;
  
  FsFile bankRoot;
  FsFile file;
  
  mutex_enter_blocking(&sd_mutex);
  
  if (!bankRoot.open(folderPath)) {
    log_message("Bank folder not found: " + String(folderPath));
    mutex_exit(&sd_mutex);
    return;
  }
  
  while (file.openNext(&bankRoot, O_RDONLY) && fileCount < (MAX_SOUNDS_PER_BANK * MAX_VARIANTS_PER_SOUND)) {
    char tempName[MAX_FILENAME_LEN];
    file.getName(tempName, sizeof(tempName));
    
    if (!file.isDirectory() && (my_strcasestr(tempName, ".mp3") || my_strcasestr(tempName, ".wav"))) {
      strcpy(allFiles[fileCount], tempName);
      fileCount++;
    }
    file.close();
  }
  bankRoot.close();
  mutex_exit(&sd_mutex);
  
  // Sort files
  for (int i = 0; i < fileCount - 1; i++) {
    for (int j = 0; j < fileCount - i - 1; j++) {
      if (strcmp(allFiles[j], allFiles[j + 1]) > 0) {
        char temp[MAX_FILENAME_LEN];
        strcpy(temp, allFiles[j]);
        strcpy(allFiles[j], allFiles[j + 1]);
        strcpy(allFiles[j + 1], temp);
      }
    }
  }
  
  // Group by base name
  int soundIndex = 0;
  for (int i = 0; i < fileCount && soundIndex < MAX_SOUNDS_PER_BANK; i++) {
    char baseName[MAX_FILENAME_LEN];
    getBaseName(allFiles[i], baseName, sizeof(baseName));
    
    // Check if this is a new sound
    bool isNewSound = true;
    if (soundIndex > 0) {
      char prevBase[MAX_FILENAME_LEN];
      getBaseName(bank->sounds[soundIndex - 1].variants[0].filename, prevBase, sizeof(prevBase));
      if (strcmp(baseName, prevBase) == 0) {
        isNewSound = false;
      }
    }
    
    if (isNewSound) {
      Sound* sound = &bank->sounds[soundIndex];
      strcpy(sound->displayName, baseName);
      sound->variantCount = 0;
      
      // Find all variants
      for (int j = i; j < fileCount && sound->variantCount < MAX_VARIANTS_PER_SOUND; j++) {
        char testBase[MAX_FILENAME_LEN];
        getBaseName(allFiles[j], testBase, sizeof(testBase));
        if (strcmp(baseName, testBase) == 0) {
          strcpy(sound->variants[sound->variantCount].filename, allFiles[j]);
          sound->variantCount++;
        }
      }
      soundIndex++;
    }
  }
  
  bank->soundCount = soundIndex;
  activeBankCount++;
  
  if (boardMode == 2) {
    log_message("Bank '" + String(bank->name) + "': " + String(soundIndex) + " sounds");
  }
}

void sendBankList() {
  mutex_enter_blocking(&log_mutex);
  Serial.println("\n=== SOUND BANKS ===");
  for (int b = 0; b < activeBankCount; b++) {
    Serial.print("Bank ");
    Serial.print(b);
    Serial.print(": ");
    Serial.print(soundBanks[b].name);
    Serial.print(" (");
    Serial.print(soundBanks[b].soundCount);
    Serial.println(" sounds)");
    
    for (int s = 0; s < soundBanks[b].soundCount; s++) {
      Serial.print("  [");
      Serial.print(s);
      Serial.print("] ");
      Serial.print(soundBanks[b].sounds[s].displayName);
      if (soundBanks[b].sounds[s].variantCount > 1) {
        Serial.print(" (");
        Serial.print(soundBanks[b].sounds[s].variantCount);
        Serial.print(" variants)");
      }
      Serial.println();
    }
  }
  Serial.println("==================\n");
  mutex_exit(&log_mutex);
}

void playSoundFromBank(int bankIndex, int soundIndex) {
  if (bankIndex < 0 || bankIndex >= activeBankCount) {
    log_message("Invalid bank: " + String(bankIndex));
    return;
  }
  
  SoundBank* bank = &soundBanks[bankIndex];
  if (soundIndex < 0 || soundIndex >= bank->soundCount) {
    log_message("Invalid sound: " + String(soundIndex));
    return;
  }
  
  Sound* sound = &bank->sounds[soundIndex];
  int variantIndex = 0;
  if (sound->variantCount > 1) {
    variantIndex = random(0, sound->variantCount);
  }
  
  char fullPath[64];
  snprintf(fullPath, sizeof(fullPath), "/%s/%s", 
           bank->folder, sound->variants[variantIndex].filename);
  
  log_message("Playing: " + String(fullPath));
  
  mutex_enter_blocking(&sd_mutex);
  bool exists = sd.exists(fullPath);
  mutex_exit(&sd_mutex);
  
  if (!exists) {
    log_message("File not found!");
    return;
  }
  
  if (my_strcasestr(fullPath, ".mp3")) {
    if (isPlayingMP3) {
      isPlayingMP3 = false;
      mutex_enter_blocking(&sd_mutex);
      if (mp3File) mp3File.close();
      mutex_exit(&sd_mutex);
      delay(5);
    }
    
    mutex_enter_blocking(&sd_mutex);
    mp3File = sd.open(fullPath, FILE_READ);
    mutex_exit(&sd_mutex);
    
    if (mp3File) {
      strcpy(core1PlayingFile, fullPath);
      currentMp3TrackNumber = -1;
      mp3Decoder.begin();
      isPlayingMP3 = true;
      loopSilentTrack = false;
    }
  } else if (my_strcasestr(fullPath, ".wav")) {
    if (!isPlayingMP3) {
      mp3TrackToPlay = 0;
      loopSilentTrack = true;
    }
    startWAVPlayback(fullPath, -1);
  }
}

// MP3 decoder callback implementation
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
        log_message("Core 1: Failed to open " + String(filename));
        isPlayingMP3 = false;
      }
    } else {
      isPlayingMP3 = false;
      if (found && !my_strcasestr(filename, ".mp3")) {
        log_message("Core 1: Track " + String(track) + " is not MP3");
      } else if (!found) {
        log_message("Core 1: Track " + String(track) + " not found");
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
          log_message("Core 1: ERROR - Failed to re-open silent track");
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
    log_message("Found " + String(trackCount) + " navigable tracks");
    for (int i=0; i < trackCount; i++) {
      mutex_enter_blocking(&log_mutex);
      Serial.println("  " + String(i+1) + ": " + String(trackList[i]));
      mutex_exit(&log_mutex);
    }
  }
}

void parseIniFile() {
  bool baudFound = false;
  bool serial1BaudFound = false;
  bool attenFound = false;
  bool modeFound = false;
  char iniBuffer[1025];
  char newSettings[256] = "";
  
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
        
        // Check BAUD1 BEFORE BAUD to prevent false match
        if (strncasecmp(command, "BAUD1", 5) == 0) {
          serial1Baud = atol(command + 5);
          serial1BaudFound = true;
        } else if (strncasecmp(command, "BAUD", 4) == 0) {
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

    if (!baudFound || !serial1BaudFound || !attenFound || !modeFound) {
      if (boardMode == 2) log_message("Updating INI with missing settings...");
      if (!baudFound) strcat(newSettings, "#BAUD 115200\r\n");
      if (!serial1BaudFound) strcat(newSettings, "#BAUD1 9600\r\n");
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
    log_message("MP3TRIGR.INI not found. Creating defaults.");
    iniFile = sd.open("MP3TRIGR.INI", FILE_WRITE);
    if (iniFile) {
      iniFile.println("#BAUD 115200");
      iniFile.println("#BAUD1 9600");
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
    log_message("Core 0: Playing " + String(channels) + "-ch WAV " + String(filename));
    currentWavTrackNumber = trackNumber;
    currentWavChannels = channels;
    wavBufferLevel = 0;
    wavBufferPos = 0;
    isPlayingWAV = true;
  } else {
    log_message("Core 0: Failed to open " + String(filename));
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
        log_message("...WAV active, switching to silent MP3");
        mp3TrackToPlay = 0;
        loopSilentTrack = true;
      } else {
        stopMp3Request = true;
      }
    } else {
      if (!shouldInterrupt && isPlayingMP3) {
        log_message("CMD: Play ignored, track playing");
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
          log_message("CMD: Play ignored, track playing");
          return;
      }
      log_message("CMD: Triggering WAV track " + String(trackNumber));
      if (needsResponse) responseRequested = true;
      if (!isPlayingMP3) {
        log_message("CMD: Starting silent track for WAV");
        mp3TrackToPlay = 0;
        loopSilentTrack = true;
      }
      startWAVPlayback(filename, trackNumber);
    }
  }
}

bool createMinimalSilenceMP3(const char* filename) {
  FsFile file;
  mutex_enter_blocking(&sd_mutex);
  bool success = file.open(filename, O_WRONLY | O_CREAT | O_TRUNC);
  mutex_exit(&sd_mutex);

  if (!success) {
    log_message("Failed to create " + String(filename));
    return false;
  }
  
  log_message("Creating " + String(filename));
  
  const uint8_t mp3FrameHeader[] = { 0xFF, 0xFA, 0x40, 0x00 };
  const int framesToCreate = 43;
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
    log_message("000SILENCE.MP3 not found, creating...");
    if (!createMinimalSilenceMP3("000SILENCE.MP3")) {
      log_message("FATAL: Could not create 000SILENCE.MP3!");
      while(true) {
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
    log_message("SD card init failed!");
    digitalWrite(LED_PIN, HIGH);
    sd.initErrorHalt(&Serial);
    while (1);
  }

  Serial.begin(serialBaud);
  long initialBaud = serialBaud;
  
  parseIniFile();
  
  if (serialBaud != initialBaud) {
    Serial.end();
    Serial.begin(serialBaud);
  }

  // Initialize Serial1 (hardware UART)
  Serial1.setRX(SERIAL1_RX);
  Serial1.setTX(SERIAL1_TX);
  Serial1.begin(serial1Baud);

  if (boardMode == 2) {
    while(!Serial && millis() < 5000);
  }
  
  ensureSilenceFileExists();
  
  if (boardMode == 2) {
    Serial.println("\n--- Pico MP3 Trigger " VERSION_STRING " ---");
    log_message("Serial: " + String(serialBaud) + " baud");
    log_message("Serial1: " + String(serial1Baud) + " baud");
    log_message("Attenuation: " + String(masterAttenuation) + "%");
    log_message("Mode: " + String(boardMode));
  }
  
  // Initialize sound banks (new feature!)
  initSoundBanks();
  
  // Legacy track list (for backward compatibility)
  scanTracks();
  
  if (!i2s.begin(SAMPLE_RATE)) {
    log_message("I2S init failed!");
    while (1);
  }
  if (boardMode == 2) log_message("I2S: " + String(SAMPLE_RATE) + "Hz");
  mp3Decoder.begin();
  if (boardMode == 2) log_message("System ready.");
}

// Process commands from a given Stream (works with Serial or Serial1)
void processCommandsFromStream(Stream &serialPort) {
  if (serialPort.available()) {
    char command = serialPort.read();
    
    switch(command) {
      // New bank commands
      case 'L': { // List all banks and sounds
        sendBankList();
        break;
      }
      case 'B': { // Play from bank: B<bank>,<sound>
        int bank = serialPort.parseInt();
        if (serialPort.read() == ',') {
          int sound = serialPort.parseInt();
          playSoundFromBank(bank, sound);
        }
        break;
      }
      
      // Original MP3 Trigger commands
      case 'T': {
        int trackNumber = serialPort.parseInt();
        if (trackNumber > 0) triggerTrack(trackNumber, true, true);
        break;
      }
      case 't': {
        if (serialPort.available()) triggerTrack(serialPort.read(), true, false);
        break;
      }
      case 'P': {
        int trackNumber = serialPort.parseInt();
        if (trackNumber > 0) triggerTrack(trackNumber, false, true);
        break;
      }
      case 'p': {
        if (serialPort.available()) triggerTrack(serialPort.read(), false, false);
        break;
      }
      case 'v': {
        int volume = -1;
        if (boardMode == 2) {
          volume = serialPort.parseInt();
        } else {
          if (serialPort.available()) {
            volume = serialPort.read();
          }
        }
        if (volume >= 0 && volume <= 255) {
          mp3Volume = wavVolume = volume;
          log_message("CMD: Volume " + String(volume));
        }
        break;
      }
      case 'O': {
        if (isPlayingMP3 || isPlayingWAV) {
          log_message("CMD: Stop");
          stopMp3Request = true;
          stopWAVPlayback();
        } else if (currentTrackIndex != -1) {
          log_message("CMD: Start");
          triggerTrack(atoi(trackList[currentTrackIndex]), true, false);
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
    
    while(serialPort.available() && (serialPort.peek() == '\n' || serialPort.peek() == '\r')) serialPort.read();
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
      log_message("BTN: Stop");
      stopMp3Request = true;
      stopWAVPlayback();
    } else if (currentTrackIndex != -1) {
      log_message("BTN: Start");
      triggerTrack(atoi(trackList[currentTrackIndex]), true, false);
    }
  }
}

void loop() {
  // Process commands from both serial ports
  processCommandsFromStream(Serial);
  processCommandsFromStream(Serial1);
  
  handleButtons();
  
  if (isPlayingWAV) {
    if (fillWAVBuffer()) {
      stopWAVPlayback();
    }
  }

  if (responseRequested && playbackStartedSuccessfully) {
    send_serial_response("P!");
    responseRequested = false;
    playbackStartedSuccessfully = false;
  }
  
  if (mp3WasPlaying && !isPlayingMP3) {
    log_message("MP3 finished");
    send_serial_response("X");
  }
  if (wavWasPlaying && !isPlayingWAV) {
    log_message("WAV finished");
    send_serial_response("X");
  }
  mp3WasPlaying = isPlayingMP3;
  wavWasPlaying = isPlayingWAV;

  // LED status
  bool isMixingRealFiles = isPlayingMP3 && isPlayingWAV && (strcmp(core1PlayingFile, "000SILENCE.MP3") != 0);
  if (isMixingRealFiles) {
    if (millis() - lastBlinkTime > blinkInterval) {
      lastBlinkTime = millis();
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    }
  } else if (isPlayingMP3 || isPlayingWAV) {
    digitalWrite(LED_PIN, HIGH);
  } else {
    digitalWrite(LED_PIN, LOW);
  }
}
