# PSPSRS

PSPSRS aims to create a spaced repetition program for the PSP, using PSPSDK and its development environment.

## Building

Install and configure [PSPSDK](https://pspdev.github.io/) and its development environment, including the `psp-cmake` command. From the repository root, run:

```bash
mkdir build && cd build
psp-cmake ..
make
```

This configures and builds the project. The resulting `eboot.pbp` is generated in the build directory.

The program requires a flascard deck, in the format of a .tsv file, with two tables (front / back). It can load any deck that has less than 2000 cards.

## Cards and limitations

The number of cards per deck is 2000, with each card limited up to 128 bytes for the front, 256 for the back, wich is enough to host a small vocabulary. Any card beyond the said number is ignored by the code, so there is no reason add more than that. 

## Saving progress

The progress on the deck is stored in a file created and updated as new cards are loaded. 
load deck -> load cards: as the cards are loaded, a new file is created, based on the deck.tsv file. The name deck.tsv -> deck.srs is inherited by this new file. New cards's data is than added or updated an entry in the .srs file. The file stores the prgress on the deck and can be shared alongside the deck.tsv to preserve or share the progress. 

