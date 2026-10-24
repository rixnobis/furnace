# PS1 example songs

`keynote.fur` ("One More Thing") and `forty-one-cycles.fur` (the Forty-One Cycles demo tune), converted from the original scores for the Sony PlayStation chip.
Samples: `forty-one-cycles` uses FluidR3_GM (Frank Wen, MIT licence); `keynote` uses TimGM6mb (Tim Brechbill, GPL-2).
`src/` holds the scores (MIDI) and sample banks (VAB) they were converted from, `tools/` the converter.
The samples are SPU ADPCM, the bank's blocks byte for byte (loop flags included), uploaded to SPU RAM as they are.
Both use the Hall reverb preset (system setting), as the PS1 player does; instruments whose score part has a reverb send (CC91) have reverb on.
