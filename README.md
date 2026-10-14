# BE 0700-AAM (Kick)

Source code of **Kick**, an MPC-style groove box / sampler that was not finished. It ran on an NXP i.MX8M Mini
(Variscite DART-MX8M-MINI) with a Buildroot Linux image and a JUCE-based application.

**Status:** development has stopped and the code is published as-is. It is not maintained.

| Folder | Contents | License |
|---|---|---|
| [`Kick/`](Kick) | The application (UI, sequencer, sampler, hardware control) | AGPL-3.0-or-later |
| [`AudIO/`](AudIO) | Audio engine library used by Kick | AGPL-3.0-or-later |
| [`ComProtocol/`](ComProtocol) | Packet protocol between the application and the device hardware | AGPL-3.0-or-later |
| [`Linux/`](Linux) | Buildroot tree for the device's Linux image | GPL-2.0 (Buildroot) |

Each folder has its own README covering build notes, removed material and bundled third-party code.
Third-party components keep their original licenses.

Copyright (c) MUSIC Tribe.
