# What's new

The streamer's What's New tab and the GitHub release notes are both made from this file. Newest version first.

## 1.1.3

- Headset apps older than 1.34.25 can't be matched automatically, because they don't send their version. They now get 1.34.23 instead of 1.20.3, so they keep eye-tracked foveated streaming and stop asking for an update
- A 1.34.24 headset app is recognised after it asks for an update once, and gets its exact version from the next connection

## 1.1.2

- Recentering tells games where the new centre is, for games like Beat Saber that were placing you in the wrong spot
- Recentering no longer lands in the wrong place when the headset's new position arrives late

## 1.1.1

- Fixed the headset microphone: apps can now use the OpenDesk Microphone input, and it no longer plays through your speakers
- WayVR and other overlays run in step with the game and no longer wait on the graphics card, for smoother panels over games

## 1.1

- WayVR and other OpenXR overlay apps now work, on top of a game or by themselves
- WayVR's sky background shows when no game is open
- Startup apps open with a VR game and close with it
- Add startup apps from a list of detected apps, or by picking an AppImage
- New Hand angle setting, and most games are back on the hand angle they were made for
- With two graphics cards you can pick which one runs VR, and encoder options follow the card that encodes
- NVIDIA encoder speed setting is back, matching AMD
- New What's New tab

## 1.0

- First public release, for Linux
- Stream your desktop and VR games to the official headset app
- VR games through OpenDesk XR, including Proton games
- OpenVR games without SteamVR, through xrizer
- Oculus PC games through RiftLift, in the headset's Games tab
- Choose which graphics card runs VR, or use one card for the game and another for encoding
- See the live VR encoder load and what each quality option costs
- Encoder speed choice on NVIDIA and AMD
- Built-in VRChat face tracking
- The AppImage now updates itself from GitHub releases
