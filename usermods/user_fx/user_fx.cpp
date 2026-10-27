#include "wled.h"

// for information how FX metadata strings work see https://kno.wled.ge/interfaces/json-api/#effect-metadata

// paletteBlend: 0 - wrap when moving, 1 - always wrap, 2 - never wrap, 3 - none (undefined)
#define PALETTE_SOLID_WRAP   (paletteBlend == 1 || paletteBlend == 3)

#define indexToVStrip(index, stripNr) ((index) | (int((stripNr)+1)<<16))

// static effect, used if an effect fails to initialize
static void mode_static(void) {
  SEGMENT.fill(SEGCOLOR(0));
}

#define FX_FALLBACK_STATIC { mode_static(); return; }

// If you define configuration options in your class and need to reference them in your effect function, add them here.
// If you only need to use them in your class you can define them as class members instead.
// bool myConfigValue = false;

/////////////////////////
//  User FX functions  //
/////////////////////////

// Diffusion Fire: fire effect intended for 2D setups smaller than 16x16
static void mode_diffusionfire(void) {
  if (!strip.isMatrix || !SEGMENT.is2D())
    FX_FALLBACK_STATIC;  // not a 2D set-up

  const int cols = SEG_W;
  const int rows = SEG_H;
  const auto XY = [&](int x, int y) { return x + y * cols; };

  const uint8_t refresh_hz = map(SEGMENT.speed, 0, 255, 20, 80);
  const unsigned refresh_ms = 1000 / refresh_hz;
  const int16_t diffusion = map(SEGMENT.custom1, 0, 255, 0, 100);
  const uint8_t spark_rate = SEGMENT.intensity;
  const uint8_t turbulence = SEGMENT.custom2;

unsigned dataSize = cols * rows;  // SEGLEN (virtual length) is equivalent to vWidth()*vHeight() for 2D
  if (!SEGENV.allocateData(dataSize))
    FX_FALLBACK_STATIC;  // allocation failed

  if (SEGENV.call == 0) {
    SEGMENT.fill(BLACK);
    SEGENV.step = 0;
  }

  if ((strip.now - SEGENV.step) >= refresh_ms) {
    // Keep for ≤~1 KiB; otherwise consider heap or reuse SEGENV.data as scratch.
    uint8_t tmp_row[cols];
    SEGENV.step = strip.now;
    // scroll up
    for (unsigned y = 1; y < rows; y++)
      for (unsigned x = 0; x < cols; x++) {
        unsigned src = XY(x, y);
        unsigned dst = XY(x, y - 1);
        SEGENV.data[dst] = SEGENV.data[src];
      }

    if (hw_random8() > turbulence) {
      // create new sparks at bottom row
      for (unsigned x = 0; x < cols; x++) {
        uint8_t p = hw_random8();
        if (p < spark_rate) {
          unsigned dst = XY(x, rows - 1);
          SEGENV.data[dst] = 255;
        }
      }
    }

    // diffuse
    for (unsigned y = 0; y < rows; y++) {
      for (unsigned x = 0; x < cols; x++) {
        unsigned v = SEGENV.data[XY(x, y)];
        if (x > 0) {
          v += SEGENV.data[XY(x - 1, y)];
        }
        if (x < (cols - 1)) {
          v += SEGENV.data[XY(x + 1, y)];
        }
        tmp_row[x] = min(255, (int)(v * 100 / (300 + diffusion)));
      }

      for (unsigned x = 0; x < cols; x++) {
        SEGENV.data[XY(x, y)] = tmp_row[x];
        if (SEGMENT.check1) {
          uint32_t color = SEGMENT.color_from_palette(tmp_row[x], true, false, 0);
          SEGMENT.setPixelColorXY(x, y, color);
        } else {
          uint32_t base = SEGCOLOR(0);
          SEGMENT.setPixelColorXY(x, y, color_fade(base, tmp_row[x]));
        }
      }
    }
  }
}
static const char _data_FX_MODE_DIFFUSIONFIRE[] PROGMEM = "Diffusion Fire@!,Spark rate,Diffusion Speed,Turbulence,,Use palette;;Color;;2;pal=35";


/*
 * Spinning Wheel effect - LED animates around 1D strip (or each column in a 2D matrix), slows down and stops at random position
 *  Created by Bob Loeffler and claude.ai
 *  First slider (Spin speed) is for the speed of the moving/spinning LED (random number within a narrow speed range).
 *     If value is 0, a random speed will be selected from the full range of values.
 *  Second slider (Spin slowdown start time) is for how long before the slowdown phase starts (random number within a narrow time range).
 *     If value is 0, a random time will be selected from the full range of values.
 *  Third slider (Spinner size) is for the number of pixels that make up the spinner.
 *  Fourth slider (Spin delay) is for how long it takes for the LED to start spinning again after the previous spin.
 *  The first checkbox allows the spinner to spin. If it's enabled, the spinner will do its thing. If it's not enabled, it will wait for the user to enable
 *     it either by clicking the checkbox or by pressing a physical button (e.g. using a playlist to run a couple presets that have JSON API codes).
 *  The second checkbox sets "color per block" mode. Enabled means that each spinner block will be the same color no matter what its LED position is.
 *  The third checkbox enables synchronized restart (all spinners restart together instead of individually).
 *  aux0 stores the settings checksum to detect changes
 *  aux1 stores the color scale for performance
 */

static void mode_spinning_wheel(void) {
  if (SEGLEN < 1) FX_FALLBACK_STATIC;

  unsigned strips = SEGMENT.nrOfVStrips();
  if (strips == 0) FX_FALLBACK_STATIC;

  constexpr unsigned stateVarsPerStrip = 8;
  unsigned dataSize = sizeof(uint32_t) * stateVarsPerStrip;
  if (!SEGENV.allocateData(dataSize * strips)) FX_FALLBACK_STATIC;
  uint32_t* state = reinterpret_cast<uint32_t*>(SEGENV.data);
  // state[0] = current position (fixed point: upper 16 bits = position, lower 16 bits = fraction)
  // state[1] = velocity (fixed point: pixels per frame * 65536)
  // state[2] = phase (0=fast spin, 1=slowing, 2=wobble, 3=stopped)
  // state[3] = stop time (when phase 3 was entered)
  // state[4] = wobble step (0=at stop pos, 1=moved back, 2=returned to stop)
  // state[5] = slowdown start time (when to transition from phase 0 to phase 1)
  // state[6] = wobble timing (for 200ms / 400ms / 300ms delays)
  // state[7] = store the stop position per strip

  // state[] index values for easier readability
  constexpr unsigned CUR_POS_IDX       = 0;  // state[0]
  constexpr unsigned VELOCITY_IDX      = 1;
  constexpr unsigned PHASE_IDX         = 2;
  constexpr unsigned STOP_TIME_IDX     = 3;
  constexpr unsigned WOBBLE_STEP_IDX   = 4;
  constexpr unsigned SLOWDOWN_TIME_IDX = 5;
  constexpr unsigned WOBBLE_TIME_IDX   = 6;
  constexpr unsigned STOP_POS_IDX      = 7;

  SEGMENT.fill(SEGCOLOR(1));

  // Handle random seeding globally (outside the virtual strip)
  if (SEGENV.call == 0) {
    SEGENV.aux1 = (255 << 8) / SEGLEN; // Cache the color scaling
  }

  // Check if settings changed (do this once, not per virtual strip)
  uint32_t settingssum = SEGMENT.speed + SEGMENT.intensity + SEGMENT.custom1 + SEGMENT.custom3 + SEGMENT.check1 + SEGMENT.check3;
  bool settingsChanged = (SEGENV.aux0 != settingssum);
  if (settingsChanged) {
    SEGENV.aux0 = settingssum;
  }

  // Check if all spinners are stopped and ready to restart (for synchronized restart)
  bool allReadyToRestart = true;
  if (SEGMENT.check3) {
    uint8_t spinnerSize = map(SEGMENT.custom1, 0, 255, 1, 10);
    uint16_t spin_delay = map(SEGMENT.custom3, 0, 31, 2000, 15000);
    uint32_t now = strip.now;

    for (unsigned stripNr = 0; stripNr < strips; stripNr += spinnerSize) {
      uint32_t* stripState = &state[stripNr * stateVarsPerStrip];
      // Check if this spinner is stopped AND has waited its delay
      if (stripState[PHASE_IDX] != 3 || stripState[STOP_TIME_IDX] == 0) {
        allReadyToRestart = false;
        break;
      }
      // Check if delay has elapsed
      if ((now - stripState[STOP_TIME_IDX]) < spin_delay) {
        allReadyToRestart = false;
        break;
      }
    }
  }
 
  struct virtualStrip {
    static void runStrip(uint16_t stripNr, uint32_t* state, bool settingsChanged, bool allReadyToRestart, unsigned strips) {
      uint8_t phase = state[PHASE_IDX];
      uint32_t now = strip.now;

      // Check for restart conditions
      bool needsReset = false;
      if (SEGENV.call == 0) {
        needsReset = true;
      } else if (settingsChanged && SEGMENT.check1) {
        needsReset = true;
      } else if (phase == 3 && state[STOP_TIME_IDX] != 0) {
          // If synchronized restart is enabled, only restart when all strips are ready
          if (SEGMENT.check3) {
            if (allReadyToRestart) {
              needsReset = true;
            }
          } else {
            // Normal mode: restart after individual strip delay
            uint16_t spin_delay = map(SEGMENT.custom3, 0, 31, 2000, 15000);
            if ((now - state[STOP_TIME_IDX]) >= spin_delay) {
              needsReset = true;
            }
          }
      }

      // Initialize or restart
      if (needsReset && SEGMENT.check1) {   // spin the spinner(s) only if the "Spin me!" checkbox is enabled
        state[CUR_POS_IDX] = 0;

        // Set velocity
        uint16_t speed = map(SEGMENT.speed, 0, 255, 300, 800);
        if (speed == 300) {  // random speed (user selected 0 on speed slider)
          state[VELOCITY_IDX] = hw_random16(200, 900) * 655;   // fixed-point velocity scaling (approx. 65536/100) 
        } else {
          state[VELOCITY_IDX] = hw_random16(speed - 100, speed + 100) * 655;
        }

        // Set slowdown start time
        uint16_t slowdown = map(SEGMENT.intensity, 0, 255, 3000, 5000);
        if (slowdown == 3000) {  // random slowdown start time (user selected 0 on intensity slider)
          state[SLOWDOWN_TIME_IDX] = now + hw_random16(2000, 6000);
        } else {
          state[SLOWDOWN_TIME_IDX] = now + hw_random16(slowdown - 1000, slowdown + 1000);
        }

        state[PHASE_IDX] = 0;
        state[STOP_TIME_IDX] = 0;
        state[WOBBLE_STEP_IDX] = 0;
        state[WOBBLE_TIME_IDX] = 0;
        state[STOP_POS_IDX] = 0; // Initialize stop position
        phase = 0;
      }

      uint32_t pos_fixed = state[CUR_POS_IDX];
      uint32_t velocity = state[VELOCITY_IDX];
      
      // Phase management
      if (phase == 0) {
        // Fast spinning phase
        if ((int32_t)(now - state[SLOWDOWN_TIME_IDX]) >= 0) {
          phase = 1;
          state[PHASE_IDX] = 1;
        }
      } else if (phase == 1) {
        // Slowing phase - apply deceleration
        uint32_t decel = velocity / 80;
        if (decel < 100) decel = 100;

        velocity = (velocity > decel) ? velocity - decel : 0;
        state[VELOCITY_IDX] = velocity;

        // Check if stopped
        if (velocity < 2000) {
          velocity = 0;
          state[VELOCITY_IDX] = 0;
          phase = 2;
          state[PHASE_IDX] = 2;
          state[WOBBLE_STEP_IDX] = 0;
          uint16_t stop_pos = (pos_fixed >> 16) % SEGLEN;
          state[STOP_POS_IDX] = stop_pos;
          state[WOBBLE_TIME_IDX] = now;
        }
      } else if (phase == 2) {
        // Wobble phase (moves the LED back one and then forward one)
        uint32_t wobble_step = state[WOBBLE_STEP_IDX];
        uint16_t stop_pos = state[STOP_POS_IDX];
        uint32_t elapsed = now - state[WOBBLE_TIME_IDX];

        if (wobble_step == 0 && elapsed >= 200) {
          // Move back one LED from stop position
          uint16_t back_pos = (stop_pos == 0) ? SEGLEN - 1 : stop_pos - 1;
          pos_fixed = ((uint32_t)back_pos) << 16;
          state[CUR_POS_IDX] = pos_fixed;
          state[WOBBLE_STEP_IDX] = 1;
          state[WOBBLE_TIME_IDX] = now;
        } else if (wobble_step == 1 && elapsed >= 400) {
          // Move forward to the stop position
          pos_fixed = ((uint32_t)stop_pos) << 16;
          state[CUR_POS_IDX] = pos_fixed;
          state[WOBBLE_STEP_IDX] = 2;
          state[WOBBLE_TIME_IDX] = now;
        } else if (wobble_step == 2 && elapsed >= 300) {
          // Wobble complete, enter stopped phase
          phase = 3;
          state[PHASE_IDX] = 3;
          state[STOP_TIME_IDX] = now;
        }
      }

      // Update position (phases 0 and 1 only)
      if (phase == 0 || phase == 1) {
        pos_fixed += velocity;
        state[CUR_POS_IDX] = pos_fixed;
      }

      // Draw LED for all phases
      uint16_t pos = (pos_fixed >> 16) % SEGLEN;

      uint8_t spinnerSize = map(SEGMENT.custom1, 0, 255, 1, 10);

      // Calculate color once per spinner block (based on strip number, not position)
      uint8_t hue;
      if (SEGMENT.check2) {
        // Each spinner block gets its own color based on strip number
        uint16_t numSpinners = max(1U, (strips + spinnerSize - 1) / spinnerSize);
        hue = (uint32_t)(255) * (stripNr / spinnerSize) / numSpinners;
      } else {
        // Color changes with position
        hue = (SEGENV.aux1 * pos) >> 8;
      }

      uint32_t color = ColorFromPalette(SEGPALETTE, hue, 255, LINEARBLEND);

      // Draw the spinner with configurable size (1-10 LEDs)
      for (int8_t x = 0; x < spinnerSize; x++) {
        for (uint8_t y = 0; y < spinnerSize; y++) {
          uint16_t drawPos = (pos + y) % SEGLEN;
          int16_t drawStrip = stripNr + x;

          // Wrap horizontally if needed, or skip if out of bounds
          if (drawStrip >= 0 && drawStrip < strips) {
            SEGMENT.setPixelColor(indexToVStrip(drawPos, drawStrip), color);
          }
        }
      }
    }
  };

  for (unsigned stripNr = 0; stripNr < strips; stripNr++) {
    // Only run on strips that are multiples of spinnerSize to avoid overlap
    uint8_t spinnerSize = map(SEGMENT.custom1, 0, 255, 1, 10);
    if (stripNr % spinnerSize == 0) {
      virtualStrip::runStrip(stripNr, &state[stripNr * stateVarsPerStrip], settingsChanged, allReadyToRestart, strips);
    }
  }
}
static const char _data_FX_MODE_SPINNINGWHEEL[] PROGMEM = "Spinning Wheel@Speed (0=random),Slowdown (0=random),Spinner size,,Spin delay,Spin me!,Color per block,Sync restart;!,!;!;;m12=1,c1=1,c3=8,o1=1,o3=1";


/*
/  Lava Lamp 2D effect
*   Uses particles to simulate rising blobs of "lava" or wax
*   Particles slowly rise, merge to create organic flowing shapes, and then fall to the bottom to start again
*   Created by Bob Loeffler using claude.ai
*   The first slider sets the number of active blobs
*   The second slider sets the size range of the blobs
*   The third slider sets the damping value for horizontal blob movement
*   The Attract checkbox sets the attraction of blobs (checked will make the blobs attract other close blobs horizontally)
*   The Keep Color Ratio checkbox sets whether we preserve the color ratio when displaying pixels that are in 2 or more overlapping blobs
*   aux0 keeps track of the blob size value
*   aux1 keeps track of the number of blobs
*/

typedef struct LavaParticle {
  float    x, y;         // Position
  float    vx, vy;       // Velocity
  float    size;         // Blob size
  uint8_t  hue;          // Color
  bool     active;       // will not be displayed if false
  uint16_t delayTop;     // number of frames to wait at top before falling again
  bool     idleTop;      // sitting idle at the top
  uint16_t delayBottom;  // number of frames to wait at bottom before rising again
  bool     idleBottom;   // sitting idle at the bottom
} LavaParticle;

static void mode_2D_lavalamp(void) {
  if (!strip.isMatrix || !SEGMENT.is2D()) FX_FALLBACK_STATIC; // not a 2D set-up

  const uint16_t cols = SEG_W;
  const uint16_t rows = SEG_H;
  constexpr float MAX_BLOB_RADIUS = 20.0f;  // cap to prevent frame rate drops on large matrices
  constexpr size_t MAX_LAVA_PARTICLES = 34;  // increasing this value could cause slowness for large matrices
  constexpr size_t MAX_TOP_FPS_DELAY = 900;  // max delay when particles are at the top
  constexpr size_t MAX_BOTTOM_FPS_DELAY = 1200;  // max delay when particles are at the bottom

  // Allocate per-segment storage
  if (!SEGENV.allocateData(sizeof(LavaParticle) * MAX_LAVA_PARTICLES)) FX_FALLBACK_STATIC;
  LavaParticle* lavaParticles = reinterpret_cast<LavaParticle*>(SEGENV.data);

  // Initialize particles on first call
  if (SEGENV.call == 0) {
    for (int i = 0; i < MAX_LAVA_PARTICLES; i++) {
      lavaParticles[i].active = false;
    }
  }

  // Track particle size and particle count slider changes, re-initialize if either changes
  uint8_t currentNumParticles = (SEGMENT.intensity >> 3) + 3;
  uint8_t currentSize = SEGMENT.custom1;
  if (currentNumParticles > MAX_LAVA_PARTICLES) currentNumParticles = MAX_LAVA_PARTICLES;
  bool needsReinit = (currentSize != SEGENV.aux0) || (currentNumParticles != SEGENV.aux1);

  if (needsReinit) {
    for (int i = 0; i < MAX_LAVA_PARTICLES; i++) {
      lavaParticles[i].active = false;
    }
    SEGENV.aux0 = currentSize;
    SEGENV.aux1 = currentNumParticles;
  }

  uint8_t size = currentSize;
  uint8_t numParticles = currentNumParticles;

  // blob size based on matrix width
  const float minSize = cols * 0.15f; // Minimum 15% of width
  const float maxSize = cols * 0.4f;  // Maximum 40% of width
  float sizeRange = (maxSize - minSize) * (size / 255.0f);
  int rangeInt = max(1, (int)(sizeRange));

  // calculate the spawning area for the particles
  const float spawnXStart = cols * 0.20f;
  const float spawnXWidth = cols * 0.60f;
  int spawnX = max(1, (int)(spawnXWidth));

  bool preserveColorRatio = SEGMENT.check3;

  // Spawn new particles at the bottom near the center
  for (int i = 0; i < MAX_LAVA_PARTICLES; i++) {
    if (!lavaParticles[i].active && hw_random8() < 32) { // spawn when slot available
      // Spawn in the middle 60% of the matrix width
      lavaParticles[i].x = spawnXStart + (float)hw_random16(spawnX);
      lavaParticles[i].y = rows - 1;
      lavaParticles[i].vx = (hw_random16(7) - 3) / 250.0f;
      lavaParticles[i].vy = -(hw_random16(20) + 10) / 100.0f * 0.3f;

      lavaParticles[i].size = minSize + (float)hw_random16(rangeInt);
      if (lavaParticles[i].size > MAX_BLOB_RADIUS) lavaParticles[i].size = MAX_BLOB_RADIUS;

      lavaParticles[i].hue = hw_random8();
      lavaParticles[i].active = true;

      // Set random delays when particles are at top and bottom
      lavaParticles[i].delayTop = hw_random16(MAX_TOP_FPS_DELAY);
      lavaParticles[i].delayBottom = hw_random16(MAX_BOTTOM_FPS_DELAY);
      lavaParticles[i].idleBottom = true;
      break;
    }
  }

  // Fade background slightly for trailing effect
  SEGMENT.fadeToBlackBy(40);

  // Update and draw particles
  int activeCount = 0;
  unsigned long currentMillis = strip.now;
  for (int i = 0; i < MAX_LAVA_PARTICLES; i++) {
    if (!lavaParticles[i].active) continue;
    activeCount++;

    // Keep particle count on target by deactivating excess particles
    if (activeCount > numParticles) {
      lavaParticles[i].active = false;
      activeCount--;
      continue;
    }

    LavaParticle *p = &lavaParticles[i];

    // Physics update
    p->x += p->vx;
    p->y += p->vy;

    // Optional particle/blob attraction
    if (SEGMENT.check2) {
      for (int j = 0; j < MAX_LAVA_PARTICLES; j++) {
        if (i == j || !lavaParticles[j].active) continue;

        LavaParticle *other = &lavaParticles[j];

        // Skip attraction if moving in same vertical direction (both up or both down)
        if ((p->vy < 0 && other->vy < 0) || (p->vy > 0 && other->vy > 0)) continue;

        float dx = other->x - p->x;
        float dy = other->y - p->y;

        // Apply weak horizontal attraction only
        float attractRange = p->size + other->size;
        float distSq = dx*dx + dy*dy;
        float attractRangeSq = attractRange * attractRange;
        if (distSq > 0 && distSq < attractRangeSq) {
          float dist = sqrt(distSq); // Only compute sqrt when needed
          float force = (1.0f - (dist / attractRange)) * 0.0001f;
          p->vx += (dx / dist) * force;
        }
      }
    }

    // Horizontal oscillation (makes it more organic)
    float damping= map(SEGMENT.custom2, 0, 255, 97, 87) / 100.0f;
    p->vx += sin((currentMillis / 1000.0f + i) * 0.5f) * 0.002f; // Reduced oscillation
    p->vx *= damping; // damping for more or less horizontal drift

    // Bounce off sides (don't affect vertical velocity)
    if (p->x < 0) {
      p->x = 0;
      p->vx = abs(p->vx); // reverse horizontal
    }
    if (p->x >= cols) {
      p->x = cols - 1;
      p->vx = -abs(p->vx); // reverse horizontal
    }

    // Adjust rise/fall velocity depending on approx distance from heat source (at bottom)
    // In top 1/4th of rows...
    if (p->y < rows * .25f) {
      if (p->vy >= 0) {  // if going down, delay the particles so they won't go down immediately
        if (p->delayTop > 0 && p->idleTop) {
          p->vy = 0.0f;
          p->delayTop--;
          p->idleTop = true;
        } else {
          p->vy = 0.01f;
          p->delayTop = hw_random16(MAX_TOP_FPS_DELAY);
          p->idleTop = false;
        }
      } else if (p->vy <= 0) {  // if going up, slow down the rise rate
        p->vy = -0.03f;
      }
    }

    // In next 1/4th of rows...
    if (p->y <= rows * .50f && p->y >= rows * .25f) {
      if (p->vy > 0) {  // if going down, speed up the fall rate
        p->vy = 0.03f;
      } else if (p->vy <= 0) {  // if going up, speed up the rise rate a little more
        p->vy = -0.05f;
      }
    }

    // In next 1/4th of rows...
    if (p->y <= rows * .75f && p->y >= rows * .50f) {
      if (p->vy > 0) {  // if going down, speed up the fall rate a little more
        p->vy = 0.04f;
      } else if (p->vy <= 0) {  // if going up, speed up the rise rate
        p->vy = -0.03f;
      }
    }

    // In bottom 1/4th of rows...
    if (p->y > rows * .75f) {
      if (p->vy >= 0) {  // if going down, slow down the fall rate
        p->vy = 0.02f;
      } else if (p->vy <= 0) {  // if going up, delay the particles so they won't go up immediately
        if (p->delayBottom > 0 && p->idleBottom) {
          p->vy = 0.0f;
          p->delayBottom--;
          p->idleBottom = true;
        } else {
          p->vy = -0.01f;
          p->delayBottom = hw_random16(MAX_BOTTOM_FPS_DELAY);
          p->idleBottom = false;
        }
      }
    }

    // Boundary handling with reversal of direction
    // When reaching TOP (y=0 area), reverse to fall back down, but need to delay first
    if (p->y <= 0.5f * p->size) {
      p->y = 0.5f * p->size;
      if (p->vy < 0) {
        p->vy = 0.005f;  // set to a tiny positive value to start falling very slowly
        p->idleTop = true;
      }
    }

    // When reaching BOTTOM (y=rows-1 area), reverse to rise back up, but need to delay first
    if (p->y >= rows - 0.5f * p->size) {
      p->y = rows - 0.5f * p->size;
      if (p->vy > 0) {
        p->vy = -0.005f;  // set to a tiny negative value to start rising very slowly
        p->idleBottom = true;
      }
    }

    // Get color
    uint32_t color;
    color = SEGMENT.color_from_palette(p->hue, true, PALETTE_SOLID_WRAP, 0);

    // Extract RGB and apply life/opacity
    uint8_t w = (W(color) * 255) >> 8;
    uint8_t r = (R(color) * 255) >> 8;
    uint8_t g = (G(color) * 255) >> 8;
    uint8_t b = (B(color) * 255) >> 8;

    // Draw blob with sub-pixel accuracy using bilinear distribution
    float sizeSq = p->size * p->size;

    // Get fractional offsets of particle center
    float fracX = p->x - floorf(p->x);
    float fracY = p->y - floorf(p->y);
    int centerX = (int)floorf(p->x);
    int centerY = (int)floorf(p->y);

    for (int dy = -(int)p->size - 1; dy <= (int)p->size + 1; dy++) {
      for (int dx = -(int)p->size - 1; dx <= (int)p->size + 1; dx++) {
        int px = centerX + dx;
        int py = centerY + dy;

        if (px < 0 || px >= cols || py < 0 || py >= rows) continue;

        // Sub-pixel distance: measure from true float center to pixel center
        float subDx = dx - fracX;  // distance from true center to this pixel's center
        float subDy = dy - fracY;
        float distSq = subDx * subDx + subDy * subDy;

        if (distSq < sizeSq) {
          float intensity = 1.0f - (distSq / sizeSq);
          intensity = intensity * intensity; // smooth falloff

          uint8_t bw = (uint8_t)(w * intensity);
          uint8_t br = (uint8_t)(r * intensity);
          uint8_t bg = (uint8_t)(g * intensity);
          uint8_t bb = (uint8_t)(b * intensity);

          uint32_t existing = SEGMENT.getPixelColorXY(px, py);
          uint32_t newColor = RGBW32(br, bg, bb, bw);
          SEGMENT.setPixelColorXY(px, py, color_add(existing, newColor, preserveColorRatio ? true : false));
        }
      }
    }
  }
}
static const char _data_FX_MODE_2D_LAVALAMP[] PROGMEM = "Lava Lamp@,# of blobs,Blob size,H. Damping,,,Attract,Keep Color Ratio;;!;2;ix=64,c2=192,o2=1,o3=1,pal=47";


/*
/  Magma effect
*   2D magma/lava animation
*   Adapted from FireLamp_JeeUI implementation (https://github.com/DmytroKorniienko/FireLamp_JeeUI/tree/dev)
*   Original idea by SottNick, remastered by kostyamat
*   Adapted to WLED by Bob Loeffler and claude.ai
*   First slider (speed) is for the speed or flow rate of the moving magma.
*   Second slider (intensity) is for the height of the magma.
*   Third slider (lava bombs) is for the number of lava bombs (particles).  The max # is 1/2 the number of columns on the 2D matrix.
*   Fourth slider (gravity) is for how high the lava bombs will go.
*   The checkbox (check2) is for whether the lava bombs can be seen in the magma or behind it.
*/

// Draw the magma
static void drawMagma(const uint16_t width, const uint16_t height, float *ff_y, float *ff_z, uint8_t *shiftHue) {
  // Noise parameters - adjust these for different magma characteristics
  // deltaValue: higher = more detailed/turbulent magma
  // deltaHue: higher = taller magma structures
  constexpr uint8_t magmaDeltaValue = 12U;
  constexpr uint8_t magmaDeltaHue   = 10U;

  uint16_t ff_y_int = (uint16_t)*ff_y;
  uint16_t ff_z_int = (uint16_t)*ff_z;

  for (uint16_t i = 0; i < width; i++) {
    for (uint16_t j = 0; j < height; j++) {
      // Generate Perlin noise value (0-255)
      uint8_t noise = perlin8(i * magmaDeltaValue, (j + ff_y_int + hw_random8(2)) * magmaDeltaHue, ff_z_int);
      uint8_t paletteIndex = qsub8(noise, shiftHue[j]);  // Apply the vertical fade gradient
      CRGB col = SEGMENT.color_from_palette(paletteIndex, false, PALETTE_SOLID_WRAP, 0);  // Get color from palette
      SEGMENT.addPixelColorXY(i, height - 1 - j, col);  // magma rises from bottom of display
    }
  }
}

// Move and draw lava bombs (particles)
static void drawLavaBombs(const uint16_t width, const uint16_t height, float *particleData, float gravity, uint8_t particleCount) {
  for (uint16_t i = 0; i < particleCount; i++) {
    uint16_t idx = i * 4;

    particleData[idx + 3] -= gravity;
    particleData[idx + 0] += particleData[idx + 2];
    particleData[idx + 1] += particleData[idx + 3];

    float posX = particleData[idx + 0];
    float posY = particleData[idx + 1];

    if (posY > height + height / 4) {
      particleData[idx + 3] = -particleData[idx + 3] * 0.8f;
    }

    if (posY < (float)(height / 8) - 1.0f || posX < 0 || posX >= width) {
      particleData[idx + 0] = hw_random(0, width * 100) / 100.0f;
      particleData[idx + 1] = hw_random(0, height * 25) / 100.0f;
      particleData[idx + 2] = hw_random(-75, 75) / 100.0f;

      float baseVelocity = hw_random(60, 120) / 100.0f;
      if (hw_random8() < 50) {
        baseVelocity *= 1.6f;
      }
      particleData[idx + 3] = baseVelocity;
      continue;
    }

    int16_t xi = (int16_t)posX;
    int16_t yi = (int16_t)posY;

    if (xi >= 0 && xi < width && yi >= 0 && yi < height) {
      // Get a random color from the current palette
      uint8_t randomIndex = hw_random8(64, 128);
      CRGB pcolor = ColorFromPalette(SEGPALETTE, randomIndex, 255, LINEARBLEND);

      // Pre-calculate anti-aliasing weights
      float xf = posX - xi;
      float yf = posY - yi;
      float ix = 1.0f - xf;
      float iy = 1.0f - yf;

      uint8_t w0 = 255 * ix * iy;
      uint8_t w1 = 255 * xf * iy;
      uint8_t w2 = 255 * ix * yf;
      uint8_t w3 = 255 * xf * yf;

      int16_t yFlipped = height - 1 - yi;  // Flip Y coordinate

      SEGMENT.addPixelColorXY(xi, yFlipped, pcolor.scale8(w0));
      if (xi + 1 < width) 
        SEGMENT.addPixelColorXY(xi + 1, yFlipped, pcolor.scale8(w1));
      if (yFlipped - 1 >= 0)
        SEGMENT.addPixelColorXY(xi, yFlipped - 1, pcolor.scale8(w2));
      if (xi + 1 < width && yFlipped - 1 >= 0)
        SEGMENT.addPixelColorXY(xi + 1, yFlipped - 1, pcolor.scale8(w3));
    }
  }
}

static void mode_2D_magma(void) {
  if (!strip.isMatrix || !SEGMENT.is2D()) FX_FALLBACK_STATIC;  // not a 2D set-up
  const uint16_t width = SEG_W;
  const uint16_t height = SEG_H;
  const uint8_t MAGMA_MAX_PARTICLES = width / 2;
  if (MAGMA_MAX_PARTICLES < 2) FX_FALLBACK_STATIC;  // matrix too narrow for lava bombs
  constexpr size_t SETTINGS_SUM_BYTES = 4; // 4 bytes for settings sum

  // Allocate memory: particles (4 floats each) + 2 floats for noise counters + shiftHue cache + settingsSum
  const uint16_t dataSize = (MAGMA_MAX_PARTICLES * 4 + 2) * sizeof(float) + height * sizeof(uint8_t) + SETTINGS_SUM_BYTES;
  if (!SEGENV.allocateData(dataSize)) FX_FALLBACK_STATIC;  // allocation failed

  float* particleData = reinterpret_cast<float*>(SEGENV.data);
  float* ff_y = &particleData[MAGMA_MAX_PARTICLES * 4];
  float* ff_z = &particleData[MAGMA_MAX_PARTICLES * 4 + 1];
  uint32_t* settingsSumPtr = reinterpret_cast<uint32_t*>(&particleData[MAGMA_MAX_PARTICLES * 4 + 2]);
  uint8_t* shiftHue = reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(settingsSumPtr) + SETTINGS_SUM_BYTES);

  // Check if settings changed
  uint32_t settingsKey = (uint32_t)SEGMENT.speed | ((uint32_t)SEGMENT.intensity << 8) |
      ((uint32_t)SEGMENT.custom1 << 16) | ((uint32_t)SEGMENT.custom2 << 24);
  bool settingsChanged = (*settingsSumPtr != settingsKey);

  if (SEGENV.call == 0 || settingsChanged) {
    // Intensity slider controls magma height
    uint16_t intensity = SEGMENT.intensity;
    uint16_t fadeRange = map(intensity, 0, 255, height / 3, height);

    // shiftHue controls the vertical color gradient (magma fades out toward top)
    for (uint16_t j = 0; j < height; j++) {
      if (j < fadeRange) {
        // prevent division issues and ensure smooth gradient
        if (fadeRange > 1) {
          shiftHue[j] = (uint8_t)(j * 255 / (fadeRange - 1));
        } else {
          shiftHue[j] = 0;  // Single row magma = no fade
        }
      } else {
        shiftHue[j] = 255;
      }
    }

    // Initialize all particles
    for (uint16_t i = 0; i < MAGMA_MAX_PARTICLES; i++) {
      uint16_t idx = i * 4;
      particleData[idx + 0] = hw_random(0, width * 100) / 100.0f;
      particleData[idx + 1] = hw_random(0, height * 25) / 100.0f;
      particleData[idx + 2] = hw_random(-75, 75) / 100.0f;

      float baseVelocity = hw_random(60, 120) / 100.0f;
      if (hw_random8() < 50) {
        baseVelocity *= 1.6f;
      }
      particleData[idx + 3] = baseVelocity;
    }
    *ff_y = 0.0f;
    *ff_z = 0.0f;
    *settingsSumPtr = settingsKey;
  }

  if (!shiftHue) FX_FALLBACK_STATIC;   // safety check

  // Speed control
  float speedfactor = SEGMENT.speed / 255.0f;
  speedfactor = speedfactor * speedfactor * 1.5f;
  if (speedfactor < 0.001f) speedfactor = 0.001f;

  // Gravity control
  float gravity = map(SEGMENT.custom2, 0, 255, 5, 20) / 100.0f;

  // Number of particles (lava bombs)
  uint8_t particleCount = map(SEGMENT.custom1, 0, 255, 0, MAGMA_MAX_PARTICLES);
  particleCount = constrain(particleCount, 0, MAGMA_MAX_PARTICLES);

  // Draw lava bombs in front of magma (or behind it)
  if (SEGMENT.check2) {
    drawMagma(width, height, ff_y, ff_z, shiftHue);
    SEGMENT.fadeToBlackBy(70);    // Dim the entire display to create trailing effect
    if (particleCount > 0) drawLavaBombs(width, height, particleData, gravity, particleCount);
  }
  else {
    if (particleCount > 0) drawLavaBombs(width, height, particleData, gravity, particleCount);
    SEGMENT.fadeToBlackBy(70);    // Dim the entire display to create trailing effect
    drawMagma(width, height, ff_y, ff_z, shiftHue);
  }

  // noise counters based on speed slider
  *ff_y += speedfactor * 2.0f;
  *ff_z += speedfactor;

  SEGENV.step++;
}
static const char _data_FX_MODE_2D_MAGMA[] PROGMEM = "Magma@Flow rate,Magma height,Lava bombs,Gravity,,,Bombs in front;;!;2;ix=192,c2=32,o2=1,pal=35";


/*
/  Ants (created by making modifications to the Rolling Balls code) - Bob Loeffler 2025
*   First slider is for the ants' speed.
*   Second slider is for the # of ants.
*   Third slider is for the Ants' size.
*   Fourth slider (custom2) is for blurring the LEDs in the segment.
*   Checkbox1 is for Gathering food (enabled if you want the ants to gather food, disabled if they are just walking).
*     We will switch directions when they get to the beginning or end of the segment when gathering food.
*     When gathering food, the Pass By option will automatically be enabled so they can drop off their food easier (and look for more food).
*   Checkbox2 is for Smear mode (enabled is smear pixel colors, disabled is no smearing)
*   Checkbox3 is for whether the ants will bump into each other (disabled) or just pass by each other (enabled)
*/

// Ant structure representing each ant's state
struct Ant {
  unsigned long lastBumpUpdate;  // the last time the ant bumped into another ant
  bool hasFood;
  float velocity;
  float position;  // (0.0 to 1.0 range)
};

constexpr unsigned MAX_ANTS = 32;
constexpr float MIN_COLLISION_TIME_MS = 2.0f;
constexpr float VELOCITY_MIN = 2.0f;
constexpr float VELOCITY_MAX = 10.0f;
constexpr unsigned ANT_SIZE_MIN = 1;
constexpr unsigned ANT_SIZE_MAX = 20;

// Helper function to get food pixel color based on ant and background colors
static uint32_t getFoodColor(uint32_t antColor, uint32_t backgroundColor) {
  if (antColor == WHITE)
    return (backgroundColor == YELLOW) ? GRAY : YELLOW;
  return (backgroundColor == WHITE) ? YELLOW : WHITE;
}

// Helper function to handle ant boundary wrapping or bouncing
static void handleBoundary(Ant& ant, float& position, bool gatherFood, bool atStart, unsigned long currentTime) {
  if (gatherFood) {
    // Bounce mode: reverse direction and update food status
    position = atStart ? 0.0f : 1.0f;
    ant.velocity = -ant.velocity;
    ant.lastBumpUpdate = currentTime;
    ant.position = position;
    ant.hasFood = atStart;  // Has food when leaving start, drops it at end
  } else {
    // Wrap mode: teleport to opposite end
    position = atStart ? 1.0f : 0.0f;
    ant.lastBumpUpdate = currentTime;
    ant.position = position;
  }
}

// Helper function to calculate ant color
static uint32_t getAntColor(int antIndex, int numAnts, bool usePalette) {
  if (usePalette)
    return SEGMENT.color_from_palette(antIndex * 255 / numAnts, false, (paletteBlend == 1 || paletteBlend == 3), 255);
  // Alternate between two colors for default palette
  return (antIndex % 3 == 1) ? SEGCOLOR(0) : SEGCOLOR(2);
}

// Helper function to render a single ant pixel with food handling
static void renderAntPixel(int pixelIndex, int pixelOffset, int antSize, const Ant& ant, uint32_t antColor, uint32_t backgroundColor, bool gatherFood) {
  bool isMovingBackward = (ant.velocity < 0);
  bool isFoodPixel = gatherFood && ant.hasFood && ((isMovingBackward && pixelOffset == 0) || (!isMovingBackward && pixelOffset == antSize - 1));
  if (isFoodPixel) {
    SEGMENT.setPixelColor(pixelIndex, getFoodColor(antColor, backgroundColor));
  } else {
    SEGMENT.setPixelColor(pixelIndex, antColor);
  }
}

static void mode_ants(void) {
  if (SEGLEN <= 1) FX_FALLBACK_STATIC;

  // Allocate memory for ant data
  uint32_t backgroundColor = SEGCOLOR(1);
  unsigned dataSize = sizeof(Ant) * MAX_ANTS;
  if (!SEGENV.allocateData(dataSize)) FX_FALLBACK_STATIC;  // Allocation failed

  Ant* ants = reinterpret_cast<Ant*>(SEGENV.data);

  // Extract configuration from segment settings
  unsigned numAnts = min(1 + (SEGLEN * SEGMENT.intensity >> 12), MAX_ANTS);
  bool gatherFood = SEGMENT.check1;
  bool SmearMode = SEGMENT.check2;
  bool passBy = SEGMENT.check3 || gatherFood;  // global no‑collision when gathering food is enabled
  unsigned antSize = map(SEGMENT.custom1, 0, 255, ANT_SIZE_MIN, ANT_SIZE_MAX) + (gatherFood ? 1 : 0);

  // Initialize ants on first call
  if (SEGENV.call == 0) {
    int confusedAntIndex = hw_random(0, numAnts);   // the first random ant to go backwards

    for (int i = 0; i < MAX_ANTS; i++) {
      ants[i].lastBumpUpdate = strip.now;

      // Random velocity
      float velocity = VELOCITY_MIN + (VELOCITY_MAX - VELOCITY_MIN) * hw_random16(1000, 5000) / 5000.0f;
      // One random ant moves in opposite direction
      ants[i].velocity = (i == confusedAntIndex) ? -velocity : velocity;
      // Random starting position (0.0 to 1.0)
      ants[i].position = hw_random16(0, 10000) / 10000.0f;
      // Ants don't have food yet
      ants[i].hasFood = false;
    }
  }

  // Calculate time conversion factor based on speed slider
  float timeConversionFactor = float(scale8(8, 255 - SEGMENT.speed) + 1) * 20000.0f;

  // Clear background if not in Smear mode
  if (!SmearMode) SEGMENT.fill(backgroundColor);

  // Update and render each ant
  for (int i = 0; i < numAnts; i++) {
    float timeSinceLastUpdate = float(int(strip.now - ants[i].lastBumpUpdate)) / timeConversionFactor;
    float newPosition = ants[i].position + ants[i].velocity * timeSinceLastUpdate;

    // Reset ants that wandered too far off-track (e.g., after intensity change)
    if (newPosition < -0.5f || newPosition > 1.5f) {
      newPosition = ants[i].position = hw_random16(0, 10000) / 10000.0f;
      ants[i].lastBumpUpdate = strip.now;
    }

    // Handle boundary conditions (bounce or wrap)
    if (newPosition <= 0.0f && ants[i].velocity < 0.0f) {
      handleBoundary(ants[i], newPosition, gatherFood, true, strip.now);
    } else if (newPosition >= 1.0f && ants[i].velocity > 0.0f) {
      handleBoundary(ants[i], newPosition, gatherFood, false, strip.now);
    }

    // Handle collisions between ants (if not passing by)
    if (!passBy) {
      for (int j = i + 1; j < numAnts; j++) {
        if (fabsf(ants[j].velocity - ants[i].velocity) < 0.001f) continue;  // Moving in same direction at same speed; avoids tiny denominators

        // Calculate collision time using physics -  collisionTime formula adapted from rolling_balls
        float timeOffset = float(int(ants[j].lastBumpUpdate - ants[i].lastBumpUpdate));
        float collisionTime = (timeConversionFactor * (ants[i].position - ants[j].position) + ants[i].velocity * timeOffset) / (ants[j].velocity - ants[i].velocity);

        // Check if collision occurred in valid time window
        float timeSinceJ = float(int(strip.now - ants[j].lastBumpUpdate));
        if (collisionTime > MIN_COLLISION_TIME_MS && collisionTime < timeSinceJ) {
          // Update positions to collision point
          float adjustedTime = (collisionTime + float(int(ants[j].lastBumpUpdate - ants[i].lastBumpUpdate))) / timeConversionFactor;
          ants[i].position += ants[i].velocity * adjustedTime;
          ants[j].position = ants[i].position;

          // Update collision time
          unsigned long collisionMoment = static_cast<unsigned long>(collisionTime + 0.5f) + ants[j].lastBumpUpdate;
          ants[i].lastBumpUpdate = collisionMoment;
          ants[j].lastBumpUpdate = collisionMoment;

          // Reverse the ant with greater speed magnitude
          if (fabsf(ants[i].velocity) > fabsf(ants[j].velocity)) {
            ants[i].velocity = -ants[i].velocity;
          } else {
            ants[j].velocity = -ants[j].velocity;
          }

          // Recalculate position after collision
          newPosition = ants[i].position + ants[i].velocity * float(int(strip.now - ants[i].lastBumpUpdate)) / timeConversionFactor;
        }
      }
    }

    // Clamp position to valid range
    newPosition = constrain(newPosition, 0.0f, 1.0f);
    unsigned pixelPosition = roundf(newPosition * (SEGLEN - 1));

    // Determine ant color
    uint32_t antColor = getAntColor(i, numAnts, SEGMENT.palette != 0);

    // Render ant pixels
    for (int pixelOffset = 0; pixelOffset < antSize; pixelOffset++) {
      unsigned currentPixel = pixelPosition + pixelOffset;
      if (currentPixel >= SEGLEN) break;
      renderAntPixel(currentPixel, pixelOffset, antSize, ants[i], antColor, backgroundColor, gatherFood);
    }

    // Update ant state
    ants[i].lastBumpUpdate = strip.now;
    ants[i].position = newPosition;
  }

  SEGMENT.blur(SEGMENT.custom2>>1);
}
static const char _data_FX_MODE_ANTS[] PROGMEM = "Ants@Ant speed,# of ants,Ant size,Blur,,Gathering food,Smear,Pass by;!,!,!;!;1;sx=192,ix=255,c1=32,c2=0,o1=1,o3=1";


/*
/  Morse Code by Bob Loeffler
*   Adapted from code by automaticaddison.com and then optimized by claude.ai
*   aux0 is the pattern offset for scrolling
*   aux1 saves settings: check2 (1 bit), check3 (1 bit), text hash (4 bits) and pattern length (10 bits)
*   The first slider (sx) selects the scrolling speed
*   The second slider selects the color mode (lower half selects color wheel, upper half selects color palettes)
*   Checkbox1 displays all letters in a word with the same color
*   Checkbox2 displays punctuation or not
*   Checkbox3 displays the End-of-message code or not
*   We get the text from the SEGMENT.name and convert it to morse code
*   This effect uses a bit array, instead of bool array, for efficient storage - 8x memory reduction (128 bytes vs 1024 bytes)
*
*   Morse Code rules:
*    - a dot is 1 pixel/LED; a dash is 3 pixels/LEDs
*    - there is 1 space between each dot or dash that make up a letter/number/punctuation
*    - there are 3 spaces between each letter/number/punctuation
*    - there are 7 spaces between each word
*/

// Bit manipulation macros
#define SET_BIT8(arr, i) ((arr)[(i) >> 3] |= (1 << ((i) & 7)))
#define GET_BIT8(arr, i) (((arr)[(i) >> 3] & (1 << ((i) & 7))) != 0)

// Build morse code pattern into a buffer
static void build_morsecode_pattern(const char *morse_code, uint8_t *pattern, uint8_t *wordIndex, uint16_t &index, uint8_t currentWord, int maxSize) {
  const char *c = morse_code;

  // Build the dots and dashes into pattern array
  while (*c != '\0') {
    // it's a dot which is 1 pixel
    if (*c == '.') {
      if (index >= maxSize - 1) return;
      SET_BIT8(pattern, index);
      wordIndex[index] = currentWord;
      index++;
    }
    else { // Must be a dash which is 3 pixels
      if (index >= maxSize - 3) return;
      SET_BIT8(pattern, index);
      wordIndex[index] = currentWord;
      index++;
      SET_BIT8(pattern, index);
      wordIndex[index] = currentWord;
      index++;
      SET_BIT8(pattern, index);
      wordIndex[index] = currentWord;
      index++;
    }

    c++;

    // 1 space between parts of a letter/number/punctuation (but not after the last one)
    if (*c != '\0') {
      if (index >= maxSize) return;
      wordIndex[index] = currentWord;
      index++;
    }
  }

  // 3 spaces between two letters/numbers/punctuation
  if (index >= maxSize - 2) return;
  wordIndex[index] = currentWord;
  index++;
  if (index >= maxSize - 1) return;
  wordIndex[index] = currentWord;
  index++;
  if (index >= maxSize) return;
  wordIndex[index] = currentWord;
  index++;
}

static void mode_morsecode(void) {
  if (SEGLEN < 1) FX_FALLBACK_STATIC;

  // A-Z in Morse Code
  static const char * letters[] = {".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--",
                     "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--.."};
  // 0-9 in Morse Code
  static const char * numbers[] = {"-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----."};

  // Punctuation in Morse Code
  struct PunctuationMapping {
    char character;
    const char* code;
  };

  static const PunctuationMapping punctuation[] = {
    {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, 
    {':', "---..."}, {'-', "-....-"}, {'!', "-.-.--"},
    {'&', ".-..."}, {'@', ".--.-."}, {')', "-.--.-"},
    {'(', "-.--."}, {'/', "-..-."}, {'\'', ".----."}
  };

  // Get the text to display
  char text[WLED_MAX_SEGNAME_LEN+1] = {'\0'};
  size_t len = 0;

  if (SEGMENT.name) len = strlen(SEGMENT.name);
  if (len == 0) {
    strcpy_P(text, PSTR("I Love WLED!"));
  } else {
    strcpy(text, SEGMENT.name);
  }

  // Convert to uppercase in place
  for (char *p = text; *p; p++) {
    *p = toupper(*p);
  }

  // Allocate per-segment storage for pattern (1023 bits = 127 bytes) + word index array (1024 bytes) + word count (1 byte)
  constexpr size_t MORSECODE_MAX_PATTERN_SIZE = 1023;
  constexpr size_t MORSECODE_PATTERN_BYTES = (MORSECODE_MAX_PATTERN_SIZE + 7) / 8; // 128 bytes
  constexpr size_t MORSECODE_WORD_INDEX_BYTES = MORSECODE_MAX_PATTERN_SIZE; // 1 byte per bit position
  constexpr size_t MORSECODE_WORD_COUNT_BYTES = 1; // 1 byte for word count
  if (!SEGENV.allocateData(MORSECODE_PATTERN_BYTES + MORSECODE_WORD_INDEX_BYTES + MORSECODE_WORD_COUNT_BYTES)) FX_FALLBACK_STATIC;
  uint8_t* morsecodePattern = reinterpret_cast<uint8_t*>(SEGENV.data);
  uint8_t* wordIndexArray = reinterpret_cast<uint8_t*>(SEGENV.data + MORSECODE_PATTERN_BYTES);
  uint8_t* wordCountPtr = reinterpret_cast<uint8_t*>(SEGENV.data + MORSECODE_PATTERN_BYTES + MORSECODE_WORD_INDEX_BYTES);

  // SEGENV.aux1 stores: [bit 15: check2] [bit 14: check3] [bits 10-13: text hash (4 bits)] [bits 0-9: pattern length]
  bool lastCheck2 = (SEGENV.aux1 & 0x8000) != 0;
  bool lastCheck3 = (SEGENV.aux1 & 0x4000) != 0;
  uint16_t lastHashBits = (SEGENV.aux1 >> 10) & 0xF; // 4 bits of hash
  uint16_t patternLength = SEGENV.aux1 & 0x3FF; // Lower 10 bits for length (up to 1023)

  // Compute text hash
  uint16_t textHash = 0;
  for (char *p = text; *p; p++) {
    textHash = ((textHash << 5) + textHash) + *p;
  }
  uint16_t currentHashBits = (textHash >> 12) & 0xF; // Use upper 4 bits of hash

  bool textChanged = (currentHashBits != lastHashBits) && (SEGENV.call > 0);

  // Check if we need to rebuild the pattern
  bool needsRebuild = (SEGENV.call == 0) || textChanged || (SEGMENT.check2 != lastCheck2) || (SEGMENT.check3 != lastCheck3);

  // Initialize on first call or rebuild pattern
  if (needsRebuild) {
    patternLength = 0;

    // Clear the bit array and word index array first
    memset(morsecodePattern, 0, MORSECODE_PATTERN_BYTES);
    memset(wordIndexArray, 0, MORSECODE_WORD_INDEX_BYTES);

    // Track current word index
    uint8_t currentWordIndex = 0;

    // Build complete morse code pattern
    for (char *c = text; *c; c++) {
      if (patternLength >= MORSECODE_MAX_PATTERN_SIZE - 10) break;

      if (*c >= 'A' && *c <= 'Z') {
        build_morsecode_pattern(letters[*c - 'A'], morsecodePattern, wordIndexArray, patternLength, currentWordIndex, MORSECODE_MAX_PATTERN_SIZE);
      }
      else if (*c >= '0' && *c <= '9') {
        build_morsecode_pattern(numbers[*c - '0'], morsecodePattern, wordIndexArray, patternLength, currentWordIndex, MORSECODE_MAX_PATTERN_SIZE);
      }
      else if (*c == ' ') {
        // Space between words - increment word index for next word
        currentWordIndex++;
        // Add 4 additional spaces (7 total with the 3 after each letter)
        for (int x = 0; x < 4; x++) {
          if (patternLength >= MORSECODE_MAX_PATTERN_SIZE) break;
          wordIndexArray[patternLength] = currentWordIndex;
          patternLength++;
        }
      }
      else if (SEGMENT.check2) {
        const char *punctuationCode = nullptr;
        for (const auto& p : punctuation) {
          if (*c == p.character) {
            punctuationCode = p.code;
            break;
          }
        }
        if (punctuationCode) {
          build_morsecode_pattern(punctuationCode, morsecodePattern, wordIndexArray, patternLength, currentWordIndex, MORSECODE_MAX_PATTERN_SIZE);
        }
      }
    }

    if (SEGMENT.check3) {
      build_morsecode_pattern(".-.-.", morsecodePattern, wordIndexArray, patternLength, currentWordIndex, MORSECODE_MAX_PATTERN_SIZE);
    }

    for (int x = 0; x < 7; x++) {
      if (patternLength >= MORSECODE_MAX_PATTERN_SIZE) break;
      wordIndexArray[patternLength] = currentWordIndex;
      patternLength++;
    }

    // Store the total number of words (currentWordIndex + 1 because it's 0-indexed)
    *wordCountPtr = currentWordIndex + 1;

    // Store pattern length, checkbox states, and hash bits in aux1
    SEGENV.aux1 = patternLength | (currentHashBits << 10) | (SEGMENT.check2 ? 0x8000 : 0) | (SEGMENT.check3 ? 0x4000 : 0);

    // Reset the scroll offset
    SEGENV.aux0 = 0;
  }

  // if pattern is empty for some reason, display black background only
  if (patternLength == 0) {
    SEGMENT.fill(BLACK);
    return;
  }

  // Update offset to make the morse code scroll
  // Use step for scroll timing only
  uint32_t cycleTime = 50 + (255 - SEGMENT.speed)*3;
  uint32_t it = strip.now / cycleTime;
  if (SEGENV.step != it) {
    SEGENV.aux0++;
    SEGENV.step = it;
  }

  // Clear background
  SEGMENT.fill(BLACK);

  // Draw the scrolling pattern
  int offset = SEGENV.aux0 % patternLength;

  // Get the word count and calculate color spacing
  uint8_t wordCount = *wordCountPtr;
  if (wordCount == 0) wordCount = 1;
  uint8_t colorSpacing = 255 / wordCount; // Distribute colors evenly across color wheel/palette

  for (int i = 0; i < SEGLEN; i++) {
    int patternIndex = (offset + i) % patternLength;
    if (GET_BIT8(morsecodePattern, patternIndex)) {
      uint8_t wordIdx = wordIndexArray[patternIndex];
      if (SEGMENT.check1) {  // make each word a separate color
        if (SEGMENT.custom3 < 16)
          // use word index to select base color, add slight offset for animation
          SEGMENT.setPixelColor(i, SEGMENT.color_wheel((wordIdx * colorSpacing) + (SEGENV.aux0 / 4)));
        else
          SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(wordIdx * colorSpacing, true, PALETTE_SOLID_WRAP, 0));
      }
      else {
        if (SEGMENT.custom3 < 16)
          SEGMENT.setPixelColor(i, SEGMENT.color_wheel(SEGENV.aux0 + i));
        else
          SEGMENT.setPixelColor(i, SEGMENT.color_from_palette(i, true, PALETTE_SOLID_WRAP, 0));
      }
    }
  }
}
static const char _data_FX_MODE_MORSECODE[] PROGMEM = "Morse Code@Speed,,,,Color mode,Color by Word,Punctuation,EndOfMessage;;!;1;sx=192,c3=8,o1=1,o2=1";





/////////////////////////////////////////////////////////////
//  Effets "Phare" : colonne de 4 ampoules, 0 = bas, 3 = haut
//  Reagissent au micro (usermod AudioReactive)
/////////////////////////////////////////////////////////////

#define PHARE_MAX 16

struct PhareState {
  float    level[PHARE_MAX];  // intensite de chaque ampoule, 0..1
  float    bassAvg;           // moyenne glissante des basses
  float    trebAvg;           // moyenne glissante des aigus
  float    env;               // enveloppe (volume ou basses lissees)
  float    phase;             // position de la vague
  uint32_t lastBeat;
  uint32_t lastTreb;
  uint32_t lastFrame;
  uint8_t  pos;               // ampoule courante (roll)
  uint8_t  side;              // ampoule du haut courante (alternance)
  float    tgt[PHARE_MAX];    // cibles / positions (bougies, neon, pluie)
  float    p0, p1;            // positions (ascenseur)
  uint32_t t0, t1;            // instants (ascenseur, orage)
  float    stp[PHARE_MAX];    // pas de fondu (bougies)
  float    pk[4], av[4];      // son : pic et moyenne par bande (basses, mediums, aigus, volume)
  uint32_t lb[4];             // son : dernier coup par bande
  float    pv[4], fa[4];      // son : valeur precedente et moyenne des "attaques" par bande
  float    q;                 // libre (pic du VU-metre, energie...)
};

// courbe filament + prechauffage : niveau percu (0..1) -> valeur envoyee a l'ampoule
static uint8_t phareOut(float level) {
  level = constrain(level, 0.0f, 1.0f);
  float duty = powf(level, 1.3f);              // un filament parait sombre en bas : on compense
  if (duty < 0.02f) duty = 0.02f;              // prechauffage : jamais completement froid (invisible)
  int v = (int)(duty * 255.0f + 0.5f);
  if (gammaCorrectCol) v = gamma8inv(v);       // annule la correction gamma de WLED (faite pour les LED)
  return v;
}

// =====================================================================
//  MOTEUR DE TEMPO : detecte kick / caisse claire / hi-hats, calcule le BPM,
//  predit chaque temps (pour flasher pile sur le kick), compte mesures et phrases
// =====================================================================
static uint16_t phareLatence = 60;        // retard micro + analyse (ms), reglable dans Usermods

struct PhareTempo {
  float    period = 500.0f;               // duree d'un temps (ms)
  uint32_t nextBeat = 0, lastTick = 0;    // prochain temps predit, dernier temps joue
  uint32_t beatCount = 0;                 // nombre de temps depuis le debut
  float    conf = 0.0f;                   // confiance dans le tempo (0..1)
  uint32_t onsets[12] = {0}; uint8_t nOn = 0;
  uint32_t lastKick = 0, lastSnare = 0, lastHat = 0;
  float    pv[3] = {0}, fa[3] = {0}, pk[3] = {40, 40, 40};
  uint8_t  votes = 0; float candidat = 0;
  uint32_t kickFlash = 0; uint8_t kickMode = 0; bool kickCote = false;
  float    energie = 0, energieLente = 0, energieTres = 0;   // volume moyen 0,5 s / 4 s / 12 s
  uint32_t silenceDepuis = 0;
  uint32_t sonDepuis = 0;  float vol10 = 10;   // Auto : silence par rapport au niveau des ~10 dernieres secondes
  volatile uint32_t taps[4] = {0}; volatile uint8_t nTap = 0; uint32_t dernierSeqTap = 0;
};
static PhareTempo T;

static float    phareTempoMs = 500.0f;    // (compatibilite) duree d'un temps
static uint32_t phareTempoLast = 0;       // (compatibilite) dernier temps
static bool phareKickActif() { return T.lastKick && millis() - T.lastKick < (uint32_t)(T.period * 2.5f) + 300; }
static bool phareCale()      { return T.conf > 0.45f && phareKickActif(); }   // tempo fiable + kick present
static bool phareMusique()   { return phareCale(); }
static float phareBpm()      { return 60000.0f / T.period; }

// phase (0..1) dans un cycle de nb temps, aligne sur la grille
static float phareCycle(float nbTemps) {
  float frac = constrain((millis() - T.lastTick) / T.period, 0.0f, 0.999f);
  float pos = fmodf((float)(T.beatCount % 64) + frac, nbTemps);
  return pos / nbTemps;
}

// =====================================================================
//  CONTEXTE DE RENDU : ce que l'image en cours doit faire, en un seul endroit
//  chaine : ecoute -> tempo -> effet -> couches (percus) -> montee / drop -> main -> sortie
//  le VJ le remplit avant de jouer un effet ; tout revient a zero apres chaque image
// =====================================================================
struct PhareRenduCtx {
  bool  vj = false;          // l'effet est joue par le VJ
  float vjCouches = 1.0f;    // force des percus voulue par le VJ
  bool  reactif = false;     // l'animation suit le tempo
  float couches = 0.0f;      // force des percus (kick / chick / hi-hats) posees par-dessus
  bool  sansKick = false;    // pas de couche kick (1 phrase sur 10 en VJ, ou Roll)
  bool  chickSeul = false;   // sans kick : seulement le chick en haut
  bool  calme = false;       // VJ calme : une seule lampe marque le kick
  float douceur = 0.0f;      // 0 = clignote (franc), 1 = veilleuse (fondus lents)
  bool  max3 = false;        // jamais les 4 lampes a fond (reserve au drop)
  float gain = 1.0f;         // fondu de sortie (extinction quand le son se coupe)
  bool  sansChick = false;   // pas de chick (formation "kick seul")
  bool  sansHats = false;    // pas de hi-hats
  float melodie = 0.0f;      // Full auto : une lampe suit la melodie par-dessus l'effet
};
// orchestration du VJ, choisie a chaque phrase (8 mesures)
struct PhareOrch {
  uint8_t formation = 3;     // 0 effet seul, 1 kick seul, 2 effet + chick, 3 tout
  uint8_t variante = 0;      // 0 normal, 1 demi-temps, 2 double-temps, 3 contretemps, 4 triolets, 5 accent sur le 1, 6 tenu, 7 sec
  bool    noirFin = false;   // un temps de noir juste avant la phrase suivante
  int8_t  mesureVide = -1;   // une mesure presque vide (numero de mesure dans la phrase)
  uint8_t fill = 0;          // fill a la 8e mesure : 0 aucun, 1 roll, 2 stroboscope
  uint8_t perm = 0;          // lampes de l'effet : 0 telles quelles, 1 inversees, 2 decalees de 2, 3 depuis le milieu
  uint8_t lampeVide = 0;     // lampe allumee pendant la mesure presque vide
  float   niveau = 1.0f;     // contraste couplet / refrain (0,4 a 1)
};
static PhareOrch O;
static bool phareFullAuto = false;           // Full auto : le VJ lit la structure du morceau et compose plus librement
// suivi de la melodie : hauteur (contour), attaques, accords, presence
struct PhareMelodie {
  float    pos = 1.5f;          // position sur la colonne (0 = lampe 1 ... 3 = lampe 4), suit le contour de la melodie
  float    centrePrec = 0;      // hauteur de la note precedente (centre des bandes)
  float    medLent = 10, medMoy = 10;
  uint32_t attaque = 0, attaquePrec = 0;
  uint8_t  accord = 1;          // notes entendues ensemble (1 a 3)
  float    presence = 0;        // 0..1 : une melodie se joue en ce moment
  float    nbRecent = 0;        // attaques recentes (moyenne glissante)
  bool     tenue = false;       // note tenue (pas d'attaque mais du son)
};
static PhareMelodie M;
static PhareRenduCtx Rd;
static void phareRenduReset() { bool vj = Rd.vj; float vc = Rd.vjCouches; Rd = PhareRenduCtx(); Rd.vj = vj; Rd.vjCouches = vc; }
// ---- reactivite des animations ----

// a appeler au debut de chaque animation
static uint8_t phareSortie[4] = {0};
static float phareRepartition[4] = {0};   // part de lumiere de chaque lampe (moyenne sur ~5 min)      // niveaux affiches (pour la page de controle)
static float phareVol = 0;                 // volume du micro (pour la page)
static bool phareInverse = false;          // bouton Inverser : tous les effets dans l'autre sens (haut <-> bas)
static bool phareExpoForce = false;        // effet Soiree lance depuis l'onglet Expo : joue sans le son
static bool phareExpo = false;             // l'effet en cours est un effet Expo (ambiance fixe, sans son)
static bool phareOverride = false;        // Override : kick / chick / hi-hats par-dessus l'effet, meme hors VJ
static bool phareSync = true;             // "Sync" general (page de controle) : tous les effets suivent le tempo, ou aucun
static bool phareSurLe1 = false;          // "Sur le 1" : Suivant / Drop / changement d'effet partent au prochain temps 1
static uint32_t phareMesureN = 0;         // compte les debuts de mesure (temps 1)
static void phareAnim() {
  Rd.reactif = Rd.vj || (phareSync && !phareExpo);
  Rd.couches = Rd.vj ? Rd.vjCouches : ((phareOverride && !phareExpo) ? 1.0f : 0.0f);   // hors VJ : seul, sauf Override
}
// temps "musical" : avance au rythme du morceau (1 temps = 500 ms, comme a 120 BPM)
static uint32_t phareNow() {
  if (!Rd.reactif || !phareCale()) return strip.now;
  float frac = constrain((millis() - T.lastTick) / T.period, 0.0f, 0.999f);
  float b = T.beatCount + frac;
  if (Rd.vj) {                                              // variantes de temps du VJ
    static const float mul[8] = {1.0f, 0.5f, 2.0f, 1.0f, 1.5f, 1.0f, 1.0f, 1.0f};
    b = b * mul[O.variante % 8] + (O.variante == 3 ? 0.5f : 0.0f);
  }
  return (uint32_t)(b * 500.0f);
}

static uint8_t phareSens = 220;           // sensibilite generale au son (Usermods -> Phare, ou page de controle)
// controle a la main (page punto1.local/phare)
static volatile float   phareMainPos = -1.0f;   // fader "Main" : 0 = bas, 1 = haut, -1 = relache
static volatile uint32_t phareMainT = 0;
static volatile uint8_t pharePads = 0;          // boutons lampes 1 2 3 4 (bit par lampe)

// couches posees par-dessus l'effet : kick (ampoules 1-2), caisse claire (4), hi-hats (2-3)
static void phareCouches(float* lv, unsigned n) {
  if (Rd.couches <= 0.0f || n < 1) return;
  uint32_t now = millis();
  float f = Rd.couches;
  if (T.kickFlash && now >= T.kickFlash && !Rd.sansKick && !Rd.chickSeul) {
    float e = expf(-(float)(now - T.kickFlash) / (T.period * 0.28f)) * f;
    unsigned h = n - 1, L[4] = {0, min(1u, h), min(2u, h), h};
    unsigned grimpe = L[(T.beatCount / 4) % 4];
    // lampe principale du kick selon la phrase (le kick se deplace)
    static const uint8_t principale[9] = {0, 0, 0, 1, 2, 3, 0, 1, 0};
    unsigned p = T.kickMode == 8 ? grimpe : (T.kickMode == 2 && T.kickCote ? L[1] : L[principale[T.kickMode % 9]]);
    if (Rd.calme) { lv[p] = max(lv[p], e); return; }                                              // calme : 1 seule lampe, rien d'autre
    lv[p] = max(lv[p], e);
    switch (T.kickMode) {
      case 1: lv[L[1]] = max(lv[L[1]], e); break;                                                 // double 1+2
      case 6: lv[L[3]] = max(lv[L[3]], e); break;                                                 // paire 1+4
      case 7: lv[L[2]] = max(lv[L[2]], e); break;                                                 // paire 2+3
    }
  }
  if (n >= 3 && T.lastSnare && now >= T.lastSnare && !Rd.sansChick)
    lv[n - 1] = max(lv[n - 1], expf(-(float)(now - T.lastSnare) / 110.0f) * f);                    // chick
  if (Rd.melodie > 0.0f && M.presence > 0.35f && M.attaque) {                                  // la melodie joue par-dessus
    unsigned k = min((unsigned)(M.pos + 0.5f), n - 1);
    float e = expf(-(float)(now - M.attaque) / (M.tenue ? 600.0f : 180.0f)) * Rd.melodie * 0.85f;
    lv[k] = max(lv[k], e);
    if (M.accord >= 2 && k + 1 < n) lv[k + 1] = max(lv[k + 1], e * 0.6f);
  }
  if (n >= 4 && T.lastHat && now >= T.lastHat && !Rd.chickSeul && !Rd.sansHats && Rd.melodie <= 0.0f) {
    float h = 0.22f * expf(-(float)(now - T.lastHat) / 60.0f) * f;                                 // hi-hats
    lv[1] = max(lv[1], h); lv[2] = max(lv[2], h);
  }
}

// un kick vient d'etre joue (predit ou detecte)
static void phareKick(uint32_t quand) {
  T.kickFlash = quand;
  if (T.kickMode == 2) T.kickCote = !T.kickCote;
}

// nouvelle phrase (8 mesures) : on choisit comment le kick tombe, et on le garde toute la phrase
static void phareNouvellePhrase() {
  // le kick change de place a chaque phrase : lead 1, lead 2, double, rebond, lampe 3, lampe 4, paire 1+4, paire 2+3, qui monte
  static const uint8_t poids[9] = {17, 17, 3, 5, 17, 17, 9, 9, 6};   // une lampe seule (1, 2, 3, 4) dans 2/3 des phrases
  uint8_t r = hw_random8() % 100, m = 0;
  while (m < 8 && r >= poids[m]) { r -= poids[m]; m++; }
  T.kickMode = m;
}

// =====================================================================
//  MONTEE / DROP (commun a tous les effets) :
//  montee = roll qui accelere -> noir -> au drop, les 4 lampes flashent ensemble sur 4 ou 8 kicks
//  aussi utilise pour "le beat est reconnu" : flashs ensemble pendant 1 ou 2 mesures
// =====================================================================
struct PhareDrop {
  uint8_t  etat = 0;             // 0 rien, 1 montee, 2 noir (attend le kick), 3 flashs
  bool     manuel = false;       // lance par les boutons
  uint32_t t = 0;                // debut de l'etat
  float    roll = 0;             // (inutilise)
  float    pos0 = 0, b0 = 0, bDebut = 0;   // montee : position du roll, temps de reference
  uint8_t  forme = 0;            // montee : 0 vers le haut, 1 vers le bas, 2 pendule (les montees auto varient)
  uint8_t  kicksRetour = 0; uint32_t kickPrec = 0;   // le beat revient-il vraiment ?
  float    vitesse = 1, mult = 1;  // montee : ampoules par temps (0,5 a 16), reglage manuel (+ / -)
  bool     suivantAttente = false;
  uint32_t kickVu = 0;           // dernier kick compte
  uint8_t  flashs = 0, total = 0;
  uint32_t flashT = 0;           // instant du dernier flash
  uint32_t fin = 0;              // fin du dernier drop / intro
  bool     decide = false;       // le DJ a deja decide pour ce break
  bool     duo = false;          // flashs sur les lampes 1 et 2 seulement (le beat arrive)
  uint32_t manuelT = 0;           // dernier appui sur Montee / Drop
  uint32_t kickDebut = 0, dureeKick = 0;   // depuis quand le kick tourne / combien de temps il a dure
};
static PhareDrop DR;
// boutons : 1 montee, 2 drop, 3 suivant, 4 plus, 5 moins, 6 off -- en file d'attente, aucune commande ne se perd
static volatile uint8_t phareCmdQ[16]; static volatile uint8_t phareCmdW = 0, phareCmdR = 0;
static void pharePousse(uint8_t c) { uint8_t w = phareCmdW; phareCmdQ[w % 16] = c; phareCmdW = w + 1; }
static bool phareSuivantDemande = false;
static float phareGainSon() { return 0.3f * powf(10.0f, phareSens / 255.0f); }   // sensibilite : x0,3 a x3
static float phareGateActif() { return 2.0f + (255 - phareSens) / 255.0f * 38.0f; }   // en dessous : silence

static float phareTempsGrille() { return (float)T.beatCount + constrain((millis() - T.lastTick) / T.period, 0.0f, 0.999f); }
// change la vitesse de la montee TOUT DE SUITE, en restant cale sur les subdivisions du temps :
// l'ampoule en cours reste allumee, la suivante tombe sur le prochain 1/r de temps
static void phareMonteeRate(float r) {
  float b = phareTempsGrille();
  float pos = DR.pos0 + (b - DR.b0) * DR.vitesse;
  DR.pos0 = floorf(max(0.0f, pos));
  DR.b0 = floorf(b * r) / r;
  DR.vitesse = r;
}
static void phareMontee(bool manuel) {
  if (DR.etat == 1 && manuel) { DR.mult = min(16.0f, DR.mult * 2.0f); phareMonteeRate(DR.mult); return; }   // appui suivant = + (jusqu'a x16)
  float b = phareTempsGrille();
  DR.etat = 1; DR.manuel = manuel; DR.t = millis(); DR.mult = 1; DR.vitesse = 1;
  DR.forme = manuel ? 0 : hw_random8() % 3; DR.kicksRetour = 0; DR.kickPrec = T.lastKick;
  DR.b0 = floorf(b); DR.bDebut = b; DR.pos0 = 0;                             // 1re ampoule tout de suite, la suivante sur le temps
}
static bool phareNoirManuel = false;          // bouton "Off" : lumieres coupees, l'ecoute continue
// + / - pendant la montee manuelle
static void phareMonteeVitesse(bool plus) {
  if (DR.etat != 1 || !DR.manuel) return;
  DR.mult = plus ? min(16.0f, DR.mult * 2.0f) : max(0.5f, DR.mult * 0.5f);
  phareMonteeRate(DR.mult);
}
static void phareNoir(bool manuel)   { DR.etat = 2; DR.manuel = manuel; DR.t = millis(); DR.kickVu = T.kickFlash; }
static void phareFlashs(uint8_t nb)  { DR.duo = false; DR.etat = 3; DR.t = millis(); DR.flashs = 0; DR.total = nb; DR.kickVu = T.kickFlash; DR.flashT = 0; }
// le drop tombe maintenant : 1er flash tout de suite, puis un par kick (4 ou 8 en tout)
static bool phareApresDrop = false;          // un drop manuel vient de se terminer : enchainer All bass / Artifice
static uint32_t phareVJVu = 0;               // derniere image jouee par le VJ
static uint8_t phareApresVJ = 0;             // 1 : Artifice 8 mesures ; 2 : All bass 4 + Artifice 4 (joue par le VJ)
static uint32_t phareApresDebut = 0;
static bool phareDropManuel = false;
static void phareDropMaintenant()   { phareFlashs(4); DR.duo = false; DR.flashs = 1; DR.flashT = millis(); DR.kickVu = T.lastTick; }

// avance la machine montee / drop (appele par phareLoop)
static void phareDropLoop(uint32_t now, uint32_t dt, float vol) {
  // kick : depuis quand il tourne
  if (phareKickActif()) { if (!DR.kickDebut) DR.kickDebut = now; DR.dureeKick = now - DR.kickDebut; DR.decide = false; }
  else DR.kickDebut = 0;

  // debut de mesure (temps 1)
  static uint32_t beatVu = 0;
  bool temps1 = T.beatCount != beatVu && T.beatCount % 4 == 0;
  beatVu = T.beatCount;
  if (temps1) phareMesureN++;
  bool attendre = phareSurLe1 && phareCale();

  while (phareCmdR != phareCmdW) {
  uint8_t c = phareCmdQ[phareCmdR % 16]; phareCmdR = phareCmdR + 1;
  if (c >= 1 && c <= 6) DR.manuelT = now;
  if (c == 4 || c == 5) phareMonteeVitesse(c == 4);
  if (c == 6) { phareNoirManuel = !phareNoirManuel; if (phareNoirManuel && DR.etat == 1) DR.etat = 0; }
  if (c == 2) phareNoirManuel = false;
  if (c == 3 && DR.etat == 1) { DR.etat = 0; DR.fin = now; }   // Suivant arrete la montee
  if (c == 1) phareMontee(true);
  else if (c == 2) { phareDropMaintenant(); phareDropManuel = true; }   // Direct On : le drop part tout de suite
  else if (c == 3) { if (attendre) DR.suivantAttente = true; else phareSuivantDemande = true; }
  }
  if (DR.suivantAttente && (temps1 || !attendre)) { DR.suivantAttente = false; phareSuivantDemande = true; }

  bool nouveauKick = T.kickFlash != DR.kickVu && T.kickFlash >= DR.t;
  bool silence = now - T.silenceDepuis > 1500;
  switch (DR.etat) {
    case 1: {                                                     // MONTEE : roll de plus en plus vite
      // vitesse : double toutes les 2 mesures, et chaque appui sur Montee la double encore (max 16 par temps)
      if (!DR.manuel) {                                           // VJ : double toutes les 2 mesures
        float cible = (float)(1 << min((int)((phareTempsGrille() - DR.bDebut) / 16.0f), 4));   // double toutes les 4 mesures
        if (cible != DR.vitesse) phareMonteeRate(cible);
      }
      if (DR.manuel) { if (now - DR.t > 120000) DR.etat = 0; break; }
      // le beat revient vraiment : au moins 3 kicks reguliers (sur les temps)
      if (T.lastKick != DR.kickPrec && T.lastKick > DR.t + 400) {
        float ecart = (T.lastKick - DR.kickPrec) / T.period;
        bool regulier = (ecart > 0.8f && ecart < 1.25f) || (ecart > 1.8f && ecart < 2.25f);
        DR.kicksRetour = regulier ? DR.kicksRetour + 1 : 1; DR.kickPrec = T.lastKick;
      }
      float tempsMontee = phareTempsGrille() - DR.bDebut;
      if (DR.kicksRetour >= 3) {
        if (tempsMontee >= 16) { phareDropMaintenant(); break; }  // 4 mesures de montee au moins : DROP
        DR.etat = 0; DR.fin = now; break;                         // le beat revient trop tot : pas de drop, le VJ reprend
      }
      if (tempsMontee > 8 && (silence || T.energie < T.energieLente * 0.35f)) { phareNoir(false); break; }   // la coupure avant le drop
      if (now - DR.t > 60000) DR.etat = 0;                        // finalement pas de drop
      break;
    }
    case 2:                                                       // NOIR : on attend le kick
      if (DR.manuel && attendre) { if (temps1) { phareDropMaintenant(); DR.flashT = T.lastTick; } }   // Sur le 1
      else if (nouveauKick || (DR.manuel && now - DR.t > 2000)) phareDropMaintenant();
      else if (!DR.manuel && now - DR.t > 4000) { DR.etat = 0; DR.fin = now; }
      break;
    case 3:                                                       // FLASHS sur chaque kick
      if (phareCale()) {                                          // cale sur les temps de la musique
        if (T.lastTick >= DR.t && T.lastTick != DR.kickVu && (DR.flashs == 0 || T.lastTick > DR.flashT + (uint32_t)(T.period * 0.5f))) {
          DR.kickVu = T.lastTick; DR.flashs++; DR.flashT = T.lastTick; }
      } else if (T.kickFlash != DR.kickVu && T.kickFlash >= DR.t && (DR.flashs == 0 || T.kickFlash > DR.flashT + (uint32_t)(T.period * 0.5f))) {
        DR.kickVu = T.kickFlash; DR.flashs++; DR.flashT = max(T.kickFlash, DR.flashT);   // pas de tempo : sur les kicks entendus
      } else if (DR.flashs > 0 && now - DR.flashT >= T.period) { DR.flashs++; DR.flashT += (uint32_t)T.period; }
      if (DR.flashs > DR.total || (DR.flashs == 0 && now - DR.t > 3000)) { DR.etat = 0; DR.fin = now; if (phareDropManuel) { phareDropManuel = false;
        if (now - phareVJVu < 300) { phareApresVJ = hw_random8() < 128 ? 1 : 2; phareApresDebut = phareMesureN; }   // VJ : il enchaine lui-meme
        else phareApresDrop = true; } }
      break;
  }
  (void)vol;
}

// remplace l'image pendant la montee / le noir / les flashs ; renvoie false sinon
static bool phareSurcouche(float* lv, unsigned n) {
  if (phareNoirManuel && DR.etat != 3 && n > 0) { for (unsigned i = 0; i < n; i++) lv[i] = 0.0f; return true; }
  if (DR.etat == 0 || n < 1) return false;
  uint32_t now = millis();
  for (unsigned i = 0; i < n; i++) lv[i] = 0.0f;
  if (DR.etat == 1) {
    // roll cale sur les temps, vitesse = DR.vitesse ampoules par temps
    float pas = max(0.0f, DR.pos0 + (phareTempsGrille() - DR.b0) * DR.vitesse);
    unsigned k = (unsigned)pas % n; float f = pas - floorf(pas);
    float in = constrain(f / 0.12f, 0.0f, 1.0f); in = in * in * (3.0f - 2.0f * in);   // fondu court et net
    if (pas < 1.0f) in = 1.0f;                                    // depart : la lampe 1 s'allume a fond tout de suite
    if (DR.forme == 2) {                                          // pendule rapide, faisceau etroit
      float y = (n - 1) * (0.5f - 0.5f * cosf(PI * pas));
      for (unsigned i = 0; i < n; i++) { float d = ((float)i - y) / 0.45f; lv[i] = expf(-d * d); }
    } else {
      unsigned kk = DR.forme == 1 ? n - 1 - k : k, pp = DR.forme == 1 ? (n - k) % n : (k + n - 1) % n;   // vers le bas : lampe 4 -> 1
      lv[kk] = in; if (pas >= 1.0f) lv[pp] = max(lv[pp], 1.0f - in);
    }
  } else if (DR.etat == 3 && DR.flashs > 0 && DR.flashs <= DR.total && now >= DR.flashT) {
    bool on = now - DR.flashT < (uint32_t)constrain(T.period * 0.45f, 90.0f, 260.0f);   // allume / eteint franc
    for (unsigned i = 0; i < n; i++) lv[i] = (on && (!DR.duo || i < 2)) ? 1.0f : 0.0f;
  }
  return true;
}

// estimation du tempo a partir des derniers kicks (histogramme des ecarts)
static void phareEstimeTempo() {
  if (T.nOn < 4) return;
  uint8_t hist[48] = {0};                                // 85 a 180 BPM, cases de 8 ms
  for (int i = 0; i < T.nOn; i++) for (int j = i + 1; j < T.nOn && j < i + 5; j++) {
    float d = (float)(T.onsets[j] - T.onsets[i]);
    while (d > 706.0f) d *= 0.5f;
    while (d < 333.0f && d > 0) d *= 2.0f;
    int b = (int)((d - 333.0f) / 8.0f);
    if (b >= 0 && b < 48) { hist[b] += 2; if (b > 0) hist[b - 1]++; if (b < 47) hist[b + 1]++; }
  }
  int best = 0; for (int b = 1; b < 48; b++) if (hist[b] > hist[best]) best = b;
  if (hist[best] < 4) return;
  float cand = 333.0f + best * 8.0f + 4.0f;
  // garder le meme ordre de grandeur que le tempo actuel (eviter les sauts x2 / /2)
  if (T.conf > 0.5f) { if (cand > T.period * 1.6f) cand *= 0.5f; else if (cand < T.period * 0.62f) cand *= 2.0f; }
  if (fabsf(cand - T.period) / T.period < 0.06f) { T.period = T.period * 0.85f + cand * 0.15f; T.votes = 0; }
  else if (T.candidat > 0 && fabsf(cand - T.candidat) / T.candidat < 0.06f) {
    if (++T.votes >= 3) { T.period = cand; T.votes = 0; T.conf = 0.3f; }
  } else { T.candidat = cand; T.votes = 1; }
}

// detection d'une attaque sur une bande
static bool phareAttaque(int i, float v, float k, uint32_t& last, uint16_t ecartMin) {
  float a = max(0.0f, v - T.pv[i]); T.pv[i] = v;
  T.pk[i] = max(v, max(T.pk[i] * 0.999f, 25.0f));
  bool hit = a > T.fa[i] * k + 6.0f && v > T.pk[i] * 0.4f && millis() - last > ecartMin;
  T.fa[i] = T.fa[i] * 0.93f + a * 0.07f;
  if (hit) last = millis();
  return hit;
}

static void phareLoop() {
  static uint32_t dernier = 0;
  uint32_t now = millis();
  if (now - dernier < 8) return;
  uint32_t dtLoop = min(now - dernier, (uint32_t)100);
  dernier = now;
  um_data_t* um;
  if (!UsermodManager::getUMData(&um, USERMOD_ID_AUDIOREACTIVE)) return;
  uint8_t* fft = (uint8_t*)um->u_data[2];
  float vol = *(float*)um->u_data[0];
  phareVol = vol;

  // energies (pour le DJ)
  T.energie      = T.energie      * 0.94f  + vol * 0.06f;
  T.energieLente = T.energieLente * 0.992f + vol * 0.008f;
  T.energieTres  = T.energieTres  * 0.997f + vol * 0.003f;
  if (vol > phareGateActif()) T.silenceDepuis = now;
  T.vol10 = T.vol10 * 0.9985f + vol * 0.0015f;
  if (vol > max(phareGateActif() * 0.6f, T.vol10 * 0.15f)) T.sonDepuis = now;   // un morceau doux n'est pas du silence

  // tap tempo (bouton 4 de la telecommande)
  if (T.nTap >= 4) {
    uint32_t a = T.taps[0], b = T.taps[3];
    T.nTap = 0;
    float p = (b - a) / 3.0f;
    if (p > 280 && p < 1200) {
      T.period = p; T.conf = 0.8f; T.votes = 0;
      T.beatCount = (T.beatCount / 4 + 1) * 4 - 1;        // le 1er tap etait un debut de mesure
      T.lastTick = b; T.nextBeat = b + (uint32_t)p;
      T.lastKick = now;                                   // considere la musique comme presente
    }
  }

  // detections
  float g = phareGainSon();
  float kick  = g * (fft[0] + fft[1]) / 2.0f;
  float snare = g * (fft[2] + fft[3] + fft[9] + fft[10] + fft[11]) / 5.0f;
  float hat   = g * (fft[14] + fft[15]) / 2.0f;
  bool k = phareAttaque(0, kick, 2.2f, T.lastKick, 200) && vol > phareGateActif();
  phareAttaque(1, snare, 2.4f, T.lastSnare, 120);
  phareAttaque(2, hat, 2.0f, T.lastHat, 70);

  if (k) {
    uint32_t vrai = now - phareLatence;                   // instant reel du kick
    if (T.nOn == 12) { memmove(T.onsets, T.onsets + 1, 11 * sizeof(uint32_t)); T.nOn = 11; }
    T.onsets[T.nOn++] = vrai;
    phareEstimeTempo();
    // recaler la grille sur ce kick
    float e1 = (float)(int32_t)(vrai - T.lastTick), e2 = (float)(int32_t)(vrai - T.nextBeat);
    float e = fabsf(e1) < fabsf(e2) ? e1 : e2;
    if (fabsf(e) < T.period * 0.3f) {
      T.nextBeat += (int32_t)(e * 0.3f);
      T.conf = min(1.0f, T.conf + 0.08f);
    } else {
      T.conf = max(0.0f, T.conf - 0.12f);
      if (T.conf < 0.2f) { T.nextBeat = vrai + (uint32_t)T.period; }   // on repart de ce kick
    }
    if (!phareCale()) phareKick(now);                     // pas encore cale : on flashe sur le kick detecte
  }
  if (T.nextBeat == 0) T.nextBeat = now + (uint32_t)T.period;

  // avancer la grille, jouer les temps predits
  while ((int32_t)(now - T.nextBeat) >= 0) {
    T.lastTick = T.nextBeat;
    T.nextBeat += (uint32_t)T.period;
    T.beatCount++;
    if (T.beatCount % 32 == 0) phareNouvellePhrase();      // nouvelle phrase (8 mesures) : nouvelle facon de jouer le kick
    if (phareCale()) phareKick(T.lastTick);               // cale : on flashe pile sur le temps
  }
  if (!phareKickActif()) T.conf = max(0.0f, T.conf - 0.0005f);
  phareTempoMs = T.period; phareTempoLast = T.lastTick;
  // --- melodie : bandes 3 a 13 (medium / aigu), hors basse et cymbales
  {
    float g = phareGainSon(), v[16], med = 0, s1 = 0, s2 = 0, mx = 0;
    for (int b = 3; b <= 13; b++) { v[b] = fft[b] * g; med += v[b]; s1 += b * v[b] * v[b]; s2 += v[b] * v[b]; mx = max(mx, v[b]); }
    med /= 11.0f;
    float centre = s2 > 1 ? s1 / s2 : M.centrePrec;
    M.medMoy = M.medMoy * 0.996f + med * 0.004f;
    bool fort = med > max(6.0f, M.medMoy * 0.5f) && vol > phareGateActif() * 0.6f;
    bool attaque = fort && now - M.attaque > 90 && (med - M.medLent > max(6.0f, M.medLent * 0.22f) || (fabsf(centre - M.centrePrec) > 0.7f && now - M.attaque > 160));
    M.medLent = M.medLent * 0.85f + med * 0.15f;
    if (attaque) {
      float d = centre - M.centrePrec;                       // la melodie monte ou descend ?
      float pas = fabsf(d) > 2.0f ? 2.0f : (fabsf(d) > 0.4f ? 1.0f : 0.0f);
      M.pos = constrain(M.pos + (d > 0 ? pas : -pas), 0.0f, 3.0f);
      M.centrePrec = centre; M.attaquePrec = M.attaque; M.attaque = now;
      uint8_t pics = 0; for (int b = 4; b <= 12; b++) if (v[b] > mx * 0.6f && v[b] >= v[b - 1] && v[b] >= v[b + 1]) pics++;
      M.accord = constrain(pics, 1, 3);
      M.nbRecent += 1.0f;
    }
    M.nbRecent *= powf(0.5f, dtLoop / 2000.0f);              // ~ attaques sur les 3 dernieres secondes
    M.tenue = fort && now - M.attaque > 350;
    float cible = constrain(M.nbRecent / 4.0f, 0.0f, 1.0f) * (fort ? 1.0f : 0.3f);
    if (M.tenue && fort) cible = max(cible, 0.5f);
    M.presence += (cible - M.presence) * min(1.0f, dtLoop / 800.0f);
  }
  phareDropLoop(now, dtLoop, vol);
}

// telecommande : le bouton 4 sert de tap tempo (appele depuis la reception ESP-NOW)
static bool phareTap(uint8_t* data, uint8_t len) {
  if (len != 13 || (data[0] != 0x81 && data[0] != 0x80) || data[6] != 19) return false;
  bool connu = false;
  for (const auto& mac : linked_remotes) if (strlen(mac.data()) == 12 && strcmp(last_signal_src, mac.data()) == 0) { connu = true; break; }
  if (!connu) return false;
  uint32_t seq = data[1] | (data[2] << 8) | (data[3] << 16) | ((uint32_t)data[4] << 24);
  if (seq == T.dernierSeqTap) return true;              // la telecommande repete chaque message
  T.dernierSeqTap = seq;
  uint32_t now = millis();
  if (T.nTap > 0 && now - T.taps[T.nTap - 1] > 1500) T.nTap = 0;
  if (T.nTap < 4) T.taps[T.nTap++] = now;
  return true;
}

// tap tempo depuis la page de controle
static void phareTapNow() {
  uint32_t now = millis();
  if (T.nTap > 0 && now - T.taps[T.nTap - 1] > 1500) T.nTap = 0;
  if (T.nTap < 4) T.taps[T.nTap++] = now;
}

// donnees du micro (ou son simule si le micro est absent)
static um_data_t* phareAudio() {
  um_data_t *um_data;
  if (!UsermodManager::getUMData(&um_data, USERMOD_ID_AUDIOREACTIVE)) um_data = simulateSound(SEGMENT.soundSim);
  return um_data;
}

// prepare l'etat, renvoie le temps ecoule depuis la derniere image (ms)
static PhareState* phareInit(uint32_t &dt) {
  if (!SEGENV.allocateData(sizeof(PhareState))) return nullptr;
  PhareState* s = reinterpret_cast<PhareState*>(SEGENV.data);
  if (SEGENV.call == 0) {
    memset(s, 0, sizeof(PhareState));
    s->bassAvg = 40.0f;
    s->trebAvg = 40.0f;
    s->lastFrame = strip.now;
  }
  dt = strip.now - s->lastFrame;
  if (dt > 100) dt = 100;
  s->lastFrame = strip.now;
  return s;
}

// seuil : en dessous, le son est ignore (curseur Seuil = custom2)
static float phareSeuil(float v) { float t = SEGMENT.custom2 * 0.5f; return v > t ? (v - t) * 255.0f / (255.0f - t) : 0.0f; }

// basses : canaux 0 a 2 (environ 40 a 200 Hz)
static float phareBass(const uint8_t* fft) { return (fft[0] + fft[1] + fft[2]) / 3.0f; }
// aigus : canaux 13 a 15 (environ 3,7 a 9 kHz : charleston, cymbales)
static float phareTreb(const uint8_t* fft) { return (fft[13] + fft[14] + fft[15]) / 3.0f; }

// detection d'un coup : le son depasse nettement sa moyenne recente
static bool phareBeat(float val, float &avg, uint32_t &last, uint8_t sensitivity, uint16_t minGapMs) {
  float k = 2.2f - (sensitivity / 255.0f) * 1.1f;   // sensibilite max -> seuil 1,1 x la moyenne
  bool beat = (val > avg * k) && (val > 12.0f) && (strip.now - last > minGapMs);
  if (beat) last = strip.now;
  avg = avg * 0.96f + val * 0.04f;
  if (avg < 8.0f) avg = 8.0f;
  return beat;
}

// fait retomber toutes les ampoules ; retombeeMs = temps pour passer de 100 % a 0
static void phareDecay(PhareState* s, unsigned n, uint32_t dt, float retombeeMs) {
  float d = dt / retombeeMs;
  for (unsigned i = 0; i < n; i++) {
    s->level[i] -= d;
    if (s->level[i] < 0.0f) s->level[i] = 0.0f;
  }
}


// DJ : au plus 3 lampes fortes en meme temps (les 4 a fond sont reservees au drop)
static void phareLimite3(float* lv, unsigned n) {
  if (!Rd.max3 || n < 4) return;
  unsigned fortes = 0, faible = 0;
  for (unsigned i = 0; i < n; i++) { if (lv[i] > 0.5f) fortes++; if (lv[i] <= lv[faible]) faible = i; }
  if (fortes >= n) lv[faible] = min(lv[faible], 0.15f);
}

static float phareSmooth(float x) { x = constrain(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); }
static float phareRetombee(uint8_t speed) { return map(speed, 0, 255, 1500, 80); }

// rendu commun a tous les effets : veilleuse, fondu du VJ, douceur, couches, drop, controle a la main
static void phareRendu(const float* lv0, unsigned n, uint8_t veilleuse) {
  uint32_t c = SEGCOLOR(0) ? SEGCOLOR(0) : WHITE;
  uint32_t now = millis();
  static float filt[PHARE_MAX]; static uint32_t prec = 0;
  float dt = min(100.0f, (float)(now - prec)); prec = now;
  float lv[PHARE_MAX];
  if (!Rd.vj && phareOverride && !phareExpo) Rd.couches = 1.0f;   // Override : aussi sur les effets musicaux
  // son coupe : extinction en 0,3 s apres 0,3 s de silence (sauf ambiances Expo)
  static float gainSilence = 1.0f;
  bool coupe = !phareExpo && (Rd.vj ? now - T.sonDepuis > 1500 : now - T.silenceDepuis > 300);   // Auto : 1,5 s de vrai silence
  gainSilence = coupe ? max(0.0f, gainSilence - dt / 300.0f) : min(1.0f, gainSilence + dt / 120.0f);
  for (unsigned i = 0; i < n; i++) lv[i] = max(lv0[i], veilleuse / 255.0f) * Rd.gain * gainSilence;
  // douceur (VJ) / mode calme : le fond devient une veilleuse, montees et descentes progressives
  float dou = Rd.calme ? max(Rd.douceur, 0.8f) : Rd.douceur;
  if (Rd.vj && O.variante == 6) dou = max(dou, 0.6f);                                       // variante notes tenues
  float tau = 15.0f + dou * dou * 700.0f;
  for (unsigned i = 0; i < n; i++) {
    float cible = lv[i] * (1.0f - 0.45f * dou) * (Rd.calme ? 0.7f : 1.0f);
    filt[i] += (cible - filt[i]) * min(1.0f, dt / tau);
    lv[i] = filt[i];
  }
  // l'effet change de lampes : bouton Inverser (partout), et en VJ une disposition par phrase
  { uint8_t perm = (Rd.vj && DR.etat == 0) ? O.perm : 0;
    if (phareInverse) perm = perm == 0 ? 1 : (perm == 1 ? 0 : perm);
    if (perm && n >= 2) { float t[PHARE_MAX]; for (unsigned i = 0; i < n; i++) t[i] = lv[i];
      static const uint8_t milieu[4] = {1, 2, 0, 3};                                          // l'effet part du milieu
      for (unsigned i = 0; i < n; i++) {
        unsigned src = perm == 1 ? n - 1 - i : (perm == 2 ? (i + 2) % n : i);
        if (perm == 3 && n == 4) { for (unsigned k = 0; k < 4; k++) if (milieu[k] == i) src = k; }
        lv[i] = t[src];
      } } }
  bool vjOrch = Rd.vj && DR.etat == 0 && phareCale();
  if (vjOrch) {
    if (O.formation == 1 && now - T.kickFlash < (uint32_t)(T.period * 1.2f)) for (unsigned i = 0; i < n; i++) lv[i] = 0.0f;   // kick seul (si le kick est bien la)
    if (O.variante == 5) { float a = (T.beatCount % 4 == 0) ? 1.0f : 0.4f; for (unsigned i = 0; i < n; i++) lv[i] *= a; }   // accent sur le 1
    if (O.variante == 7) for (unsigned i = 0; i < n; i++) lv[i] = lv[i] > 0.35f ? 1.0f : 0.0f;                                 // coups secs
  }
  if (!phareSurcouche(lv, n)) {
    phareCouches(lv, n);
    if (vjOrch) {
      uint32_t p = T.beatCount % 32; float frac = constrain((now - T.lastTick) / T.period, 0.0f, 0.999f);
      if (O.mesureVide >= 0 && (int)(p / 4) == O.mesureVide) { for (unsigned i = 0; i < n; i++) lv[i] = 0.0f; lv[min((unsigned)O.lampeVide, n - 1)] = 0.15f; }   // mesure presque vide
      if (O.fill && (p == 29 || p == 30)) {                                                    // fill de fin de phrase
        float x = (p - 29 + frac) * 8.0f; unsigned st = (unsigned)x;
        for (unsigned i = 0; i < n; i++) lv[i] = 0.0f;
        if (O.fill == 1) lv[st % n] = 1.0f;                                                    // roll en doubles-croches
        else if (st % 2 == 0) { lv[min(1u, n - 1)] = 1.0f; lv[min(2u, n - 1)] = 1.0f; }        // stroboscope sur 2 lampes
      }
      if (O.noirFin && p == 31) for (unsigned i = 0; i < n; i++) lv[i] = 0.0f;                 // un temps de noir avant la phrase
      for (unsigned i = 0; i < n; i++) lv[i] *= O.niveau;                                       // couplet plus bas que le refrain
      bool noirVoulu = O.noirFin && p == 31;
      float mx = 0; for (unsigned i = 0; i < n; i++) mx = max(mx, lv[i]);
      if (!noirVoulu && mx < 0.08f && now - T.sonDepuis < 1500) lv[min((unsigned)O.lampeVide, n - 1)] = 0.08f;   // jamais tout noir tant qu'il y a du son
    }
    phareLimite3(lv, n);
  }
  // a la main : le fader deplace la lumiere, les boutons allument les lampes
  float m = phareMainPos;
  if (m >= 0.0f && now - phareMainT < 1500) {
    float y = m * (n - 1);
    for (unsigned i = 0; i < n; i++) lv[i] = max(0.0f, 1.0f - fabsf((float)i - y) * 0.9f);
  }
  // premier allumage : demarre comme un vieux neon (~2 s)
  static uint32_t neonDebut = 0; static bool neonFini = false;
  if (!neonFini) {
    if (!neonDebut) neonDebut = now;
    uint32_t t = now - neonDebut;
    if (t > 2200) neonFini = true;
    else {
      float p = t / 2200.0f;                                     // de plus en plus souvent allume
      bool on = hw_random8() < (uint8_t)(40 + p * 200) && (t % 300 > 40 || p > 0.8f);
      for (unsigned i = 0; i < n; i++) lv[i] = on ? (0.4f + 0.6f * p) * (0.7f + hw_random8() / 850.0f) : 0.0f;
    }
  }
  uint8_t pads = pharePads;
  for (unsigned i = 0; i < n && i < 8; i++) if (pads & (1 << i)) lv[i] = 1.0f;
  for (unsigned i = 0; i < n; i++) { SEGMENT.setPixelColor(i, color_fade(c, phareOut(lv[i]))); if (i < 4) phareSortie[i] = (uint8_t)(constrain(lv[i], 0.0f, 1.0f) * 255); }
  for (unsigned i = 0; i < n && i < 4; i++) phareRepartition[i] += (constrain(lv[i], 0.0f, 1.0f) - phareRepartition[i]) * min(1.0f, dt / 300000.0f);   // moyenne sur ~5 min
  phareRenduReset();                                      // image suivante : contexte a zero (sauf le VJ en cours)
}
static void phareDraw(PhareState* s, unsigned n, uint8_t veilleuse) { phareRendu(s->level, n, veilleuse); }
static void phareShow(const float* lv0, unsigned n, uint8_t veilleuse) { phareRendu(lv0, n, veilleuse); }

static unsigned phareN() { return min((unsigned)SEGLEN, (unsigned)PHARE_MAX); }
static float phareStepMs(uint8_t speed) { return map(speed, 0, 255, 1200, 60); }

// ---- lecture du son, adaptative : chaque bande est comparee a son propre maximum recent ----
// Sensibilite (curseur 2) : plus haut = reagit aux sons plus faibles
// Seuil (curseur 4)       : en dessous, le son est ignore (bruit de fond, voix)
struct PhareSon { float b, m, t, v; bool bb, mb, tb; };

static PhareSon phareEcoute(PhareState* s, uint32_t dt) {
  um_data_t* um = phareAudio();
  uint8_t* fft = (uint8_t*)um->u_data[2];
  float raw[4] = {
    (fft[0] + fft[1]) / 2.0f,                            // basses (kick : ~40-90 Hz)
    (fft[5] + fft[6] + fft[7] + fft[8] + fft[9]) / 5.0f,  // mediums
    (fft[12] + fft[13] + fft[14] + fft[15]) / 4.0f,       // aigus
    *(float*)um->u_data[0]                                // volume
  };
  { float g = phareGainSon(); for (int i = 0; i < 4; i++) raw[i] = min(255.0f, raw[i] * g); }
  float sens = phareSens / 255.0f;                      // sensibilite generale
  float plancher = 0.45f - sens * 0.35f;                // part du maximum consideree comme "rien"
  float k = 2.0f - sens * 0.8f;                         // force d'un coup par rapport a la moyenne
  float decroit = powf(0.5f, dt / 4000.0f);             // le maximum retombe de moitie en 4 s
  float out[4]; bool beat[4];
  for (int i = 0; i < 4; i++) {
    float v = phareSeuil(raw[i]);
    if (SEGENV.call == 0) { s->pk[i] = 40.0f; s->av[i] = 20.0f; }
    s->pk[i] = max(v, max(s->pk[i] * decroit, 25.0f));
    // "punch" : ce qui depasse la moyenne recente ; un son continu (basse tenue) retombe a 0
    float marge = max(s->pk[i] - s->av[i], 12.0f);
    float n = (v - s->av[i]) / marge;
    out[i] = constrain((n - plancher * 0.5f) / (1.0f - plancher * 0.5f), 0.0f, 1.0f);
    float attaque = max(0.0f, v - s->pv[i]);             // de combien le son vient de monter
    s->pv[i] = v;
    beat[i] = attaque > s->fa[i] * (k + 0.8f) + 6.0f && v > s->pk[i] * 0.45f
              && strip.now - s->lb[i] > (i == 0 ? 200u : 90u);
    if (beat[i]) s->lb[i] = strip.now;
    s->fa[i] = s->fa[i] * 0.93f + attaque * 0.07f;
    s->av[i] = s->av[i] * 0.9f + v * 0.1f; if (s->av[i] < 6.0f) s->av[i] = 6.0f;   // moyenne sur ~0,25 s
  }
  return {out[0], out[1], out[2], out[3], beat[0], beat[1], beat[2]};
}


// 01. Musique bass : les 2 du bas suivent les basses, les 2 du haut alternent sur les aigus
static void mode_phare_bass(void) {
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  PhareSon son = phareEcoute(s, dt);
  unsigned half = n / 2;
  phareDecay(s, n, dt, phareRetombee(SEGMENT.speed));
  if (son.bb) for (unsigned i = 0; i < half; i++) s->level[i] = 1.0f;   // le bas : uniquement les coups de basse
  if (son.tb) { s->side = (s->side + 1) % (n - half); s->level[half + s->side] = 1.0f; }
  phareDraw(s, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_BASS[] PROGMEM = "1 Soiree Musique bass@Extinction,,Veilleuse,;!;;1f;sx=200,ix=128,c1=0,c2=30";

// 02. Clignotant musical : ampoule 2 = basses, 1 et 3 = mediums (en alternance), 4 = aigus
static void mode_phare_clignomusic(void) {
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  PhareSon son = phareEcoute(s, dt);
  phareDecay(s, n, dt, phareRetombee(SEGMENT.speed));
  unsigned lb = min(1u, n - 1), lt = n - 1;
  if (son.bb) s->level[lb] = 1.0f;                       // ampoule 2 : uniquement les coups de basse
  if (son.mb) {                                          // mediums : les autres ampoules, chacune son tour
    unsigned tries = 0;
    do { s->side = (s->side + 1) % n; } while ((s->side == lb || s->side == lt) && ++tries < n);
    if (s->side != lb && s->side != lt) s->level[s->side] = 1.0f;
  }
  if (son.tb && lt != lb) s->level[lt] = 1.0f;
  phareDraw(s, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_CLIGNOMUSIC[] PROGMEM = "1 Soiree Clignotant musical@Extinction,,Veilleuse,;!;;1f;sx=190,ix=128,c1=0,c2=30";

// 03. Roll basse : la lumiere monte d'une ampoule a chaque coup de basse
static void mode_phare_roll_bass(void) {
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  PhareSon son = phareEcoute(s, dt);
  phareDecay(s, n, dt, phareRetombee(SEGMENT.speed));
  if (son.bb) { s->pos = (s->pos + 1) % n; s->level[s->pos] = 1.0f; }
  phareDraw(s, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_ROLL_BASS[] PROGMEM = "1 Soiree Roll basse@Extinction,,Veilleuse,;!;;1f;sx=120,ix=128,c1=20,c2=30";

// 04. Flamme musicale : braise en bas, la flamme monte avec la musique et vacille en haut
static void mode_phare_flamme(void) {
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  PhareSon son = phareEcoute(s, dt);
  float cible = max(son.v, son.b);
  float up = 0.15f + SEGMENT.speed / 255.0f * 0.6f;
  if (cible > s->env) s->env += (cible - s->env) * up;
  else s->env = max(0.0f, s->env - dt / 900.0f);       // retombe en ~1 s
  float h = 0.6f + s->env * (n - 0.6f);                 // hauteur de flamme, en ampoules
  float braise = SEGMENT.custom1 / 255.0f;
  float lv[PHARE_MAX];
  for (unsigned i = 0; i < n; i++) {
    float f = constrain(h - i, 0.0f, 1.0f);
    float vac = 1.0f - (hw_random8() / 255.0f) * 0.45f;                 // toute la colonne vacille
    lv[i] = f * vac * (0.25f + 0.75f * s->env);
  }
  lv[0] = max(lv[0], braise * (0.7f + hw_random8() / 850.0f));
  phareShow(lv, n, 0);
}
static const char _data_FX_MODE_PHARE_FLAMME[] PROGMEM = "1 Soiree Flamme musicale@Reactivite,,Braise,;!;;1v;sx=128,ix=128,c1=40,c2=30";

// 05. Explosion : chaque basse allume tout, puis ca s'eteint du haut vers le bas
static void mode_phare_explosion(void) {
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  PhareSon son = phareEcoute(s, dt);
  float T = phareRetombee(SEGMENT.speed);
  for (unsigned i = 0; i < n; i++) {                    // le haut retombe plus vite que le bas
    float Ti = T * (1.0f - 0.65f * (float)i / max(1u, n - 1));
    s->level[i] = max(0.0f, s->level[i] - dt / Ti);
  }
  if (son.bb) for (unsigned i = 0; i < n; i++) s->level[i] = 1.0f;
  phareDraw(s, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_EXPLOSION[] PROGMEM = "1 Soiree Explosion@Extinction,,Veilleuse,;!;;1f;sx=140,ix=128,c1=0,c2=30";

// 06. Artifice : sur chaque son, des ampoules au hasard petillent tres vite
static void mode_phare_artifice(void) {
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  PhareSon son = phareEcoute(s, dt);
  phareDecay(s, n, dt, map(SEGMENT.speed, 0, 255, 400, 50));
  if (son.bb) { s->side = 3 + hw_random8() % 4; s->t1 = strip.now; }   // une basse lance une gerbe
  if (son.mb || son.tb) s->level[hw_random8() % n] = 1.0f;
  if (s->side > 0 && strip.now >= s->t1) {                            // petillements de la gerbe
    s->level[hw_random8() % n] = 0.6f + hw_random8() / 640.0f;
    s->side--; s->t1 = strip.now + 30 + hw_random8() % 70;
  }
  phareDraw(s, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_ARTIFICE[] PROGMEM = "1 Soiree Artifice@Brievete,,Veilleuse,;!;;1f;sx=180,ix=140,c1=0,c2=30";

// 08. Niveau : la lumiere monte avec le volume, un point reste en haut puis retombe
static void mode_phare_vumetre(void) {
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  PhareSon son = phareEcoute(s, dt);
  float cible = max(son.v, son.b) * n;
  if (cible > s->env) s->env = cible; else s->env = max(0.0f, s->env - dt / 250.0f);
  if (s->env >= s->q) { s->q = s->env; s->t0 = strip.now; }              // nouveau pic
  else if (strip.now - s->t0 > 400) s->q = max(0.0f, s->q - dt / map(SEGMENT.speed, 0, 255, 900, 120)); // retombe
  float lv[PHARE_MAX];
  unsigned pic = (unsigned)min((float)(n - 1), max(0.0f, s->q - 0.5f));
  for (unsigned i = 0; i < n; i++) lv[i] = constrain(s->env - i, 0.0f, 1.0f) * 0.8f;
  if (s->q > 0.6f && s->q > s->env + 0.3f) lv[pic] = 1.0f;          // le pic, seulement au-dessus de la barre
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_VUMETRE[] PROGMEM = "1 Soiree Niveau@Chute du pic,,Veilleuse,;!;;1v;sx=128,ix=128,c1=0,c2=30";

// 08. Echo : chaque basse allume le bas, l'echo remonte en s'affaiblissant
static void mode_phare_echo(void) {
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  PhareSon son = phareEcoute(s, dt);
  const unsigned maxEchos = 4;
  if (SEGENV.call == 0) for (unsigned e = 0; e < maxEchos; e++) s->tgt[e] = -1.0f;
  float vit = (2.0f + SEGMENT.speed / 255.0f * 10.0f) * dt / 1000.0f;   // ampoules par seconde
  if (son.bb) for (unsigned e = 0; e < maxEchos; e++) if (s->tgt[e] < 0) { s->tgt[e] = 0.0f; break; }
  float lv[PHARE_MAX]; for (unsigned i = 0; i < n; i++) lv[i] = 0.0f;
  for (unsigned e = 0; e < maxEchos; e++) {
    if (s->tgt[e] < 0) continue;
    float y = s->tgt[e], amp = powf(0.55f, y);          // s'affaiblit en montant
    for (unsigned i = 0; i < n; i++) lv[i] = max(lv[i], amp * max(0.0f, 1.0f - fabsf((float)i - y)));
    s->tgt[e] += vit; if (s->tgt[e] > n) s->tgt[e] = -1.0f;
  }
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_ECHO[] PROGMEM = "1 Soiree Echo (ancien)@Vitesse,,Veilleuse,;!;;1f;sx=110,ix=128,c1=0,c2=30";

// 31. All bass : les 4 lampes s'allument et s'eteignent ensemble sur chaque kick
static void mode_phare_allbass(void) {
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  PhareSon son = phareEcoute(s, dt);
  phareDecay(s, n, dt, phareRetombee(SEGMENT.speed));
  if (son.bb) for (unsigned i = 0; i < n; i++) s->level[i] = 1.0f;
  phareDraw(s, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_ALLBASS[] PROGMEM = "1 Soiree All bass@Extinction,,Veilleuse,;!;;1f;sx=200,ix=128,c1=0,c2=30";

// Veilleuse voix (DJ, quand il y a du son sans kick) : lumiere basse qui suit la voix / la melodie,
// chaque lampe avec un petit retard sur la precedente (la vague monte, ou descend avec la case 1)
static void mode_phare_veilleuse(void) {
  phareAnim();
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  uint8_t* fft = (uint8_t*)phareAudio()->u_data[2];
  float voix = 0; for (int b = 3; b <= 10; b++) voix += fft[b]; voix /= 8.0f;          // ~200 Hz a 2 kHz
  s->q = max(voix, max(s->q * powf(0.5f, dt / 6000.0f), 20.0f));                       // maximum recent
  float cible = constrain((voix / s->q - 0.25f) / 0.75f, 0.0f, 1.0f);
  s->env += (cible - s->env) * min(1.0f, dt / (cible > s->env ? 90.0f : 450.0f));       // attaque douce, retombee lente
  // historique de l'enveloppe : une case toutes les 50 ms (16 cases = 0,8 s)
  if (strip.now - s->t0 >= 50) { s->t0 = strip.now; s->pos = (s->pos + 1) % PHARE_MAX; s->tgt[s->pos] = s->env; }
  float maxi = 0.12f + SEGMENT.custom1 / 255.0f * 0.4f;
  float lv[PHARE_MAX];
  for (unsigned i = 0; i < n; i++) {
    unsigned rang = SEGMENT.check1 ? n - 1 - i : i;                                     // ordre de la vague
    unsigned retard = min(rang * 3u, (unsigned)PHARE_MAX - 1);                          // 150 ms par lampe
    float e = s->tgt[(s->pos + PHARE_MAX - retard) % PHARE_MAX];
    float fond = 0.05f + 0.03f * sinf(strip.now / 2500.0f + i * 1.3f);                   // respiration tres lente
    lv[i] = fond + e * maxi;
  }
  phareShow(lv, n, 0);
}

// instant du dernier kick (predit sur la grille quand le tempo est cale, sinon detecte)
static uint32_t phareDernierKick() { return T.kickFlash; }
static float phareImpulsion(uint32_t depuis, float dureeMs) {      // flash qui retombe
  uint32_t now = millis();
  if (!depuis || now < depuis) return 0.0f;
  return expf(-(float)(now - depuis) / max(dureeMs, 10.0f));
}

// 32. Gradient veilleuse : un degrade qui se deplace doucement, sans jamais s'eteindre
static void mode_phare_gradient(void) {
  phareAnim();
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  float period = map(SEGMENT.speed, 0, 255, 12000, 2000);
  if (Rd.reactif && phareCale()) period = 8000.0f;            // 16 temps
  float amp = 0.15f + SEGMENT.intensity / 255.0f * 0.6f;
  float plancher = 0.05f + SEGMENT.custom1 / 255.0f * 0.4f;
  float x = (phareNow() % (uint32_t)period) / period;
  float lv[PHARE_MAX];
  for (unsigned i = 0; i < n; i++) {
    float ph = (float)(SEGMENT.check1 ? n - 1 - i : i) / n;
    float w = 0.5f + 0.5f * sinf(2.0f * PI * (x - ph * 0.5f));
    lv[i] = plancher + amp * w * w;
  }
  phareShow(lv, n, 0);
}
static const char _data_FX_MODE_PHARE_GRADIENT[] PROGMEM = "2 Expo Gradient veilleuse@Vitesse,Amplitude,Plancher,,,Sens inverse,;!;;1;sx=60,ix=140,c1=40";

// 33. Duos : deux ampoules ensemble, une paire par temps (bas, milieu, haut, extremes)
static void mode_phare_duos(void) {
  phareAnim();
  unsigned n = phareN(); if (n < 4) FX_FALLBACK_STATIC;
  static const uint8_t paires[6][2] = {{0, 1}, {1, 2}, {2, 3}, {0, 3}, {0, 2}, {1, 3}};
  float b;
  if (Rd.reactif && phareCale()) b = phareTempsGrille();
  else b = strip.now / phareStepMs(SEGMENT.speed) / 2.0f;
  uint32_t pas = (uint32_t)b; float f = b - floorf(b);
  uint8_t p = SEGMENT.check1 ? (uint8_t)(((pas * 2654435761u) >> 16) % 6) : pas % 6;      // ordre fixe ou au hasard (6 paires)
  float fondu = 0.05f + SEGMENT.intensity / 255.0f * 0.5f;
  float lv[PHARE_MAX]; for (unsigned i = 0; i < n; i++) lv[i] = 0.0f;
  float a = 1.0f - phareSmooth((f - (1.0f - fondu)) / fondu);                           // s'eteint a la fin du temps
  lv[paires[p][0]] = a; lv[paires[p][1]] = a;
  Rd.sansKick = true;
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_DUOS[] PROGMEM = "1 Soiree Duos@Vitesse,Fondu,Veilleuse,,,Ordre au hasard,;!;;1;sx=150,ix=60,c1=0";

// 34. Batterie : kick sur la lampe 1, caisse claire sur la 4, hi-hats en petits eclats sur la 2 et la 3
static void mode_phare_batterie(void) {
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 4) FX_FALLBACK_STATIC;
  float ext = map(SEGMENT.speed, 0, 255, 600, 80);
  if (T.lastHat != s->lastTreb) { s->lastTreb = T.lastHat; s->side ^= 1; }             // hi-hats : 2 et 3 a tour de role
  float lv[PHARE_MAX]; for (unsigned i = 0; i < n; i++) lv[i] = 0.0f;
  lv[0] = phareImpulsion(phareDernierKick(), ext);
  lv[n - 1] = phareImpulsion(T.lastSnare, ext * 0.6f);
  lv[1 + s->side] = 0.55f * phareImpulsion(T.lastHat, 60.0f);
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_BATTERIE[] PROGMEM = "1 Soiree Batterie@Extinction,,Veilleuse;!;;1f;sx=150,c1=0";

// 35. Question / reponse : une mesure le bas (1+2) joue sur le kick, la suivante le haut (3+4) repond
static void mode_phare_question(void) {
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 4) FX_FALLBACK_STATIC;
  float ext = map(SEGMENT.speed, 0, 255, 600, 80);
  uint32_t k = phareDernierKick();
  if (k != s->lastBeat) { s->lastBeat = k; s->q += 1.0f; }                             // compte les kicks
  bool haut = phareCale() ? (T.beatCount / 4) % 2 : ((uint32_t)s->q / 4) % 2;
  if (SEGMENT.check1) haut = !haut;
  float e = phareImpulsion(k, ext);
  float lv[PHARE_MAX]; for (unsigned i = 0; i < n; i++) lv[i] = 0.0f;
  unsigned a = haut ? n - 2 : 0;
  lv[a] = e; lv[a + 1] = e;
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_QUESTION[] PROGMEM = "1 Soiree Question / reponse@Extinction,,Veilleuse,,,Commencer par le haut;!;;1f;sx=150,c1=0";

// 36. Ricochet : chaque kick allume la lampe 1, puis rebondit vers le haut en s'affaiblissant
static void mode_phare_ricochet(void) {
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  uint32_t k = phareDernierKick();
  float pas = T.period / (SEGMENT.check1 ? 2.0f : 4.0f);                                // doubles-croches (ou croches)
  float duree = pas * 0.7f;
  float lv[PHARE_MAX];
  for (unsigned i = 0; i < n; i++) {
    float force = powf(0.6f, (float)i) * (1.0f - SEGMENT.intensity / 255.0f * 0.3f * i);
    lv[i] = max(0.0f, force) * phareImpulsion(k + (uint32_t)(pas * i), duree);
  }
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_RICOCHET[] PROGMEM = "1 Soiree Echo@,,Veilleuse,,,Plus lent (croches);!;;1f;ix=60,c1=0";

// 37. Scintillement : le kick tient la lampe 1, les hi-hats font scintiller la 3 et la 4
static void mode_phare_scintillement(void) {
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 3) FX_FALLBACK_STATIC;
  float tenue = T.period * (0.4f + SEGMENT.speed / 255.0f * 0.8f);
  if (T.lastHat != s->lastTreb) { s->lastTreb = T.lastHat; s->side = hw_random8() & 1; }
  float lv[PHARE_MAX]; for (unsigned i = 0; i < n; i++) lv[i] = 0.0f;
  lv[0] = phareImpulsion(phareDernierKick(), tenue);
  lv[n - 2 + s->side] = phareImpulsion(T.lastHat, 35.0f);
  if (T.lastSnare > T.lastHat) lv[n - 1] = max(lv[n - 1], phareImpulsion(T.lastSnare, 50.0f));
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_SCINTILLEMENT[] PROGMEM = "1 Soiree Scintillement@Tenue du kick,,Veilleuse;!;;1f;sx=128,c1=0";

// 38. Notes : la lumiere suit la melodie (contour : monte quand la melodie monte, descend quand elle descend)
//     accords -> 2 ou 3 lampes ; notes tenues -> lueur douce et etalee ; notes piquees -> eclats
static void mode_phare_notes(void) {
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  float ext = map(SEGMENT.speed, 0, 255, 2500, 250);
  if (M.attaque != s->lastBeat) {                                                  // nouvelle note
    s->lastBeat = M.attaque;
    unsigned k = min((unsigned)(M.pos * (n - 1) / 3.0f + 0.5f), n - 1);
    bool piquee = M.attaque - M.attaquePrec < 250;
    s->level[k] = 1.0f;
    if (M.accord >= 2 && k + 1 < n) s->level[k + 1] = max(s->level[k + 1], 0.7f);    // accord : la lampe voisine aussi
    if (M.accord >= 3 && k >= 1) s->level[k - 1] = max(s->level[k - 1], 0.6f);
    s->q = piquee ? 1.0f : 0.0f; s->pos = k;
  }
  float plancher = SEGMENT.custom1 / 255.0f * 0.4f;
  float lv[PHARE_MAX];
  for (unsigned i = 0; i < n; i++) {
    float d = fabsf((float)i - s->pos);
    if (M.tenue) {                                                                   // note tenue : douce et etalee
      float cible = 0.55f * max(0.0f, 1.0f - d * 0.6f) * (0.5f + 0.5f * M.presence);
      s->level[i] += (cible - s->level[i]) * min(1.0f, dt / 600.0f);
    } else s->level[i] = max(0.0f, s->level[i] - dt / (s->q > 0.5f ? ext * 0.4f : ext));   // piquee : eclat plus court
    lv[i] = plancher + (0.9f - plancher) * s->level[i];
  }
  phareShow(lv, n, 0);
}
static const char _data_FX_MODE_PHARE_NOTES[] PROGMEM = "1 Soiree Notes@Extinction,,Veilleuse;!;;1f;sx=128,ix=160,c1=40";

// ---- Memoire : chaque coup laisse une trace qui s'eteint lentement (Soiree : sur le son ; Expo : gouttes au hasard)
static void phareMemoire(bool son) {
  if (son) phareAnim();
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  float ext = map(SEGMENT.speed, 0, 255, 6000, 800);                 // temps d'extinction d'une trace
  for (unsigned i = 0; i < n; i++) s->level[i] = max(0.0f, s->level[i] - dt / ext);
  if (son) {
    if (T.kickFlash != s->lastBeat) { s->lastBeat = T.kickFlash; unsigned k = hw_random8() % min(2u, n); s->level[k] = 1.0f; }
    if (T.lastSnare != s->t0) { s->t0 = T.lastSnare; s->level[n - 1] = max(s->level[n - 1], 0.9f); }
    if (T.lastHat != s->t1) { s->t1 = T.lastHat; if (hw_random8() < 70 && n > 2) { unsigned k = 1 + hw_random8() % (n - 2); s->level[k] = max(s->level[k], 0.5f); } }
  } else if (hw_random16() < 40 + SEGMENT.intensity * 3) {          // gouttes de lumiere au hasard
    unsigned k = hw_random8() % n; s->level[k] = max(s->level[k], 0.5f + hw_random8() / 512.0f);
  }
  phareDraw(s, n, SEGMENT.custom1);
}
static void mode_phare_memoire(void)   { phareMemoire(true); }
static void mode_phare_memoire_e(void) { phareMemoire(false); }
static const char _data_FX_MODE_PHARE_MEMOIRE[]   PROGMEM = "1 Soiree Memoire@Extinction,,Veilleuse;!;;1f;sx=120,c1=0";
static const char _data_FX_MODE_PHARE_MEMOIRE_E[] PROGMEM = "2 Expo Memoire@Extinction,Gouttes,Veilleuse;!;;1;sx=110,ix=60,c1=10";

// ---- Pendule : un balancement fluide avec ralentis aux extremites (cale sur 2 temps quand ca suit le tempo)
static void pharePendule(bool son) {
  if (son) phareAnim();
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  float ph;
  if (son && Rd.reactif && phareCale()) ph = phareNow() / 1000.0f;   // phareNow : 500 par temps -> un aller en 2 temps
  else ph = strip.now / (float)map(SEGMENT.speed, 0, 255, 6000, 600);
  float y = (n - 1) * (0.5f - 0.5f * cosf(PI * ph));
  float larg = 0.3f + SEGMENT.intensity / 255.0f * 1.0f;
  float lv[PHARE_MAX];
  for (unsigned i = 0; i < n; i++) {
    float d = ((float)i - y) / larg, b = expf(-d * d);
    s->level[i] = max(b, s->level[i] - dt / 400.0f);                  // petite trainee
    lv[i] = s->level[i];
  }
  phareShow(lv, n, SEGMENT.custom1);
}
static void mode_phare_pendule(void)   { pharePendule(true); }
static void mode_phare_pendule_e(void) { pharePendule(false); }
static const char _data_FX_MODE_PHARE_PENDULE[]   PROGMEM = "1 Soiree Pendule@Vitesse,Largeur,Veilleuse;!;;1;sx=128,ix=60,c1=0";
static const char _data_FX_MODE_PHARE_PENDULE_E[] PROGMEM = "2 Expo Pendule@Vitesse,Largeur,Veilleuse;!;;1;sx=70,ix=70,c1=10";

// ---- Gonflement : la lumiere enfle du bas vers le haut sur 8 mesures, puis retombe d'un coup
static void phareGonflement(bool son) {
  if (son) phareAnim();
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  float p;
  if (son && Rd.reactif && phareCale()) p = fmodf(phareNow() / 500.0f, 32.0f) / 32.0f;   // 8 mesures
  else { uint32_t per = map(SEGMENT.speed, 0, 255, 60000, 8000); p = (strip.now % per) / (float)per; }
  float e = powf(p, 1.6f);
  float lv[PHARE_MAX];
  for (unsigned i = 0; i < n; i++) lv[i] = constrain(e * (n + 0.5f) - i, 0.0f, 1.0f) * (0.25f + 0.75f * e);
  phareShow(lv, n, SEGMENT.custom1);
}
static void mode_phare_gonflement(void)   { phareGonflement(true); }
static void mode_phare_gonflement_e(void) { phareGonflement(false); }
static const char _data_FX_MODE_PHARE_GONFLEMENT[]   PROGMEM = "1 Soiree Gonflement@Duree,,Veilleuse;!;;1;sx=128,c1=0";
static const char _data_FX_MODE_PHARE_GONFLEMENT_E[] PROGMEM = "2 Expo Gonflement@Duree,,Veilleuse;!;;1;sx=100,c1=10";

// ---- Sequenceur : motifs rythmiques en 16 pas par mesure, cales sur la grille (le plus "dansant")
//      chaque pas = quelles lampes s'allument (bit 0 = lampe 1 ... bit 3 = lampe 4)
#define PHARE_NB_MOTIFS 10
static uint8_t phareSeqMotif = 0;           // motif choisi par le VJ a chaque phrase
static const uint8_t phareMotifs[PHARE_NB_MOTIFS][16] PROGMEM = {
  {1,0,4,0, 9,0,4,0, 1,0,4,0, 9,0,4,0},     // four on the floor : kick 1, contretemps 3, clap 4 sur 2 et 4
  {1,0,2,0, 4,0,8,0, 1,0,2,0, 4,0,8,0},     // chenillard en croches
  {1,0,4,0, 2,0,8,0, 1,0,4,0, 2,0,8,0},     // 1-3-2-4
  {8,0,2,0, 4,0,1,0, 8,0,2,0, 4,0,1,0},     // 4-2-3-1
  {9,0,0,2, 0,0,4,0, 9,0,0,2, 0,0,4,0},     // syncope
  {1,0,6,0, 8,0,6,0, 1,0,6,0, 8,0,6,0},     // ping-pong extremes, milieu en contretemps
  {1,0,6,0, 1,0,6,0, 1,0,6,0, 1,0,6,0},     // house : kick + contretemps au milieu
  {1,2,4,8, 0,0,0,0, 8,4,2,1, 0,0,0,0},     // escalier aller-retour en doubles-croches
  {1,0,0,0, 14,0,0,0, 1,0,0,0, 14,0,0,0},   // clap sur 2 et 4 (3 lampes du haut)
  {3,0,0,12, 0,0,3,0, 12,0,0,3, 0,0,12,0},  // tresillo 3-3-2, bas et haut qui se repondent
};
static void mode_phare_sequenceur(void) {
  phareAnim();
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  float b;
  if (Rd.reactif && phareCale()) b = phareTempsGrille();
  else b = strip.now / (float)map(SEGMENT.speed, 0, 255, 900, 250);   // sans tempo : la vitesse du curseur
  uint8_t motif = Rd.vj ? phareSeqMotif : (uint8_t)(SEGMENT.intensity * PHARE_NB_MOTIFS / 256);
  float pas = b * 4.0f; unsigned st = (unsigned)pas % 16; float f = pas - floorf(pas);
  uint8_t m = pgm_read_byte(&phareMotifs[motif % PHARE_NB_MOTIFS][st]);
  float e = f < 0.6f ? 1.0f : max(0.0f, 1.0f - (f - 0.6f) / 0.4f);      // coup franc, petite queue
  float lv[PHARE_MAX];
  for (unsigned i = 0; i < n; i++) lv[i] = (m >> min(i, 3u)) & 1 ? e : 0.0f;
  Rd.sansKick = true;                                                  // le motif fait deja le kick
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_SEQUENCEUR[] PROGMEM = "1 Soiree Sequenceur@Vitesse,Motif,Veilleuse;!;;1;sx=128,ix=0,c1=0";

// ---- Kick + bass : les extremites (1 et 4) tapent le kick, le milieu (2 et 3) clignote vite et serre sur la basse / les hi-hats
static void mode_phare_kickbass(void) {
  phareAnim();
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 4) FX_FALLBACK_STATIC;
  PhareSon son = phareEcoute(s, dt);
  float ext = map(SEGMENT.speed, 0, 255, 500, 90);
  float kick = phareImpulsion(T.kickFlash, ext);
  // milieu : doubles-croches (sur la grille si le tempo est cale), allumees selon la basse et les hi-hats
  float energie = max(son.b, 0.7f * phareImpulsion(T.lastHat, 90.0f));
  bool pas;
  if (phareCale()) { float b = phareTempsGrille() * 4.0f; pas = ((unsigned)b % 2 == 0) && (b - floorf(b)) < 0.55f; }
  else pas = (strip.now / 45) % 2 == 0;
  float dens = 0.35f + SEGMENT.intensity / 255.0f * 0.65f;
  float milieu = pas ? constrain(energie / max(1.0f - dens, 0.05f) * 0.6f, 0.0f, 1.0f) : 0.0f;
  float lv[PHARE_MAX];
  lv[0] = kick; lv[n - 1] = kick; lv[1] = milieu; lv[2] = milieu;
  Rd.sansKick = true;                                          // le kick est deja dans l'effet
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_KICKBASS[] PROGMEM = "1 Soiree Kick + bass@Extinction,Densite,Veilleuse;!;;1f;sx=150,ix=160,c1=0";

// ---- melange : artifice sur les aigus (max 60 %) + une couche basses ----
// mode basses : 0 = explosion (tout flashe), 1 = bas de colonne, 2 = roll basse, 3 = alterne toutes les 30 s
static void phareMix(PhareState* s, const PhareSon& son, uint32_t dt, unsigned n, uint8_t mode, float* lv) {
  if (mode == 3) {
    if (s->t0 == 0 || strip.now - s->t0 > 30000) { s->stp[0] = (int)(s->stp[0] + 1) % 3; s->t0 = strip.now; }
    mode = (uint8_t)s->stp[0];
  }
  for (unsigned i = 0; i < n; i++) s->tgt[i] = max(0.0f, s->tgt[i] - dt / 90.0f);   // aigus : petillements courts
  if (son.tb || son.mb) s->tgt[hw_random8() % n] = 1.0f;
  if (son.tb && hw_random8() < 100) s->tgt[hw_random8() % n] = 0.8f;
  phareDecay(s, n, dt, mode == 2 ? 450.0f : 320.0f);                             // basses
  if (son.bb) {
    if (mode == 0)      for (unsigned i = 0; i < n; i++) s->level[i] = 1.0f;
    else if (mode == 1) for (unsigned i = 0; i < min(2u, n); i++) s->level[i] = 1.0f;
    else                { s->pos = (s->pos + 1) % n; s->level[s->pos] = 1.0f; }
  }
  for (unsigned i = 0; i < n; i++) lv[i] = max(s->tgt[i] * 0.6f, s->level[i]);
}

static uint8_t phareModeBasses() { return min(3, SEGMENT.custom3 / 8); }

// 07. Artifice + basses
static void mode_phare_mix(void) {
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  PhareSon son = phareEcoute(s, dt);
  float lv[PHARE_MAX];
  phareMix(s, son, dt, n, phareModeBasses(), lv);
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_MIX[] PROGMEM = "1 Soiree Artifice + basses@,,Veilleuse,,;!;;1f;ix=128,c1=0,c2=30,c3=31";

// declenchement automatique de la montee : quand le kick disparait apres un passage rythme (7 fois sur 10)
static void phareDropAuto() {
  uint32_t now = millis();
  bool silence = now - T.silenceDepuis > 1500;
  if (DR.etat == 0 && !phareKickActif() && !silence && !DR.decide && T.lastKick && now - T.lastKick > T.period * 8 && now - T.lastKick < 15000
      && DR.dureeKick > 8000 && now - DR.fin > 3000 && (!DR.manuelT || now - DR.manuelT > 30000)) {
    DR.decide = true;
    if (hw_random8() < 128) phareMontee(false);                // 1 break sur 2
  }
}

// 10. Drop : artifice + basses ; quand le kick disparait : roll qui accelere, noir, puis 4 ou 8 flashs sur le kick
static void mode_phare_drop(void) {
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  PhareSon son = phareEcoute(s, dt);
  phareDropAuto();
  float lv[PHARE_MAX];
  phareMix(s, son, dt, n, phareModeBasses(), lv);
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_DROP[] PROGMEM = "1 Soiree Drop@,,Veilleuse,,;!;;1f;ix=128,c1=0,c2=30,c3=31";

// 10. Respiration musicale : respire lentement, plus fort quand la musique est forte
static void mode_phare_respimusic(void) {
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  PhareSon son = phareEcoute(s, dt);
  s->env += (son.v - s->env) * min(1.0f, dt / 1500.0f);  // volume moyen, lent
  float period = map(SEGMENT.speed, 0, 255, 8000, 2000);
  if (phareMusique()) period = phareTempoMs * 8.0f;      // calee sur 8 temps
  float x = phareMusique() ? phareCycle(8) : (strip.now % (uint32_t)period) / period;
  float b = (expf(sinf(2.0f * PI * x - PI / 2.0f)) - 0.36788f) / 2.35040f;
  float lv[PHARE_MAX];
  for (unsigned i = 0; i < n; i++) lv[i] = 0.08f + b * (0.25f + 0.75f * s->env);
  phareShow(lv, n, 0);
}
static const char _data_FX_MODE_PHARE_RESPIMUSIC[] PROGMEM = "1 Soiree Respiration musicale@Vitesse,,,;!;;1v;sx=100,ix=128,c2=30";

/////////////////////////////////////////////////////////////
//  Animations du phare (sans musique) : remplacent les "_etape"
/////////////////////////////////////////////////////////////

// joue une suite d'ampoules : chacune s'allume en fondu pendant que la precedente s'eteint
static void phareSequence(const uint8_t* seq, unsigned len, unsigned n, float D, float Dlast, float fadeFrac, float* lv) {
  float cycle = D * (len - 1) + Dlast;
  float tc = (float)(phareNow() % (uint32_t)max(cycle, 1.0f));
  unsigned k; float local, dur;
  if (tc < D * (len - 1)) { k = (unsigned)(tc / D); local = tc - k * D; dur = D; }
  else                    { k = len - 1; local = tc - D * (len - 1); dur = Dlast; }
  float F = max(fadeFrac * dur, 1.0f);
  for (unsigned i = 0; i < n; i++) lv[i] = 0.0f;
  uint8_t cur = seq[k], prev = seq[(k + len - 1) % len];
  float in = phareSmooth(local / F);
  if (prev != cur) lv[prev] = max(lv[prev], 1.0f - in);
  lv[cur] = max(lv[cur], in);
}

// Roll : du bas vers le haut en boucle
static void phareRollCore(bool bas);
static void mode_phare_roll(void) { phareRollCore(false); }
static void mode_phare_roll_bas(void) { phareRollCore(true); }
static void phareRollCore(bool bas) {
  phareAnim();
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  uint8_t seq[PHARE_MAX]; for (unsigned i = 0; i < n; i++) seq[i] = bas ? n - 1 - i : i;
  float D = phareStepMs(SEGMENT.speed);
  float Dlast = SEGMENT.check1 ? D * 0.4f : D;
  float lv[PHARE_MAX];
  if (Rd.reactif && phareCale()) {                     // une ampoule par temps, qui change pile sur le beat
    float b = phareTempsGrille();
    unsigned k = (unsigned)b % n, pr = (k + n - 1) % n; float f = b - floorf(b);
    if (bas) { k = n - 1 - k; pr = n - 1 - pr; }
    float in = phareSmooth(f / 0.1f);
    for (unsigned i = 0; i < n; i++) lv[i] = 0.0f;
    lv[k] = in; lv[pr] = max(lv[pr], 1.0f - in);
    Rd.sansKick = true;                                // pas de couche kick : le roll reste lisible
  } else phareSequence(seq, n, n, D, Dlast, 0.1f + SEGMENT.intensity / 255.0f * 0.9f, lv);
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_ROLL[] PROGMEM = "1 Soiree Roll haut@Vitesse,Fondu,Veilleuse,,,Retour eclair,;!;;1;sx=150,ix=120,c1=0,o1=1";
static const char _data_FX_MODE_PHARE_ROLL_BAS[] PROGMEM = "1 Soiree Roll bas@Vitesse,Fondu,Veilleuse,,,Retour eclair,;!;;1;sx=150,ix=120,c1=0,o1=1";

// Rebond : aller-retour bas -> haut -> bas
static void mode_phare_rebond(void) {
  phareAnim();
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  uint8_t seq[2 * PHARE_MAX]; unsigned len = 0;
  for (unsigned i = 0; i < n; i++) seq[len++] = i;
  for (int i = n - 2; i > 0; i--) seq[len++] = i;
  float D = phareStepMs(SEGMENT.speed);
  if (Rd.reactif && phareCale()) D = 500.0f;
  float lv[PHARE_MAX];
  phareSequence(seq, len, n, D, D, 0.1f + SEGMENT.intensity / 255.0f * 0.9f, lv);
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_REBOND[] PROGMEM = "1 Soiree Roll A/R@Vitesse,Fondu,Veilleuse,,,,;!;;1;sx=150,ix=120,c1=0";

// Remplissage : la colonne se remplit, reste pleine, puis s'eteint (ou se vide par le haut)
static void mode_phare_remplir(void) {
  phareAnim();
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  float D = phareStepMs(SEGMENT.speed);
  if (Rd.reactif && phareCale()) D = 500.0f;
  float hold = D * (1.0f + SEGMENT.intensity / 255.0f * 6.0f);
  bool vider = SEGMENT.check1;
  float tFill = D * n, tEmpty = vider ? D * n : D * 2.0f, tOff = D * 1.5f;
  float cycle = tFill + hold + tEmpty + tOff;
  float t = (float)(phareNow() % (uint32_t)cycle);
  float lv[PHARE_MAX];
  for (unsigned i = 0; i < n; i++) {
    float on = phareSmooth((t - i * D) / D);
    float off = 0.0f;
    float te = t - tFill - hold;
    if (te > 0) off = vider ? phareSmooth((te - (n - 1 - i) * D) / D) : phareSmooth(te / tEmpty);
    lv[i] = on * (1.0f - off);
  }
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_REMPLIR[] PROGMEM = "1 Soiree Remplissage@Vitesse,Temps plein,Veilleuse,,,Vider par le haut,;!;;1;sx=150,ix=60,c1=0";

// Balle qui rebondit : tombe du haut et rebondit de moins en moins haut
static void mode_phare_balle(void) {
  phareAnim();
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  float H = n - 1;
  float tFall = map(SEGMENT.speed, 0, 255, 1600, 300) / 1000.0f;   // duree de la premiere chute (s)
  float g = 2.0f * H / (tFall * tFall);
  float e = 0.45f + SEGMENT.intensity / 255.0f * 0.4f;             // rebond
  float v = g * tFall, tot = tFall;
  float times[12], vels[12]; unsigned nb = 0;
  while (nb < 12) { v *= e; if (v * v / (2 * g) < 0.15f) break; vels[nb] = v; times[nb] = 2 * v / g; tot += times[nb]; nb++; }
  float rest = 1.2f;
  float t = (phareNow() % (uint32_t)((tot + rest) * 1000.0f)) / 1000.0f;
  float y = 0.0f;
  if (t < tFall) y = H - 0.5f * g * t * t;
  else {
    t -= tFall; y = 0.0f;
    for (unsigned b = 0; b < nb; b++) {
      if (t < times[b]) { y = vels[b] * t - 0.5f * g * t * t; break; }
      t -= times[b];
    }
  }
  float lv[PHARE_MAX];
  for (unsigned i = 0; i < n; i++) lv[i] = max(0.0f, 1.0f - fabsf((float)i - y));
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_BALLE[] PROGMEM = "2 Expo Balle qui rebondit@Vitesse,Rebond,Veilleuse,,,,;!;;1;sx=128,ix=128,c1=0";

// Goutte : se forme en haut, tombe en accelerant, eclabousse en bas
static void mode_phare_goutte(void) {
  phareAnim();
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  float k = map(SEGMENT.speed, 0, 255, 2500, 500) / 1000.0f;       // echelle de temps
  float tForm = 1.2f * k * (0.5f + SEGMENT.intensity / 255.0f), tFall = 0.45f * k, tSplash = 0.5f * k, tPause = 0.8f * k;
  float cycle = tForm + tFall + tSplash + tPause;
  float t = (phareNow() % (uint32_t)(cycle * 1000.0f)) / 1000.0f;
  float lv[PHARE_MAX]; for (unsigned i = 0; i < n; i++) lv[i] = 0.0f;
  float H = n - 1;
  if (t < tForm) {
    float f = t / tForm;
    lv[n - 1] = f < 0.8f ? 0.35f * f / 0.8f : 0.35f + 0.65f * (f - 0.8f) / 0.2f;
  } else if (t < tForm + tFall) {
    float u = (t - tForm) / tFall;
    float y = H * (1.0f - u * u);
    for (unsigned i = 0; i < n; i++) lv[i] = max(0.0f, 1.0f - fabsf((float)i - y));
  } else if (t < tForm + tFall + tSplash) {
    float u = (t - tForm - tFall) / tSplash;
    lv[0] = 1.0f - u;
    if (n > 1) lv[1] = 0.6f * (1.0f - phareSmooth(u * 1.6f)) * phareSmooth(u * 4.0f);
  }
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_GOUTTE[] PROGMEM = "2 Expo Goutte@Vitesse,Formation,Veilleuse,,,,;!;;1;sx=128,ix=128,c1=0";

// Alternance : 1+3 / 2+4, ou centre / bords
static void mode_phare_alternance(void) {
  phareAnim();
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  float period = map(SEGMENT.speed, 0, 255, 4000, 300);
  if (Rd.reactif && phareCale()) period = 1000.0f;            // 2 temps
  float s = 0.5f + 0.5f * sinf(2.0f * PI * (phareNow() % (uint32_t)period) / period);
  float sharp = 1.0f + SEGMENT.intensity / 255.0f * 6.0f;          // plus haut = transition plus franche
  float a = constrain((s - 0.5f) * sharp + 0.5f, 0.0f, 1.0f);
  float lv[PHARE_MAX];
  for (unsigned i = 0; i < n; i++) {
    bool grpA = SEGMENT.check1 ? (i == 0 || i == n - 1) : (i % 2 == 0);
    lv[i] = grpA ? a : 1.0f - a;
  }
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_ALTERNANCE[] PROGMEM = "1 Soiree Alternance@Vitesse,Nettete,Veilleuse,,,Centre et bords,;!;;1;sx=120,ix=40,c1=0";

// Clignotant sequentiel : remplissage sec, puis noir
static void mode_phare_clignotant(void) {
  phareAnim();
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  float D = map(SEGMENT.speed, 0, 255, 400, 60);
  float pauseU = 1.0f + SEGMENT.intensity / 255.0f * 4.0f;
  float cycle = D * n + D * 1.5f + D * pauseU;
  float t = (float)(strip.now % (uint32_t)cycle);
  if (Rd.reactif && phareMusique()) {   // un cycle complet = 4 temps
    cycle = phareTempoMs * 4.0f; D = cycle / (n + 1.5f + pauseU);
    t = phareCycle(4) * cycle;
  }
  float lv[PHARE_MAX];
  for (unsigned i = 0; i < n; i++) lv[i] = (t >= i * D && t < D * n + D * 1.5f) ? 1.0f : 0.0f;
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_CLIGNOTANT[] PROGMEM = "1 Soiree Clignotant sequentiel@Vitesse,Pause,Veilleuse,,,,;!;;1;sx=128,ix=100,c1=0";

// Respiration : douce, avec un minimum reglable, ensemble ou decalee
static void mode_phare_respiration(void) {
  phareAnim();
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  float period = map(SEGMENT.speed, 0, 255, 9000, 1200);
  if (Rd.reactif && phareCale()) period = 4000.0f;            // 8 temps
  float lo = SEGMENT.intensity / 255.0f, hi = max(lo, SEGMENT.custom1 / 255.0f);
  float lv[PHARE_MAX];
  for (unsigned i = 0; i < n; i++) {
    float ph = SEGMENT.check1 ? (float)i / n : 0.0f;
    float x = fmodf((phareNow() % (uint32_t)period) / period + ph, 1.0f);
    float b = (expf(sinf(2.0f * PI * x - PI / 2.0f)) - 0.36788f) / 2.35040f;   // respiration naturelle 0..1
    lv[i] = lo + (hi - lo) * b;
  }
  phareShow(lv, n, 0);
}
static const char _data_FX_MODE_PHARE_RESPIRATION[] PROGMEM = "2 Expo Respiration@Vitesse,Minimum,Maximum,,,Decalee,;!;;1;sx=60,ix=40,c1=255";

// SOS en morse sur toute la colonne
static void mode_phare_sos(void) {
  phareAnim();
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  static const char pat[] = "101010001110111011100010101000000000";  // 1 = allume, une case = une unite
  unsigned len = sizeof(pat) - 1;
  uint32_t u = map(SEGMENT.speed, 0, 255, 400, 80);
  bool on = pat[(phareNow() / u) % len] == '1';
  float lv[PHARE_MAX]; for (unsigned i = 0; i < n; i++) lv[i] = on ? 1.0f : 0.0f;
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_SOS[] PROGMEM = "2 Expo SOS@Vitesse,,Veilleuse,,,,;!;;1;sx=128,c1=0";



/////////////////////////////////////////////////////////////
//  Effets d'ambiance
/////////////////////////////////////////////////////////////

// Bougies : algorithme "Candle" de WLED, une bougie independante par ampoule
static void mode_phare_bougies(void) {
  phareAnim();
  uint32_t dt; PhareState* s = phareInit(dt);
  if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  if (SEGENV.call == 0) for (unsigned i = 0; i < n; i++) { s->level[i] = 128; s->tgt[i] = 130 + hw_random8(4); s->stp[i] = 1; }
  if (strip.now - s->t0 >= FRAMETIME_FIXED) {
    s->t0 = strip.now;
    unsigned valrange = SEGMENT.intensity, rndval = valrange >> 1;
    unsigned speedFactor = SEGMENT.speed > 252 ? 1 : SEGMENT.speed > 99 ? 2 : SEGMENT.speed > 49 ? 3 : 4;
    for (unsigned i = 0; i < n; i++) {
      int v = s->level[i], target = s->tgt[i], step = max(1, (int)s->stp[i]);
      bool nouveau = false;
      if (target > v) { v = min(255, v + step); if (v >= target) nouveau = true; }
      else            { v = max(0, v - step);   if (v <= target) nouveau = true; }
      if (nouveau) {
        target = hw_random8(rndval) + hw_random8(rndval);
        if (target < (int)(rndval >> 1)) target = (rndval >> 1) + hw_random8(rndval);
        target += 255 - valrange;
        step = max(1, abs(target - v) >> speedFactor);
      }
      s->level[i] = v; s->tgt[i] = target; s->stp[i] = step;
    }
  }
  float lv[PHARE_MAX];
  for (unsigned i = 0; i < n; i++) {
    float h = SEGMENT.check1 ? 1.0f - 0.75f * (float)i / max(1u, n - 1) : 1.0f;
    float c = s->level[i] / 255.0f;
    lv[i] = c * c * h * (SEGMENT.custom1 / 255.0f);     // au carre : creux plus marques
  }
  phareShow(lv, n, 0);
}
static const char _data_FX_MODE_PHARE_BOUGIES[] PROGMEM = "2 Expo Bougies@Vitesse,Vacillement,Intensite,,,Plus fort en bas,;!;;1;sx=96,ix=224,c1=110";

// Battement de coeur : double pulsation
static void mode_phare_coeur(void) {
  phareAnim();
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  float period = 60000.0f / map(SEGMENT.speed, 0, 255, 40, 150);   // 40 a 150 battements/min
  float t = (strip.now % (uint32_t)period) / period;
  if (Rd.reactif && phareMusique()) { period = phareTempoMs; t = phareCycle(1); }
  float second = 0.3f + SEGMENT.intensity / 255.0f * 0.7f;
  auto pulse = [](float x) { return x < 0 ? 0.0f : phareSmooth(x / 0.06f) * expf(-x * 9.0f); };   // montee et descente douces
  float b = pulse(t) + second * pulse(t - 0.26f);
  float lv[PHARE_MAX]; for (unsigned i = 0; i < n; i++) lv[i] = min(b, 1.0f);
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_COEUR[] PROGMEM = "2 Expo Battement de coeur@Rythme,Second battement,Veilleuse,,,,;!;;1;sx=35,ix=150,c1=10";

// Feux de detresse : clignotement avec montee et descente rapides comme une ampoule
static void mode_phare_detresse(void) {
  phareAnim();
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  float period = map(SEGMENT.speed, 0, 255, 1500, 400);
  float t = (strip.now % (uint32_t)period) / period;
  if (Rd.reactif && phareMusique()) { period = phareTempoMs * 2.0f; t = phareCycle(2); }
  float ramp = 0.04f + SEGMENT.intensity / 255.0f * 0.2f;
  float b = t < 0.5f ? phareSmooth(t / ramp) : 1.0f - phareSmooth((t - 0.5f) / ramp);
  float lv[PHARE_MAX]; for (unsigned i = 0; i < n; i++) lv[i] = b;
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_DETRESSE[] PROGMEM = "2 Expo Feux de detresse@Vitesse,Douceur,Veilleuse,,,,;!;;1;sx=128,ix=60,c1=0";

// Lava lampe : des bulles de lumiere montent et descendent lentement et se melangent
static void mode_phare_lava(void) {
  phareAnim();
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  float sp = 0.05f + SEGMENT.speed / 255.0f * 0.6f;                 // tours par seconde (environ)
  unsigned blobs = 1 + SEGMENT.intensity / 86;                      // 1 a 3 bulles
  float t = phareNow() / 1000.0f;
  static const float fr[3] = {1.0f, 0.73f, 1.31f}, ph[3] = {0.0f, 2.1f, 4.2f};
  float lv[PHARE_MAX]; for (unsigned i = 0; i < n; i++) lv[i] = 0.0f;
  for (unsigned b = 0; b < blobs; b++) {
    float x = sinf(t * sp * fr[b] + ph[b]) * 0.6f + sinf(t * sp * fr[b] * 0.37f + ph[b] * 1.7f) * 0.4f;
    float y = (x * 0.5f + 0.5f) * (n - 1);
    for (unsigned i = 0; i < n; i++) { float d = (i - y) / 0.9f; lv[i] += expf(-d * d); }
  }
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_LAVA[] PROGMEM = "2 Expo Lava lampe@Vitesse,Bulles,Veilleuse,,,,;!;;1;sx=60,ix=120,c1=15";


// Orage : nuit calme, puis eclairs en rafales, plutot du haut
static void mode_phare_orage(void) {
  phareAnim();
  uint32_t dt; PhareState* s = phareInit(dt);
  if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  phareDecay(s, n, dt, 140.0f);
  if (s->side == 0 && hw_random16() < 120 + SEGMENT.intensity * 10) { // nouvel orage : 2 a 4 eclairs
    s->side = 2 + hw_random8() % 3; s->t1 = strip.now;
  }
  if (s->side > 0 && strip.now >= s->t1) {
    unsigned from = hw_random8() % n;                               // l'eclair descend depuis le haut
    for (unsigned i = from; i < n; i++) s->level[i] = 0.6f + hw_random8() / 640.0f;
    s->level[n - 1] = 1.0f;
    s->side--; s->t1 = strip.now + 60 + hw_random16(220);
  }
  phareDraw(s, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_ORAGE[] PROGMEM = "2 Expo Orage@,Frequence,Ciel,,,,;!;;1;ix=40,c1=8";

// Neon defectueux : allume, avec des crises de gresillement a intervalle aleatoire (15 s minimum)
static void mode_phare_neon(void) {
  phareAnim();
  uint32_t dt; PhareState* s = phareInit(dt);
  if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  uint32_t maxAttente = 15000 + (255 - SEGMENT.intensity) * 300;   // 15 s a ~90 s entre deux crises
  if (SEGENV.call == 0) {                                          // demarrage qui hesite
    s->t0 = strip.now; s->t1 = strip.now + 400 + hw_random16(900); s->side = 0xFF;
  }
  if (strip.now > s->t1) {                                         // crise terminee -> on programme la suivante
    s->t0 = strip.now + hw_random(15000, maxAttente);
    s->t1 = s->t0 + 300 + hw_random16(1500);
    s->side = (hw_random8() < 140) ? hw_random8() % n : 0xFF;      // une ampoule ou toutes
  }
  bool crise = strip.now >= s->t0 && strip.now < s->t1;
  float lv[PHARE_MAX];
  for (unsigned i = 0; i < n; i++) {
    bool touche = crise && (s->side == 0xFF || s->side == i);
    lv[i] = touche ? ((hw_random8() < 110) ? 0.03f : 0.4f + hw_random8() / 425.0f) : 1.0f;
  }
  phareShow(lv, n, 0);
}
static const char _data_FX_MODE_PHARE_NEON[] PROGMEM = "2 Expo Neon defectueux@,Frequence des pannes,,,,,;!;;1;ix=100";

// Phare marin : un faisceau tourne, la colonne s'illumine a chaque passage
static void mode_phare_marin(void) {
  phareAnim();
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  float period = map(SEGMENT.speed, 0, 255, 8000, 1500);
  if (Rd.reactif && phareCale()) period = 2000.0f;            // 1 mesure
  float t = (phareNow() % (uint32_t)period) / period;
  bool deux = SEGMENT.check1;
  float w = 0.03f + SEGMENT.intensity / 255.0f * 0.1f;
  float lv[PHARE_MAX];
  for (unsigned i = 0; i < n; i++) {
    float tt = t - 0.01f * i;                                       // leger decalage bas -> haut
    float d1 = (tt - 0.5f) / w, b = expf(-d1 * d1);
    if (deux) { float d2 = (tt - 0.62f) / w; b = max(b, expf(-d2 * d2)); }
    lv[i] = b;
  }
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_MARIN[] PROGMEM = "2 Expo Phare marin@Rotation,Largeur faisceau,Veilleuse,,,Double eclat,;!;;1;sx=90,ix=110,c1=20";

// Pluie : des gouttes tombent du haut, plusieurs a la fois
static void mode_phare_pluie(void) {
  phareAnim();
  uint32_t dt; PhareState* s = phareInit(dt);
  if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  const unsigned maxGouttes = 6;
  if (SEGENV.call == 0) for (unsigned g = 0; g < maxGouttes; g++) s->tgt[g] = -10.0f;
  float vit = (1.5f + SEGMENT.speed / 255.0f * 8.0f) * dt / 1000.0f; // ampoules par seconde
  float lv[PHARE_MAX]; for (unsigned i = 0; i < n; i++) lv[i] = 0.0f;
  for (unsigned g = 0; g < maxGouttes; g++) {
    if (s->tgt[g] < -1.0f) {
      if (hw_random16() < 60 + SEGMENT.intensity * 4) s->tgt[g] = n - 1;
      continue;
    }
    float y = s->tgt[g];
    for (unsigned i = 0; i < n; i++) {
      float d = (float)i - y;                                        // trainee au-dessus de la goutte
      float b = d < 0 ? 1.0f + d : (d < 1.6f ? 1.0f - d / 1.6f : 0.0f);
      if (b > lv[i]) lv[i] = b;
    }
    s->tgt[g] -= vit;
  }
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_PLUIE[] PROGMEM = "2 Expo Pluie@Vitesse,Densite,Veilleuse,,,,;!;;1;sx=110,ix=80,c1=0";

// masque tous les effets qui ne sont pas ceux du phare (sauf Solid)
static const char _data_PHARE_HIDDEN[] PROGMEM = "RSVD";
static void phareMasquerNatifs(const char* const* nos, unsigned nb) {
  const char** data = strip.getModeDataSrc();
  for (unsigned id = 1; id < strip.getModeCount(); id++) {
    bool garder = false;
    for (unsigned k = 0; k < nb; k++) if (data[id] == nos[k]) { garder = true; break; }
    if (!garder) data[id] = _data_PHARE_HIDDEN;
  }
}


/////////////////////////////////////////////////////////////
//  Auto : enchaine des effets au hasard
/////////////////////////////////////////////////////////////

struct PhareChoix { void (*fx)(void); uint8_t sx, ix, c1, c2; bool o1, o2; };
struct PhareAuto  { uint8_t idx; uint32_t debut; };

static void phareAuto(const PhareChoix* liste, unsigned nb) {
  if (!SEGENV.allocateData(sizeof(PhareState) + sizeof(PhareAuto))) FX_FALLBACK_STATIC;
  PhareAuto* a = reinterpret_cast<PhareAuto*>(SEGENV.data + sizeof(PhareState));
  uint32_t duree = map(SEGMENT.speed, 0, 255, 300000, 20000);      // 5 min a 20 s par effet
  bool change = (SEGENV.call == 0);
  if (!change && strip.now - a->debut > duree) {
    uint8_t nouveau; do { nouveau = hw_random8() % nb; } while (nb > 1 && nouveau == a->idx);
    a->idx = nouveau; change = true;
  }
  if (SEGENV.call == 0) a->idx = hw_random8() % nb;
  if (change) a->debut = strip.now;
  const PhareChoix& c = liste[a->idx];
  // l'effet choisi tourne avec ses propres reglages
  uint8_t sx = SEGMENT.speed, ix = SEGMENT.intensity, c1 = SEGMENT.custom1, c2 = SEGMENT.custom2;
  bool o1 = SEGMENT.check1, o2 = SEGMENT.check2; uint32_t appel = SEGENV.call;
  SEGMENT.speed = c.sx; SEGMENT.intensity = c.ix; SEGMENT.custom1 = c.c1; SEGMENT.custom2 = c.c2;
  SEGMENT.check1 = c.o1; SEGMENT.check2 = c.o2;
  if (change) SEGENV.call = 0;
  c.fx();
  SEGENV.call = appel;
  SEGMENT.speed = sx; SEGMENT.intensity = ix; SEGMENT.custom1 = c1; SEGMENT.custom2 = c2;
  SEGMENT.check1 = o1; SEGMENT.check2 = o2;
}

// 12. DJ auto : ecoute, calcule le tempo, choisit et enchaine les effets en rythme
//   pas de son -> eteint ; son sans kick -> veilleuse qui suit la voix (ou montee 7 fois sur 10) ;
//   le beat arrive -> les 4 flashent ensemble 1 ou 2 mesures ; kick -> effets en phrases de 8 mesures,
//   kick sur l'ampoule 1 ou 2 (90 % des phrases), chick en haut, hi-hats
struct PhareDJ { uint8_t cat, idx; uint32_t choix, phrase, mesure; bool sansKick; float gain; uint8_t sxVu; uint32_t beatVu; bool vite; uint16_t hist[4]; uint8_t nHist; uint8_t pool, arc; };

static bool phareFlip = false;
static bool phareCalmeAuto = false;
static uint8_t phareVJCat = 255;
static uint8_t phareVJPool = 255;             // categorie jouee (pour l'indicateur)          // le VJ a detecte un passage calme (energie en baisse)
static void (*phareFxJoue)() = nullptr;     // effet que le VJ est en train de jouer
static void phareJoue(const PhareChoix& c, bool change, int8_t vitesse) {
  uint8_t sx = SEGMENT.speed, ix = SEGMENT.intensity, c1 = SEGMENT.custom1, c2 = SEGMENT.custom2, c3 = SEGMENT.custom3;
  bool o1 = SEGMENT.check1, o2 = SEGMENT.check2, o3 = SEGMENT.check3; uint32_t appel = SEGENV.call;
  SEGMENT.speed = constrain((int)c.sx + vitesse, 0, 255); SEGMENT.intensity = c.ix; SEGMENT.custom1 = c.c1; SEGMENT.custom2 = c.c2;
  SEGMENT.custom3 = 31; SEGMENT.check1 = c.o1 ^ phareFlip; SEGMENT.check2 = c.o2; SEGMENT.check3 = false;
  if (change) SEGENV.call = 0;
  phareFxJoue = c.fx;
  c.fx();
  SEGENV.call = appel;
  SEGMENT.speed = sx; SEGMENT.intensity = ix; SEGMENT.custom1 = c1; SEGMENT.custom2 = c2; SEGMENT.custom3 = c3;
  SEGMENT.check1 = o1; SEGMENT.check2 = o2; SEGMENT.check3 = o3;
  Rd = PhareRenduCtx();                                  // fin du VJ pour cette image : tout a zero
}

static void mode_phare_dj(void) {
  if (!SEGENV.allocateData(sizeof(PhareState) + sizeof(PhareDJ))) FX_FALLBACK_STATIC;
  PhareDJ* d = reinterpret_cast<PhareDJ*>(SEGENV.data + sizeof(PhareState));
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  uint32_t now = millis();
  if (SEGENV.call == 0) { memset(d, 0, sizeof(PhareDJ)); d->cat = 255; d->pool = 255; d->gain = 1.0f; }
  float sens = SEGMENT.intensity / 255.0f;                  // proximite au beat : force des couches kick / chick / hi-hats
  bool silence = now - T.sonDepuis > 1500;
  // passage calme : l'energie baisse nettement par rapport aux 12 dernieres secondes
  static uint32_t calmeT = 0;
  float rapport = T.energie / max(T.energieTres, 1.0f);
  if (!phareCalmeAuto && !silence && rapport < 0.65f) { phareCalmeAuto = true; calmeT = now; d->vite = true; }
  else if (phareCalmeAuto && (silence || (rapport > 0.85f && now - calmeT > 4000))) { phareCalmeAuto = false; d->vite = true; }
  // en passage calme : Douceur +20 %, Energie plafonnee a 30 %
  uint8_t sxEff = phareCalmeAuto ? min((int)SEGMENT.speed, 76) : SEGMENT.speed;
  uint8_t c1Eff = phareCalmeAuto ? min(255, (int)SEGMENT.custom1 + 51) : SEGMENT.custom1;
  float douceur = c1Eff / 255.0f;                           // clignote (0) <-> veilleuse (1)
  int8_t vitesse = ((int)sxEff - 128) / 2;                  // plus calme <-> plus rapide
  bool kick = phareKickActif();
  phareVJVu = now;
  phareDropAuto();                                          // montee / drop automatiques

  // --- ambiance ---
  static const PhareChoix veille[] = {{mode_phare_veilleuse, 0, 0, 90, 0, false, false}};
  // sans kick : ambiances (effets Soiree uniquement)
  static const PhareChoix voix[]   = {{mode_phare_notes, 128, 160, 40, 0, false, false},   {mode_phare_notes, 100, 160, 40, 0, true, false},
                                      {mode_phare_veilleuse, 0, 0, 110, 0, false, false},   {mode_phare_veilleuse, 0, 0, 110, 0, true, false},
                                      {mode_phare_respimusic, 70, 128, 0, 30, false, false}, {mode_phare_alternance, 25, 0, 30, 0, false, false},
                                      {mode_phare_duos, 40, 255, 20, 0, false, false},      {mode_phare_flamme, 100, 128, 40, 30, false, false}};
  // calme : un fond doux + une seule lampe qui marque le kick
  static const PhareChoix calme[]  = {{mode_phare_notes, 128, 160, 40, 0, false, false},   {mode_phare_veilleuse, 0, 0, 140, 0, false, false},
                                      {mode_phare_respimusic, 90, 128, 0, 30, false, false}, {mode_phare_alternance, 60, 0, 30, 0, false, false},
                                      {mode_phare_duos, 80, 255, 20, 0, false, false},      {mode_phare_roll, 80, 255, 20, 0, false, false},
                                      {mode_phare_rebond, 80, 255, 20, 0, false, false},    {mode_phare_flamme, 128, 128, 40, 30, false, false},
                                      {mode_phare_pendule, 128, 140, 15, 0, false, false},  {mode_phare_gonflement, 128, 0, 10, 0, false, false}};
  // moyen : effets graphiques (roll, remplissage, alternance, clignotant, duos, pendule, sequenceur) ~2x plus souvent
  static const PhareChoix moyen[]  = {{mode_phare_sequenceur, 150, 0, 0, 0, false, false}, {mode_phare_sequenceur, 150, 0, 0, 0, false, false},
                                      {mode_phare_roll, 150, 120, 0, 0, false, false},      {mode_phare_roll, 150, 60, 0, 0, true, false},
                                      {mode_phare_rebond, 150, 120, 0, 0, false, false},    {mode_phare_rebond, 150, 60, 0, 0, false, false},
                                      {mode_phare_remplir, 150, 60, 0, 0, true, false},     {mode_phare_remplir, 150, 30, 0, 0, false, false},
                                      {mode_phare_alternance, 120, 120, 0, 0, true, false}, {mode_phare_alternance, 120, 200, 0, 0, false, false},
                                      {mode_phare_clignotant, 128, 80, 0, 0, false, false}, {mode_phare_clignotant, 128, 40, 0, 0, false, false},
                                      {mode_phare_duos, 150, 60, 0, 0, false, false},       {mode_phare_duos, 150, 60, 0, 0, true, false},
                                      {mode_phare_pendule, 128, 60, 0, 0, false, false},    {mode_phare_pendule, 128, 30, 0, 0, false, false},
                                      {mode_phare_gonflement, 128, 0, 0, 0, false, false},  {mode_phare_artifice, 200, 150, 0, 30, false, false},
                                      {mode_phare_batterie, 150, 0, 0, 0, false, false},    {mode_phare_question, 150, 0, 0, 0, false, false},
                                      {mode_phare_ricochet, 0, 60, 0, 0, true, false},      {mode_phare_bass, 200, 128, 0, 30, false, false},
                                      {mode_phare_memoire, 120, 0, 0, 0, false, false},     {mode_phare_kickbass, 150, 160, 0, 0, false, false}};
  static const PhareChoix excite[] = {{mode_phare_sequenceur, 160, 0, 0, 0, false, false}, {mode_phare_sequenceur, 160, 0, 0, 0, false, false},
                                      {mode_phare_roll, 200, 60, 0, 0, true, false},        {mode_phare_roll, 200, 30, 0, 0, false, false},
                                      {mode_phare_rebond, 200, 60, 0, 0, false, false},     {mode_phare_rebond, 200, 30, 0, 0, false, false},
                                      {mode_phare_clignotant, 128, 60, 0, 0, false, false}, {mode_phare_clignotant, 160, 30, 0, 0, false, false},
                                      {mode_phare_remplir, 200, 30, 0, 0, true, false},     {mode_phare_remplir, 200, 20, 0, 0, false, false},
                                      {mode_phare_alternance, 160, 255, 0, 0, false, false}, {mode_phare_alternance, 160, 255, 0, 0, true, false},
                                      {mode_phare_duos, 200, 30, 0, 0, true, false},        {mode_phare_duos, 200, 30, 0, 0, false, false},
                                      {mode_phare_artifice, 200, 150, 0, 30, false, false}, {mode_phare_batterie, 200, 0, 0, 0, false, false},
                                      {mode_phare_question, 200, 0, 0, 0, false, false},    {mode_phare_ricochet, 0, 60, 0, 0, false, false},
                                      {mode_phare_scintillement, 128, 0, 0, 0, false, false}, {mode_phare_memoire, 170, 0, 0, 0, false, false},
                                      {mode_phare_kickbass, 200, 200, 0, 0, false, false},  {mode_phare_kickbass, 200, 120, 0, 0, false, false}};
  // Full auto : breakdown melodique (Notes en vedette, ~60 %) et pic (toute la palette, flashs pleins)
  static const PhareChoix melo[]   = {{mode_phare_notes, 128, 160, 30, 0, false, false},   {mode_phare_notes, 100, 160, 20, 0, false, false},
                                      {mode_phare_notes, 150, 160, 40, 0, false, false},    {mode_phare_notes, 128, 160, 10, 0, false, false},
                                      {mode_phare_notes, 80, 160, 30, 0, false, false},     {mode_phare_notes, 128, 160, 50, 0, false, false},
                                      {mode_phare_veilleuse, 0, 0, 110, 0, false, false},   {mode_phare_gradient, 40, 140, 30, 0, false, false},
                                      {mode_phare_respimusic, 60, 128, 0, 30, false, false}, {mode_phare_pendule, 40, 60, 20, 0, false, false}};
  static const PhareChoix pic[]    = {{mode_phare_sequenceur, 180, 0, 0, 0, false, false}, {mode_phare_sequenceur, 180, 0, 0, 0, false, false},
                                      {mode_phare_kickbass, 220, 220, 0, 0, false, false},  {mode_phare_kickbass, 220, 160, 0, 0, false, false},
                                      {mode_phare_allbass, 200, 128, 0, 30, false, false},  {mode_phare_explosion, 160, 128, 0, 30, false, false},
                                      {mode_phare_artifice, 220, 160, 0, 30, false, false}, {mode_phare_scintillement, 160, 0, 0, 0, false, false},
                                      {mode_phare_clignotant, 200, 30, 0, 0, false, false}, {mode_phare_roll, 230, 30, 0, 0, true, false},
                                      {mode_phare_duos, 230, 20, 0, 0, true, false},        {mode_phare_alternance, 200, 255, 0, 0, false, false}};
  static const PhareChoix* cats[]  = {veille, voix, calme, moyen, excite, melo, pic};
  static const uint8_t nbCat[]     = {1, 8, 10, 24, 22, 10, 12};

  uint8_t cat;
  if (silence) cat = 0;                                     // plus de son : on eteint
  else if (!kick) cat = 1;                                  // son sans kick : veilleuse sur la voix
  else {
    float score = (phareBpm() - 105.0f) / 80.0f + (T.energie / max(T.energieTres, 1.0f) - 1.0f) + vitesse / 64.0f;
    if (d->cat == 4) cat = score < 0.4f ? 3 : 4;            // on ne redescend d'excite que nettement en dessous
    else if (d->cat == 2) cat = score > 0.0f ? 3 : 2;       // on ne sort du calme que nettement au-dessus
    else cat = score < -0.2f ? 2 : (score > 0.8f ? 4 : 3);
    if (phareCalmeAuto) cat = 2;                            // passage calme : une lampe sur le kick, le reste en veilleuse
  }

  // quelle liste d'effets (la categorie, ou en Full auto : breakdown melodique / pic)
  uint8_t pool = cat;
  float rapFA = T.energie / max(T.energieTres, 1.0f);
  if (phareFullAuto && cat == 1 && M.presence > 0.35f) pool = 5;
  if (phareFullAuto && cat == 4 && d->arc >= 2 && rapFA > 0.95f) pool = 6;

  // fondu : s'eteint en 1 s quand le son se coupe, revient en 0,3 s
  float dtg = min(100.0f, (float)(now - SEGENV.step)); SEGENV.step = now;
  if (cat == 0) d->gain = max(0.0f, d->gain - dtg / 1000.0f); else d->gain = min(1.0f, d->gain + dtg / 300.0f);

  // --- quand changer : en debut de phrase (8 mesures) quand le tempo est cale, sinon toutes les 15 s ---
  bool cale = phareCale();
  uint32_t mesureIdx = T.beatCount / 4, phraseIdx = T.beatCount / 32;
  bool finMesure = cale ? (mesureIdx != d->mesure) : false;
  bool finPhrase = cale ? (phraseIdx != d->phrase) : (now - d->choix > 15000);
  // Calme / rapide bouge : on change d'effet au prochain temps (ou au prochain 1 avec "Sur le 1")
  if (abs((int)SEGMENT.speed - (int)d->sxVu) > 6) { if (SEGENV.call) d->vite = true; d->sxVu = SEGMENT.speed; }
  bool tempsNeuf = T.beatCount != d->beatVu; d->beatVu = T.beatCount;
  if (d->vite && (!cale || (tempsNeuf && (!phareSurLe1 || T.beatCount % 4 == 0)))) { finPhrase = true; d->vite = false; }
  bool change = (SEGENV.call == 0) || d->cat == 255;
  bool suivant = phareSuivantDemande; phareSuivantDemande = false;
  if (suivant) change = true;
  // en calme / sans beat, l'effet dure 2 phrases (16 mesures) ; breakdown melodique (Full auto) : 4 phrases
  bool phraseUtile = finPhrase && (cat >= 3 || (pool == 5 ? phraseIdx % 4 == 0 : phraseIdx % 2 == 0) || !cale);
  if (cat != d->cat || pool != d->pool) {
    bool urgent = cat <= 1 || d->cat <= 1 || d->cat == 255 || pool == 5 || d->pool == 5;
    if (urgent || finPhrase) change = true;                 // sinon on attend la fin de la phrase
    else { cat = d->cat; pool = d->pool; }
  } else if (phraseUtile) change = true;                     // nouvel effet a chaque phrase (ou toutes les 2-4 en calme)
  if (finPhrase && cat >= 3 && d->arc < 250) d->arc++;       // depuis combien de phrases le beat tourne
  if (cat <= 1) d->arc = 0;
  if (finMesure && !change && mesureIdx % 8 == 4 && hw_random8() < 128) phareFlip = !phareFlip;   // petite variation a la 4e mesure
  if (finMesure) d->mesure = mesureIdx;
  if (finPhrase) d->phrase = phraseIdx;
  if (change) {
    uint8_t nb = nbCat[pool], avant = (pool == d->pool) ? d->idx : 255, nv = 0, va = 0;
    // anti-repetition : ni l'effet ni la variante parmi les 4 derniers joues
    for (int essai = 0; essai < 12; essai++) {
      if (suivant && pool == d->pool && essai == 0) nv = (d->idx + 1) % nb;
      else nv = hw_random8() % nb;
      va = hw_random8() < 100 ? 0 : 1 + hw_random8() % 7;
      uint16_t cle = (pool << 8) | (nv << 3) | va; bool deja = nb > 1 && nv == avant && !suivant;
      for (uint8_t h = 0; h < d->nHist && !deja; h++) { uint8_t hc = d->hist[h] >> 8, hi = (d->hist[h] >> 3) & 31;
        if (hc == pool && hi < nbCat[pool] && cats[pool][hi].fx == cats[pool][nv].fx) deja = true; }
      if (!deja || essai == 11) break;
    }
    d->idx = nv; d->cat = cat; d->pool = pool; d->choix = now;
    memmove(d->hist + 1, d->hist, 3 * sizeof(uint16_t)); d->hist[0] = (pool << 8) | (nv << 3) | va; if (d->nHist < 4) d->nHist++;
    d->sansKick = hw_random8() < 26;                            // 1 phrase sur 10 sans la couche kick
    // orchestration de la phrase : formation (densite selon l'energie), variante de temps, respirations
    float rap = T.energie / max(T.energieTres, 1.0f);
    uint8_t r = hw_random8() % 100;
    if (cat == 4 || rap > 1.15f) O.formation = r < 10 ? 0 : (r < 25 ? 1 : (r < 50 ? 2 : 3));
    else if (cat == 3)           O.formation = r < 20 ? 0 : (r < 30 ? 1 : (r < 65 ? 2 : 3));   // tempo seul 20 %, kick seul 10 %, effet + chick 35 %, tout 35 %
    else if (cat == 2)           O.formation = r < 50 ? 0 : 3;              // calme : une phrase sur deux en tempo seul
    else                         O.formation = 3;
    if (phareFullAuto && cat >= 3) {                              // Full auto : la densite monte au fil des phrases (arc)
      if (d->arc < 2)      O.formation = r < 50 ? 0 : 2;          // debut : tempo seul / effet + chick
      else if (d->arc < 4) O.formation = r < 45 ? 2 : 3;
      else                 O.formation = r < 25 ? 2 : 3;          // le beat tourne depuis longtemps : tout
      if (pool == 6) O.formation = 3;
    }
    O.variante = cat >= 3 ? va : 0;
    O.perm = hw_random8() % 4;                                    // telles quelles / inversees / decalees / depuis le milieu
    { void (*f)() = cats[pool][nv].fx; if ((f == mode_phare_roll || f == mode_phare_rebond) && O.perm >= 2) O.perm &= 1; }   // les rolls restent entiers
    O.lampeVide = hw_random8() % 4;
    phareSeqMotif = hw_random8() % PHARE_NB_MOTIFS;
    // une seule respiration par phrase au maximum : fill 25 %, noir 15 %, mesure presque vide 10 %, rien 50 %
    O.noirFin = false; O.mesureVide = -1; O.fill = 0;
    if (cat >= 3) { uint8_t rr = hw_random8() % 100;
      if (rr < 25) O.fill = (millis() - T.lastSnare < 1500 && (hw_random8() & 1)) ? 2 : 1;
      else if (rr < 40) O.noirFin = true;
      else if (rr < 50) O.mesureVide = hw_random8() & 1 ? 3 : 5; }
  }
  // contraste : couplets plus bas (40 %), refrains a fond, en douceur
  { float rap = T.energie / max(T.energieTres, 1.0f);
    float cible = 0.6f + 0.4f * constrain((rap - 0.75f) / 0.4f, 0.0f, 1.0f);   // 60 a 100 %
    if (phareFullAuto) {                                       // Full auto : grands contrastes (breakdown tres doux, pic a fond)
      static const float niv[7] = {0.3f, 0.4f, 0.5f, 0.6f, 0.9f, 0.3f, 1.0f};
      cible = niv[min((int)d->pool, 6)];
      if (d->pool == 3) cible = min(0.95f, 0.6f + 0.08f * d->arc);
    }
    O.niveau += (cible - O.niveau) * min(1.0f, dtg / 2000.0f); }
  if (d->pool > 6) d->pool = d->cat;
  if (d->idx >= nbCat[d->pool]) d->idx = 0;
  Rd.vj = true;
  Rd.vjCouches = d->cat >= 2 ? (0.25f + 0.75f * sens) * (d->cat == 2 ? 1.0f : 1.0f) : (d->cat == 1 ? 0.5f * sens + 0.2f : 0.0f);
  Rd.sansKick = (d->sansKick || O.formation == 2) && d->cat != 2;
  if (d->cat >= 2 && O.formation == 0) Rd.vjCouches = 0.0f;                  // tempo seul : l'animation suit le tempo, sans flashs
  if (d->cat >= 3) {
    Rd.sansChick = O.formation == 1;                                         // kick seul
    Rd.sansHats = O.formation == 1 || O.formation == 2;                      // effet + chick
  }
  Rd.chickSeul = d->cat == 1;
  phareVJCat = d->cat; phareVJPool = d->pool;
  Rd.gain = d->gain;
  Rd.max3 = !(phareFullAuto && d->pool == 6);              // pic en Full auto : flashs pleins autorises
  if (phareFullAuto && d->cat >= 3 && M.presence > 0.35f) Rd.melodie = 1.0f;   // une lampe joue avec la melodie
  Rd.douceur = douceur;
  Rd.calme = d->cat == 2;
  // apres un drop manuel : le VJ enchaine Artifice (8 mesures) ou All bass (4) puis Artifice (4), puis reprend
  if (phareApresVJ) {
    static const PhareChoix art = {mode_phare_artifice, 200, 150, 0, 30, false, false}, all = {mode_phare_allbass, 200, 128, 0, 30, false, false};
    static const PhareChoix* joue = nullptr;
    uint32_t m = phareMesureN - phareApresDebut;
    const PhareChoix* c = (phareApresVJ == 2 && m < 4) ? &all : &art;
    if (m >= 8) { phareApresVJ = 0; joue = nullptr; phareSuivantDemande = true; }   // le VJ reprend avec un nouvel effet
    else { bool neuf = c != joue; joue = c; Rd.max3 = false; phareJoue(*c, neuf, 0); return; }
  }
  phareJoue(cats[d->pool][d->idx], change, d->cat <= 1 ? 0 : vitesse);
}
static const char _data_FX_MODE_PHARE_DJ[] PROGMEM = "0 VJ auto@Calme / rapide,Proximite au beat,Clignote / veilleuse;!;;1f;sx=128,ix=170,c1=60";

static void mode_phare_auto_anim(void) {
  static const PhareChoix liste[] = {
    {mode_phare_roll, 150, 120, 0, 0, true, false},     {mode_phare_rebond, 150, 120, 0, 0, false, false},
    {mode_phare_remplir, 150, 60, 0, 0, false, false},  {mode_phare_balle, 128, 128, 0, 0, false, false},
    {mode_phare_goutte, 128, 128, 0, 0, false, false},  {mode_phare_alternance, 120, 40, 0, 0, false, false},
    {mode_phare_clignotant, 128, 100, 0, 0, false, true}, {mode_phare_respiration, 60, 40, 255, 0, false, false},
    {mode_phare_bougies, 96, 224, 110, 0, false, false},  {mode_phare_coeur, 35, 150, 10, 0, false, false},
    {mode_phare_detresse, 128, 60, 0, 0, false, true},  {mode_phare_lava, 60, 120, 15, 0, false, false},
    {mode_phare_orage, 128, 40, 8, 0, false, false},
    {mode_phare_neon, 128, 100, 0, 0, false, false},    {mode_phare_marin, 90, 110, 20, 0, false, false},
    {mode_phare_pluie, 110, 80, 0, 0, false, false},
  };
  phareAuto(liste, sizeof(liste) / sizeof(liste[0]));
}
static const char _data_FX_MODE_PHARE_AUTO_ANIM[] PROGMEM = "30 Auto animations@Duree par effet;!;;1;sx=200";


/////////////////////////////////////////////////////////////
//  Page de controle : http://punto1.local/phare  (websocket /phws)
/////////////////////////////////////////////////////////////
static const char PAGE_PHARE[] PROGMEM = R"=====(<!doctype html><html lang="fr"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<meta name="apple-mobile-web-app-capable" content="yes"><meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black">
<title>Phare</title>
<style>
:root{--bg:#050506;--deck:#0d0d0f;--pan:#1a1b1f;--pan2:#232429;--alu:#8d8f96;--alu2:#a7a9b0;--line:#2c2d33;--tx:#f2f1ee;--mu:#7d7e84;
--led:#ff8a2a;--red:#ff3b30;--amb:#ffa21a;--vio:#9b7bff;--glow:0 0 10px}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;user-select:none;-webkit-user-select:none}
html,body{margin:0;background:var(--bg);color:var(--tx);font:500 13px/1.25 -apple-system,"SF Pro Text","Helvetica Neue",system-ui,sans-serif;touch-action:manipulation;overscroll-behavior:none}
.key,.vf,.tg,.pwr{touch-action:none;cursor:pointer}
.hf,.tile{touch-action:pan-y;cursor:pointer}
.w{max-width:820px;margin:0 auto;padding:14px 14px 28px}
.cap{font-size:10px;letter-spacing:.22em;text-transform:uppercase;color:var(--mu)}
/* entete */
.top{display:flex;align-items:center;gap:10px;padding:6px 4px 14px}
.logo{display:flex;align-items:center;gap:10px}
.logo svg{width:30px;height:30px}
.logo b{font-size:22px;letter-spacing:.28em;font-weight:700}
.logo span{display:block;font-size:10px;letter-spacing:.2em;color:var(--mu);margin-top:2px;text-transform:uppercase}
.lcd{margin-left:auto;background:#000;border:1px solid var(--line);border-radius:10px;padding:6px 12px;display:flex;align-items:center;gap:10px;box-shadow:inset 0 2px 8px #000}
.lcd b{font:600 22px/1 ui-monospace,"SF Mono",Menlo,monospace;letter-spacing:.06em;min-width:44px;text-align:right}
.lcd .cap{font-size:9px}
.lcd b.old{color:#5d5e63}
.led{width:9px;height:9px;border-radius:50%;background:#2a1a10;flex:none;transition:background .06s,box-shadow .06s}
.led.on{background:var(--led);box-shadow:var(--glow) var(--led)}
.pwr{width:46px;height:46px;border-radius:50%;background:radial-gradient(circle at 50% 35%,#3a3b41,#18191c);border:1px solid #3a3b42;display:flex;align-items:center;justify-content:center;box-shadow:0 3px 8px #000,inset 0 1px 0 #55565d}
.pwr svg{width:20px;height:20px;stroke:#666;transition:stroke .1s}
.pwr.on svg{stroke:var(--led);filter:drop-shadow(0 0 5px var(--led))}
.fx{display:flex;align-items:center;gap:8px;margin:0 4px 12px;color:var(--alu2);font-size:12px;letter-spacing:.08em;text-transform:uppercase;min-height:16px}
/* chassis */
.deck{background:linear-gradient(#cfd0d4,#9c9ea4);border-radius:18px;padding:10px;box-shadow:0 10px 30px #000,inset 0 1px 0 #fff}
.mod{background:var(--pan);border-radius:12px;padding:10px;box-shadow:inset 0 1px 0 #34353b,0 2px 0 #6e7076}
.mod>.cap{display:flex;align-items:center;gap:6px;margin-bottom:10px}
.mod>.cap:before{content:"";width:8px;height:8px;border-radius:2px;background:#e9e9ea;opacity:.85}
.row{display:grid;gap:10px}
.r1{grid-template-columns:1fr auto;margin-bottom:10px}
.r2{grid-template-columns:auto auto auto 1fr auto;margin-bottom:10px}
.vf.vb{width:46px}.vb .fill{position:absolute;left:50%;bottom:14px;width:4px;margin-left:-2px;border-radius:2px;background:var(--led);box-shadow:0 0 8px var(--led)}
/* touches */
.key{position:relative;border-radius:10px;background:linear-gradient(#2e2f35,#1f2024);border:1px solid #0b0b0d;box-shadow:inset 0 1px 0 #44454c,0 3px 0 #0b0b0d,0 4px 8px #0008;
 display:flex;flex-direction:column;align-items:center;justify-content:center;gap:5px;color:var(--tx);font-size:11px;letter-spacing:.16em;text-transform:uppercase;transition:transform .04s,box-shadow .04s,background .06s}
.key svg{width:22px;height:22px;stroke:currentColor;fill:none;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round}
.key .led{position:absolute;top:7px;left:7px;width:7px;height:7px}
.key.press,.key:active{transform:translateY(2px);box-shadow:inset 0 1px 0 #44454c,0 1px 0 #0b0b0d,0 1px 3px #0008;background:linear-gradient(#36373e,#26272c)}
.key.on .led{background:var(--led);box-shadow:var(--glow) var(--led)}
.key.arm .led{background:var(--led);animation:arm .3s steps(2) infinite}
@keyframes arm{50%{background:#2a1a10;box-shadow:none}}
.key.hot{color:#ff9a7a}
.key.on.hot{background:linear-gradient(#ff7a3d,#e2481f);color:#111;border-color:#7a2410}
.key.on.hot .led{background:#fff;box-shadow:0 0 8px #fff}
/* lampes */
.pads{display:grid;grid-template-rows:repeat(4,58px) 40px;gap:8px;width:64px}
.pad{font:700 18px/1 ui-monospace,"SF Mono",Menlo,monospace;letter-spacing:0}
.pad.r{color:#ff7a70}.pad.o{color:#ffbe5c}
.pad.on.r{background:linear-gradient(#ff4a3d,#c8231a);color:#fff;border-color:#5c120c;box-shadow:inset 0 1px 0 #ff9a90,0 1px 0 #0b0b0d,0 0 18px #ff3b3088}
.pad.on.o{background:linear-gradient(#ffb43a,#e8860c);color:#111;border-color:#6b3d05;box-shadow:inset 0 1px 0 #ffe0a0,0 1px 0 #0b0b0d,0 0 18px #ffa21a88}
.pad.solid{font-family:inherit;font-size:10px;font-weight:600;letter-spacing:.16em}
.pad.solid.on{background:linear-gradient(#f4f4f2,#c9cac9);color:#111}
.tg{display:flex;align-items:center;justify-content:center;gap:8px;height:44px;border-radius:10px;background:#141518;border:1px solid #0b0b0d;box-shadow:inset 0 2px 6px #000;font-size:11px;letter-spacing:.16em;text-transform:uppercase;color:var(--mu)}
.tg svg{width:16px;height:16px;stroke:currentColor;fill:none;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
.tg.on{color:var(--tx)}.tg.on .led{background:var(--led);box-shadow:var(--glow) var(--led)}
.hold{height:34px;margin-top:8px;font-size:9px}
/* fader main */
.vf{position:relative;width:62px;height:330px;border-radius:10px;background:#0f1012;box-shadow:inset 0 2px 10px #000;border:1px solid #000}
.vf:before{content:"";position:absolute;left:50%;top:14px;bottom:14px;width:4px;margin-left:-2px;border-radius:2px;background:#000;box-shadow:0 1px 0 #2c2d33}
.vf .tick{position:absolute;left:8px;right:8px;height:1px;background:#2a2b30}
.vf .k{position:absolute;left:6px;right:6px;height:44px;border-radius:7px;transform:translateY(-50%);
 background:linear-gradient(#d9dadd,#8f9197);box-shadow:0 4px 8px #000,inset 0 1px 0 #fff;transition:top .25s}
.vf .k:after{content:"";position:absolute;left:8px;right:8px;top:50%;height:2px;margin-top:-1px;background:#222}
.vf.act .k{transition:none;background:linear-gradient(#ffb070,#ff7a2a)}
/* VJ */
.vj{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr);gap:8px;align-content:start}
.vj .key{height:84px}
.mtrow{grid-column:1/-1;display:grid;grid-template-columns:2fr 1fr 1fr;gap:8px}
.vj .key.mini{height:84px}.key.mini svg{width:20px;height:20px}

.vj .key small{font:600 12px/1 ui-monospace,"SF Mono",Menlo,monospace;letter-spacing:.04em;color:var(--led);min-height:12px}
/* metre */
.meter{display:block}
.mbox{display:flex;gap:8px}.mbox>*{flex:none}
.vf.vs{width:40px;height:330px}
.dots{display:grid;grid-template-rows:repeat(22,1fr);gap:4px;height:330px;padding:6px 7px;background:#0b0b0d;border-radius:8px;box-shadow:inset 0 2px 8px #000}
.dots i{width:7px;height:7px;border-radius:50%;background:#26272b}
.dots i.on{background:#f4f3ef;box-shadow:0 0 5px #fff6}
.dots i.on.pk{background:var(--led);box-shadow:var(--glow) var(--led)}
.lamps{display:grid;grid-template-rows:repeat(4,1fr);gap:8px;height:330px;padding:6px 0}
.lamps i{width:16px;border-radius:5px;background:#26272b;transition:background .08s,box-shadow .08s}
/* faders horizontaux */
.hs{display:grid;gap:12px}
.h .t{display:flex;justify-content:space-between;align-items:center;margin-bottom:6px}
.h .t b{font-size:11px;letter-spacing:.18em;text-transform:uppercase;font-weight:600}
.h .t span{font-size:9px;letter-spacing:.14em;text-transform:uppercase;color:var(--mu)}
.hf{position:relative;height:44px;border-radius:10px;background:#0f1012;box-shadow:inset 0 2px 8px #000;border:1px solid #000}
.hf:before{content:"";position:absolute;top:50%;left:14px;right:14px;height:4px;margin-top:-2px;border-radius:2px;background:#000;box-shadow:0 1px 0 #2c2d33}
.hf .f{position:absolute;top:50%;left:14px;height:4px;margin-top:-2px;border-radius:2px;background:var(--led);box-shadow:0 0 8px var(--led)}
.hf .k{position:absolute;top:4px;bottom:4px;width:30px;margin-left:-15px;border-radius:7px;background:linear-gradient(90deg,#d9dadd,#8f9197);box-shadow:0 3px 6px #000,inset 1px 0 0 #fff}
.hf .k:after{content:"";position:absolute;top:8px;bottom:8px;left:50%;width:2px;margin-left:-1px;background:#222}
.vjo{display:none}.vjo.ok{display:block}
/* favoris */
.tiles{display:grid;grid-template-columns:repeat(auto-fill,minmax(112px,1fr));gap:8px}
.tile{position:relative;min-height:54px;padding:16px 8px 8px;border-radius:9px;background:linear-gradient(#2a2b31,#1d1e22);border:1px solid #0b0b0d;box-shadow:inset 0 1px 0 #3f4047,0 2px 0 #0b0b0d;
 font-size:11px;letter-spacing:.06em;text-align:center;display:flex;align-items:center;justify-content:center;color:var(--alu2)}
.tile .led{position:absolute;top:6px;left:6px;width:6px;height:6px}
.tile .tag{position:absolute;top:5px;right:7px;font-size:8px;letter-spacing:.16em;color:var(--mu)}
.tile.sub .led{background:#fff;box-shadow:0 0 6px #fff9}
.tile.cur{color:var(--tx)}.tile.cur .led{background:var(--led);box-shadow:var(--glow) var(--led)}
.tile.arm .led{background:var(--led);animation:arm .3s steps(2) infinite}
.tile.press{transform:translateY(1px);background:linear-gradient(#34353c,#25262b)}
.sec{margin-top:10px}
.tgs{display:flex;flex-wrap:wrap;gap:8px;margin-top:10px}.tgs .tg{flex:1;min-width:140px}
.prog{height:8px;border-radius:4px;background:#0b0b0d;box-shadow:inset 0 1px 3px #000;margin-top:10px;overflow:hidden}.prog div{height:100%;width:0;background:var(--led);box-shadow:0 0 8px var(--led);transition:width .2s}
.maj{display:flex;align-items:center;justify-content:space-between;gap:12px}
.maj .key{height:44px;padding:0 16px;flex-direction:row}
.maj .msg{color:var(--mu);font-size:11px;margin-top:6px}
.bandeau{position:fixed;left:0;right:0;bottom:0;background:#2a0f0a;color:#ffb3a6;text-align:center;padding:9px;font-size:11px;letter-spacing:.14em;text-transform:uppercase;display:none}
@media(max-width:560px){.r2{grid-template-columns:auto auto auto 1fr}.vj .key{height:84px}
 .meter{grid-column:1/-1}.mbox{flex-direction:row;align-items:center}.dots{grid-template-rows:none;grid-template-columns:repeat(22,1fr);height:auto;flex:1}.lamps{grid-template-rows:none;grid-template-columns:repeat(4,1fr);height:36px;width:84px;padding:0}.lamps i{width:100%;height:30px}.lamps{gap:6px;height:auto;width:110px;flex:none}.vf.vs{height:96px!important;width:44px!important;flex:none}.mbox .dots{flex:1 1 auto;min-width:0;gap:3px;grid-template-columns:repeat(22,minmax(0,1fr))!important;padding:6px}.dots i{width:100%;height:auto;aspect-ratio:1;max-width:8px}.meter .cap+.mbox+.cap{display:none}}
@media(max-width:430px){.logo b{font-size:18px;letter-spacing:.18em}.logo svg{width:24px}.gear,.pwr{width:40px;height:40px}.lcd{padding:6px 9px}.lcd b{font-size:19px;min-width:36px}.pads{grid-template-rows:repeat(4,52px) 38px;width:52px}.vf{width:50px;height:306px}.vf.vb{width:40px}.vj .key,.vj .key.mini{height:62px;font-size:9px;letter-spacing:.1em}.mtrow{grid-template-columns:1fr 1fr}.mtrow .key:first-child{grid-column:1/-1}.mod{padding:8px}.row{gap:7px}}
.seg3{display:grid;grid-template-columns:repeat(3,1fr);gap:4px;padding:4px;background:#0b0b0d;border-radius:12px;box-shadow:inset 0 2px 6px #000}
.seg3 div{height:42px;border-radius:9px;display:flex;align-items:center;justify-content:center;gap:7px;font-size:10px;letter-spacing:.16em;text-transform:uppercase;color:var(--mu);cursor:pointer;touch-action:manipulation}
.seg3 div .led{width:7px;height:7px}
.seg3 div.on{background:linear-gradient(#34353c,#24252a);color:var(--tx);box-shadow:inset 0 1px 0 #4a4b52,0 2px 4px #0008}
.seg3 div.on .led{background:var(--led);box-shadow:var(--glow) var(--led)}
.modes{margin-bottom:10px}.modes div{height:48px;font-size:12px}
.mod.vide{display:none!important}
.grise{opacity:.3;pointer-events:none;filter:grayscale(1)}
.gear,.pwr{flex:none}
.gear{width:46px;height:46px;border-radius:50%;background:radial-gradient(circle at 50% 35%,#3a3b41,#18191c);border:1px solid #3a3b42;display:flex;align-items:center;justify-content:center;box-shadow:0 3px 8px #000,inset 0 1px 0 #55565d;cursor:pointer}
.gear svg{width:20px;height:20px;stroke:#aaa;fill:none;stroke-width:1.8}
.sheet{position:fixed;inset:0;background:#000a;display:none;align-items:flex-end;justify-content:center;z-index:9}
.sheet.ok{display:flex}
.sheet .box{width:100%;max-width:560px;background:var(--pan);border-radius:18px 18px 0 0;padding:16px 16px 28px;box-shadow:0 -10px 30px #000}
.sheet .row2{display:flex;align-items:center;justify-content:space-between;gap:10px;margin:12px 0}
.sheet .num{font:600 20px/1 ui-monospace,"SF Mono",Menlo,monospace;min-width:70px;text-align:center}
.sheet .key{height:44px;min-width:56px;padding:0 14px;flex-direction:row}
.ms{font:600 13px ui-monospace,"SF Mono",Menlo,monospace;color:var(--led)}

/* ================= DA Y2K : chrome liquide, plastique moule, plasma orange ================= */
:root{--bg:#090607;--bord:#580E13;--rouge:#E52B20;--org:#FF6A00;--org2:#FF9B27;--champ:#C9A49A;--led:#FF6A00;--tx:#fff3ea;--mu:#c98a6a;--alu2:#e9c3b0;--bpm:.5s}
html,body{background:var(--bg);color:var(--tx)}
body{background:radial-gradient(ellipse 80% 50% at 15% 0%,#5a1208 0%,transparent 60%),radial-gradient(ellipse 70% 60% at 100% 100%,#4a0c06 0%,transparent 60%),radial-gradient(ellipse at 50% 40%,#1c0705,#090607 75%) fixed}
body:before{content:"";position:fixed;inset:-30%;z-index:-1;background:conic-gradient(from 0deg at 50% 50%,#ff6a0000,#ff6a0018,#e52b2010,#ff9b2714,#ff6a0000);filter:blur(60px);animation:plasma 40s linear infinite;pointer-events:none}
@keyframes plasma{to{transform:rotate(360deg)}}
.cap{color:var(--org2);text-shadow:0 0 6px #ff6a0066}
/* entete */
.top{position:relative;padding:4px 2px 10px}
.logo{gap:6px;position:relative}
.logo .pico{display:none}
.logo svg.car{width:150px;height:78px;flex:none;filter:drop-shadow(0 6px 10px #000) drop-shadow(0 0 12px #ff6a0040);margin:-6px -4px -8px -6px}
.titre b{font:900 italic 30px/1 "Arial Black","Helvetica Neue",system-ui,sans-serif;letter-spacing:.06em;background:linear-gradient(180deg,#ffe3c2 0%,#ff9b27 35%,#e52b20 60%,#7a1008 100%);-webkit-background-clip:text;background-clip:text;color:transparent;filter:drop-shadow(0 1px 0 #2a0503) drop-shadow(0 0 8px #ff6a0055);-webkit-text-stroke:.5px #ffd0a0}
.fx{color:var(--org2);text-shadow:0 0 6px #ff6a0055;margin-left:10px}
.lcd{background:radial-gradient(ellipse at 50% 0%,#2a0a06,#0a0302 70%);border:2px solid #000;border-radius:22px 14px 22px 14px;box-shadow:0 0 0 2px #c9a49a55,inset 0 2px 10px #000,inset 0 -1px 0 #ff6a0033,0 6px 14px #000}
.lcd b{font:italic 700 26px/1 ui-monospace,"SF Mono",Menlo,monospace;color:var(--org2);text-shadow:0 0 8px #ff6a00,0 0 2px #ffd0a0;transform:skewX(-6deg)}
.lcd b.old{color:#6a3020;text-shadow:none}
.led{background:#3a0e08}
.led.on{background:#ffb060;box-shadow:0 0 8px #ff6a00,0 0 2px #fff}
.pwr,.gear{background:radial-gradient(circle at 40% 30%,#fff6f0 0%,#c9a49a 18%,#5a3a34 45%,#1a0a08 70%,#c9a49a 92%);border:2px solid #120403;box-shadow:0 6px 12px #000,inset 0 -3px 6px #000a,0 0 14px #ff6a0030}
.pwr svg,.gear svg{stroke:#2a0a06}
.pwr.on svg{stroke:#ff6a00;filter:drop-shadow(0 0 6px #ff6a00)}
.gear svg{stroke:#3a120a}
/* chassis chrome */
.deck{background:linear-gradient(125deg,#fff4ee 0%,#9c7d76 12%,#2a1210 26%,#d9b8ad 40%,#4a2420 55%,#f2dcd3 68%,#6d4842 82%,#e6cfc6 100%);border-radius:46px 30px 52px 34px/34px 48px 30px 44px;padding:12px;box-shadow:0 24px 60px #000,0 0 40px #ff6a0026,inset 0 2px 2px #fff,inset 0 -4px 8px #0009,inset 0 0 0 1px #00000080}
.mod{background:radial-gradient(ellipse 120% 80% at 50% 0%,#2e0d09 0%,#160605 55%,#0b0303 100%);border-radius:30px 20px 34px 22px/22px 32px 20px 30px;box-shadow:inset 0 0 0 2px #000,inset 0 4px 14px #000,inset 0 -2px 0 #ff6a0020,0 0 0 2px #c9a49a70,0 2px 0 3px #3a1a16}
.mod>.cap:before{width:8px;height:8px;border-radius:50%;background:#ff8a2a;box-shadow:0 0 8px #ff6a00,0 0 2px #fff;opacity:1}
.sec.mod{background:radial-gradient(ellipse at 50% 0%,#24090a,#0d0404 70%);border-radius:34px 22px 38px 24px/24px 36px 22px 32px}
/* capsules glossy (touches) */
.key{background:radial-gradient(ellipse 90% 70% at 50% 18%,#ff9a6a 0%,#e52b20 28%,#8a160e 66%,#3a0604 100%);border:1px solid #1a0302;border-radius:30px 22px 32px 22px/24px 32px 22px 30px;color:#fff6ee;text-shadow:0 1px 0 #3a0604,0 0 6px #ff9b2788;
 box-shadow:inset 0 2px 1px #ffd0b0aa,inset 0 -8px 14px #000a,0 4px 0 #2a0503,0 7px 14px #000d,0 0 0 2px #c9a49a40}
.key:after{content:"";position:absolute;left:14%;right:14%;top:5%;height:30%;border-radius:50%;background:linear-gradient(#ffffff70,#ffffff00);pointer-events:none}
.key svg{stroke:#fff6ee;filter:drop-shadow(0 0 4px #ff9b27aa)}
.key.press,.key:active{transform:translateY(3px);box-shadow:inset 0 2px 1px #ffd0b088,inset 0 -4px 10px #000a,0 1px 0 #2a0503,0 2px 6px #000d,0 0 0 2px #c9a49a40;background:radial-gradient(ellipse 90% 70% at 50% 25%,#ffb07a 0%,#ff4a2a 30%,#a01c12 70%,#4a0806 100%)}
.key.on{background:radial-gradient(ellipse 90% 70% at 50% 25%,#fff0c0 0%,#ffb040 22%,#ff6a00 50%,#b02a08 85%,#5a0c04 100%);color:#2a0503;text-shadow:0 1px 0 #ffd0a0;box-shadow:inset 0 2px 1px #fff8,inset 0 -6px 12px #7a1a0488,0 4px 0 #2a0503,0 0 24px #ff6a00aa,0 0 0 2px #ffd0a0}
.key.on svg{stroke:#2a0503;filter:none}
.key.hot{color:#fff6ee}
.key .led{top:9px;left:10px}
.key.on .led,.key.arm .led{background:#fff;box-shadow:0 0 8px #fff,0 0 14px #ff6a00}
.vj .key small{color:#ffe0b0;text-shadow:0 0 6px #ff6a00}
.key.on small{color:#2a0503;text-shadow:none}
.vj .key.mini{border-radius:26px 26px 30px 30px/30px 30px 26px 26px}
#bdr{background:radial-gradient(ellipse 90% 70% at 50% 18%,#ffb070 0%,#ff4a1a 30%,#a8180c 70%,#4a0604 100%)}
/* lampes : lentilles du feu */
.pad{border-radius:50% 46% 48% 50%/46% 50% 46% 50%;font:900 italic 22px/1 "Arial Black",system-ui,sans-serif;color:#ffb090}
.pad.r{background:radial-gradient(circle at 50% 30%,#a8301e 0%,#5a0e08 55%,#1a0302 100%);color:#ff8a70}
.pad.o{background:radial-gradient(circle at 50% 30%,#b0661a 0%,#5a2a06 55%,#1a0a02 100%);color:#ffc070}
.pad.on.r{background:radial-gradient(circle at 50% 35%,#fff0e0 0%,#ff6a50 25%,#e52b20 55%,#7a0e08 100%);color:#fff;box-shadow:inset 0 2px 1px #fff9,0 0 30px #ff2a10,0 0 0 2px #ffd0c0}
.pad.on.o{background:radial-gradient(circle at 50% 35%,#fffbe0 0%,#ffc040 25%,#ff8a00 55%,#7a3a04 100%);color:#2a0a02;box-shadow:inset 0 2px 1px #fff9,0 0 30px #ff9b27,0 0 0 2px #ffe0b0}
.pad.solid{border-radius:20px;font:700 10px/1 system-ui,sans-serif;letter-spacing:.18em;color:#ffd0c0;background:radial-gradient(ellipse at 50% 20%,#5a2018,#200604)}
.pad.solid.on{background:radial-gradient(ellipse at 50% 30%,#fff,#ffd0b0 40%,#ff9b27);color:#2a0503}
.tg{background:radial-gradient(ellipse at 50% 0%,#2a0a06,#0a0302);border:1px solid #000;border-radius:22px;box-shadow:inset 0 2px 8px #000,0 0 0 2px #c9a49a40;color:#c98a6a}
.tg.on{color:#fff3ea}.tg.on .led{background:#ffb060;box-shadow:0 0 8px #ff6a00}
/* faders : rails lumineux, curseurs chromes */
.vf,.hf{background:linear-gradient(90deg,#0a0302,#1a0605 50%,#0a0302);border:2px solid #000;border-radius:30px;box-shadow:inset 0 3px 12px #000,0 0 0 2px #c9a49a55,0 2px 0 3px #2a1210}
.vf:before,.hf:before{background:#2a0704;box-shadow:0 0 6px #ff6a0040}
.vf .tick{background:#ff6a0030}
.vf .k,.hf .k{background:linear-gradient(180deg,#fffaf6 0%,#e0c2b8 22%,#7a5850 48%,#2a1612 52%,#c9a49a 78%,#fff0e8 100%);border-radius:14px;box-shadow:0 5px 10px #000,0 0 0 1px #000,inset 0 1px 0 #fff,0 0 12px #ff6a0030}
.hf .k{background:linear-gradient(90deg,#fffaf6 0%,#e0c2b8 22%,#7a5850 48%,#2a1612 52%,#c9a49a 78%,#fff0e8 100%)}
.vf .k:after,.hf .k:after{background:#2a0a06;box-shadow:0 0 4px #ff6a00}
.vf.act .k{background:linear-gradient(180deg,#fff6e0,#ffb060 40%,#ff6a00 60%,#ffd0a0)}
.hf .f,.vb .fill{background:linear-gradient(90deg,#e52b20,#ff6a00,#ff9b27);box-shadow:0 0 10px #ff6a00,0 0 2px #fff}
.vb .fill{background:linear-gradient(0deg,#e52b20,#ff6a00,#ff9b27)}
.h .t b{color:#fff3ea;text-shadow:0 0 6px #ff6a0066}
.h .t span{color:#ff8a5a}
/* son */
.dots{background:#0a0302;border-radius:14px;box-shadow:inset 0 2px 8px #000,0 0 0 1px #c9a49a40}
.dots i{background:#2a0804}
.dots i.on{background:#ffb060;box-shadow:0 0 6px #ff6a00}
.dots i.on.pk{background:#fff;box-shadow:0 0 8px #ff6a00}
.lamps i{border-radius:40%}
/* segments (modes, reactivite) */
.seg3{background:radial-gradient(ellipse at 50% 0%,#1e0605,#070201);border-radius:30px 22px 30px 22px;box-shadow:inset 0 2px 8px #000,0 0 0 2px #c9a49a60,0 3px 0 3px #2a1210}
.seg3 div{border-radius:26px 18px 26px 18px;color:#c98a6a}
.seg3 div.on{background:radial-gradient(ellipse 90% 80% at 50% 20%,#ffd0a0 0%,#ff6a00 35%,#a8200a 80%);color:#2a0503;text-shadow:0 1px 0 #ffd0a0;box-shadow:inset 0 2px 1px #fffa,0 0 18px #ff6a0088}
.seg3 div.on .led{background:#fff;box-shadow:0 0 6px #fff}
/* tuiles */
.tile{background:radial-gradient(ellipse 90% 70% at 50% 15%,#5a1a10 0%,#2a0806 55%,#120302 100%);border:1px solid #000;border-radius:24px 16px 24px 16px/18px 24px 16px 24px;color:#ffc8a8;box-shadow:inset 0 1px 1px #ffb09055,inset 0 -4px 8px #0009,0 3px 0 #1a0302,0 0 0 1px #c9a49a30}
.tile.cur{background:radial-gradient(ellipse 90% 70% at 50% 25%,#ffe0b0 0%,#ff6a00 35%,#9a1a06 85%);color:#2a0503;box-shadow:inset 0 2px 1px #fff9,0 0 18px #ff6a0099}
.tile.cur .led{background:#fff;box-shadow:0 0 6px #fff}
.tile.sub{box-shadow:inset 0 1px 1px #ffb09055,0 0 0 2px #ff9b27,0 0 14px #ff6a0066}
/* feuille reglages */
.sheet .box{background:radial-gradient(ellipse at 50% 0%,#2a0a08,#0d0403 70%);border-radius:36px 36px 0 0;box-shadow:0 -10px 40px #000,0 0 0 2px #c9a49a60}
.prog{background:#0a0302;box-shadow:inset 0 1px 4px #000,0 0 0 1px #c9a49a40}
.prog div{background:linear-gradient(90deg,#e52b20,#ff6a00,#ff9b27)}
.bandeau{background:#3a0604;color:#ffd0b0}
/* pulsation au BPM (tres legere) */
.pulse .deck{animation:pls var(--bpm) ease-out infinite}
@keyframes pls{0%{box-shadow:0 24px 60px #000,0 0 60px #ff6a0060,inset 0 2px 2px #fff,inset 0 -4px 8px #0009}100%{box-shadow:0 24px 60px #000,0 0 30px #ff6a0018,inset 0 2px 2px #fff,inset 0 -4px 8px #0009}}
@media(max-width:430px){.encours{font-size:11px;letter-spacing:.04em}.top{flex-wrap:wrap;row-gap:6px}.logo{width:100%}.logo svg.car{width:150px;height:78px}.titre b{font-size:34px}.lcd{margin-left:0;flex:1;justify-content:center}.deck{border-radius:34px 24px 38px 26px/26px 36px 24px 32px;padding:9px}}
.encours{grid-column:1/-1;display:flex;align-items:center;justify-content:center;gap:8px;min-height:44px;margin-top:2px;padding:8px;border-radius:14px;background:#0a0302;box-shadow:inset 0 2px 8px #000;font:700 15px/1.2 system-ui,sans-serif;letter-spacing:.08em;text-transform:uppercase;color:var(--org2);text-shadow:0 0 8px #ff6a0088;text-align:center}
.encours .led{width:8px;height:8px;flex:none}
.rep{display:flex;gap:8px;align-items:flex-end;height:60px;margin-top:6px}.rep div{flex:1;background:linear-gradient(0deg,#e52b20,#ff9b27);border-radius:6px 6px 2px 2px;min-height:2px;position:relative}.rep span{position:absolute;top:-16px;left:0;right:0;text-align:center;font-size:10px;color:var(--org2)}
.vjinfo{grid-column:1/-1;text-align:center;font:600 10px/1.4 system-ui,sans-serif;letter-spacing:.08em;text-transform:uppercase;color:var(--mu);min-height:14px}
.seg3.seg2{grid-template-columns:1fr 1fr}
</style></head><body><div class="w">
<div class="top">
 <div class="logo"><svg class="car" viewBox="0 0 200 104" aria-hidden="true">
 <defs>
  <linearGradient id="cb" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ff7a52"/><stop offset=".35" stop-color="#e52b20"/><stop offset=".8" stop-color="#7a1008"/><stop offset="1" stop-color="#3a0604"/></linearGradient>
  <linearGradient id="cw" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#3a2a2a"/><stop offset=".5" stop-color="#0c0606"/><stop offset="1" stop-color="#2a1a1a"/></linearGradient>
  <linearGradient id="cf" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#ffd23a"/><stop offset=".5" stop-color="#ff8a1a"/><stop offset="1" stop-color="#ff3a10" stop-opacity="0"/></linearGradient>
  <radialGradient id="cr" cx=".5" cy=".5" r=".5"><stop offset="0" stop-color="#e8e1de"/><stop offset=".45" stop-color="#8e817c"/><stop offset=".7" stop-color="#2a2222"/><stop offset="1" stop-color="#0a0606"/></radialGradient>
  <radialGradient id="cx" cx="0" cy=".5" r="1"><stop offset="0" stop-color="#bfe4ff"/><stop offset=".3" stop-color="#ff9b27"/><stop offset="1" stop-color="#ff3a10" stop-opacity="0"/></radialGradient>
 </defs>
 <ellipse cx="104" cy="92" rx="84" ry="7" fill="#000" opacity=".55"/>
 <path d="M10 70 C10 58 18 52 30 50 L58 46 C70 30 86 22 108 21 L140 21 C156 22 170 30 182 44 L190 54 C194 60 194 70 190 76 L184 80 L22 82 C14 82 10 78 10 70Z" fill="url(#cb)" stroke="#2a0503" stroke-width="1.5"/>
 <path d="M64 46 C74 33 88 27 106 26 L120 26 L118 46Z" fill="url(#cw)"/>
 <path d="M124 26 L142 26 C154 27 164 33 174 44 L172 47 L124 46Z" fill="url(#cw)"/>
 <path d="M26 54 C40 50 60 48 80 50" stroke="#ffd6c0" stroke-width="1.6" fill="none" opacity=".7"/>
 <path d="M60 66 C74 58 84 62 92 56 C88 64 100 62 108 56 C104 66 118 64 126 58 C122 68 138 66 150 60 C140 72 110 74 70 72 Z" fill="url(#cf)" opacity=".95"/>
 <rect x="178" y="56" width="8" height="10" rx="2" fill="#ffb02e"/>
 <circle cx="52" cy="80" r="15" fill="#0a0606"/><circle cx="52" cy="80" r="11" fill="url(#cr)"/>
 <circle cx="156" cy="80" r="15" fill="#0a0606"/><circle cx="156" cy="80" r="11" fill="url(#cr)"/>
 <ellipse cx="0" cy="76" rx="22" ry="7" fill="url(#cx)" transform="translate(6 0)"/>
</svg><svg class="pico" viewBox="0 0 32 32"><g fill="none" stroke="#f2f1ee" stroke-width="1.6"><rect x="9" y="2" width="14" height="28" rx="4"/></g><g><circle cx="16" cy="8" r="2.6" fill="#ff3b30"/><circle cx="16" cy="14" r="2.6" fill="#ff3b30"/><circle cx="16" cy="20" r="2.6" fill="#ffa21a"/><circle cx="16" cy="26" r="2.6" fill="#ff3b30"/></g></svg>
  <div class="titre"><b>PUNTO</b></div></div>
 <div class="lcd"><span class="led" id="dot"></span><b id="bpm">---</b><span class="cap">bpm</span></div>
 <div class="gear" id="gear"><svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="3"/><path d="M12 2v3M12 19v3M2 12h3M19 12h3M4.9 4.9l2.1 2.1M17 17l2.1 2.1M4.9 19.1 7 17M17 7l2.1-2.1"/></svg></div>
 <div class="pwr on" id="pwr"><svg viewBox="0 0 24 24" fill="none" stroke-width="2.2" stroke-linecap="round"><path d="M12 3v8"/><path d="M6.3 6.6a8 8 0 1 0 11.4 0"/></svg></div>
</div>
<div class="seg3 modes" id="modes"><div data-m="auto"><span class="led"></span>Auto</div><div data-m="manuel"><span class="led"></span>Manuel</div><div data-m="expo"><span class="led"></span>Expo</div></div>
<div class="deck">
 <div class="row r1">
  <div class="seg3 seg2" id="vjm" style="display:none"><div data-a="1"><span class="led"></span>Full auto</div><div data-a="0"><span class="led"></span>Control</div></div>
  <div class="seg3" id="reac"><div data-r="0"><span class="led"></span>Libre</div><div data-r="1"><span class="led"></span>Tempo</div><div data-r="2"><span class="led"></span>Tempo + percus</div></div>
  <div class="tg" id="inv" style="height:50px;padding:0 16px"><span class="led"></span><svg viewBox="0 0 24 24"><path d="M8 4v16M4 8l4-4 4 4M16 20V4M12 16l4 4 4-4"/></svg>Inverser</div>
 </div>
 <div class="row r2">
  <div class="mod"><div class="cap">Lampes</div>
   <div class="pads">
    <div class="key pad r" data-l="3"><span class="led"></span>4</div>
    <div class="key pad r" data-l="2"><span class="led"></span>3</div>
    <div class="key pad o" data-l="1"><span class="led"></span>2</div>
    <div class="key pad r" data-l="0"><span class="led"></span>1</div>
    <div class="key pad solid" data-l="all">Solid</div>
   </div>
   <div class="tg hold" id="hold"><span class="led"></span>Maintien</div>
  </div>
  <div class="mod"><div class="cap">Main</div><div class="vf" id="main"><div class="k"></div></div></div>
  <div class="mod"><div class="cap">Lum.</div><div class="vf vb" data-k="br"><div class="fill"></div><div class="k"></div></div></div>
  <div class="mod"><div class="cap">VJ</div><div class="vj">
   <div class="mtrow"><div class="key" data-b="1" id="bmt"><span class="led"></span><svg viewBox="0 0 24 24"><path d="m6 11 6-6 6 6"/><path d="m6 18 6-6 6 6"/></svg>Montée<small id="mtx"></small></div>
    <div class="key mini" data-b="5"><svg viewBox="0 0 24 24"><path d="M6 12h12"/></svg></div><div class="key mini" data-b="4"><svg viewBox="0 0 24 24"><path d="M6 12h12M12 6v12"/></svg></div></div>
   <div class="key" data-b="6" id="boff"><span class="led"></span><svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="8"/><path d="M6.5 17.5l11-11"/></svg><span id="offt">Off</span><small></small></div>
   <div class="key hot" data-b="2" id="bdr"><span class="led"></span><svg viewBox="0 0 24 24"><path d="M12 2v4M12 18v4M2 12h4M18 12h4M4.9 4.9l2.8 2.8M16.3 16.3l2.8 2.8M4.9 19.1l2.8-2.8M16.3 7.7l2.8-2.8"/><circle cx="12" cy="12" r="2.5"/></svg>Drop<small></small></div>
   <div class="key" data-b="3" id="bsv"><span class="led"></span><svg viewBox="0 0 24 24"><path d="m5 5 9 7-9 7z"/><path d="M19 5v14"/></svg>Suivant<small></small></div>
   <div class="key" data-b="t" id="btp"><span class="led"></span><svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="8"/><circle cx="12" cy="12" r="2.5"/></svg>Tap<small></small></div>
   <div class="encours"><span class="led on"></span><span id="fx">—</span></div>
   <div class="vjinfo" id="vi"></div>
  </div></div>
  <div class="mod meter"><div class="cap">Son</div><div class="mbox"><div class="dots" id="dots"></div><div class="lamps" id="lamps"><i></i><i></i><i></i><i></i></div><div class="vf vb vs" id="sens"><div class="fill"></div><div class="k"></div></div></div><div class="cap" style="margin-top:8px;text-align:right">Sensib.</div></div>
 </div>
 <div class="mod hs vueAuto" id="fadersVJ" style="margin-top:10px">
  <div class="h" id="h_sx"><div class="t"><span>Calme</span><b>Énergie</b><span>Rapide</span></div><div class="hf" data-k="sx"><div class="f"></div><div class="k"></div></div></div>
  <div class="h" id="h_c1"><div class="t"><span>Clignote</span><b>Douceur</b><span>Veilleuse</span></div><div class="hf" data-k="c1"><div class="f"></div><div class="k"></div></div></div>
  <div class="h" id="h_ix"><div class="t"><span>Libre</span><b>Proximité au beat</b><span>Collé</span></div><div class="hf" data-k="ix"><div class="f"></div><div class="k"></div></div></div>
 </div>
</div>
<div class="mod sec vueManuel vueExpo" style="margin-top:12px" id="regl"><div class="cap" id="reglt">Réglages</div><div class="hs" id="reglh"></div><div class="tgs" id="reglo"></div></div>
<div class="mod sec vueManuel"><div class="cap">Effets soirée</div><div class="tiles" id="tSoiree"></div></div>
<div class="mod sec vueExpo"><div class="cap">Ambiances expo</div><div class="tiles" id="tExpo"></div></div>
<div class="sheet" id="sheet"><div class="box">
 <div class="cap">Réglages du phare</div>
 <div class="row2"><span>Latence micro</span><div style="display:flex;align-items:center;gap:8px"><div class="key" id="latm">−</div><span class="num" id="lat">60</span><div class="key" id="latp">+</div></div></div>
 <div class="row2"><span>Test réseau <span class="ms" id="rtt"></span></span><div class="key" id="bping">Mesurer</div></div>
 <div class="row2"><span>Flash test (toutes les lampes)</span><div class="key" id="bflash">Flash</div></div>
 <div class="cap" style="margin-top:18px">Répartition de la lumière (5 dernières min)</div>
 <div class="rep" id="rep"><div><span></span></div><div><span></span></div><div><span></span></div><div><span></span></div></div>
 <div style="display:flex;justify-content:space-around;font-size:10px;color:var(--mu);margin-top:4px"><span>lampe 1</span><span>lampe 2</span><span>lampe 3</span><span>lampe 4</span></div>
 <div class="cap" style="margin-top:18px">Firmware</div>
 <div class="maj" style="margin-top:8px"><span id="ver" class="cap" style="color:var(--alu2)">Version …</span><div class="key" id="bmaj"><span class="led"></span>Vérifier</div></div><div class="prog" id="barre" style="display:none"><div></div></div><div class="msg" id="majmsg"></div>
 <div class="key" id="fermer" style="margin-top:18px;height:46px">Fermer</div>
</div></div>
</div><div class="bandeau" id="off">Connexion au phare…</div>
<script>
var vjId=-2;
function propre(n){return n.replace(/^\d+\s+(Soiree|Expo)\s+/,'').replace(/^0\s+/,'')}
var vfx=-1,ws,last={},pads=0,hold=false,drag={},cur=-1,armFx=-1,direct=true;
function $(i){return document.getElementById(i)}
var verrou={};function bloque(k){verrou[k]=Date.now()+1000}function libre(k){return !(verrou[k]>Date.now())}
function snd(m){if(ws&&ws.readyState==1)ws.send(m)}
function thr(k,m){var n=Date.now();clearTimeout(last[k+'t']);if(last[k]&&n-last[k]<30){last[k+'t']=setTimeout(function(){snd(m);last[k]=Date.now()},30);return}last[k]=n;snd(m)}
function press(el){el.classList.add('press');setTimeout(function(){el.classList.remove('press')},120)}
// vumetre
var dots=$('dots');for(var i=0;i<22;i++){var d=document.createElement('i');dots.appendChild(d)}
var dl=dots.children,pk=0;
function vu(v){var n=Math.round(v/255*22);pk=Math.max(n,pk-1);for(var i=0;i<22;i++){var j=21-i;dl[i].className=(j<n?'on':'')+(j==pk-1&&pk>1?' on pk':'')}}
var lampEls=$('lamps').children,lampCol=['#ff3b30','#ffa21a','#ff3b30','#ff3b30'];
function lamps(a){for(var i=0;i<4;i++){var v=a[i]/255,el=lampEls[3-i],c=lampCol[i];el.style.background=v>0.04?c:'#26272b';el.style.opacity=v>0.04?0.25+0.75*v:1;el.style.boxShadow=v>0.3?'0 0 '+(6+12*v)+'px '+c:'none'}}
function conn(){ws=new WebSocket('ws://'+location.host+'/phws');
 ws.onopen=function(){$('off').style.display='none'};
 ws.onclose=function(){$('off').style.display='block';setTimeout(conn,1200)};
 ws.onmessage=function(e){try{var d=JSON.parse(e.data)}catch(x){return}
  if(d.z){$('rtt').textContent=(Date.now()-pingT)+' ms';return}
  if(d.b&&d.bc){document.documentElement.style.setProperty('--bpm',(60/d.b).toFixed(3)+'s');document.body.classList.add('pulse')}else document.body.classList.remove('pulse');
  $('bpm').textContent=d.b?d.b:'---';$('bpm').classList.toggle('old',!d.bc);
  if(d.k){var t=$('dot');t.classList.add('on');setTimeout(function(){t.classList.remove('on')},90)}
  if(d.fx!==undefined)$('fx').textContent=d.vn?'VJ › '+propre(d.vn):propre(d.fx);$('vi').textContent=d.vi||'';
  if(d.vf!==undefined&&d.vf!=vfx){vfx=d.vf;marqueTuile()}
  if(d.lv!==undefined)vu(d.lv);
  if(d.l)lamps(d.l);
  if(d.on!==undefined&&libre('on'))$('pwr').classList.toggle('on',!!d.on);
  ['sx','c1','ix'].forEach(function(k){if(d[k]!==undefined&&!drag[k])setH(k,d[k])});
  if(d.br!==undefined&&!drag.br)vLum.set(d.br);
  if(d.se!==undefined&&!drag.se)vSens.set(d.se);
  ['sx','ix','c1','c2','c3','o'].forEach(function(k){if(d[k]!==undefined)regVals[k]=d[k]});
  if(d.fm!==undefined&&armFx<0&&d.fm!=cur&&libre('fm')){cur=d.fm;marqueTuile()}
  if(d.fm!==undefined&&armFx<0)chargeReglages(d.fm);majReglages();
  if(d.pf!==undefined&&armFx>=0&&!d.pf){cur=armFx;armFx=-1;marqueTuile()}
  if(libre('mt')){var mt=$('bmt');mt.classList.toggle('on',d.mt>0);$('mtx').textContent=d.mt>0?(d.mt==1?'×½':'×'+d.mt/2):''}
  if(d.nm!==undefined&&libre('nm'))offEtat(!!d.nm);
  if(libre('dr')){$('bdr').classList.toggle('on',d.dr==2);$('bdr').classList.toggle('arm',d.dr==1)}
  $('h_sx').querySelector('b').textContent=d.ca?'Énergie · calme auto':'Énergie';
  $('bsv').classList.toggle('arm',!!d.pa);
  if(d.rt!==undefined&&libre('rt'))reacEtat(d.rt);
  if(d.fa!==undefined&&libre('fa'))vjmEtat(d.fa);
  if(d.inv!==undefined&&libre('inv'))$('inv').classList.toggle('on',!!d.inv);
  if(d.rp){var rb=$('rep').children;for(var q=0;q<4;q++){rb[q].style.height=Math.max(2,d.rp[q]*0.55)+'px';rb[q].firstChild.textContent=d.rp[q]+' %'}}
  if(d.lat!==undefined&&!latDrag)$('lat').textContent=d.lat;
  modeReel=d.vj?'auto':(d.ex?'expo':'manuel');if(!modeVue)vue(modeReel);   // l'onglet ne bouge que si tu le touches
  griser(d);
  if(d.v!==undefined){$('ver').textContent='Version #'+d.v;if(verLoc!==d.v){verLoc=d.v;afficheMaj()}}
 }}
conn();
// on / off
$('pwr').addEventListener('pointerdown',function(e){e.preventDefault();var on=!this.classList.contains('on');this.classList.toggle('on',on);snd('o'+(on?1:0));bloque('on')});
// lampes
function showPads(){document.querySelectorAll('.pad').forEach(function(q){var b=q.dataset.l=='all'?15:(1<<+q.dataset.l);q.classList.toggle('on',(pads&b)==b)})}
document.querySelectorAll('.pad').forEach(function(p){
 var bit=p.dataset.l=='all'?15:(1<<+p.dataset.l);
 p.addEventListener('pointerdown',function(e){e.preventDefault();p.setPointerCapture(e.pointerId);
  if(hold){pads=((pads&bit)==bit)?(pads&~bit):(pads|bit)}else pads|=bit;showPads();snd('p'+pads)});
 function up(){if(!hold){pads&=~bit;showPads();snd('p'+pads)}}
 p.addEventListener('pointerup',up);p.addEventListener('pointercancel',up)});
$('hold').addEventListener('pointerdown',function(e){e.preventDefault();hold=!hold;this.classList.toggle('on',hold);if(!hold){pads=0;showPads();snd('p0')}});
// reactivite : Libre / Tempo / Tempo + percus
function reacEtat(r){document.querySelectorAll('#reac div').forEach(function(x){x.classList.toggle('on',x.dataset.r==r)})}
document.querySelectorAll('#reac div').forEach(function(x){x.addEventListener('pointerdown',function(e){e.preventDefault();reacEtat(x.dataset.r);bloque('rt');snd('r'+x.dataset.r)})});
$('inv').addEventListener('pointerdown',function(e){e.preventDefault();var on=!this.classList.contains('on');this.classList.toggle('on',on);bloque('inv');snd('n'+(on?1:0))});
// Auto : Full auto / Control
function vjmEtat(a){document.querySelectorAll('#vjm div').forEach(function(x){x.classList.toggle('on',x.dataset.a==a)})}
document.querySelectorAll('#vjm div').forEach(function(x){x.addEventListener('pointerdown',function(e){e.preventDefault();vjmEtat(x.dataset.a);bloque('fa');snd('a'+x.dataset.a)})});
// modes : Auto / Manuel / Expo (chaque mode n'affiche que ses commandes)
var modeReel='manuel',modeVue='',modeForce=0,dernier={manuel:-1,expo:-1},tuiles={};
try{dernier=JSON.parse(localStorage.getItem('punto_dernier'))||dernier}catch(x){}
function vue(m){if(m==modeVue)return;modeVue=m;document.querySelectorAll('#modes div').forEach(function(x){x.classList.toggle('on',x.dataset.m==m)});
 document.querySelectorAll('.vueAuto,.vueManuel,.vueExpo').forEach(function(el){el.style.display=el.classList.contains('vue'+m[0].toUpperCase()+m.slice(1))?'':'none'})}
document.querySelectorAll('#modes div').forEach(function(x){x.addEventListener('pointerdown',function(e){e.preventDefault();var m=x.dataset.m;modeForce=Date.now();vue(m);
 if(m=='auto'&&vjId>=0)lance(vjId);
 else if(m!='auto'&&modeReel!=m){var f=dernier[m];if(!(f>=0)){var l=$(m=='expo'?'tExpo':'tSoiree').firstChild;f=l?+l.dataset.fx:-1}if(f>=0)lance(f,m)}})});
function lance(fx,ctx){ctx=ctx||(tuiles[fx]&&tuiles[fx].m);snd((ctx=='expo'?'g':'f')+fx);cur=fx;bloque('fm');armFx=-1;marqueTuile();chargeReglages(fx);
 if(ctx&&ctx!='auto'){dernier[ctx]=fx;try{localStorage.setItem('punto_dernier',JSON.stringify(dernier))}catch(x){}}}
// griser ce qui ne sert pas
function gr(el,g){if(el)el.classList.toggle('grise',!!g)}
function griser(d){var mt=libre('mt')?d.mt>0:$('bmt').classList.contains('on');
 gr($('reac'),modeVue!='manuel');$('reac').style.display=modeVue=='auto'?'none':'';$('vjm').style.display=modeVue=='auto'?'':'none';
 document.querySelectorAll('.mini').forEach(function(b){gr(b,!mt)});
 var h0=document.querySelector('#reglh .h');if(h0)gr(h0,!!d.sg)}
// engrenage
$('gear').addEventListener('click',function(){$('sheet').classList.add('ok')});
$('fermer').addEventListener('click',function(){$('sheet').classList.remove('ok')});
$('sheet').addEventListener('click',function(e){if(e.target==this)this.classList.remove('ok')});
var latDrag=0,pingT=0;
function latPas(d){var v=Math.max(0,Math.min(300,(+$('lat').textContent||0)+d));$('lat').textContent=v;latDrag=Date.now();setTimeout(function(){latDrag=0},1500);snd('l'+v)}
$('latm').addEventListener('click',function(){latPas(-10)});$('latp').addEventListener('click',function(){latPas(10)});
$('bping').addEventListener('click',function(){pingT=Date.now();$('rtt').textContent='…';snd('z')});
$('bflash').addEventListener('click',function(){snd('x')});
// fader main
(function(){var el=$('main'),k=el.querySelector('.k'),on=false,v=0;
 for(var i=1;i<10;i++){var t=document.createElement('div');t.className='tick';t.style.top=(i*10)+'%';el.insertBefore(t,k)}
 function pos(x){k.style.top='calc(24px + '+((1-x)*100)+'% - '+((1-x)*48)+'px)'}pos(0);
 function mv(e){var r=el.getBoundingClientRect();v=1-(e.clientY-r.top-24)/(r.height-48);v=Math.max(0,Math.min(1,v));pos(v);thr('m','m'+Math.round(v*1000))}
 el.addEventListener('pointerdown',function(e){e.preventDefault();el.setPointerCapture(e.pointerId);on=true;el.classList.add('act');mv(e)});
 el.addEventListener('pointermove',function(e){if(on)mv(e)});
 function up(){if(!on)return;on=false;el.classList.remove('act');pos(0);setTimeout(function(){snd('m-')},40)}
 el.addEventListener('pointerup',up);el.addEventListener('pointercancel',up);
 setInterval(function(){if(on)snd('m'+Math.round(v*1000))},500)})();
function offEtat(o){$('boff').classList.toggle('on',o);$('boff').classList.toggle('eteint',o);$('offt').textContent=o?'On':'Off'}
function monteeStop(){$('bmt').classList.remove('on');$('mtx').textContent='';document.querySelectorAll('.mini').forEach(function(m){gr(m,true)})}
// boutons VJ : reaction immediate a l'ecran
document.querySelectorAll('.vj .key').forEach(function(b){b.addEventListener('pointerdown',function(e){e.preventDefault();press(b);
 var c=b.dataset.b;snd(c=='t'?'t':'b'+c);if(c=='1'||c=='4'||c=='5'||c=='2'||c=='3'||c=='6'){bloque('mt');bloque('dr');bloque('nm')}if(navigator.vibrate)navigator.vibrate(8);
 if(c=='1'&&!b.classList.contains('on')){b.classList.add('on');$('mtx').textContent='×1';c='';document.querySelectorAll('.mini').forEach(function(m){gr(m,false)})}
 if(c=='1')c='4';
 if(c=='4'||c=='5'){var x=$('mtx').textContent;if(x){var r=x=='×½'?0.5:parseFloat(x.slice(1));r=c=='4'?Math.min(16,r*2):Math.max(0.5,r/2);$('mtx').textContent=r==0.5?'×½':'×'+r}}
 if(c=='6'){var o=!b.classList.contains('on');offEtat(o);if(o)monteeStop()}
 if(c=='2'){b.classList.add('on');offEtat(false);monteeStop()}
 if(c=='3')monteeStop();
 if(c=='t'){b.classList.add('on');setTimeout(function(){b.classList.remove('on')},120)}})});
// faders horizontaux
function setH(k,v){var el=document.querySelector('.hf[data-k="'+k+'"]');if(!el)return;var w=el.clientWidth-36,p=v/255;el.querySelector('.f').style.width=(4+p*w)+'px';el.querySelector('.k').style.left=(18+p*w)+'px'}
function hfInit(el,k,envoi){
 var x0=0,y0=0,go=false;
 function mv(e){var r=el.getBoundingClientRect(),v=Math.round(Math.max(0,Math.min(1,(e.clientX-r.left-18)/(r.width-36)))*255);if(k=='br')v=Math.max(1,v);setH(k,v);envoi(v)}
 el.addEventListener('pointerdown',function(e){x0=e.clientX;y0=e.clientY;go=false;drag[k]=1});
 el.addEventListener('pointermove',function(e){if(!drag[k])return;if(!go){if(Math.abs(e.clientX-x0)>6&&Math.abs(e.clientX-x0)>Math.abs(e.clientY-y0)){go=true;try{el.setPointerCapture(e.pointerId)}catch(x){}}else return}mv(e)});
 function up(e){if(drag[k]&&!go&&e.type=='pointerup'&&Math.abs(e.clientY-y0)<6)mv(e);setTimeout(function(){drag[k]=0},400)}
 el.addEventListener('pointerup',up);el.addEventListener('pointercancel',up)}
document.querySelectorAll('.hs .hf').forEach(function(el){var k=el.dataset.k;hfInit(el,k,function(v){thr(k,k+v)})});
// luminosite (fader vertical)
// faders verticaux (luminosite, sensibilite)
function vfInit(el,k,mini){
 function set(v){var h=el.clientHeight-48,p=v/255;el.querySelector('.k').style.top=(24+(1-p)*h)+'px';el.querySelector('.fill').style.height=Math.max(0,p*h-4)+'px'}
 function mv(e){var r=el.getBoundingClientRect(),v=Math.round(Math.max(0,Math.min(1,1-(e.clientY-r.top-24)/(r.height-48)))*255);v=Math.max(mini,v);set(v);thr(k,k+v)}
 el.addEventListener('pointerdown',function(e){e.preventDefault();el.setPointerCapture(e.pointerId);drag[k]=1;mv(e)});
 el.addEventListener('pointermove',function(e){if(drag[k])mv(e)});
 ['pointerup','pointercancel'].forEach(function(ev){el.addEventListener(ev,function(){setTimeout(function(){drag[k]=0},400)})});
 set(255);return {set:set}}
var vLum=vfInit(document.querySelector('.vb[data-k="br"]'),'br',1),vSens=vfInit($('sens'),'se',0);
// effets (tuiles) : changent tout de suite, ou s'arment jusqu'au prochain 1
function marqueTuile(){document.querySelectorAll('.tile').forEach(function(t){t.classList.toggle('cur',t.dataset.fx==cur);t.classList.toggle('arm',t.dataset.fx==armFx);t.classList.toggle('sub',t.dataset.fx==vfx&&t.dataset.fx!=cur)})}
var AUSSI_EXPO=/^1 Soiree (Roll haut|Roll A\/R|Remplissage|Alternance|Clignotant sequentiel|Duos|Sequenceur)$/;
function tuile(fx,nom,ctx){var d=document.createElement('div');d.className='tile';d.dataset.fx=fx;
  d.innerHTML='<span class="led"></span>';d.appendChild(document.createTextNode(nom));
  var tx=0,ty=0;d.addEventListener('pointerdown',function(e){tx=e.clientX;ty=e.clientY});
  d.addEventListener('pointerup',function(e){if(Math.abs(e.clientX-tx)>8||Math.abs(e.clientY-ty)>8)return;press(d);modeForce=Date.now();lance(fx,ctx)});
  $(ctx=='expo'?'tExpo':'tSoiree').appendChild(d)}
fetch('/presets.json').then(function(r){return r.json()}).then(function(p){
 var ids=Object.keys(p).map(Number).filter(function(i){return i>0&&p[i]&&p[i].n&&p[i].seg}).sort(function(a,b){return propre(p[a].n).localeCompare(propre(p[b].n))});
 ids.forEach(function(i){var fx=p[i].seg[0].fx,n=p[i].n,expo=/^\d+\s+Expo\s/.test(n),vjt=/^0\s/.test(n);
  if(!tuiles[fx])tuiles[fx]={m:vjt?'auto':(expo?'expo':'manuel')};if(vjt){vjId=fx;return}
  tuile(fx,propre(n),expo?'expo':'manuel');
  if(AUSSI_EXPO.test(n))tuile(fx,propre(n),'expo')});marqueTuile()}).catch(function(){});
// reglages de l'effet en cours (noms tires de l'effet)
var fxdata=null,reglFx=-1,regVals={};
fetch('/json/fxdata').then(function(r){return r.json()}).then(function(a){fxdata=a;if(cur>=0)chargeReglages(cur)}).catch(function(){});
var CLES=['s','i','1','2','3'],KV=['sx','ix','c1','c2','c3'];
// panneau Reglages : construit une seule fois, on ne change que les noms et les valeurs (pas de clic perdu)
var regRows=[],regTgs=[];
(function(){var h=$('reglh'),o=$('reglo');
 for(var i=0;i<5;i++){var w=document.createElement('div');w.className='h';w.style.display='none';w.innerHTML='<div class="t"><span></span><b></b><span></span></div><div class="hf" data-k="r'+i+'"><div class="f"></div><div class="k"></div></div>';h.appendChild(w);
  (function(i,w){hfInit(w.querySelector('.hf'),'r'+i,function(v){thr('r'+i,'k'+CLES[i]+v)})})(i,w);regRows.push(w)}
 for(var j=0;j<3;j++){var t=document.createElement('div');t.className='tg';t.dataset.o=j;t.style.display='none';t.innerHTML='<span class="led"></span><span class="lb"></span>';o.appendChild(t);
  (function(t,j){t.addEventListener('pointerdown',function(e){e.preventDefault();var on=!t.classList.contains('on');t.classList.toggle('on',on);bloque('o');snd('k'+'abc'[j]+(on?1:0))})})(t,j);regTgs.push(t)}})();
function chargeReglages(fx){if(!fxdata||fx==reglFx)return;reglFx=fx;var md=fxdata[fx]||'',box=$('regl');
 var lab=(md.split(';')[0]||'').split(','),n=0;
 if(fx==vjId){box.classList.add('vide');return}
 for(var i=0;i<5;i++){var L=lab[i],r=regRows[i];if(!L){r.style.display='none';continue}if(L=='!')L=['Vitesse','Intensité','Réglage 1','Réglage 2','Réglage 3'][i];n++;r.style.display='';r.querySelector('b').textContent=L}
 for(var j=0;j<3;j++){var L2=lab[5+j],t=regTgs[j];if(!L2){t.style.display='none';continue}n++;t.style.display='';t.querySelector('.lb').textContent=L2}
 box.classList.toggle('vide',!n);majReglages()}
function majReglages(){for(var i=0;i<5;i++)if(regVals[KV[i]]!==undefined&&!drag['r'+i])setH('r'+i,regVals[KV[i]]);
 if(libre('o'))regTgs.forEach(function(t){t.classList.toggle('on',!!((regVals.o||0)>>t.dataset.o&1))})}
// mise a jour du firmware : le navigateur telecharge la derniere version sur GitHub et l'envoie au phare
var SRC='https://raw.githubusercontent.com/gazelita/WLED/punto1-firmware/',verLoc=null,verNet=null,majEnCours=false;
function msgMaj(t){$('majmsg').textContent=t}
function verifMaj(){fetch(SRC+'version.txt?t='+Date.now(),{cache:'no-store'}).then(function(r){return r.text()}).then(function(t){verNet=parseInt(t);afficheMaj()}).catch(function(){})}
function afficheMaj(){if(majEnCours||verNet==null||verLoc==null)return;var b=$('bmaj');
 if(verNet>verLoc){b.lastChild.textContent='Installer #'+verNet;b.classList.add('on');msgMaj('Nouvelle version disponible')}else{b.lastChild.textContent='Vérifier';b.classList.remove('on');msgMaj('À jour')}}
verifMaj();
function barre(p){$('barre').style.display='';$('barre').firstChild.style.width=Math.round(p*100)+'%'}
$('bmaj').addEventListener('click',function(){
 if(majEnCours)return;
 if(!(verNet>verLoc)){msgMaj('Recherche…');verifMaj();return}
 majEnCours=true;msgMaj('Téléchargement de la version #'+verNet+'…');barre(0);
 fetch(SRC+'firmware.bin?t='+Date.now(),{cache:'no-store'}).then(function(r){if(!r.ok)throw 0;
  var total=+r.headers.get('Content-Length')||1600000,recu=0,morceaux=[],lecteur=r.body.getReader();
  function lire(){return lecteur.read().then(function(x){if(x.done)return new Blob(morceaux);morceaux.push(x.value);recu+=x.value.length;barre(0.5*Math.min(1,recu/total));msgMaj('Téléchargement… '+Math.round(100*Math.min(1,recu/total))+' %');return lire()})}
  return lire()}).then(function(bin){
  try{ws.onclose=null;ws.close()}catch(x){}
  return new Promise(function(ok,ko){var fd=new FormData();fd.append('update',bin,'firmware.bin');var x=new XMLHttpRequest();x.open('POST','/update');
   x.upload.onprogress=function(e){if(e.lengthComputable){barre(0.5+0.45*e.loaded/e.total);msgMaj('Installation… '+Math.round(100*e.loaded/e.total)+' % (ne ferme pas la page)')}};
   var envoye=false;x.upload.onload=function(){envoye=true};
   x.onload=function(){if(x.status>=200&&x.status<300)ok();else ko(x.responseText)};x.onerror=function(){if(envoye)ok();else ko('connexion coupée pendant l\'envoi')};x.send(fd)})
 }).then(function(){var t=25;barre(0.95);(function compte(){msgMaj('Installé ! Le phare redémarre… '+t+' s');barre(0.95+0.05*(25-t)/25);if(t--<=0){location.reload();return}setTimeout(compte,1000)})()})
 .catch(function(e){majEnCours=false;$('barre').style.display='none';var t=(typeof e=='string'?e:'').replace(/<[^>]*>/g,' ').replace(/\s+/g,' ').trim();msgMaj('Échec : '+(t?t.slice(0,160):'téléchargement impossible depuis GitHub'));setTimeout(conn,500)})});
</script></body></html>
)=====";

static AsyncWebSocket phareWs("/phws");
#ifndef PHARE_BUILD
  #define PHARE_BUILD 0
#endif
static uint8_t phareIdVJ = 255;
static volatile bool phareMajEnCours = false;
static void (*phareRegFx[48])() = {nullptr}; static uint8_t phareRegId[48]; static uint8_t phareRegN = 0;
static uint8_t phareAjoute(void (*fx)(), const char* data, uint8_t id = 255) {
  uint8_t r = strip.addEffect(id, fx, data);
  if (phareRegN < 48) { phareRegFx[phareRegN] = fx; phareRegId[phareRegN++] = r; }
  return r;
}
static int phareIdDe(void (*fx)()) { for (uint8_t i = 0; i < phareRegN; i++) if (phareRegFx[i] == fx) return phareRegId[i]; return -1; }
static float phareBpmVu = 0;
// liste des effets visibles, triee par nom (pour "Suivant" en Manuel / Expo)
static uint8_t phareListe[48]; static uint8_t phareListeN = 0;
static void phareListeFaire(const char* const* nos, unsigned nb) {
  phareListeN = 0;
  for (uint8_t i = 0; i < phareRegN; i++) {
    const char* d = strip.getModeData(phareRegId[i]); bool vis = false;
    for (unsigned k = 0; k < nb; k++) if (nos[k] == d) vis = true;
    if (vis && phareListeN < 48) phareListe[phareListeN++] = phareRegId[i];
  }
  for (uint8_t a = 1; a < phareListeN; a++) for (uint8_t b = a; b > 0 && strcmp_P(strip.getModeData(phareListe[b - 1]), strip.getModeData(phareListe[b])) > 0; b--) {
    uint8_t t = phareListe[b]; phareListe[b] = phareListe[b - 1]; phareListe[b - 1] = t;
  }
}
static int phareSuivantDe(uint8_t id) {           // effet suivant de la meme famille (Soiree / Expo)
  char fam = pgm_read_byte(strip.getModeData(id));
  int pos = -1; for (uint8_t i = 0; i < phareListeN; i++) if (phareListe[i] == id) pos = i;
  if (pos < 0) return -1;
  for (uint8_t k = 1; k <= phareListeN; k++) {
    uint8_t c = phareListe[(pos + k) % phareListeN];
    if (pgm_read_byte(strip.getModeData(c)) == fam && c != phareIdVJ) return c;
  }
  return -1;
}
static volatile int16_t phareSetSx = -1, phareSetIx = -1, phareSetC1 = -1, phareSetBri = -1, phareSetSens = -1;
static uint32_t phareSensChange = 0;
static volatile int8_t phareSetOn = -1;
static volatile uint32_t phareFlashTest = 0;
static volatile int16_t phareSetK[8] = {-1, -1, -1, -1, -1, -1, -1, -1};   // reglages de l'effet : sx ix c1 c2 c3 o1 o2 o3
// memoire des reglages de chaque effet (enregistree dans le phare : /phare_fx.bin)
static uint8_t phareMemo[256][6]; static bool phareMemoOk[256];
static bool phareMemoCharge = false, phareMemoSale = false; static uint32_t phareMemoT = 0;
static void phareMemoLire() {
  phareMemoCharge = true;
  memset(phareMemoOk, 0, sizeof(phareMemoOk));
  File f = WLED_FS.open(F("/phare_fx.bin"), "r");
  if (!f) return;
  if (f.size() == sizeof(phareMemo) + sizeof(phareMemoOk)) { f.read((uint8_t*)phareMemo, sizeof(phareMemo)); f.read((uint8_t*)phareMemoOk, sizeof(phareMemoOk)); }
  f.close();
}
static void phareMemoEcrire() {
  File f = WLED_FS.open(F("/phare_fx.bin"), "w");
  if (!f) return;
  f.write((const uint8_t*)phareMemo, sizeof(phareMemo)); f.write((const uint8_t*)phareMemoOk, sizeof(phareMemoOk));
  f.close();
}
static volatile int16_t phareSetFx = -1; static uint32_t phareFxMesure = 0;

static void phareWsEvent(AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type != WS_EVT_DATA) return;
  AwsFrameInfo* info = (AwsFrameInfo*)arg;
  if (!info->final || info->index != 0 || info->len != len || info->opcode != WS_TEXT || len == 0 || len > 15) return;
  char m[16]; memcpy(m, data, len); m[len] = 0;
  switch (m[0]) {
    case 'm': if (m[1] == '-') phareMainPos = -1.0f; else { phareMainPos = constrain(atoi(m + 1), 0, 1000) / 1000.0f; phareMainT = millis(); } break;
    case 'p': pharePads = (uint8_t)atoi(m + 1); break;
    case 'b':
      if (m[1] == 'r') phareSetBri = constrain(atoi(m + 2), 1, 255);    // luminosite
      else pharePousse((uint8_t)atoi(m + 1));                           // 1 montee, 2 drop, 3 suivant, 4 +, 5 -, 6 off
      break;
    case 't': phareTapNow(); break;
    case 'o': phareSetOn = m[1] == '1'; break;                                         // on / off general
    case 'k': {                                                                        // reglage de l'effet en cours
      const char* cles = "si123abc"; const char* p = strchr(cles, m[1]);
      if (p && m[1]) phareSetK[p - cles] = constrain(atoi(m + 2), 0, 255);
      break;
    }
    case 'y': phareSync = m[1] == '1'; phareSensChange = millis(); break;               // Sync general
    case 'v': phareOverride = m[1] == '1'; phareSensChange = millis(); break;          // Override
    case 'r': { int r = atoi(m + 1); phareSync = r >= 1; phareOverride = r >= 2; phareSensChange = millis(); break; }   // Libre / Tempo / Tempo + percus
    case 'z': if (client) client->text(F("{\"z\":1}")); break;                        // test de latence (aller-retour)
    case 'l': phareLatence = constrain(atoi(m + 1), 0, 300); phareSensChange = millis(); break;   // latence micro
    case 'x': pharePads = 15; phareFlashTest = millis(); break;                          // flash test
    case 'f': phareSetFx = constrain(atoi(m + 1), 0, 254); phareFxMesure = phareMesureN; phareExpoForce = false; break;   // changer d'effet
    case 'g': phareSetFx = constrain(atoi(m + 1), 0, 254); phareFxMesure = phareMesureN; phareExpoForce = true; break;    // effet lance depuis l'onglet Expo (sans le son)
    case 'a': phareFullAuto = m[1] == '1'; phareSensChange = millis(); break;                                        // Full auto / Control
    case 'n': phareInverse = m[1] == '1'; phareSensChange = millis(); break;                                       // Inverser
    case 's':
      if (m[1] == 'x') phareSetSx = constrain(atoi(m + 2), 0, 255);
      else if (m[1] == 'e') phareSetSens = constrain(atoi(m + 2), 0, 255);
      break;
    case 'i': if (m[1] == 'x') phareSetIx = constrain(atoi(m + 2), 0, 255); break;
    case 'c': if (m[1] == '1') phareSetC1 = constrain(atoi(m + 2), 0, 255); break;
  }
}

/////////////////////
//  UserMod Class  //
/////////////////////

class UserFxUsermod : public Usermod {
 private:
 public:
  void setup() override {
    // repere le marqueur "emplacement libre" de WLED (pour reutiliser des emplacements d'effets natifs masques)
    const char** dat = strip.getModeDataSrc();
    const char* libre = nullptr;
    for (unsigned i = 1; i < strip.getModeCount(); i++) if (strcmp_P(dat[i], PSTR("RSVD")) == 0) { libre = dat[i]; break; }
    strip.addEffect(255, &mode_diffusionfire, _data_FX_MODE_DIFFUSIONFIRE);
    strip.addEffect(255, &mode_spinning_wheel, _data_FX_MODE_SPINNINGWHEEL);
    strip.addEffect(255, &mode_2D_lavalamp, _data_FX_MODE_2D_LAVALAMP);
    strip.addEffect(255, &mode_2D_magma, _data_FX_MODE_2D_MAGMA);
    strip.addEffect(255, &mode_ants, _data_FX_MODE_ANTS);
    strip.addEffect(255, &mode_morsecode, _data_FX_MODE_MORSECODE);

    ////////////////////////////////////////
    //  add your effect function(s) here  //
    ////////////////////////////////////////

    // Effets du phare
    phareAjoute(&mode_phare_bass, _data_FX_MODE_PHARE_BASS);
    phareAjoute(&mode_phare_clignomusic, _data_FX_MODE_PHARE_CLIGNOMUSIC);
    phareAjoute(&mode_phare_roll_bass, _data_FX_MODE_PHARE_ROLL_BASS);
    phareAjoute(&mode_phare_flamme, _data_FX_MODE_PHARE_FLAMME);
    phareAjoute(&mode_phare_explosion, _data_FX_MODE_PHARE_EXPLOSION);
    phareAjoute(&mode_phare_artifice, _data_FX_MODE_PHARE_ARTIFICE);
    phareAjoute(&mode_phare_mix, _data_FX_MODE_PHARE_MIX);
    phareAjoute(&mode_phare_vumetre, _data_FX_MODE_PHARE_VUMETRE);
    phareAjoute(&mode_phare_echo, _data_FX_MODE_PHARE_ECHO);
    phareAjoute(&mode_phare_drop, _data_FX_MODE_PHARE_DROP);
    phareAjoute(&mode_phare_respimusic, _data_FX_MODE_PHARE_RESPIMUSIC);
    phareIdVJ = phareAjoute(&mode_phare_dj, _data_FX_MODE_PHARE_DJ);
    phareAjoute(&mode_phare_roll, _data_FX_MODE_PHARE_ROLL);
    phareAjoute(&mode_phare_rebond, _data_FX_MODE_PHARE_REBOND);
    phareAjoute(&mode_phare_remplir, _data_FX_MODE_PHARE_REMPLIR);
    phareAjoute(&mode_phare_balle, _data_FX_MODE_PHARE_BALLE);
    phareAjoute(&mode_phare_goutte, _data_FX_MODE_PHARE_GOUTTE);
    phareAjoute(&mode_phare_alternance, _data_FX_MODE_PHARE_ALTERNANCE);
    phareAjoute(&mode_phare_clignotant, _data_FX_MODE_PHARE_CLIGNOTANT);
    phareAjoute(&mode_phare_respiration, _data_FX_MODE_PHARE_RESPIRATION);
    phareAjoute(&mode_phare_bougies, _data_FX_MODE_PHARE_BOUGIES);
    phareAjoute(&mode_phare_coeur, _data_FX_MODE_PHARE_COEUR);
    phareAjoute(&mode_phare_detresse, _data_FX_MODE_PHARE_DETRESSE);
    phareAjoute(&mode_phare_lava, _data_FX_MODE_PHARE_LAVA);
    phareAjoute(&mode_phare_orage, _data_FX_MODE_PHARE_ORAGE);
    phareAjoute(&mode_phare_neon, _data_FX_MODE_PHARE_NEON);
    phareAjoute(&mode_phare_marin, _data_FX_MODE_PHARE_MARIN);
    phareAjoute(&mode_phare_pluie, _data_FX_MODE_PHARE_PLUIE);
    phareAjoute(&mode_phare_sos, _data_FX_MODE_PHARE_SOS);
    phareAjoute(&mode_phare_auto_anim, _data_FX_MODE_PHARE_AUTO_ANIM);
    phareAjoute(&mode_phare_allbass, _data_FX_MODE_PHARE_ALLBASS);
    // nouveaux effets : WLED est limite a 255 effets, on prend la place d'effets natifs (masques) n° 1 a 7
    struct { void (*fx)(); const char* data; } neufs[] = {
      {&mode_phare_gradient, _data_FX_MODE_PHARE_GRADIENT}, {&mode_phare_duos, _data_FX_MODE_PHARE_DUOS},
      {&mode_phare_batterie, _data_FX_MODE_PHARE_BATTERIE}, {&mode_phare_question, _data_FX_MODE_PHARE_QUESTION},
      {&mode_phare_ricochet, _data_FX_MODE_PHARE_RICOCHET}, {&mode_phare_scintillement, _data_FX_MODE_PHARE_SCINTILLEMENT},
      {&mode_phare_notes, _data_FX_MODE_PHARE_NOTES},
      {&mode_phare_memoire, _data_FX_MODE_PHARE_MEMOIRE}, {&mode_phare_memoire_e, _data_FX_MODE_PHARE_MEMOIRE_E},
      {&mode_phare_pendule, _data_FX_MODE_PHARE_PENDULE}, {&mode_phare_pendule_e, _data_FX_MODE_PHARE_PENDULE_E},
      {&mode_phare_gonflement, _data_FX_MODE_PHARE_GONFLEMENT}, {&mode_phare_gonflement_e, _data_FX_MODE_PHARE_GONFLEMENT_E},
      {&mode_phare_roll_bas, _data_FX_MODE_PHARE_ROLL_BAS}, {&mode_phare_sequenceur, _data_FX_MODE_PHARE_SEQUENCEUR}, {&mode_phare_kickbass, _data_FX_MODE_PHARE_KICKBASS}};
    for (unsigned i = 0; i < sizeof(neufs) / sizeof(neufs[0]); i++) {
      if (libre) { strip.getModeDataSrc()[1 + i] = libre; phareAjoute(neufs[i].fx, neufs[i].data, 1 + i); }
      else phareAjoute(neufs[i].fx, neufs[i].data);
    }
    static const char* const nos[] = {_data_FX_MODE_PHARE_BASS, _data_FX_MODE_PHARE_FLAMME, _data_FX_MODE_PHARE_ARTIFICE, _data_FX_MODE_PHARE_RESPIMUSIC, _data_FX_MODE_PHARE_DJ, _data_FX_MODE_PHARE_ROLL, _data_FX_MODE_PHARE_REBOND, _data_FX_MODE_PHARE_REMPLIR, _data_FX_MODE_PHARE_BALLE, _data_FX_MODE_PHARE_GOUTTE, _data_FX_MODE_PHARE_ALTERNANCE, _data_FX_MODE_PHARE_CLIGNOTANT, _data_FX_MODE_PHARE_RESPIRATION, _data_FX_MODE_PHARE_BOUGIES, _data_FX_MODE_PHARE_COEUR, _data_FX_MODE_PHARE_DETRESSE, _data_FX_MODE_PHARE_LAVA, _data_FX_MODE_PHARE_ORAGE, _data_FX_MODE_PHARE_NEON, _data_FX_MODE_PHARE_MARIN, _data_FX_MODE_PHARE_PLUIE, _data_FX_MODE_PHARE_SOS, _data_FX_MODE_PHARE_ALLBASS, _data_FX_MODE_PHARE_GRADIENT, _data_FX_MODE_PHARE_DUOS, _data_FX_MODE_PHARE_BATTERIE, _data_FX_MODE_PHARE_QUESTION, _data_FX_MODE_PHARE_RICOCHET, _data_FX_MODE_PHARE_SCINTILLEMENT, _data_FX_MODE_PHARE_NOTES, _data_FX_MODE_PHARE_MEMOIRE, _data_FX_MODE_PHARE_MEMOIRE_E, _data_FX_MODE_PHARE_PENDULE, _data_FX_MODE_PHARE_PENDULE_E, _data_FX_MODE_PHARE_GONFLEMENT, _data_FX_MODE_PHARE_GONFLEMENT_E, _data_FX_MODE_PHARE_SEQUENCEUR, _data_FX_MODE_PHARE_KICKBASS};
    phareMasquerNatifs(nos, sizeof(nos) / sizeof(nos[0]));
    phareListeFaire(nos, sizeof(nos) / sizeof(nos[0]));

    // use id=255 for all custom user FX (the final id is assigned when adding the effect)

    // strip.addEffect(255, &mode_your_effect, _data_FX_MODE_YOUR_EFFECT);
    // strip.addEffect(255, &mode_your_effect2, _data_FX_MODE_YOUR_EFFECT2);
    // strip.addEffect(255, &mode_your_effect3, _data_FX_MODE_YOUR_EFFECT3);
  }


  ///////////////////////////////////////////////////////////////////////////////////////////////
  //  If you want configuration options in the usermod settings page, implement these methods  //
  ///////////////////////////////////////////////////////////////////////////////////////////////

  // void addToConfig(JsonObject& root) override
  // {
  //   JsonObject top = root.createNestedObject(FPSTR("User FX"));
  //   top["myConfigValue"] = myConfigValue;
  // }
  // bool readFromConfig(JsonObject& root) override
  // {
  //   JsonObject top = root[FPSTR("User FX")];
  //   bool configComplete = !top.isNull();
  //   configComplete &= getJsonValue(top["myConfigValue"], myConfigValue);
  //   return configComplete;
  // }

  void loop() override {
    phareLoop();                                       // tempo, kick, caisse claire, hi-hats
    // reglages venus de la page de controle
    Segment& seg = strip.getMainSegment();
    bool vj = seg.mode == phareIdVJ;
    if (phareSetSx >= 0) { if (vj) seg.speed = phareSetSx; phareSetSx = -1; }
    if (phareSetIx >= 0) { if (vj) seg.intensity = phareSetIx; phareSetIx = -1; }
    if (phareSetC1 >= 0) { if (vj) seg.custom1 = phareSetC1; phareSetC1 = -1; }
    if (phareSetSens >= 0) { phareSens = phareSetSens; phareSetSens = -1; phareSensChange = millis(); }
    if (phareSensChange && millis() - phareSensChange > 3000) { phareSensChange = 0; configNeedsWrite = true; }   // memorise la sensibilite
    // changement d'effet (tuiles) : tout de suite, ou au prochain temps 1 avec "Sur le 1"
    if (phareSetFx >= 0 && (!phareSurLe1 || !phareCale() || phareMesureN != phareFxMesure)) {
      uint16_t tr = strip.getTransition(); strip.setTransition(0);
      seg.setMode(phareSetFx, true); strip.setTransition(tr);
      if (phareMemoOk[seg.mode]) {                       // on retrouve les derniers reglages de cet effet
        const uint8_t* r = phareMemo[seg.mode];
        seg.speed = r[0]; seg.intensity = r[1]; seg.custom1 = r[2]; seg.custom2 = r[3]; seg.custom3 = r[4];
        seg.check1 = r[5] & 1; seg.check2 = (r[5] >> 1) & 1; seg.check3 = (r[5] >> 2) & 1;
      }
      phareSetFx = -1; stateUpdated(CALL_MODE_DIRECT_CHANGE);
    }
    if (phareSetOn >= 0) { if ((bri > 0) != (phareSetOn == 1)) { toggleOnOff(); stateUpdated(CALL_MODE_BUTTON); } phareSetOn = -1; }
    // apres un drop manuel : Artifice (8 mesures), ou All bass (4) puis Artifice (4), puis on revient a l'effet d'avant
    static uint8_t apresEtape = 0, apresAvant = 0, apresSeq[2]; static uint8_t apresMes[2]; static uint32_t apresDebut = 0;
    if (phareApresDrop) {
      phareApresDrop = false;
      int idAll = phareIdDe(&mode_phare_allbass), idArt = phareIdDe(&mode_phare_artifice);
      if (idAll >= 0 && idArt >= 0) {
        if (!apresEtape) apresAvant = seg.mode;
        if (hw_random8() < 128) { apresSeq[0] = idArt; apresMes[0] = 8; apresSeq[1] = 255; }
        else { apresSeq[0] = idAll; apresMes[0] = 4; apresSeq[1] = idArt; apresMes[1] = 4; }
        apresEtape = 1; apresDebut = phareMesureN; phareSetFx = apresSeq[0];
      }
    }
    if (apresEtape && phareSetFx < 0) {
      bool vj = false;
      if (seg.mode != apresSeq[apresEtape - 1]) apresEtape = 0;                     // tu as choisi un autre effet : on arrete
      else if (phareMesureN - apresDebut >= apresMes[apresEtape - 1]) {
        if (apresEtape == 1 && apresSeq[1] != 255) { apresEtape = 2; apresDebut = phareMesureN; phareSetFx = apresSeq[1]; }
        else { apresEtape = 0; phareSetFx = apresAvant; }
      }
      (void)vj;
    }
    if (phareFlashTest && millis() - phareFlashTest > 150) { phareFlashTest = 0; pharePads = 0; }
    // effet Expo (ambiance fixe) : jamais calé sur le son
    { const char* d = strip.getModeData(seg.mode); phareExpo = strncmp_P("2 Expo", d, 6) == 0 || (phareExpoForce && seg.mode != phareIdVJ); }
    // Suivant hors VJ : effet suivant de la liste (Soiree en Manuel, Expo en Expo)
    if (phareSuivantDemande && seg.mode != phareIdVJ) { phareSuivantDemande = false; int s2 = phareSuivantDe(seg.mode); if (s2 >= 0) phareSetFx = s2; }
    // reglages de l'effet envoyes par la page
    for (int i = 0; i < 8; i++) if (phareSetK[i] >= 0) {
      uint8_t v = (uint8_t)phareSetK[i]; phareSetK[i] = -1;
      switch (i) { case 0: seg.speed = v; break; case 1: seg.intensity = v; break; case 2: seg.custom1 = v; break; case 3: seg.custom2 = v; break;
                   case 4: seg.custom3 = v >> 3; break; case 5: seg.check1 = v; break; case 6: seg.check2 = v; break; case 7: seg.check3 = v; break; }
    }
    // memoire des reglages par effet
    if (!phareMemoCharge && millis() > 3000) phareMemoLire();
    if (phareMemoCharge) {
      uint8_t r[6] = {seg.speed, seg.intensity, seg.custom1, seg.custom2, seg.custom3, (uint8_t)(seg.check1 | (seg.check2 << 1) | (seg.check3 << 2))};
      if (!phareMemoOk[seg.mode] || memcmp(phareMemo[seg.mode], r, 6)) { memcpy(phareMemo[seg.mode], r, 6); phareMemoOk[seg.mode] = true; phareMemoSale = true; phareMemoT = millis(); }
      if (phareMemoSale && millis() - phareMemoT > 5000) { phareMemoSale = false; phareMemoEcrire(); }
    }
    static uint32_t briT = 0;
    if (phareSetBri >= 0 && millis() - briT > 80) { bri = phareSetBri; phareSetBri = -1; briT = millis(); stateUpdated(CALL_MODE_DIRECT_CHANGE); }
    // etat envoye a la page, 10 fois par seconde
    static uint32_t envoi = 0, kickVu = 0, nettoie = 0;
    if (!phareMajEnCours && millis() - envoi >= 200 && phareWs.count() && phareWs.availableForWriteAll()) {
      envoi = millis();
      if (phareCale()) phareBpmVu = phareBpm();          // le BPM reste affiche sur le dernier entendu
      auto nomDe = [](uint8_t id, char* nom, unsigned taille) {
        const char* d = strip.getModeData(id); unsigned i = 0;
        while (i < taille - 1) { char ch = pgm_read_byte(d + i); if (!ch || ch == '@') break; nom[i++] = (ch == '"' || ch == '\\') ? ' ' : ch; }
        nom[i] = 0;
      };
      char nom[40]; nomDe(seg.mode, nom, sizeof(nom));
      int vf = vj ? phareIdDe(phareFxJoue) : -1;
      // la vitesse de l'effet est imposee par le tempo (curseur grise sur la page)
      static void (*const caleTempo[])() = {mode_phare_roll, mode_phare_rebond, mode_phare_remplir, mode_phare_alternance, mode_phare_clignotant, mode_phare_duos};
      bool sg = false;
      if (phareSync && !phareExpo && phareCale()) for (auto f : caleTempo) if (phareIdDe(f) == seg.mode) sg = true;
      char vn[40] = ""; if (vf >= 0) nomDe((uint8_t)vf, vn, sizeof(vn));
      float tot = phareRepartition[0] + phareRepartition[1] + phareRepartition[2] + phareRepartition[3];
      int rp[4]; for (int i = 0; i < 4; i++) rp[i] = tot > 0.001f ? (int)(100.0f * phareRepartition[i] / tot + 0.5f) : 0;
      // ce que le VJ joue, en clair
      char vi[96] = "";
      if (vj && phareVJCat <= 4) {
        static const char* const catN[] = {"silence", "sans beat", "calme", "moyen", "excité"};
        static const char* const forN[] = {"tempo seul", "kick seul", "effet + chick", "tout"};
        static const char* const varN[] = {"", " · demi-temps", " · double-temps", " · contretemps", " · triolets", " · accent sur le 1", " · tenu", " · sec"};
        static const char* const kickN[] = {"kick lampe 1", "kick 1+2", "kick rebond", "kick lampe 2", "kick lampe 3", "kick lampe 4", "kick 1+4", "kick 2+3", "kick qui monte"};
        if (phareVJPool == 5) snprintf(vi, sizeof(vi), "breakdown mélodique · notes%s", M.accord >= 2 ? " · accords" : "");
        else if (phareVJCat >= 2) snprintf(vi, sizeof(vi), "%s · %s%s · %s%s", phareVJPool == 6 ? "pic" : catN[phareVJCat], phareVJCat == 2 && O.formation == 3 ? "1 lampe sur le kick" : forN[O.formation % 4],
                                      phareVJCat >= 3 ? varN[O.variante % 8] : "", O.formation == 0 ? "sans flash" : kickN[T.kickMode % 9], phareCalmeAuto ? " · calme auto" : "");
        else snprintf(vi, sizeof(vi), "%s", catN[phareVJCat]);
      }
      char buf[640];
      snprintf(buf, sizeof(buf), "{\"b\":%d,\"k\":%d,\"vj\":%d,\"fx\":\"%s\",\"sx\":%d,\"ix\":%d,\"c1\":%d,\"se\":%d,\"br\":%d,\"fm\":%d,\"mt\":%d,\"dr\":%d,\"sy\":%d,\"s1\":%d,\"v\":%d,\"on\":%d,\"lv\":%d,\"l\":[%d,%d,%d,%d],\"pf\":%d,\"pa\":%d,\"bc\":%d,\"vf\":%d,\"vn\":\"%s\",\"c2\":%d,\"c3\":%d,\"o\":%d,\"nm\":%d,\"ov\":%d,\"ex\":%d,\"sg\":%d,\"rt\":%d,\"lat\":%d,\"ca\":%d,\"inv\":%d,\"xf\":%d,\"rp\":[%d,%d,%d,%d],\"vi\":\"%s\",\"fa\":%d,\"me\":%d}",
               (int)(phareBpmVu + 0.5f), T.kickFlash != kickVu ? 1 : 0, vj ? 1 : 0, nom,
               (vj && phareCalmeAuto) ? min((int)seg.speed, 76) : seg.speed, seg.intensity, (vj && phareCalmeAuto) ? min(255, seg.custom1 + 51) : seg.custom1, phareSens, bri, seg.mode,
               DR.etat == 1 ? (int)(DR.vitesse * 2) : 0, DR.etat == 2 ? 1 : (DR.etat == 3 ? 2 : 0), phareSync ? 1 : 0, phareSurLe1 ? 1 : 0, PHARE_BUILD, bri > 0 ? 1 : 0, (int)constrain(phareVol, 0.0f, 255.0f),
               bri ? phareSortie[0] : 0, bri ? phareSortie[1] : 0, bri ? phareSortie[2] : 0, bri ? phareSortie[3] : 0, phareSetFx >= 0 ? 1 : 0, DR.suivantAttente ? 1 : 0, phareCale() ? 1 : 0, vf, vn, seg.custom2, seg.custom3 << 3, seg.check1 | (seg.check2 << 1) | (seg.check3 << 2), phareNoirManuel ? 1 : 0, phareOverride ? 1 : 0, phareExpo ? 1 : 0, sg ? 1 : 0, phareSync ? (phareOverride ? 2 : 1) : 0, phareLatence, (vj && phareCalmeAuto) ? 1 : 0, phareInverse ? 1 : 0, phareExpoForce ? 1 : 0, rp[0], rp[1], rp[2], rp[3], vi, phareFullAuto ? 1 : 0, (int)(M.presence * 100));
      kickVu = T.kickFlash;
      static char dernierEnvoi[640] = "";
      if (strcmp(buf, dernierEnvoi)) { strcpy(dernierEnvoi, buf); phareWs.textAll(buf); }   // seulement si quelque chose a change
    }
    if (millis() - nettoie > 2000) { nettoie = millis(); phareWs.cleanupClients(); }
  }

  // mise a jour en cours : on arrete d'envoyer l'etat a la page (moins de charge pendant l'ecriture)
  void onUpdateBegin(bool debut) override { phareMajEnCours = debut; if (debut) phareWs.closeAll(); }

  void connected() override {
    static bool fait = false;
    if (fait) return;
    fait = true;
    phareWs.onEvent(phareWsEvent);
    server.addHandler(&phareWs);
    server.on(F("/phare"), HTTP_GET, [](AsyncWebServerRequest* request) { request->send_P(200, F("text/html"), PAGE_PHARE); });
  }

  // bouton 4 de la telecommande = tap tempo
  bool onEspNowMessage(uint8_t* sender, uint8_t* payload, uint8_t len) override { return phareTap(payload, len); }

  // boutons (favoris rapides) : {"phare":{"montee":1}} / {"phare":{"drop":1}} / {"phare":{"suivant":1}}
  void readFromJsonState(JsonObject& root) override {
    JsonObject ph = root[F("phare")];
    if (ph.isNull()) return;
    if (ph[F("montee")] | 0) pharePousse(1);
    if (ph[F("drop")] | 0) pharePousse(2);
    if (ph[F("suivant")] | 0) pharePousse(3);
  }

  // BPM affiche dans l'ecran Info
  void addToJsonInfo(JsonObject& root) override {
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");
    JsonArray tempo = user.createNestedArray(F("Tempo"));
    if (phareCale()) { tempo.add((int)(phareBpm() + 0.5f)); tempo.add(F(" BPM")); }
    else tempo.add(F("pas de kick"));
  }

  void addToConfig(JsonObject& root) override {
    JsonObject top = root.createNestedObject(F("Phare"));
    top[F("Latence micro (ms)")] = phareLatence;
    top[F("Sensibilite generale (0-255)")] = phareSens;
    top[F("Sync general")] = phareSync;
    top[F("Override")] = phareOverride;
    top[F("Inverser")] = phareInverse;
    top[F("Full auto")] = phareFullAuto;
  }
  bool readFromConfig(JsonObject& root) override {
    JsonObject top = root[F("Phare")];
    bool ok = !top.isNull();
    ok &= getJsonValue(top[F("Latence micro (ms)")], phareLatence, (uint16_t)60);
    ok &= getJsonValue(top[F("Sensibilite generale (0-255)")], phareSens, (uint8_t)220);
    ok &= getJsonValue(top[F("Sync general")], phareSync, true);
    ok &= getJsonValue(top[F("Override")], phareOverride, false);
    ok &= getJsonValue(top[F("Inverser")], phareInverse, false);
    ok &= getJsonValue(top[F("Full auto")], phareFullAuto, false);
    return ok;
  }
  uint16_t getId() override { return USERMOD_ID_USER_FX; }
};

static UserFxUsermod user_fx;
REGISTER_USERMOD(user_fx);
