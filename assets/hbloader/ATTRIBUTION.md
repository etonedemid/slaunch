# Bundled homebrew loader

`main` / `main.npdm` here are a build of sLaunch's fork of
**[nx-hbloader](https://github.com/switchbrew/nx-hbloader)**
((c) 2017-2018 nx-hbloader Authors, ISC license). The fork source lives in
`projects/hbloader/` (see its README for the changes); `make hbloader` rebuilds
`main` from it. sSystem serves this folder as external code for the album
applet slot via ECS when you open the Homebrew menu; launched with no target,
nx-hbloader loads `sdmc:/hbmenu.nro`, and with `sdmc:/slaunch/hbtarget.txt`
present it loads that NRO and exits back to the menu when it closes.

ISC license (permits redistribution with this notice):

> Permission to use, copy, modify, and/or distribute this software for any
> purpose with or without fee is hereby granted, provided that the above
> copyright notice and this permission notice appear in all copies.
