# RSeries Audio Engine
Hardware and firmware for a new sound file playing/mixing PCB. Perfectly suited for Droid Builder use in an Astromech droid.

Some project goals...
- play and mix multiple audio filetype (initially stereo MP3 and mono WAV)
- also take an external audio input (such as HCR Vocalizer) and mix with gain adjustment
- affordable PCB with minimal easy to source components
- easy to implement in current droid control systems (Padawan/Shadow/Kyber)
- room on the hardware for advanced droid nonsense

## Proof of Concept Hardware
Breadboard circuit uses...
- Raspberry Pi Pico 2 dev board
- Generic microSD card holder
- Generic PCM5102A I2S module
- 1 momentary pushbutton

## mp3-wav-mix-example sketch
For this test sketch the microSD card should be loaded with two example audio files.
- **test1.mp3** : a stereo music track
- **test2.wav** : a mono droid beepboop

## pico-mp3-trigger sketch
This is the testbed sketch for what is to become the "RSeries Audio Engine". The sketch turns the Pico 2 into a fairly full featured audio player. Functionality and command protocol is based around that of the Sparkfun/Robertsonics MP3 Trigger. The Robertsonics board has been the heart of many an Astromech droid over the years, due to its ease of use, availability and the fact that it just works.
The Pico MP3 Trigger concept was to take everything we love about the faithful Robertsonics board and expand upon it, with features that are geared more closely to the needs of droid builders than anything else.
In a droid that might be running Padawan, Shadow or other droid control system variants it is a drop-in replacement for the Sparkfun MP3 Trigger. It will respond to the same serial commands, and play solo MP3 files just as you would expect. It can also play WAV files. It can also play an MP3 and a WAV together simultaneously, mixing them effortlessly and offering individual volume control.

This sketch expects MP3 and WAV files to be on the microSD card with a particular filename format. Filenames need to start with a 3 digit number, such as 001_beepboop.wav or 450-my-macarena-droid-mix.mp3. It also requires a "silent" MP3 file to be on the root of the SD card, named 000SILENCE.MP3.
