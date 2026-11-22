"""GhettoRack bridge on the T95, split into building blocks:

  common   constants, logging, stdlib imports
  util     clock, raw tty, SSE encoding
  daisy    the Daisy link (serial, telemetry, control frames, ping)
  midi     USB MIDI ports
  samples  sample library and transfers
  state    state.json
  ir       the remote
  hdmi     sound on the TV
  web      HTTP, SSE, JSON API, HTTPS
  bridge   puts them together
"""
