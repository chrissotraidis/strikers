# Strikers - A Native PC Port of Super Mario Strikers

*Requires game data from your own copy of Super Mario Strikers. No game assets are included.*

Super Mario Strikers, rebuilt to run natively on Windows, Linux and macOS, with modern display support, configurable controls and a focus on performance across both powerful and low-power hardware.

Built on the community decompilation by the excellent [Yannick Suter](https://github.com/yannicksuter), this project brings the original game to modern systems without changing how it plays or looks.

## Features

- High framerate support with configurable frame limits and VSync.
- Widescreen and ultrawide support, with an expanded view of the pitch and corrected aspect ratios.
- Higher-resolution rendering with adjustable internal resolution, 4x MSAA and up to 16x anisotropic filtering.
- Modern graphics backends through Metal, Vulkan and Direct3D 12 via [Aurora](https://github.com/encounter/aurora). Vulkan is personally recommended on non-macOS systems.
- Controller support for Xbox, PlayStation, Switch Pro and compatible GameCube adapters through SDL.
- Customisable controls, including keyboard and gamepad bindings, stick deadzones and rumble.
- Faster loading compared with original hardware.
- Direct disc-image loading from ISO/GCM, CISO and GCZ files, alongside extracted game folders.
- US, European and Japanese game support in one executable, with region-aware saves and language handling.
- A dedicated settings app for configuring graphics, controls, audio and game-data location. Some basic localisation included to help.

The obvious omission is online multiplayer. I have no desire to implement this as I don't have much experience with netcode. But I am sure someone will fork this and build it. It will be a fun challenge and I look forward to trying it myself.

## Hardware and performance

Developed on a modern MacBook Pro and tested on a variety of hardware I can get my hands on right now.

Efficient performance is a priority, especially on smaller systems and handhelds. Steam Deck testing is still to come when I have access to mine again. The goal is a later release tuned for smooth gameplay, low power draw and longer battery life, but from testing with the hardware I have, I think it's 90% there.

## Notes

This has been a solo effort. I am releasing this on what is effectively a 'burner' GitHub account because I don't want it attached to my name for professional and legal reasons. I have not done this port for reasons of ego or notoriety. Mario Strikers is one of my favourite games and it's been a dream of mine to bring it to the PC platform without the pains of emulation. My goal is simply to have it running on Steam Deck or similar low-end hardware with as little power draw as possible and nice performance, as I play the original game on Steam Deck a lot and bring it to parties and such.

[Yannick Suter](https://github.com/yannicksuter), who headed the decomp effort, and the other contributors deserve a ton of credit for the work that's gone into the mind-numbing process of decompilation. And of course, the team behind [Aurora](https://github.com/encounter/aurora), for making this kind of project as easy as possible, deserve endless respect.

Pull requests are welcome, but it's more than likely this project will be forked by people more invested than I, as I feel like my job is done.

## Licensing

This project contains material with different licences and rights statuses.

My original porting code, tools and documentation are offered under CC0 1.0, except where otherwise noted, and only to the extent that I own the relevant rights. This does not relicense third-party material or grant rights to the original game.

The main distinctions are:

- Reconstructed game code: this is an unofficial source reconstruction, not an official source release. Reconstructed material may remain subject to third-party rights; this project claims no ownership of those rights and grants no permission on behalf of their holders.
- MusyX audio middleware: the upstream decompilation carries an MIT licence, which is preserved here. That notice does not, by itself, establish that its licensors hold all rights in the reconstructed middleware.
- ODE physics: upstream ODE portions use the historical BSD-style licence preserved in the repository. That licence does not, by itself, establish the licensing status of independently copyrightable game-specific modifications.
- Other third-party material and dependencies: these retain their applicable licences and notices. FFmpeg's terms depend on its build configuration.

The project's current distribution policy is source-only. This is a project policy, not a claim that all reconstructed source is cleared for redistribution. Public availability does not itself grant permission to reuse or redistribute third-party material.

You must supply game data from your own lawfully obtained copy. This project grants no rights to distribute game assets or disc images.

This is an unofficial project, unaffiliated with and not endorsed by Nintendo or Next Level Games.
