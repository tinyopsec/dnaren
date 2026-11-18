/* dnaren - minimal DNA renderer (suckless style) */
/* configuration is done here at compile time */

#ifndef DNAREN_H
#define DNAREN_H

#define VERSION "0.1"
#define TITLE "dnaren"

#define WIDTH 800
#define HEIGHT 600

/* helix geometry (B-DNA: 10 bp per turn, pitch is 1.7 x diameter) */
#define HELIX_RADIUS 1.5f
#define HELIX_PITCH 5.1f        /* height of one turn */
#define HELIX_TURNS 3.5f
#define STEPS_PER_TURN 60
#define BASEPAIR_EVERY 6        /* STEPS_PER_TURN / 10 bp per turn */
#define GROOVE_ANGLE 140.0f     /* degrees between strands, < 180 gives minor/major groove */

/* backbone and base pairs */
#define BACKBONE_RADIUS 0.22f
#define BASE_RADIUS 0.12f
#define BASE_GAP 0.20f          /* open part of a rung, hydrogen bonds are drawn here */
#define BOND_SPACING 0.10f
#define ROD_SLICES 10
#define SEED 7u                 /* base sequence seed */

/* sphere detail (nucleotide beads) */
#define SPHERE_SLICES 16
#define SPHERE_STACKS 12
#define SPHERE_RADIUS 0.34f

/* speed */
#define SPEED_DEFAULT 1.0f
#define SPEED_STEP 0.1f
#define SPEED_MIN 0.0f
#define SPEED_MAX 5.0f

/* camera */
#define CAM_DISTANCE_DEFAULT 26.0f
#define CAM_DISTANCE_STEP 1.0f
#define CAM_DISTANCE_MIN 5.0f
#define CAM_DISTANCE_MAX 60.0f
#define CAM_ROTATE_STEP 5.0f

/* mouse */
#define MOUSE_SENS 0.5f

/* rendering */
#define MSAA_SAMPLES 4
#define FRAME_DELAY_MS 16

#endif