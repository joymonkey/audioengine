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

MicroSD card should be loaded with two example audio files.
- **test1.mp3** : a stereo music track
- **test2.wav** : a mono droid beepboop
