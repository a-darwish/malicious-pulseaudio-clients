
## Introduction

This is a repository of malicious PulseAudio clients. Check
[this PulseAudio Access Control document](https://www.freedesktop.org/wiki/Software/PulseAudio/Documentation/Developer/AccessControl/)
for further details.

### Applications

- exhaust-open-streams: Exhaust default sink number of open streams, effectively
  disabling all other new apps from outputting audio and force-muting the system.

- kill-server-quickly-open-write-streams: Kill the PA server by opening and
  writing to multiple connected streams in parallel. This forces multiple rewinds
  in the server, leading it to exceed its real-time budget and getting killed by
  the kernel.
