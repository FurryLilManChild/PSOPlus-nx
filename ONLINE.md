# Playing online on Schtserv (first-time setup)

This port can connect to **Schtserv**, a community server for Phantasy Star Online on the GameCube. This page is for
players who have never used Schtserv before. If you already played on Schtserv with a GameCube, Dolphin or
Nintendont and your save carries that Hunter's License, you can skip to "Going online".

## What you need

- The port set up and running offline (see README.md).
- The Switch connected to the internet (System Settings > Internet).
- A web browser on a phone or PC, for a one-time registration.

## The one rule that matters: register your serial first

Schtserv only lets in serial numbers that have been registered on its website. A new Hunter's License made in the
game is unknown to the server, and connecting with it ends with "You are banned from this server." followed by
"(No.100) The line was disconnected." This is not a real ban and not a problem with the port: the serial just
is not registered yet.

## Step by step

1. **Make your Hunter's License in the game.**
   Start the port, choose to go online from the title menu, and when the game asks for a serial number, an access
   key and a password, make them up and type them in. Avoid easy patterns like 1111-1111. **Write all three down
   exactly as you typed them.** The game saves the license to your memory card file.
   If the connection is refused at the end of this step, that is expected: the serial is not registered yet.

2. **Register the same serial on Schtserv's My HL page.**
   Open https://api.schtserv.com/MyHL/ in a browser and register your Hunter's License with the serial number and
   access key you wrote down (and anything else it asks for). They must match what you typed in the game exactly.
   Schtserv's GameCube guide links a video walkthrough of this page:
   https://schtserv.com/forums/app.php/pso-gc-connection-guide

3. **Go online again from the port.** Choose online from the title menu. You should reach the Schtserv ship and
   lobby list.

4. **Still refused?** Ask on the Schtserv Discord (https://discord.gg/J2VFPR4) or forums with your serial number;
   staff can see why a serial was refused.

## What you can skip in Schtserv's guide

Schtserv's connection guide also explains the game's Network Option (a provider entry, IP set to automatic, the
DNS address 3.18.217.27, proxy fields, Dolphin settings and Action Replay codes for error 102). **None of that is
needed with this port.** The port uses a built-in network profile and finds Schtserv's address by itself
(psobb.dyndns.org, with Schtserv's own DNS servers as a fallback). You do not need to create a network file.

## Going online

- Choose online from the title menu, as on the GameCube.
- **Chat:** press the right stick to open the Switch keyboard, type a line, and press Enter to send it. Letters,
  digits and symbols are supported. Word Select works as in the original game.
- **HOME button:** a short visit to the HOME menu is fine. If the game stays in the background (HOME menu, sleep)
  for 15 seconds or more, the connection is ended and the game shows a disconnect message, as a GameCube would if
  its cable were pulled.
- **Losing the internet** (airplane mode, out of Wi-Fi range) also ends the connection cleanly.
- **Your memory card is backed up** before the first connection of each session, in `sdmc:/switch/psoplus/saves/`:
  `card_a.raw.before-online.raw` is the very first backup and is never overwritten, and each later session adds a
  dated `card_a.raw.before-online-YYYYMMDD-HHMMSS.raw`. Delete old dated ones whenever you like.
- The port menu (hold Minus for half a second) shows the server and the connection state on its System page.

## Other servers

The port's server is the `net_server` line in `sdmc:/switch/psoplus/settings.ini` (a host name or an IPv4
address; empty means Schtserv). Other servers have their own registration rules; check their sites.

## Server rules

Schtserv has its own rules (https://schtserv.com/forums/app.php/schtserv-server-rules): cheating, duping and
harassment are bannable. The port does not change gameplay; please play fair.
