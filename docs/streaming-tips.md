# Streaming tips

The tip shown in the corner while a stream connects ("Hint: ..."). Since 2026-11-13 the tips are ranked per
platform and the app remembers which ones it has shown, so users who only ever see a few tips see the
important ones.

## How it works

- **Source of truth:** `scripts/tips/tips_gen.py` (text, platforms, importance order). It generates
  `src/app/ui/streaming/hints_table.inc` (`python scripts/tips/tips_gen.py c > src/app/ui/streaming/hints_table.inc`)
  and the tables below (`... md`). Edit the script, regenerate both, never hand-edit the `.inc`.
- **Order:** each stream start shows the most important tip this device has **not shown before**
  (`hints_core.c`). On a co-op stream the unseen co-op tips come first, on an HDR stream the unseen HDR tips;
  otherwise co-op/HDR-only tips never appear. When every tip has been shown, the history is cleared and the
  order starts again from rank 1.
- **Memory:** shown tip ids, one per line, in `hints_seen` in the settings folder (beside `moonlight.conf`).
  Ids, not positions: a later release can add, drop or reorder tips. A new tip is unseen, so it appears at its
  rank; a removed id left in the file is ignored. Deleting the file restarts the order.
- **Platform:** chosen at build time (`TARGET_WEBOS` / `TARGET_ANDROID` / `_WIN32` / otherwise Linux). Rank 0 (shown
  as — below) = never shown on that platform.
- **"HDR stream"** means HDR is turned on in Settings > Video (`hdr`), **"co-op stream"** means the launch asked
  for co-op (`app_launch_coop_requested()`): the tip is chosen as the stream screen is built, before the host
  has answered.
- **Settings screen too** (user 2026-11-13): the launcher's settings bar shows "Tip: ..." in the space between the
  tab icons and the close button, the NEXT tip in the same rotation each time settings opens (every tip is eligible
  there, co-op and HDR included, in its own order: the general order with the co-op/HDR tips slotted in from
  position 11, one every 5 -- `SETTINGS_CTX*` in tips_gen.py). "Next tip" cycles (each one counts as shown), "All tips" opens a scrollable list of every tip this
  platform shows, grouped (every stream / co-op / HDR) and numbered in order; listing marks nothing shown.
  `settings.controller.c` (`settings_launcher_embedded_create`), `hints_all_text()`, `hints_ordered()`.
- The label now wraps inside the right 60% of the screen (tips run to 120 characters; a single line ran off the
  left edge).
- New strings are `translatable(...)` but not in the `.po` files yet: they show in English until translated.

## Decisions behind specific tips (2026-11-13 review)

| Tip | What was checked |
|---|---|
| Old "long press BACK" tip | Removed: no code handles a long press of BACK; BACK sends Escape (`keyboard_webos.c`). Colour-button tips also dropped: not every Magic Remote has them. |
| `pads4` (removed) | The 4-controller limit was Eclipse's own (`max_num_gamepads = 4`, from Moonlight TV, set when GeForce Experience took 4). moonlight-common-c allows 16 with Sunshine-family hosts, so on 2026-11-13 the limit was raised to 16 (`APP_GAMEPAD_SLOTS`) and the tip dropped at the user's request. |
| `fps` | The Aurora author's README says "60 or 120" (GuiDev1994/aurora-tv README, Quick start); no "set it higher than the display" advice was found there or in the author's issue replies (#82, #83). Moonlight-qt's own notes say set the stream FPS to the same as, or lower than, the display's refresh rate. Tip says match. If there is a source for "higher", it should be linked here before the tip changes. |
| `b_*` bitrate tips | Moonlight's defaults (about 20 Mbps 1080p60, 80 Mbps 4K60), the moonlight-tv README's near-lossless HEVC table (1080p60 40–60, 4K60 100–130, 4K120 180–230), and the Aurora README (4K120 HDR start 120–180, at or below 250, little gain above). The `b_stable` 80 Mbps ceiling is for Wi-Fi and the TV's 100 Mbps port. |
| `tv_usb_eth`, `tv_usb_wifi` | Tested adapter: TP-Link UE300 (RTL8153) on the LG G5, ~256 Mbps. Upstream issue GuiDev1994/aurora-tv#58 reports the same UE300 unstable above ~25–30 Mbps on an LG C3, so the tip doesn't recommend buying one. The wired setting keeps the stream on the adapter while Wi-Fi stays on, and needs Umbra (Settings > Basic note). |
| `custom_res` | Co-op's per-PC frame rate and picture size are in Settings > Co-op > Advanced and, left unset, follow the Video settings; each PC then runs up to that rate independently. |
| `fullrange` | Applies to co-op as to any stream, except the experimental GPU-HEVC co-op path, which forces limited range (`session.c`, `coop_gpu_hevc_resolved()`). Not called out in the tip. |
| `pdr` | Setting is Settings > Video > "Periodic decoder refresh (HEVC)"; it needs H.265 turned on. |
| `coop_orient` | There is one orientation value: the co-op launch screen opens on it and saves the choice made there (`coop.dialog.c`), so it opens on whatever was used last; Settings > Co-op > Split orientation changes the same value. |
| `coop_kbm` | "Assign mouse and keyboard like controllers": each stays with the PC it's given to for the whole session (pane text). |
| `umbra_restore` | Monitors are restored when the stream ends (Umbra restores when running sessions reach 0). The unreleased fix (ddf55d13) is for a PAUSED stream leaving them off, a different case. |
| `wrongscreen` | "Start streaming (host monitor off)" comes from Settings > Host > "Computer's monitor while streaming" (in 1.3.6). |
| `logs` | Settings > About > "Keep diagnostic logs" (restart), then "Share logs...": desktop writes one file; TV and Android give an address to open on another device. |
| `coop_local` | Local-pane co-op is Windows only (`platform/windows/local_pane_win.c`); confirmed for the release by the user. |

Still worth confirming before release: `gyro` on each platform, `optimize` (a GeForce Experience-era option; Sunshine
may ignore it) and `stick_scroll` (on the release line, not in the 1.3.6 tag).

## Ranked tips (generated)

### Every stream

| ID | Tip | webOS | Win | Linux | Android |
|---|---|---|---|---|---|
| `menu_pad` | Press LB + RB + Back + Start together, then let go, to open the streaming menu from any controller. | 1 | 1 | 1 | 1 |
| `back_esc` | BACK on the remote sends Escape to the PC. When the on-screen keyboard is open, BACK just closes it. | 2 | — | — | — |
| `mouse_release` | Click inside the window to give the mouse to the stream; Ctrl + Alt + Shift + Z gives it back. | — | 2 | 2 | — |
| `and_menu` | Press INFO, GUIDE or MENU on the remote (or RED, if it has one) to open the streaming menu. | — | — | — | 2 |
| `wired_host` | A wired connection on the host PC is the single biggest improvement you can make. | 3 | 3 | 3 | 3 |
| `tv_port_100` | Many LG TVs' built-in Ethernet port tops out at 100 Mbps. On it, keep the bitrate under about 80 Mbps. | 4 | — | — | — |
| `b_stable` | A steady lower bitrate beats a stuttering high one. On Wi-Fi or a 100 Mbps link, stay under about 80 Mbps. | 5 | 4 | 4 | 4 |
| `and_remote_keys` | The SHIELD remote can't send every PC key. Use a controller shortcut or a USB keyboard for full control. | — | — | — | 5 |
| `recording` | Turn off Steam Game Recording and NVIDIA or AMD Instant Replay on the PC. They compete with the stream's encoder. | 11 | 5 | 5 | 6 |
| `menu_kb` | On a keyboard, Ctrl + Alt + Shift + S opens the streaming menu. | 30 | 6 | 6 | 29 |
| `tv_usb_eth` | Only some USB Ethernet adapters work on LG TVs. Tested: TP-Link UE300 on an LG G5. | 6 | — | — | — |
| `kbd_pad` | Hold RB and click the right stick, then let go, to open the on-screen keyboard. | 8 | 14 | 14 | 7 |
| `quit_kb` | On a keyboard, Ctrl + Alt + Shift + Q ends the stream. | 31 | 7 | 7 | 30 |
| `tv_usb_wifi` | USB adapter fitted? Settings > Basic > Use the wired network puts just the stream on it; other apps stay on Wi-Fi. | 7 | — | — | — |
| `syskeys` | Capture system keys (Settings > Input) sends the Windows key and similar shortcuts to the PC instead. | — | 8 | 8 | — |
| `and_blue` | Press BLUE on the remote, if it has one, to open the on-screen keyboard. | — | — | — | 8 |
| `ok_hold` | Hold OK on the remote for about half a second to right-click. | 9 | — | — | — |
| `and_switch_plus` | On the SHIELD, a Switch Pro Controller's + button works as Start, even though Android calls it Play/Pause. | — | — | — | 9 |
| `fd_bitrate` | Frame drops (FD) above 0% in the stats? Lower the bitrate a little before changing anything else. | 13 | 9 | 9 | 10 |
| `stats_on` | Turn on performance stats in Settings > Basic to see FPS, delay and bitrate during every stream. | 14 | 10 | 10 | 11 |
| `xbox_bt` | Xbox controllers must be paired over Bluetooth on the TV. A USB cable or the Xbox Wireless Adapter won't work. | 10 | — | — | — |
| `b_1080` | Bitrate guide: 1080p at 60 fps looks good at about 20 Mbps and close to lossless at 40-60 Mbps. | 54 | 11 | 11 | 12 |
| `b_4k60` | Bitrate guide: 4K at 60 fps looks good at about 80 Mbps and close to lossless at 100-130 Mbps. | 15 | 12 | 12 | 13 |
| `b_4k120` | Bitrate guide: 4K at 120 fps, start at 120-150 Mbps. Above about 250 Mbps you rarely see a difference. | 12 | 13 | 13 | 14 |
| `wired_client` | Wired beats Wi-Fi on this device too. On Wi-Fi, keep it and the router in the same room. | 41 | 15 | 15 | 15 |
| `vmouse_pad` | Hold LB and click the right stick to turn the virtual mouse on or off (enable it in Settings > Input). | 16 | 26 | 26 | 26 |
| `wifi_band` | Use 5 GHz or 6 GHz Wi-Fi for streaming. 2.4 GHz is slower and shared with many household devices. | 20 | 16 | 16 | 16 |
| `vmouse_use` | Virtual mouse: the right stick moves, LT and RT click, and clicking the left stick turns on scrolling. | 17 | 27 | 27 | 27 |
| `switch` | Other traffic can cause stutter. Put the PC and this device on their own switch to keep the stream off the router. | 21 | 17 | 17 | 17 |
| `pointer` | Point the Magic Remote at the screen to use it as a mouse while streaming. | 18 | — | — | — |
| `fps` | Set the frame rate to your display's refresh rate (60, 120...). More than the screen can show adds nothing. | 23 | 18 | 18 | 18 |
| `umbra_vd` | Umbra makes a virtual display at exactly your resolution and refresh rate, so no monitor changes are needed. | 19 | 19 | 19 | 19 |
| `wake` | Wake & sign-in (Settings > Host) can wake a sleeping PC and sign it in with its PIN or password. | 22 | 20 | 20 | 20 |
| `link_speed` | Check your Ethernet links run at 1 Gbps. A faulty cable or port can quietly drop to 100 Mbps. | 27 | 21 | 21 | 21 |
| `wifi_channel` | Stutter on 5 GHz Wi-Fi? Pick a less crowded channel in your router's settings. | 24 | 22 | 22 | 22 |
| `custom_res` | Set any resolution or frame rate in Settings > Basic. Co-op uses them too, unless changed in Co-op > Advanced. | 26 | 23 | 23 | 23 |
| `pdr` | Smearing after a glitch? Periodic decoder refresh (Settings > Video, needs H.265) sends a clean frame every few seconds. | 28 | 24 | 24 | 24 |
| `trumotion` | Turn off the TV's motion smoothing (TruMotion) and noise reduction: they add delay and can smear the stream. | 25 | — | — | — |
| `audio_device` | Choose which speakers or headphones Eclipse uses in Settings > Audio > Audio output device. | — | 25 | 25 | 25 |
| `stats_pad` | Hold LB and click the left stick to pin or unpin the performance stats. | 29 | 28 | 28 | 28 |
| `osk_sticky` | The on-screen keyboard has sticky Ctrl, Alt, Shift and Win: tap Alt, then Tab, to send Alt+Tab. | 32 | 29 | 29 | 31 |
| `stick_scroll` | In Eclipse's menus, the right stick scrolls long lists and dialogs. | 33 | 30 | 30 | 32 |
| `deadzone` | Stick drift? Raise the analog stick deadzone in Settings > Input. | 34 | 31 | 31 | 33 |
| `wrongscreen` | Game on the wrong screen? Make the stream's display primary, or stream with the PC's monitor off (Settings > Host). | 35 | 32 | 32 | 34 |
| `mute` | "Mute computer while streaming" (Settings > Host) stops the sound playing from the PC's own speakers. | 36 | 33 | 33 | 35 |
| `umbra_monoff` | With Umbra, your PC's own monitor can stay off while you stream. | 37 | 34 | 34 | 36 |
| `umbra_restore` | Umbra puts your monitors back the way they were when the stream ends. | 38 | 35 | 35 | 37 |
| `hdr_needs` | For HDR, turn on H.265 or AV1 in Settings > Video and turn on HDR in Windows on the host. | 40 | 36 | 36 | 38 |
| `rtt` | A high N (network round trip) in the stats points at Wi-Fi or the router, not the PC or the decoder. | 42 | 37 | 37 | 39 |
| `powersave_net` | Turn off power-saving modes on your router and network adapters. They add delay to streaming. | 43 | 38 | 38 | 40 |
| `cable` | Use Cat5e or better Ethernet cables. Old Cat5 cables can limit the link to 100 Mbps. | 44 | 39 | 39 | 41 |
| `bt_pair` | Bluetooth controllers work best paired directly to the TV in its Bluetooth settings. | 39 | — | — | — |
| `mesh` | Avoid mesh "satellite" hops for streaming. Connect to the main router where you can. | 45 | 40 | 40 | 42 |
| `powerline` | Powerline adapters vary a lot. If you use one, watch for frame drops in the stats. | 46 | 41 | 41 | 43 |
| `vpn` | A VPN on the PC or this device can send the stream the long way round. Pause it when streaming at home. | 47 | 42 | 42 | 44 |
| `remote` | Streaming away from home? Lower the bitrate: home upload speeds are usually far below download speeds. | 48 | 43 | 43 | 45 |
| `nintendo` | Nintendo controllers: choose printed or Xbox button positions for A/B and X/Y in Settings > Input. | 49 | 44 | 44 | 46 |
| `gyro` | DualShock 4 and DualSense gyro reaches the PC, so gyro aiming works in games that support it. | 50 | 45 | 45 | 47 |
| `absmouse` | Absolute mouse mode (Settings > Input) puts the pointer exactly where you click. Best for desktops, not games. | 51 | 46 | 46 | — |
| `fullrange` | Banding or washed-out colour in SDR? Try Full range YUV (Settings > Video). Your host must support it. | 52 | 47 | 47 | 48 |
| `lowres` | Streaming below your display's resolution saves bandwidth; the picture is scaled up on this device. | 53 | 48 | 48 | 49 |
| `powerplan` | Use the PC's "Balanced" or "High performance" power plan so the encoder doesn't slow down. | 55 | 49 | 49 | 50 |
| `gamecap` | Capping a game at the stream's frame rate saves GPU power and can steady the stream. | 56 | 50 | 50 | 51 |
| `optimize` | "Optimize game settings for streaming" lets supported games switch to the stream's resolution. | 57 | 51 | 51 | 52 |
| `pair` | A PC only needs the pairing PIN the first time you connect to it. | 58 | 52 | 52 | 53 |
| `coop_demo` | Test split-screen with one PC (Settings > Co-op) is a preview of co-op only; you can't play in it. | 59 | 53 | 53 | 54 |
| `surround` | Surround sound is in Settings > Audio > Sound Channels (experimental). | 60 | 54 | 54 | 55 |
| `audio_restart` | Audio settings apply to the next stream after you restart the app. | 61 | 55 | 55 | 56 |
| `lipsync` | Sound out of sync? Check your TV or receiver for an audio delay or "lip sync" setting. | 62 | 56 | 56 | 57 |
| `logs` | Reporting a bug? Turn on Keep diagnostic logs in Settings > About, restart, repeat it, then use Share logs. | 63 | 57 | 57 | 58 |
| `report` | Report bugs on the Eclipse GitHub page with your version number (Settings > About) and your shared logs. | 64 | 58 | 58 | 59 |
| `oss` | Eclipse is free and open source. Feel free to fork it or contribute on GitHub. | 65 | 59 | 59 | 60 |

### Co-op streams only (own order, shown first on a co-op stream)

| ID | Tip | webOS | Win | Linux | Android |
|---|---|---|---|---|---|
| `coop_pointer` | In co-op, the mouse pointer follows the half of the screen it's over, so one mouse works both PCs. | 1 | 1 | 1 | 1 |
| `coop_pads` | Assign controllers in Settings > Co-op > Default controllers so each player lands on the right PC. | 2 | 2 | 2 | 2 |
| `coop_kbm` | Settings > Co-op can give the mouse and keyboard to one PC like a controller; they then stay on that half. | 3 | 3 | 3 | 3 |
| `coop_orient` | Co-op opens on the split you used last. Change it on the launch screen or in Settings > Co-op > Split orientation. | 4 | 4 | 4 | 4 |
| `coop_local` | On Windows, this PC can be one of the co-op players: its own half runs locally, only the other PC is streamed. | — | 5 | — | — |
| `coop_half` | In a co-op stream, each PC sends half the picture, so each needs less bandwidth than a full stream. | 6 | 6 | 6 | 6 |
| `coop_wired` | Co-op runs two streams over one link, so wired connections matter even more. | 7 | 7 | 7 | 7 |
| `coop_reset` | If co-op misbehaves after changing advanced settings, use "Restore advanced defaults" in Settings > Co-op. | 8 | 8 | 8 | 8 |

### HDR streams only (own order, shown first on an HDR stream)

| ID | Tip | webOS | Win | Linux | Android |
|---|---|---|---|---|---|
| `hdr_dull` | HDR looks dull or grey? Make sure the host's display (or virtual display) is in HDR mode before you start. | 1 | 1 | 1 | 1 |
| `hdr_calib` | Games with an HDR calibration screen look best when you calibrate them through the stream. | 2 | 2 | 2 | 2 |

### Settings bar order (every tip, co-op and HDR slotted in)

**webOS:** 1. `menu_pad`, 2. `back_esc`, 3. `wired_host`, 4. `tv_port_100`, 5. `b_stable`, 6. `tv_usb_eth`, 7. `tv_usb_wifi`, 8. `kbd_pad`, 9. `ok_hold`, 10. `xbox_bt`, 11. `coop_pointer`, 12. `recording`, 13. `b_4k120`, 14. `fd_bitrate`, 15. `stats_on`, 16. `b_4k60`, 17. `coop_pads`, 18. `vmouse_pad`, 19. `vmouse_use`, 20. `pointer`, 21. `umbra_vd`, 22. `wifi_band`, 23. `hdr_dull`, 24. `switch`, 25. `wake`, 26. `fps`, 27. `wifi_channel`, 28. `trumotion`, 29. `coop_kbm`, 30. `custom_res`, 31. `link_speed`, 32. `pdr`, 33. `stats_pad`, 34. `menu_kb`, 35. `coop_orient`, 36. `quit_kb`, 37. `osk_sticky`, 38. `stick_scroll`, 39. `deadzone`, 40. `wrongscreen`, 41. `hdr_calib`, 42. `mute`, 43. `umbra_monoff`, 44. `umbra_restore`, 45. `bt_pair`, 46. `hdr_needs`, 47. `coop_half`, 48. `wired_client`, 49. `rtt`, 50. `powersave_net`, 51. `cable`, 52. `mesh`, 53. `coop_wired`, 54. `powerline`, 55. `vpn`, 56. `remote`, 57. `nintendo`, 58. `gyro`, 59. `coop_reset`, 60. `absmouse`, 61. `fullrange`, 62. `lowres`, 63. `b_1080`, 64. `powerplan`, 65. `gamecap`, 66. `optimize`, 67. `pair`, 68. `coop_demo`, 69. `surround`, 70. `audio_restart`, 71. `lipsync`, 72. `logs`, 73. `report`, 74. `oss`

**Windows and Linux:** 1. `menu_pad`, 2. `mouse_release`, 3. `wired_host`, 4. `b_stable`, 5. `recording`, 6. `menu_kb`, 7. `quit_kb`, 8. `syskeys`, 9. `fd_bitrate`, 10. `stats_on`, 11. `coop_pointer`, 12. `b_1080`, 13. `b_4k60`, 14. `b_4k120`, 15. `kbd_pad`, 16. `wired_client`, 17. `coop_pads`, 18. `wifi_band`, 19. `switch`, 20. `fps`, 21. `umbra_vd`, 22. `wake`, 23. `hdr_dull`, 24. `link_speed`, 25. `wifi_channel`, 26. `custom_res`, 27. `pdr`, 28. `audio_device`, 29. `coop_kbm`, 30. `vmouse_pad`, 31. `vmouse_use`, 32. `stats_pad`, 33. `osk_sticky`, 34. `stick_scroll`, 35. `coop_orient`, 36. `deadzone`, 37. `wrongscreen`, 38. `mute`, 39. `umbra_monoff`, 40. `umbra_restore`, 41. `hdr_calib`, 42. `hdr_needs`, 43. `rtt`, 44. `powersave_net`, 45. `cable`, 46. `mesh`, 47. `coop_local`, 48. `powerline`, 49. `vpn`, 50. `remote`, 51. `nintendo`, 52. `gyro`, 53. `coop_half`, 54. `absmouse`, 55. `fullrange`, 56. `lowres`, 57. `powerplan`, 58. `gamecap`, 59. `coop_wired`, 60. `optimize`, 61. `pair`, 62. `coop_demo`, 63. `surround`, 64. `audio_restart`, 65. `coop_reset`, 66. `lipsync`, 67. `logs`, 68. `report`, 69. `oss`

**Android:** 1. `menu_pad`, 2. `and_menu`, 3. `wired_host`, 4. `b_stable`, 5. `and_remote_keys`, 6. `recording`, 7. `kbd_pad`, 8. `and_blue`, 9. `and_switch_plus`, 10. `fd_bitrate`, 11. `coop_pointer`, 12. `stats_on`, 13. `b_1080`, 14. `b_4k60`, 15. `b_4k120`, 16. `wired_client`, 17. `coop_pads`, 18. `wifi_band`, 19. `switch`, 20. `fps`, 21. `umbra_vd`, 22. `wake`, 23. `hdr_dull`, 24. `link_speed`, 25. `wifi_channel`, 26. `custom_res`, 27. `pdr`, 28. `audio_device`, 29. `coop_kbm`, 30. `vmouse_pad`, 31. `vmouse_use`, 32. `stats_pad`, 33. `menu_kb`, 34. `quit_kb`, 35. `coop_orient`, 36. `osk_sticky`, 37. `stick_scroll`, 38. `deadzone`, 39. `wrongscreen`, 40. `mute`, 41. `hdr_calib`, 42. `umbra_monoff`, 43. `umbra_restore`, 44. `hdr_needs`, 45. `rtt`, 46. `powersave_net`, 47. `coop_half`, 48. `cable`, 49. `mesh`, 50. `powerline`, 51. `vpn`, 52. `remote`, 53. `coop_wired`, 54. `nintendo`, 55. `gyro`, 56. `fullrange`, 57. `lowres`, 58. `powerplan`, 59. `coop_reset`, 60. `gamecap`, 61. `optimize`, 62. `pair`, 63. `coop_demo`, 64. `surround`, 65. `audio_restart`, 66. `lipsync`, 67. `logs`, 68. `report`, 69. `oss`

