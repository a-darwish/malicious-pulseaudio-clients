
## Introduction

This is a repository of malicious PulseAudio clients. Check the
[PulseAudio Access Control](https://www.freedesktop.org/wiki/Software/PulseAudio/Documentation/Developer/AccessControl/)
document for further details.

### Applications

- `exhaust-open-streams`: Exhaust server's default sink max number of connected
  streams, effectively disabling all other new apps from outputting any audio and
  force-muting the system.

- `kill-server-quickly-open-write-streams`: Force the server daemon to be killed.
  Quickly open, and write to, multiple connected streams in parallel. This will
  result in excessive rewinding, leading the server to exceed its 200ms real-time
  budged, and thus getting a SIGKILL from the kernel.
