# CHIRP Audio Trigger Arduino Sketches
Final code for the CHIRP Audio Trigger and several test sketches live here.

- CHIRP_Audio : this is intended to be the actual functional final code for the CHIRP Audio Trigger Board
- mp3-wav-mix-example : early code to see how feasible mixing a wav with a decoded MP3 would be
- pico-mp3-trigger-251013 : a work in progress sketch using breadboard hardware
- pico-mp3-trigger : earlier version of the above

## Proof of Concept Code
The test sketches (likely the final ones too) run on a Raspberry Pi Pico 2, using the Arduino IDE and [Earle F. Philhower's Pico core](https://github.com/earlephilhower/arduino-pico). They utilize the I2S and SdFat libraries included with Earle's Pico core. They also require [Phil Schatzmann's arduino-libhelix library](https://github.com/pschatzmann/arduino-libhelix) to do the actual MP3 decoding work for us.

### mp3-wav-mix-example sketch
For this test sketch the microSD card should be loaded with two example audio files.
- **test1.mp3** : a stereo music track
- **test2.wav** : a mono droid beepboop

### pico-mp3-trigger sketch
This is the testbed sketch for what is to become the "RSeries Audio Engine". The sketch turns the Pico 2 into a fairly full featured audio player. Functionality and command protocol is based around that of the [Sparkfun/Robertsonics MP3 Trigger](https://www.sparkfun.com/mp3-trigger.html). The Robertsonics board has been the heart of many an Astromech droid over the years, due to its ease of use, availability and the fact that it just works.
The Pico MP3 Trigger concept was to take everything we love about the faithful Robertsonics board and expand upon it, with features that are geared more closely to the needs of droid builders than anything else.
In a droid that might be running Padawan, Shadow or other droid control system variants it is a drop-in replacement for the Sparkfun MP3 Trigger. It will respond to the same serial commands, and play solo MP3 files just as you would expect. It can also play WAV files. It can also play an MP3 and a WAV together simultaneously, mixing them effortlessly and offering individual volume control.

This sketch expects MP3 and WAV files to be on the microSD card with a particular filename format. Filenames need to start with a 3 digit number, such as 001_beepboop.wav or 450-my-macarena-droid-mix.mp3. It also requires a "silent" MP3 file to be on the root of the SD card, named 000SILENCE.MP3 (this MP3 is auto generated in recent versions of this sketch).
