# PS1 example songs

`keynote.fur` ("One More Thing") and `forty-one-cycles.fur` (the Forty-One Cycles demo tune), converted from the original scores for the Sony PlayStation chip.
`raised-sixth.fur` is a waltz written for the chip: D Dorian with a trio in B-flat Lydian, 3/4 at 144 BPM, hemiola cadences. Its score is the generator `src/raised-sixth.py` (it writes `src/raised-sixth.mid`), on the Forty-One Cycles bank: `score2ps1.py src/raised-sixth.mid src/forty-one-cycles.vab <dir> "Raised Sixth" --names kitb --bank-order src/forty-one-cycles.mid --beats 3`, then `ps1_examples <dir> raised-sixth.fur --reverb 5`.
Samples: `forty-one-cycles` uses FluidR3_GM (Frank Wen, MIT licence); `keynote` uses TimGM6mb (Tim Brechbill, GPL-2).
`src/` holds the scores (MIDI) and sample banks (VAB) they were converted from, `tools/` the converter.
The samples are SPU ADPCM, the bank's blocks byte for byte (loop flags included), uploaded to SPU RAM as they are.
Both use the Hall reverb preset (system setting), as the PS1 player does; instruments whose score part has a reverb send (CC91) have reverb on.
