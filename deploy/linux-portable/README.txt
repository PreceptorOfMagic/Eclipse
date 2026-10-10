Eclipse for Linux (64-bit x86) -- portable build
================================================

START
  1. Unpack the archive anywhere, e.g. your home folder.
  2. Run ./install.sh. Eclipse is installed for your user and appears in the applications menu.
     No administrator password, background service or startup task is needed.
     After installation you can delete the unpacked download folder.
  3. For portable use instead, run ./eclipse directly. Optional: ./install-desktop-entry.sh adds
     this portable folder to the menu; run it again after moving the folder.

REQUIREMENTS
  - A 64-bit x86 PC with an X11 or Wayland desktop and glibc 2.35 or newer: Ubuntu 22.04 or newer,
    Linux Mint 21+, Pop!_OS 22.04+, Debian 12+, Fedora 36+, Arch, SteamOS 3.5+ and similar.
  - Nothing to install on a normal desktop. If something is missing, ./eclipse names it and the
    command that installs it.
  - The picture is decoded on the graphics card when a driver allows it: NVIDIA's own driver (NVDEC)
    or VA-API on Intel and AMD (packages intel-media-va-driver / mesa-va-drivers or your
    distribution's equivalent). Otherwise it is decoded on the processor.

PAIRING A PC
  Eclipse streams from PCs running Apollo or Sunshine on the same network. Choose the PC, then enter
  the PIN Eclipse shows in the PC's web interface (PIN page). On Apollo also allow the new device
  "Launch Apps" and "Controller Input" (PIN > Device Management), otherwise starting a game is
  refused with "403 Permission denied".

WHERE THINGS ARE
  Settings, paired PCs and keys:  ~/.local/share/com.limelight/moonlight-tv/
  Log when started from the menu: ~/.local/state/eclipse/eclipse.log (previous run: eclipse.log.1)
  Installed application:         ~/.local/share/eclipse/app/
  Cache:                         ~/.cache/moonlight-tv/
  XDG_DATA_HOME, XDG_STATE_HOME and XDG_CACHE_HOME override the corresponding defaults above.

UNINSTALL
  Run ~/.local/share/eclipse/app/uninstall.sh (or ./uninstall.sh in a portable copy).
  This closes that copy of Eclipse, removes its application folder and menu entry, and deletes
  your settings, pairing keys, cache and logs. Use --keep-settings to retain that personal data.
  The application data directory is the historical Moonlight TV directory named above; keeping
  a second Eclipse/Moonlight TV install with shared settings requires --keep-settings.
  Uninstalling one portable copy does not remove a menu entry owned by another copy.

CONTROLLERS
  Controllers work through the system's normal input devices. PlayStation and Switch controllers get
  their full feature set with the udev rules most distributions ship for Steam (package
  "steam-devices" or "steam-udev"); without them they still work as generic controllers.

TROUBLESHOOTING
  - Start ./eclipse from a terminal to see its messages.
  - Choose the display system:  SDL_VIDEODRIVER=x11 ./eclipse   or   SDL_VIDEODRIVER=wayland ./eclipse
  - Choose the video decoder:   ECLIPSE_DIAGNOSTIC_VIDEO_DECODER=software ./eclipse   (or nvdec, vaapi)

VERIFYING THIS DOWNLOAD
  This build is not signed. Compare the archive with the published .sha256 file, and inside this
  folder run:  sha256sum -c SHA256SUMS
