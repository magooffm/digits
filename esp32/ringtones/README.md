# Ringtone clips

Put your own recordings or songs here; they are built into the firmware and
selectable like the built-in melodies (`phone ringtones`, `phone ringtone a 12`).
Everything in this folder except this README is ignored by git: keep
commercial songs out of the repository.

- **Name:** `NN-Name.ext`, where `NN` (10 to 99) is the ringtone number that
  callers send, for example `12-Birthday song.mp3`. Keep the same number for
  the same file on every phone. Numbers 0 to 9 are the built-in melodies.
- **Format:** anything ffmpeg reads (mp3, wav, m4a, ogg, flac, ...). The build
  converts it to 24 kHz mono, trims silence at both ends, normalizes it to the
  ringer level and cuts it at 30 seconds with a short fade-out.
- **Space:** about 48 KB per second; the 8 MB partition holds roughly 170
  seconds in total. The build fails with a clear message if it does not fit.
- **Distribution:** the called phone plays the clip, so flash every phone
  from the same folder. A phone without the requested clip rings with the
  standard ring instead.

The Pion helper (`tools/interop`) plays the same files from this folder.
