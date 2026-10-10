Eclipse for Windows x64

Recommended install: run eclipse-windows-x64-setup-unsigned.exe. It installs
for your account, adds Eclipse to the Start menu and Windows Installed Apps,
and needs no administrator privileges. Upgrades keep your settings/pairings.
Nothing is added to startup and no background service is installed.

Portable: extract the entire ZIP into a folder, then run moonlight-tv.exe.
Keep all DLLs and subfolders together. No MSYS2/compiler is needed to run it.

Windows 10 version 1803 or newer and a compatible hardware video decoder are
required; keep the PC's GPU driver current. Add/pair your streaming host in
Eclipse. Co-op's host extensions are supplied by the matching Umbra installer.

Close Eclipse to stop the client and its owned helper processes. Disconnect
pauses the host game; Quit ends the host session. Umbra's background service
follows Apollo's lifecycle and is managed separately on the host PC.

Remove an installed copy through Windows Installed Apps > Eclipse > Uninstall.
By default it removes its settings, pairings, cache and logs for this account;
select Keep settings if reinstalling. Unknown files you put into the installation
folder are retained. For a portable copy, close it and delete its extracted
folder. Its shared per-user settings are in:
  %APPDATA%\com.limelight\moonlight-tv
Remove that application folder too if you want to erase portable settings.

This community build is unsigned. Compare the adjacent SHA-256 checksum with
Get-FileHash before running downloaded packages. See COPYRIGHT, LICENSE.txt,
THIRD-PARTY-NOTICES.txt, SOURCE-MANIFEST.txt and licenses/ for provenance.
