# Example files

Real, valid files of different kinds for `demo.sh` (step 2) and for trying `encclient` by hand. They were generated for this project; nothing here is private.

| File | Kind | Size |
|---|---|---|
| `hello.txt` | short text note | 397 B |
| `config.json` | structured settings | 455 B |
| `report.pdf` | one-page PDF document | 1.5 KB |
| `qnx-pi.png` | 480 × 270 image | 51 KB |
| `tone.wav` | 2-second audio clip | 86 KB |
| `sensor-log.csv` | an hour of simulated sensor readings | 182 KB |

Any other file works too: copy it into this folder and run `./demo.sh 2`, or

```
./encclient -e examples/yourfile.ext
./encclient -d yourfile.ext.enc -o yourfile-copy.ext
cmp examples/yourfile.ext yourfile-copy.ext && echo identical
```

`demo.sh` also makes a 12 MB `big.bin` of random bytes in `demo-out/` (step 3), so the repository stays small.
