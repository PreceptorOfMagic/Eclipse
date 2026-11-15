"""Single source for the streaming tips: emits the doc table and the C table.

Usage (from the repo root):
       python scripts/tips/tips_gen.py c  > src/app/ui/streaming/hints_table.inc
       python scripts/tips/tips_gen.py md   (paste into docs/streaming-tips.md)
"""
import sys

# id, when (any|coop|hdr), platforms (W=webOS D=Windows L=Linux A=Android), text
TIPS = [
    # --- controls
    ("menu_pad", "any", "WDLA", "Press LB + RB + Back + Start together, then let go, to open the streaming menu from any controller."),
    ("kbd_pad", "any", "WDLA", "Hold RB and click the right stick, then let go, to open the on-screen keyboard."),
    ("vmouse_pad", "any", "WDLA", "Hold LB and click the right stick to turn the virtual mouse on or off (enable it in Settings > Input)."),
    ("vmouse_use", "any", "WDLA", "Virtual mouse: the right stick moves, LT and RT click, and clicking the left stick turns on scrolling."),
    ("stats_pad", "any", "WDLA", "Hold LB and click the left stick to pin or unpin the performance stats."),
    ("menu_kb", "any", "WDLA", "On a keyboard, Ctrl + Alt + Shift + S opens the streaming menu."),
    ("quit_kb", "any", "WDLA", "On a keyboard, Ctrl + Alt + Shift + Q ends the stream."),
    ("osk_sticky", "any", "WDLA", "The on-screen keyboard has sticky Ctrl, Alt, Shift and Win: tap Alt, then Tab, to send Alt+Tab."),
    ("stick_scroll", "any", "WDLA", "In Eclipse's menus, the right stick scrolls long lists and dialogs."),
    ("back_esc", "any", "W", "BACK on the remote sends Escape to the PC. When the on-screen keyboard is open, BACK just closes it."),
    ("ok_hold", "any", "W", "Hold OK on the remote for about half a second to right-click."),
    ("pointer", "any", "W", "Point the Magic Remote at the screen to use it as a mouse while streaming."),
    ("mouse_release", "any", "DL", "Click inside the window to give the mouse to the stream; Ctrl + Alt + Shift + Z gives it back."),
    ("syskeys", "any", "DL", "Capture system keys (Settings > Input) sends the Windows key and similar shortcuts to the PC instead."),
    ("and_menu", "any", "A", "Press INFO, GUIDE or MENU on the remote (or RED, if it has one) to open the streaming menu."),
    ("and_blue", "any", "A", "Press BLUE on the remote, if it has one, to open the on-screen keyboard."),
    ("and_remote_keys", "any", "A", "The SHIELD remote can't send every PC key. Use a controller shortcut or a USB keyboard for full control."),
    ("and_switch_plus", "any", "A", "On the SHIELD, a Switch Pro Controller's + button works as Start, even though Android calls it Play/Pause."),
    # --- network
    ("wired_host", "any", "WDLA", "A wired connection on the host PC is the single biggest improvement you can make."),
    ("wired_client", "any", "WDLA", "Wired beats Wi-Fi on this device too. On Wi-Fi, keep it and the router in the same room."),
    ("wifi_band", "any", "WDLA", "Use 5 GHz or 6 GHz Wi-Fi for streaming. 2.4 GHz is slower and shared with many household devices."),
    ("wifi_channel", "any", "WDLA", "Stutter on 5 GHz Wi-Fi? Pick a less crowded channel in your router's settings."),
    ("mesh", "any", "WDLA", "Avoid mesh \"satellite\" hops for streaming. Connect to the main router where you can."),
    ("powerline", "any", "WDLA", "Powerline adapters vary a lot. If you use one, watch for frame drops in the stats."),
    ("switch", "any", "WDLA", "Other traffic can cause stutter. Put the PC and this device on their own switch to keep the stream off the router."),
    ("fd_bitrate", "any", "WDLA", "Frame drops (FD) above 0% in the stats? Lower the bitrate a little before changing anything else."),
    ("rtt", "any", "WDLA", "A high N (network round trip) in the stats points at Wi-Fi or the router, not the PC or the decoder."),
    ("powersave_net", "any", "WDLA", "Turn off power-saving modes on your router and network adapters. They add delay to streaming."),
    ("vpn", "any", "WDLA", "A VPN on the PC or this device can send the stream the long way round. Pause it when streaming at home."),
    ("remote", "any", "WDLA", "Streaming away from home? Lower the bitrate: home upload speeds are usually far below download speeds."),
    ("cable", "any", "WDLA", "Use Cat5e or better Ethernet cables. Old Cat5 cables can limit the link to 100 Mbps."),
    ("link_speed", "any", "WDLA", "Check your Ethernet links run at 1 Gbps. A faulty cable or port can quietly drop to 100 Mbps."),
    ("tv_port_100", "any", "W", "Many LG TVs' built-in Ethernet port tops out at 100 Mbps. On it, keep the bitrate under about 80 Mbps."),
    ("tv_usb_eth", "any", "W", "Only some USB Ethernet adapters work on LG TVs. Tested: TP-Link UE300 on an LG G5."),
    ("tv_usb_wifi", "any", "W", "USB adapter fitted? Settings > Basic > Use the wired network puts just the stream on it; other apps stay on Wi-Fi."),
    # --- picture
    ("b_stable", "any", "WDLA", "A steady lower bitrate beats a stuttering high one. On Wi-Fi or a 100 Mbps link, stay under about 80 Mbps."),
    ("b_1080", "any", "WDLA", "Bitrate guide: 1080p at 60 fps looks good at about 20 Mbps and close to lossless at 40-60 Mbps."),
    ("b_4k60", "any", "WDLA", "Bitrate guide: 4K at 60 fps looks good at about 80 Mbps and close to lossless at 100-130 Mbps."),
    ("b_4k120", "any", "WDLA", "Bitrate guide: 4K at 120 fps, start at 120-150 Mbps. Above about 250 Mbps you rarely see a difference."),
    ("fps", "any", "WDLA", "Set the frame rate to your display's refresh rate (60, 120...). More than the screen can show adds nothing."),
    ("custom_res", "any", "WDLA", "Set any resolution or frame rate in Settings > Basic. Co-op uses them too, unless changed in Co-op > Advanced."),
    ("stats_on", "any", "WDLA", "Turn on performance stats in Settings > Basic to see FPS, delay and bitrate during every stream."),
    ("fullrange", "any", "WDLA", "Banding or washed-out colour in SDR? Try Full range YUV (Settings > Video). Your host must support it."),
    ("pdr", "any", "WDLA", "Smearing after a glitch? Periodic decoder refresh (Settings > Video, needs H.265) sends a clean frame every few seconds."),
    ("lowres", "any", "WDLA", "Streaming below your display's resolution saves bandwidth; the picture is scaled up on this device."),
    ("hdr_needs", "any", "WDLA", "For HDR, turn on H.265 or AV1 in Settings > Video and turn on HDR in Windows on the host."),
    ("hdr_dull", "hdr", "WDLA", "HDR looks dull or grey? Make sure the host's display (or virtual display) is in HDR mode before you start."),
    ("hdr_calib", "hdr", "WDLA", "Games with an HDR calibration screen look best when you calibrate them through the stream."),
    # --- input
    ("nintendo", "any", "WDLA", "Nintendo controllers: choose printed or Xbox button positions for A/B and X/Y in Settings > Input."),
    ("deadzone", "any", "WDLA", "Stick drift? Raise the analog stick deadzone in Settings > Input."),
    ("gyro", "any", "WDLA", "DualShock 4 and DualSense gyro reaches the PC, so gyro aiming works in games that support it."),
    ("absmouse", "any", "WDL", "Absolute mouse mode (Settings > Input) puts the pointer exactly where you click. Best for desktops, not games."),
    ("trumotion", "any", "W", "Turn off the TV's motion smoothing (TruMotion) and noise reduction: they add delay and can smear the stream."),
    ("bt_pair", "any", "W", "Bluetooth controllers work best paired directly to the TV in its Bluetooth settings."),
    ("xbox_bt", "any", "W", "Xbox controllers must be paired over Bluetooth on the TV. A USB cable or the Xbox Wireless Adapter won't work."),
    # --- host
    ("wake", "any", "WDLA", "Wake & sign-in (Settings > Host) can wake a sleeping PC and sign it in with its PIN or password."),
    ("mute", "any", "WDLA", "\"Mute computer while streaming\" (Settings > Host) stops the sound playing from the PC's own speakers."),
    ("optimize", "any", "WDLA", "\"Optimize game settings for streaming\" lets supported games switch to the stream's resolution."),
    ("recording", "any", "WDLA", "Turn off Steam Game Recording and NVIDIA or AMD Instant Replay on the PC. They compete with the stream's encoder."),
    ("powerplan", "any", "WDLA", "Use the PC's \"Balanced\" or \"High performance\" power plan so the encoder doesn't slow down."),
    ("wrongscreen", "any", "WDLA", "Game on the wrong screen? Make the stream's display primary, or stream with the PC's monitor off (Settings > Host)."),
    ("gamecap", "any", "WDLA", "Capping a game at the stream's frame rate saves GPU power and can steady the stream."),
    ("pair", "any", "WDLA", "A PC only needs the pairing PIN the first time you connect to it."),
    ("umbra_vd", "any", "WDLA", "Umbra makes a virtual display at exactly your resolution and refresh rate, so no monitor changes are needed."),
    ("umbra_monoff", "any", "WDLA", "With Umbra, your PC's own monitor can stay off while you stream."),
    ("umbra_restore", "any", "WDLA", "Umbra puts your monitors back the way they were when the stream ends."),
    # --- audio
    ("surround", "any", "WDLA", "Surround sound is in Settings > Audio > Sound Channels (experimental)."),
    ("audio_restart", "any", "WDLA", "Audio settings apply to the next stream after you restart the app."),
    ("lipsync", "any", "WDLA", "Sound out of sync? Check your TV or receiver for an audio delay or \"lip sync\" setting."),
    ("audio_device", "any", "DLA", "Choose which speakers or headphones Eclipse uses in Settings > Audio > Audio output device."),
    # --- co-op
    ("coop_pointer", "coop", "WDLA", "In co-op, the mouse pointer follows the half of the screen it's over, so one mouse works both PCs."),
    ("coop_pads", "coop", "WDLA", "Assign controllers in Settings > Co-op > Default controllers so each player lands on the right PC."),
    ("coop_kbm", "coop", "WDLA", "Settings > Co-op can give the mouse and keyboard to one PC like a controller; they then stay on that half."),
    ("coop_orient", "coop", "WDLA", "Co-op opens on the split you used last. Change it on the launch screen or in Settings > Co-op > Split orientation."),
    ("coop_half", "coop", "WDLA", "In a co-op stream, each PC sends half the picture, so each needs less bandwidth than a full stream."),
    ("coop_wired", "coop", "WDLA", "Co-op runs two streams over one link, so wired connections matter even more."),
    ("coop_local", "coop", "D", "On Windows, this PC can be one of the co-op players: its own half runs locally, only the other PC is streamed."),
    ("coop_reset", "coop", "WDLA", "If co-op misbehaves after changing advanced settings, use \"Restore advanced defaults\" in Settings > Co-op."),
    ("coop_demo", "any", "WDLA", "Test split-screen with one PC (Settings > Co-op) is a preview of co-op only; you can't play in it."),
    # --- general
    ("logs", "any", "WDLA", "Reporting a bug? Turn on Keep diagnostic logs in Settings > About, restart, repeat it, then use Share logs."),
    ("report", "any", "WDLA", "Report bugs on the Eclipse GitHub page with your version number (Settings > About) and your shared logs."),
    ("oss", "any", "WDLA", "Eclipse is free and open source. Feel free to fork it or contribute on GitHub."),
]

# Importance order per platform (first = shown first). General tips only; co-op/HDR have their own order.
ORDER = {
    "W": """menu_pad back_esc wired_host tv_port_100 b_stable tv_usb_eth tv_usb_wifi kbd_pad ok_hold xbox_bt recording
            b_4k120 fd_bitrate stats_on b_4k60 vmouse_pad vmouse_use pointer umbra_vd wifi_band switch wake fps
            wifi_channel trumotion custom_res link_speed pdr stats_pad menu_kb quit_kb osk_sticky stick_scroll deadzone
            wrongscreen mute umbra_monoff umbra_restore bt_pair hdr_needs wired_client rtt powersave_net cable mesh
            powerline vpn remote  nintendo gyro absmouse fullrange lowres b_1080 powerplan gamecap optimize pair
            coop_demo surround audio_restart lipsync logs report oss""",
    "D": """menu_pad mouse_release wired_host b_stable recording menu_kb quit_kb syskeys fd_bitrate stats_on b_1080 b_4k60
            b_4k120 kbd_pad wired_client wifi_band switch fps umbra_vd wake link_speed wifi_channel custom_res pdr
            audio_device vmouse_pad vmouse_use stats_pad osk_sticky stick_scroll deadzone wrongscreen mute umbra_monoff
            umbra_restore hdr_needs rtt powersave_net cable mesh powerline vpn remote  nintendo gyro absmouse
            fullrange lowres powerplan gamecap optimize pair coop_demo surround audio_restart lipsync logs report oss""",
    "A": """menu_pad and_menu wired_host b_stable and_remote_keys recording kbd_pad and_blue and_switch_plus fd_bitrate
            stats_on b_1080 b_4k60 b_4k120 wired_client wifi_band switch fps umbra_vd wake link_speed wifi_channel
            custom_res pdr audio_device vmouse_pad vmouse_use stats_pad menu_kb quit_kb osk_sticky stick_scroll deadzone
            wrongscreen mute umbra_monoff umbra_restore hdr_needs rtt powersave_net cable mesh powerline vpn remote 
            nintendo gyro fullrange lowres powerplan gamecap optimize pair coop_demo surround audio_restart lipsync logs
            report oss""",
}
ORDER["L"] = ORDER["D"]
CTX_ORDER = {
    "coop": "coop_pointer coop_pads coop_kbm coop_orient coop_local coop_half coop_wired coop_reset",
    "hdr": "hdr_dull hdr_calib",
}
# The settings bar is not a stream, so it shows every tip, co-op and HDR included (user 2026-11-13). Its single
# order per platform: the general order, with the co-op/HDR tips (in this order) slotted in one at a time --
# the first after general tip SETTINGS_CTX_FIRST, then one every SETTINGS_CTX_EVERY general tips.
SETTINGS_CTX = "coop_pointer coop_pads hdr_dull coop_kbm coop_orient hdr_calib coop_local coop_half coop_wired coop_reset"
SETTINGS_CTX_FIRST = 10
SETTINGS_CTX_EVERY = 5
PLATS = "WDLA"
PNAME = {"W": "webOS", "D": "Windows", "L": "Linux", "A": "Android"}


def ranks():
    by_id = {t[0]: t for t in TIPS}
    assert len(by_id) == len(TIPS), "duplicate id"
    r = {}
    for p in PLATS:
        seq = ORDER[p].split()
        assert len(seq) == len(set(seq)), (p, "duplicate in order")
        for i, tid in enumerate(seq, 1):
            t = by_id[tid]
            assert t[1] == "any" and p in t[2], (p, tid, "not a general tip for this platform")
            r[(tid, p)] = i
    for ctx, s in CTX_ORDER.items():
        for i, tid in enumerate(s.split(), 1):
            t = by_id[tid]
            assert t[1] == ctx, (tid, ctx)
            for p in t[2]:
                r[(tid, p)] = i
    for t in TIPS:
        for p in t[2]:
            assert (t[0], p) in r, (t[0], p, "has no rank")
        for p in PLATS:
            if p not in t[2]:
                assert (t[0], p) not in r
        assert len(t[3]) <= 120, (t[0], len(t[3]))
    return r


def settings_order(p):
    by_id = {t[0]: t for t in TIPS}
    general = ORDER[p].split()
    ctx = [tid for tid in SETTINGS_CTX.split() if p in by_id[tid][2]]
    assert sorted(SETTINGS_CTX.split()) == sorted(t[0] for t in TIPS if t[1] != "any"), "SETTINGS_CTX must list every co-op/HDR tip"
    out = []
    for i, tid in enumerate(general, 1):
        out.append(tid)
        if ctx and i >= SETTINGS_CTX_FIRST and (i - SETTINGS_CTX_FIRST) % SETTINGS_CTX_EVERY == 0:
            out.append(ctx.pop(0))
    out.extend(ctx)
    assert len(out) <= 255
    return out


def settings_ranks():
    return {(tid, p): i for p in PLATS for i, tid in enumerate(settings_order(p), 1)}


def md():
    r = ranks()
    for ctx, title in (("any", "Every stream"), ("coop", "Co-op streams only (own order, shown first on a co-op stream)"),
                       ("hdr", "HDR streams only (own order, shown first on an HDR stream)")):
        print(f"\n### {title}\n")
        print("| ID | Tip | webOS | Win | Linux | Android |")
        print("|---|---|---|---|---|---|")
        rows = [t for t in TIPS if t[1] == ctx]
        rows.sort(key=lambda t: min(r.get((t[0], p), 999) for p in PLATS))
        for t in rows:
            cells = [str(r.get((t[0], p), "—")) for p in PLATS]
            print(f"| `{t[0]}` | {t[3]} | " + " | ".join(cells) + " |")
    print()
    print("### Settings bar order (every tip, co-op and HDR slotted in)")
    print()
    for p in PLATS:
        if p == "L":
            continue
        name = "Windows and Linux" if p == "D" else PNAME[p]
        print(f"**{name}:** " + ", ".join(f"{i}. `{tid}`" for i, tid in enumerate(settings_order(p), 1)))
        print()


def c_escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def c():
    r = ranks()
    print("/* GENERATED by scripts/tips/tips_gen.py. Edit there, not here. */")
    sr = settings_ranks()
    print("/* {id, text, when, stream rank {webOS, Windows, Linux, Android}, settings-bar rank {same}; 0 = not shown} */")
    for t in TIPS:
        when = {"any": "HINT_ANY", "coop": "HINT_COOP", "hdr": "HINT_HDR"}[t[1]]
        rk = ", ".join(str(r.get((t[0], p), 0)) for p in PLATS)
        so = ", ".join(str(sr.get((t[0], p), 0)) for p in PLATS)
        print(f'{{"{t[0]}", translatable("{c_escape(t[3])}"), {when}, {{{rk}}}, {{{so}}}}},')


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", newline=chr(10))   # Windows defaults to cp1252 and CRLF
    {"md": md, "c": c}[sys.argv[1]]()
