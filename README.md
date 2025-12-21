# CHIRP Droid Control Project
Hardware and firmware for a droid control system, evolved from the little used [ShadyRC dEvolution](https://github.com/joymonkey/dEvolution/blob/master/sketches/ShadyRC_Crossfire_250211/ShadyRC_Crossfire_250211.ino) sketch that's been tried and tested over the last few years.

# CHIRP Audio Trigger
This is an advanced MP3 and WAV file decoder/mixer/player, heavily inspired by the [Sparkfun/Robertsonics MP3 Trigger](https://www.sparkfun.com/mp3-trigger.html) that has been used across droid control systems since the [original Padawan PS2 days](https://astromech.net/forums/showthread.php?8354-Using-a-Wireless-PS2-controller-with-ardurino). This project covers DIY breadboard hardware, a custom PCB and the Arduino code that does the audio magic.
![chirp-board-proto](https://github.com/user-attachments/assets/db2b1b3f-c6e0-4041-a972-08b1c2d54b92)


# CHIRP Droid Control
An evolution of the previosly mentioned ShadyRC system. The intent is to use a microcontroller and ExpressLRS radio receiver to send signals/commands to the various motion, sound and lighting systems of an Astromech droid. Some goals...

- Send system status and audio file details to the operators radio transmitter via ExpressLRS telemetry packets
- Don't require re-programming just to update sounds; if sound files on the Audio Trigger change, the system knows how to roll with it
- Be controller agnostic; initially supporting several different EdgeTX based radio transmitters such as the Radiomaster Zorro
- Be safe but also easy to pick up; the system should police itself to some extent
- Be expandable; when new droid gizmos show up we should be able to easily implement them
