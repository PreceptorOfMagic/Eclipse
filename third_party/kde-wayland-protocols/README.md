# KDE Wayland protocols (host-and-client co-op on KDE Plasma)

Unmodified protocol descriptions from KDE's plasma-wayland-protocols, tag v1.21.0
(https://invent.kde.org/libraries/plasma-wayland-protocols/-/tree/v1.21.0/src/protocols).
`wayland-scanner` turns them into client code at build time (src/app/platform/linux/local_pane_kde.c).

| File | Licence (in the file) |
|---|---|
| `zkde-screencast-unstable-v1.xml`, `kde-output-device-v2.xml`, `kde-output-management-v2.xml` | MIT-CMU (`MIT-CMU.txt`) |
| `fake-input.xml` | LGPL-2.1-or-later (SPDX header in the file) |

The first three are the copies Umbra for Linux uses (third-party/kde-output-protocols in the Umbra repository).
