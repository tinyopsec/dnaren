# dnaren

DNA renderer for X11/OpenGL, plus a terminal version that needs no display server.

Renders a B-DNA double helix: 10 base pairs per turn, major and minor groove, sugar-phosphate backbones, nucleotide beads. Base pairs are colored by type (A-T, G-C) and joined by hydrogen bonds, two or three. Lit scene with MSAA. The window title shows speed, pause state, and palette number.

## Screenshot

![dnaren](https://raw.githubusercontent.com/tinyopsec/assets/main/dnaren/dnaren.png)

## Build

    make            # builds dnaren and dnaren-cli
    make debug      # debug build (dnaren_dbg)
    make debug-cli  # debug build (dnaren-cli_dbg)

## Run

    ./dnaren        # X11/OpenGL renderer
    ./dnaren -v     # print version
    ./dnaren-cli    # terminal renderer, ANSI, no X11 needed

## Controls

- ESC or q: quit
- Space: pause
- + / - (main and keypad): speed up / down
- 0 (main and keypad): reset speed
- Arrow keys or WASD: rotate camera
- z / x: zoom in / out
- r: reset camera
- c: cycle color scheme
- f or F11: toggle fullscreen
- Mouse drag: rotate camera (auto-rotation pauses while the button is held)
- Mouse wheel: zoom

Geometry, base sequence seed, sizes, MSAA samples, frame delay, mouse
sensitivity, and camera step live in dnaren.h.

## Install

    make install
    make uninstall

## Dependencies

- X11, OpenGL, GLU, libm (dnaren)
- libc, libm (dnaren-cli)