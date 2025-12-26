# CHIRP Audio Trigger
The CHIRP Audio Trigger is a sound file player similar to the trusty Sparkfun/Robertsonics MP3 Trigger board. Functionality is focused on using the CHIRP Audio Trigger as the sound system for an Astromech droid, but it is also very usable as a general sound player for other projects, and is a fun platform to experiment with audio playback and processing programming. The stock firmware can read the contents of the SD card and provide a droids primary microcontroller with a manifest of sounds - allowing sound names to be sent to the operators radio transmitter and displayed as telemetry on the transmitters GUI.

# Initial Audio Engine Details
Hardware and firmware for a new sound file playing/mixing PCB. Perfectly suited for Droid Builder use in an Astromech droid.

Some project goals...
- play and mix multiple audio filetypes (initially stereo MP3 and mono WAV)
- affordable PCB with minimal easy to source components
- easy to implement in current droid control systems (Padawan/Shadow/Kyber)
- room on the hardware for advanced droid nonsense

## Proof of Concept Hardware
Breadboard circuit uses...
- Raspberry Pi Pico 2 dev board (later a Pimoroni Pico Plus 2 for its 16MB flash and 8PM PSRAM)
- Generic microSD card holder
- Generic PCM5102A I2S module
- 1 momentary pushbutton
