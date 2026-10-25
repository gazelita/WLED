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
  float    q;                 // libre (pic du VU-metre, energie...)
};

// ---- tempo de la musique, mesure en continu ----
static float    phareTempoMs   = 500.0f;  // duree d'un temps (ms)
static uint32_t phareTempoLast = 0;       // dernier temps detecte
static uint8_t  phareTempoNb   = 0;       // nombre de temps reguliers consecutifs
static bool phareMusique() { return phareTempoNb >= 4 && (millis() - phareTempoLast) < 2500; }

// courbe filament + prechauffage : niveau percu (0..1) -> valeur envoyee a l'ampoule
static uint8_t phareOut(float level) {
  level = constrain(level, 0.0f, 1.0f);
  float duty = powf(level, 1.3f);              // un filament parait sombre en bas : on compense
  if (duty < 0.02f) duty = 0.02f;              // prechauffage : jamais completement froid (invisible)
  int v = (int)(duty * 255.0f + 0.5f);
  if (gammaCorrectCol) v = gamma8inv(v);       // annule la correction gamma de WLED (faite pour les LED)
  return v;
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

// affiche les intensites, avec un fond "veilleuse" minimum
static void phareDraw(PhareState* s, unsigned n, uint8_t veilleuse) {
  uint32_t c = SEGCOLOR(0) ? SEGCOLOR(0) : WHITE;
  for (unsigned i = 0; i < n; i++)
    SEGMENT.setPixelColor(i, color_fade(c, phareOut(max(s->level[i], veilleuse / 255.0f))));
}

static float phareRetombee(uint8_t speed) { return map(speed, 0, 255, 1500, 80); }

static float phareSmooth(float x) { x = constrain(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); }

static void phareShow(const float* lv, unsigned n, uint8_t veilleuse) {
  uint32_t c = SEGCOLOR(0) ? SEGCOLOR(0) : WHITE;
  for (unsigned i = 0; i < n; i++)
    SEGMENT.setPixelColor(i, color_fade(c, phareOut(max(lv[i], veilleuse / 255.0f))));
}

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
    (fft[0] + fft[1] + fft[2]) / 3.0f,                  // basses
    (fft[5] + fft[6] + fft[7] + fft[8] + fft[9]) / 5.0f,  // mediums
    (fft[12] + fft[13] + fft[14] + fft[15]) / 4.0f,       // aigus
    *(float*)um->u_data[0]                                // volume
  };
  float sens = SEGMENT.intensity / 255.0f;
  float plancher = 0.45f - sens * 0.35f;                // part du maximum consideree comme "rien"
  float k = 2.0f - sens * 0.8f;                         // force d'un coup par rapport a la moyenne
  float decroit = powf(0.5f, dt / 4000.0f);             // le maximum retombe de moitie en 4 s
  float out[4]; bool beat[4];
  for (int i = 0; i < 4; i++) {
    float v = phareSeuil(raw[i]);
    if (SEGENV.call == 0) { s->pk[i] = 40.0f; s->av[i] = 20.0f; }
    s->pk[i] = max(v, max(s->pk[i] * decroit, 25.0f));
    float n = v / s->pk[i];
    out[i] = constrain((n - plancher) / (1.0f - plancher), 0.0f, 1.0f);
    beat[i] = v > s->av[i] * k && v > s->pk[i] * 0.5f && strip.now - s->lb[i] > (i == 0 ? 180u : 90u);
    if (beat[i]) s->lb[i] = strip.now;
    s->av[i] = s->av[i] * 0.95f + v * 0.05f; if (s->av[i] < 6.0f) s->av[i] = 6.0f;
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
  for (unsigned i = 0; i < half; i++) s->level[i] = max(s->level[i], son.b);
  if (son.tb) { s->side = (s->side + 1) % (n - half); s->level[half + s->side] = 1.0f; }
  phareDraw(s, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_BASS[] PROGMEM = "01 Musique bass@Retombee,Sensibilite,Veilleuse,Seuil;!;;1f;sx=150,ix=128,c1=0,c2=30";

// 02. Clignotant musical : ampoule 2 = basses, 1 et 3 = mediums (en alternance), 4 = aigus
static void mode_phare_clignomusic(void) {
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  PhareSon son = phareEcoute(s, dt);
  phareDecay(s, n, dt, phareRetombee(SEGMENT.speed));
  unsigned lb = min(1u, n - 1), lt = n - 1;
  s->level[lb] = max(s->level[lb], son.b);
  if (son.mb) {                                          // mediums : les autres ampoules, chacune son tour
    unsigned tries = 0;
    do { s->side = (s->side + 1) % n; } while ((s->side == lb || s->side == lt) && ++tries < n);
    if (s->side != lb && s->side != lt) s->level[s->side] = 1.0f;
  }
  if (son.tb && lt != lb) s->level[lt] = 1.0f;
  phareDraw(s, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_CLIGNOMUSIC[] PROGMEM = "02 Clignotant musical@Retombee,Sensibilite,Veilleuse,Seuil;!;;1f;sx=190,ix=128,c1=0,c2=30";

// 03. Roll basse : la lumiere monte d'une ampoule a chaque coup de basse
static void mode_phare_roll_bass(void) {
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  PhareSon son = phareEcoute(s, dt);
  phareDecay(s, n, dt, phareRetombee(SEGMENT.speed));
  if (son.bb) { s->pos = (s->pos + 1) % n; s->level[s->pos] = 1.0f; }
  phareDraw(s, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_ROLL_BASS[] PROGMEM = "03 Roll basse@Retombee,Sensibilite,Veilleuse,Seuil;!;;1f;sx=120,ix=128,c1=20,c2=30";

// 04. Flamme musicale : braise en bas, la flamme monte avec la musique et vacille en haut
static void mode_phare_flamme(void) {
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  PhareSon son = phareEcoute(s, dt);
  float cible = max(son.v, son.b);
  float up = 0.08f + SEGMENT.speed / 255.0f * 0.5f;
  if (cible > s->env) s->env += (cible - s->env) * up; else s->env += (cible - s->env) * up * 0.2f;
  float h = 0.3f + s->env * (n - 0.3f);                 // hauteur de flamme, en ampoules
  float braise = SEGMENT.custom1 / 255.0f;
  float lv[PHARE_MAX];
  for (unsigned i = 0; i < n; i++) {
    float f = constrain(h - i, 0.0f, 1.0f);
    float vac = 1.0f - (hw_random8() / 255.0f) * (0.25f + 0.5f * (float)i / n);
    lv[i] = f * vac * (0.35f + 0.65f * s->env);         // plus de son = plus lumineux
  }
  lv[0] = max(lv[0], braise * (0.7f + hw_random8() / 850.0f));
  phareShow(lv, n, 0);
}
static const char _data_FX_MODE_PHARE_FLAMME[] PROGMEM = "04 Flamme musicale@Reactivite,Sensibilite,Braise,Seuil;!;;1v;sx=128,ix=128,c1=40,c2=30";

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
static const char _data_FX_MODE_PHARE_EXPLOSION[] PROGMEM = "05 Explosion@Retombee,Sensibilite,Veilleuse,Seuil;!;;1f;sx=140,ix=128,c1=0,c2=30";

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
static const char _data_FX_MODE_PHARE_ARTIFICE[] PROGMEM = "06 Artifice@Brievete,Sensibilite,Veilleuse,Seuil;!;;1f;sx=180,ix=140,c1=0,c2=30";

// 07. VU-metre a pic : la lumiere monte avec le volume, un point reste en haut puis retombe
static void mode_phare_vumetre(void) {
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  PhareSon son = phareEcoute(s, dt);
  float cible = max(son.v, son.b) * n;
  if (cible > s->env) s->env = cible; else s->env += (cible - s->env) * 0.15f;
  if (s->env >= s->q) { s->q = s->env; s->t0 = strip.now; }              // nouveau pic
  else if (strip.now - s->t0 > 400) s->q = max(0.0f, s->q - dt / map(SEGMENT.speed, 0, 255, 900, 120)); // retombe
  float lv[PHARE_MAX];
  unsigned pic = (unsigned)min((float)(n - 1), max(0.0f, s->q - 0.5f));
  for (unsigned i = 0; i < n; i++) lv[i] = constrain(s->env - i, 0.0f, 1.0f) * 0.8f;
  if (s->q > 0.6f) lv[pic] = 1.0f;
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_VUMETRE[] PROGMEM = "07 VU-metre a pic@Chute du pic,Sensibilite,Veilleuse,Seuil;!;;1v;sx=128,ix=128,c1=0,c2=30";

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
static const char _data_FX_MODE_PHARE_ECHO[] PROGMEM = "08 Echo@Vitesse,Sensibilite,Veilleuse,Seuil;!;;1f;sx=110,ix=128,c1=0,c2=30";

// 09. Drop : douce pendant la montee, stroboscope quand la basse revient fort
static void mode_phare_drop(void) {
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  PhareSon son = phareEcoute(s, dt);
  bool calme = s->t0 && strip.now - s->t0 > 2000;     // plus de 2 s sans basse = montee
  if (son.bb && calme) s->t1 = strip.now + 2500 + SEGMENT.speed * 12;   // la basse revient : DROP
  if (son.b < 0.25f) { if (!s->t0) s->t0 = strip.now; } else s->t0 = 0;
  float lv[PHARE_MAX];
  if (strip.now < s->t1) {                               // stroboscope
    bool on = (strip.now / 45) % 2 == 0;
    for (unsigned i = 0; i < n; i++) lv[i] = on ? 1.0f : 0.0f;
  } else {                                               // en attendant : lueur qui suit la musique
    s->env += (son.v * 0.5f - s->env) * 0.1f;
    for (unsigned i = 0; i < n; i++) lv[i] = s->env * (1.0f - 0.15f * i);
    if (son.bb) for (unsigned i = 0; i < n; i++) lv[i] = max(lv[i], 0.7f);
  }
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_DROP[] PROGMEM = "09 Drop@Duree du strobo,Sensibilite,Veilleuse,Seuil;!;;1f;sx=80,ix=128,c1=0,c2=30";

// 10. Respiration musicale : respire lentement, plus fort quand la musique est forte
static void mode_phare_respimusic(void) {
  uint32_t dt; PhareState* s = phareInit(dt); if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  PhareSon son = phareEcoute(s, dt);
  s->env += (son.v - s->env) * min(1.0f, dt / 1500.0f);  // volume moyen, lent
  float period = map(SEGMENT.speed, 0, 255, 8000, 2000);
  if (phareMusique()) period = phareTempoMs * 8.0f;      // calee sur 8 temps
  float x = (strip.now % (uint32_t)period) / period;
  float b = (expf(sinf(2.0f * PI * x - PI / 2.0f)) - 0.36788f) / 2.35040f;
  float lv[PHARE_MAX];
  for (unsigned i = 0; i < n; i++) lv[i] = 0.08f + b * (0.25f + 0.75f * s->env);
  phareShow(lv, n, 0);
}
static const char _data_FX_MODE_PHARE_RESPIMUSIC[] PROGMEM = "10 Respiration musicale@Vitesse,Sensibilite,,Seuil;!;;1v;sx=100,ix=128,c2=30";

/////////////////////////////////////////////////////////////
//  Animations du phare (sans musique) : remplacent les "_etape"
/////////////////////////////////////////////////////////////

// joue une suite d'ampoules : chacune s'allume en fondu pendant que la precedente s'eteint
static void phareSequence(const uint8_t* seq, unsigned len, unsigned n, float D, float Dlast, float fadeFrac, float* lv) {
  float cycle = D * (len - 1) + Dlast;
  float tc = (float)(strip.now % (uint32_t)max(cycle, 1.0f));
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
static void mode_phare_roll(void) {
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  uint8_t seq[PHARE_MAX]; for (unsigned i = 0; i < n; i++) seq[i] = i;
  float D = phareStepMs(SEGMENT.speed);
  float Dlast = SEGMENT.check1 ? D * 0.4f : D;
  float lv[PHARE_MAX];
  phareSequence(seq, n, n, D, Dlast, 0.1f + SEGMENT.intensity / 255.0f * 0.9f, lv);
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_ROLL[] PROGMEM = "12 Roll@Vitesse,Fondu,Veilleuse,,,Retour rapide;!;;1;sx=150,ix=120,c1=0,o1=1";

// Rebond : aller-retour bas -> haut -> bas
static void mode_phare_rebond(void) {
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  uint8_t seq[2 * PHARE_MAX]; unsigned len = 0;
  for (unsigned i = 0; i < n; i++) seq[len++] = i;
  for (int i = n - 2; i > 0; i--) seq[len++] = i;
  float D = phareStepMs(SEGMENT.speed);
  float lv[PHARE_MAX];
  phareSequence(seq, len, n, D, D, 0.1f + SEGMENT.intensity / 255.0f * 0.9f, lv);
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_REBOND[] PROGMEM = "13 Rebond@Vitesse,Fondu,Veilleuse;!;;1;sx=150,ix=120,c1=0";

// Remplissage : la colonne se remplit, reste pleine, puis s'eteint (ou se vide par le haut)
static void mode_phare_remplir(void) {
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  float D = phareStepMs(SEGMENT.speed);
  float hold = D * (1.0f + SEGMENT.intensity / 255.0f * 6.0f);
  bool vider = SEGMENT.check1;
  float tFill = D * n, tEmpty = vider ? D * n : D * 2.0f, tOff = D * 1.5f;
  float cycle = tFill + hold + tEmpty + tOff;
  float t = (float)(strip.now % (uint32_t)cycle);
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
static const char _data_FX_MODE_PHARE_REMPLIR[] PROGMEM = "14 Remplissage@Vitesse,Temps plein,Veilleuse,,,Vider par le haut;!;;1;sx=150,ix=60,c1=0";

// Balle qui rebondit : tombe du haut et rebondit de moins en moins haut
static void mode_phare_balle(void) {
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  float H = n - 1;
  float tFall = map(SEGMENT.speed, 0, 255, 1600, 300) / 1000.0f;   // duree de la premiere chute (s)
  float g = 2.0f * H / (tFall * tFall);
  float e = 0.45f + SEGMENT.intensity / 255.0f * 0.4f;             // rebond
  float v = g * tFall, tot = tFall;
  float times[12], vels[12]; unsigned nb = 0;
  while (nb < 12) { v *= e; if (v * v / (2 * g) < 0.15f) break; vels[nb] = v; times[nb] = 2 * v / g; tot += times[nb]; nb++; }
  float rest = 1.2f;
  float t = (strip.now % (uint32_t)((tot + rest) * 1000.0f)) / 1000.0f;
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
static const char _data_FX_MODE_PHARE_BALLE[] PROGMEM = "15 Balle qui rebondit@Vitesse,Rebond,Veilleuse;!;;1;sx=128,ix=128,c1=0";

// Goutte : se forme en haut, tombe en accelerant, eclabousse en bas
static void mode_phare_goutte(void) {
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  float k = map(SEGMENT.speed, 0, 255, 2500, 500) / 1000.0f;       // echelle de temps
  float tForm = 1.2f * k * (0.5f + SEGMENT.intensity / 255.0f), tFall = 0.45f * k, tSplash = 0.5f * k, tPause = 0.8f * k;
  float cycle = tForm + tFall + tSplash + tPause;
  float t = (strip.now % (uint32_t)(cycle * 1000.0f)) / 1000.0f;
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
static const char _data_FX_MODE_PHARE_GOUTTE[] PROGMEM = "16 Goutte@Vitesse,Formation,Veilleuse;!;;1;sx=128,ix=128,c1=0";

// Alternance : 1+3 / 2+4, ou centre / bords
static void mode_phare_alternance(void) {
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  float period = map(SEGMENT.speed, 0, 255, 4000, 300);
  float s = 0.5f + 0.5f * sinf(2.0f * PI * (strip.now % (uint32_t)period) / period);
  float sharp = 1.0f + SEGMENT.intensity / 255.0f * 6.0f;          // plus haut = transition plus franche
  float a = constrain((s - 0.5f) * sharp + 0.5f, 0.0f, 1.0f);
  float lv[PHARE_MAX];
  for (unsigned i = 0; i < n; i++) {
    bool grpA = SEGMENT.check1 ? (i == 0 || i == n - 1) : (i % 2 == 0);
    lv[i] = grpA ? a : 1.0f - a;
  }
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_ALTERNANCE[] PROGMEM = "17 Alternance@Vitesse,Nettete,Veilleuse,,,Centre et bords;!;;1;sx=120,ix=40,c1=0";

// Clignotant sequentiel : remplissage sec, puis noir
static void mode_phare_clignotant(void) {
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  float D = map(SEGMENT.speed, 0, 255, 400, 60);
  float pauseU = 1.0f + SEGMENT.intensity / 255.0f * 4.0f;
  float cycle = D * n + D * 1.5f + D * pauseU;
  float t = (float)(strip.now % (uint32_t)cycle);
  if (SEGMENT.check2 && phareMusique()) {           // un cycle complet = 4 temps
    cycle = phareTempoMs * 4.0f; D = cycle / (n + 1.5f + pauseU);
    t = fmodf((float)(millis() - phareTempoLast), cycle);
  }
  float lv[PHARE_MAX];
  for (unsigned i = 0; i < n; i++) lv[i] = (t >= i * D && t < D * n + D * 1.5f) ? 1.0f : 0.0f;
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_CLIGNOTANT[] PROGMEM = "18 Clignotant sequentiel@Vitesse,Pause,Veilleuse,,,,Suivre la musique;!;;1;sx=128,ix=100,c1=0,o2=1";

// Respiration : douce, avec un minimum reglable, ensemble ou decalee
static void mode_phare_respiration(void) {
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  float period = map(SEGMENT.speed, 0, 255, 9000, 1200);
  float lo = SEGMENT.intensity / 255.0f, hi = max(lo, SEGMENT.custom1 / 255.0f);
  float lv[PHARE_MAX];
  for (unsigned i = 0; i < n; i++) {
    float ph = SEGMENT.check1 ? (float)i / n : 0.0f;
    float x = fmodf((strip.now % (uint32_t)period) / period + ph, 1.0f);
    float b = (expf(sinf(2.0f * PI * x - PI / 2.0f)) - 0.36788f) / 2.35040f;   // respiration naturelle 0..1
    lv[i] = lo + (hi - lo) * b;
  }
  phareShow(lv, n, 0);
}
static const char _data_FX_MODE_PHARE_RESPIRATION[] PROGMEM = "19 Respiration@Vitesse,Minimum,Maximum,,,Decalee;!;;1;sx=60,ix=40,c1=255";

// SOS en morse sur toute la colonne
static void mode_phare_sos(void) {
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  static const char pat[] = "101010001110111011100010101000000000";  // 1 = allume, une case = une unite
  unsigned len = sizeof(pat) - 1;
  uint32_t u = map(SEGMENT.speed, 0, 255, 400, 80);
  bool on = pat[(strip.now / u) % len] == '1';
  float lv[PHARE_MAX]; for (unsigned i = 0; i < n; i++) lv[i] = on ? 1.0f : 0.0f;
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_SOS[] PROGMEM = "29 SOS@Vitesse,,Veilleuse;!;;1;sx=128,c1=0";

// Test ordre : une ampoule a la fois, 1 seconde chacune
static void mode_phare_test(void) {
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  unsigned k = (strip.now / 1000) % n;
  float lv[PHARE_MAX]; for (unsigned i = 0; i < n; i++) lv[i] = (i == k) ? 1.0f : 0.0f;
  phareShow(lv, n, 0);
}
static const char _data_FX_MODE_PHARE_TEST[] PROGMEM = "30 Test ordre@;!;;1";


/////////////////////////////////////////////////////////////
//  Effets d'ambiance
/////////////////////////////////////////////////////////////

// Bougies : algorithme "Candle" de WLED, une bougie independante par ampoule
static void mode_phare_bougies(void) {
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
static const char _data_FX_MODE_PHARE_BOUGIES[] PROGMEM = "20 Bougies@Vitesse,Vacillement,Intensite,,,Plus fort en bas;!;;1;sx=96,ix=224,c1=110";

// Battement de coeur : double pulsation
static void mode_phare_coeur(void) {
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  float period = 60000.0f / map(SEGMENT.speed, 0, 255, 40, 150);   // 40 a 150 battements/min
  float t = (strip.now % (uint32_t)period) / period;
  if (SEGMENT.check2 && phareMusique()) { period = phareTempoMs; t = fmodf((millis() - phareTempoLast) / period, 1.0f); }
  float second = 0.3f + SEGMENT.intensity / 255.0f * 0.7f;
  float b = expf(-t * 25.0f) + second * expf(-fmaxf(t - 0.22f, 0.0f) * 25.0f) * (t >= 0.22f ? 1.0f : 0.0f);
  float lv[PHARE_MAX]; for (unsigned i = 0; i < n; i++) lv[i] = min(b, 1.0f);
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_COEUR[] PROGMEM = "21 Battement de coeur@Rythme,Second battement,Veilleuse,,,,Suivre la musique;!;;1;sx=60,ix=150,c1=10,o2=1";

// Feux de detresse : clignotement avec montee et descente rapides comme une ampoule
static void mode_phare_detresse(void) {
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  float period = map(SEGMENT.speed, 0, 255, 1500, 400);
  float t = (strip.now % (uint32_t)period) / period;
  if (SEGMENT.check2 && phareMusique()) { period = phareTempoMs * 2.0f; t = fmodf((millis() - phareTempoLast) / period, 1.0f); }
  float ramp = 0.04f + SEGMENT.intensity / 255.0f * 0.2f;
  float b = t < 0.5f ? phareSmooth(t / ramp) : 1.0f - phareSmooth((t - 0.5f) / ramp);
  float lv[PHARE_MAX]; for (unsigned i = 0; i < n; i++) lv[i] = b;
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_DETRESSE[] PROGMEM = "22 Feux de detresse@Vitesse,Douceur,Veilleuse,,,,Suivre la musique;!;;1;sx=128,ix=60,c1=0,o2=1";

// Lava lampe : des bulles de lumiere montent et descendent lentement et se melangent
static void mode_phare_lava(void) {
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  float sp = 0.05f + SEGMENT.speed / 255.0f * 0.6f;                 // tours par seconde (environ)
  unsigned blobs = 1 + SEGMENT.intensity / 86;                      // 1 a 3 bulles
  float t = strip.now / 1000.0f;
  static const float fr[3] = {1.0f, 0.73f, 1.31f}, ph[3] = {0.0f, 2.1f, 4.2f};
  float lv[PHARE_MAX]; for (unsigned i = 0; i < n; i++) lv[i] = 0.0f;
  for (unsigned b = 0; b < blobs; b++) {
    float x = sinf(t * sp * fr[b] + ph[b]) * 0.6f + sinf(t * sp * fr[b] * 0.37f + ph[b] * 1.7f) * 0.4f;
    float y = (x * 0.5f + 0.5f) * (n - 1);
    for (unsigned i = 0; i < n; i++) { float d = (i - y) / 0.9f; lv[i] += expf(-d * d); }
  }
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_LAVA[] PROGMEM = "23 Lava lampe@Vitesse,Bulles,Veilleuse;!;;1;sx=60,ix=120,c1=15";

// Ascenseur : la lumiere glisse d'etage en etage, s'arrete, repart
static void mode_phare_ascenseur(void) {
  uint32_t dt; PhareState* s = phareInit(dt);
  if (!s) FX_FALLBACK_STATIC;
  unsigned n = phareN(); if (n < 2) FX_FALLBACK_STATIC;
  float msEtage = map(SEGMENT.speed, 0, 255, 1200, 200);
  if (SEGENV.call == 0) { s->p0 = 0; s->p1 = 0; s->t0 = strip.now; s->t1 = strip.now; }
  float trajet = fabsf(s->p1 - s->p0) * msEtage;
  float pause = 400.0f + (255 - SEGMENT.intensity) * 12.0f;
  float el = strip.now - s->t0;
  if (el > trajet + pause) {                                        // nouvel etage
    s->p0 = s->p1;
    unsigned nf; do { nf = hw_random8() % n; } while (nf == (unsigned)s->p0);
    s->p1 = nf; s->t0 = strip.now; el = 0; trajet = fabsf(s->p1 - s->p0) * msEtage;
  }
  float u = trajet > 0 ? phareSmooth(el / trajet) : 1.0f;
  float y = s->p0 + (s->p1 - s->p0) * u;
  float lv[PHARE_MAX];
  for (unsigned i = 0; i < n; i++) lv[i] = max(0.0f, 1.0f - fabsf((float)i - y));
  phareShow(lv, n, SEGMENT.custom1);
}
static const char _data_FX_MODE_PHARE_ASCENSEUR[] PROGMEM = "24 Ascenseur@Vitesse,Pauses courtes,Veilleuse;!;;1;sx=120,ix=100,c1=10";

// Orage : nuit calme, puis eclairs en rafales, plutot du haut
static void mode_phare_orage(void) {
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
static const char _data_FX_MODE_PHARE_ORAGE[] PROGMEM = "25 Orage@,Frequence,Ciel;!;;1;ix=40,c1=8";

// Neon defectueux : allume, avec des crises de gresillement a intervalle aleatoire (15 s minimum)
static void mode_phare_neon(void) {
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
static const char _data_FX_MODE_PHARE_NEON[] PROGMEM = "26 Neon defectueux@,Frequence des pannes;!;;1;ix=100";

// Phare marin : un faisceau tourne, la colonne s'illumine a chaque passage
static void mode_phare_marin(void) {
  unsigned n = phareN(); if (n < 1) FX_FALLBACK_STATIC;
  float period = map(SEGMENT.speed, 0, 255, 8000, 1500);
  float t = (strip.now % (uint32_t)period) / period;
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
static const char _data_FX_MODE_PHARE_MARIN[] PROGMEM = "27 Phare marin@Rotation,Largeur faisceau,Veilleuse,,,Double eclat;!;;1;sx=90,ix=110,c1=20";

// Pluie : des gouttes tombent du haut, plusieurs a la fois
static void mode_phare_pluie(void) {
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
static const char _data_FX_MODE_PHARE_PLUIE[] PROGMEM = "28 Pluie@Vitesse,Densite,Veilleuse;!;;1;sx=110,ix=80,c1=0";

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

// 11. DJ auto : ecoute la musique et choisit l'effet qui colle
//   silence -> bougies ; morceau calme -> respiration / flamme ; montee puis basse -> drop ;
//   basse forte et reguliere -> explosion, roll basse, musique bass, echo ;
//   beaucoup d'aigus -> artifice, clignotant musical ; le reste -> VU-metre
struct PhareDJ { float energie, aigus, basses; uint32_t silence, sansBasse, choix, verrou; uint8_t cat; };

static void mode_phare_dj(void) {
  if (!SEGENV.allocateData(sizeof(PhareState) + sizeof(PhareAuto) + sizeof(PhareDJ))) FX_FALLBACK_STATIC;
  PhareAuto* a = reinterpret_cast<PhareAuto*>(SEGENV.data + sizeof(PhareState));
  PhareDJ*   d = reinterpret_cast<PhareDJ*>(SEGENV.data + sizeof(PhareState) + sizeof(PhareAuto));
  um_data_t* um = phareAudio();
  uint8_t* fft = (uint8_t*)um->u_data[2];
  float vol = *(float*)um->u_data[0];
  float bass = (fft[0] + fft[1] + fft[2]) / 3.0f, treb = (fft[12] + fft[13] + fft[14] + fft[15]) / 4.0f;
  if (SEGENV.call == 0) { memset(d, 0, sizeof(PhareDJ)); d->cat = 255; }
  d->energie = d->energie * 0.98f + vol * 0.02f;
  d->basses  = d->basses  * 0.98f + bass * 0.02f;
  d->aigus   = d->aigus   * 0.98f + treb * 0.02f;
  if (vol > 3.0f) d->silence = strip.now;
  if (bass > 25.0f) d->sansBasse = strip.now;

  static const PhareChoix bougie[]  = {{mode_phare_bougies, 96, 224, 90, 0, false, false}};
  static const PhareChoix calme[]   = {{mode_phare_respimusic, 100, 128, 0, 30, false, false}, {mode_phare_flamme, 128, 128, 40, 30, false, false}};
  static const PhareChoix drop[]    = {{mode_phare_drop, 80, 128, 0, 30, false, false}};
  static const PhareChoix basse[]   = {{mode_phare_explosion, 140, 128, 0, 30, false, false}, {mode_phare_roll_bass, 120, 128, 20, 30, false, false},
                                       {mode_phare_bass, 150, 128, 0, 30, false, false},      {mode_phare_echo, 110, 128, 0, 30, false, false}};
  static const PhareChoix aigu[]    = {{mode_phare_artifice, 180, 140, 0, 30, false, false}, {mode_phare_clignomusic, 190, 128, 0, 30, false, false}};
  static const PhareChoix autre[]   = {{mode_phare_vumetre, 128, 128, 0, 30, false, false}, {mode_phare_bass, 150, 128, 0, 30, false, false}};
  static const PhareChoix* cats[]   = {bougie, calme, drop, basse, aigu, autre};
  static const uint8_t     nbCat[]  = {1, 2, 1, 4, 2, 2};

  uint8_t cat;
  if (strip.now - d->silence > 3000)                       cat = 0;   // silence
  else if (strip.now - d->sansBasse > 2500 && d->energie > 8) cat = 2; // montee sans basse : on prepare le drop
  else if (d->energie < 25)                                cat = 1;   // calme
  else if (phareMusique() && d->basses > d->aigus * 1.3f)  cat = 3;   // basse forte et reguliere
  else if (d->aigus > d->basses * 0.9f)                    cat = 4;   // beaucoup d'aigus
  else                                                     cat = 5;
  if (d->cat == 2 && cat != 2 && strip.now < d->verrou) cat = 2;      // laisse le drop exploser
  if (cat == 2 && d->cat != 2) d->verrou = strip.now + 6000;

  uint32_t duree = map(SEGMENT.speed, 0, 255, 90000, 15000);         // changement dans une meme ambiance
  bool change = (SEGENV.call == 0);
  if (cat != d->cat && (strip.now - d->choix > 6000 || cat == 0 || cat == 2 || d->cat == 255)) change = true;
  if (strip.now - d->choix > duree) change = true;
  if (change) {
    if (cat != d->cat) a->idx = hw_random8() % nbCat[cat];
    else if (nbCat[cat] > 1) { uint8_t nv; do { nv = hw_random8() % nbCat[cat]; } while (nv == a->idx); a->idx = nv; }
    d->cat = cat; d->choix = strip.now;
  }
  if (a->idx >= nbCat[d->cat]) a->idx = 0;
  const PhareChoix& c = cats[d->cat][a->idx];
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
static const char _data_FX_MODE_PHARE_DJ[] PROGMEM = "11 DJ auto@Changements;!;;1f;sx=150";

static void mode_phare_auto_anim(void) {
  static const PhareChoix liste[] = {
    {mode_phare_roll, 150, 120, 0, 0, true, false},     {mode_phare_rebond, 150, 120, 0, 0, false, false},
    {mode_phare_remplir, 150, 60, 0, 0, false, false},  {mode_phare_balle, 128, 128, 0, 0, false, false},
    {mode_phare_goutte, 128, 128, 0, 0, false, false},  {mode_phare_alternance, 120, 40, 0, 0, false, false},
    {mode_phare_clignotant, 128, 100, 0, 0, false, true}, {mode_phare_respiration, 60, 40, 255, 0, false, false},
    {mode_phare_bougies, 96, 224, 0, 0, false, false},  {mode_phare_coeur, 60, 150, 10, 0, false, true},
    {mode_phare_detresse, 128, 60, 0, 0, false, true},  {mode_phare_lava, 60, 120, 15, 0, false, false},
    {mode_phare_ascenseur, 120, 100, 10, 0, false, false}, {mode_phare_orage, 128, 60, 8, 0, false, false},
    {mode_phare_neon, 128, 100, 0, 0, false, false},    {mode_phare_marin, 90, 110, 20, 0, false, false},
    {mode_phare_pluie, 110, 80, 0, 0, false, false},
  };
  phareAuto(liste, sizeof(liste) / sizeof(liste[0]));
}
static const char _data_FX_MODE_PHARE_AUTO_ANIM[] PROGMEM = "31 Auto animations@Duree par effet;!;;1;sx=200";

/////////////////////////////////////////////////////////////
//  Tache de fond : tempo de la musique + double clap
/////////////////////////////////////////////////////////////

static void phareLoop() {
  static uint32_t dernier = 0;
  if (millis() - dernier < 10) return;
  dernier = millis();
  um_data_t* um;
  if (!UsermodManager::getUMData(&um, USERMOD_ID_AUDIOREACTIVE)) return;
  uint8_t* fft = (uint8_t*)um->u_data[2];

  // tempo : intervalle entre coups de basse reguliers (40 a 200 bpm)
  static float moy = 40.0f; static uint32_t dernierCoup = 0;
  float bass = (fft[0] + fft[1] + fft[2]) / 3.0f;
  if (bass > moy * 1.5f && bass > 20.0f && millis() - dernierCoup > 280) {
    uint32_t ecart = millis() - dernierCoup; dernierCoup = millis();
    if (ecart >= 300 && ecart <= 1500) {
      float rapport = ecart / phareTempoMs;
      if (rapport > 0.8f && rapport < 1.25f) { if (phareTempoNb < 255) phareTempoNb++; }
      else phareTempoNb = 0;
      phareTempoMs = phareTempoMs * 0.7f + ecart * 0.3f;
      phareTempoLast = millis();
    } else if (ecart > 1500) phareTempoNb = 0;
  }
  moy = moy * 0.97f + bass * 0.03f; if (moy < 8.0f) moy = 8.0f;

}

/////////////////////
//  UserMod Class  //
/////////////////////

class UserFxUsermod : public Usermod {
 private:
 public:
  void setup() override {
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
    strip.addEffect(255, &mode_phare_bass, _data_FX_MODE_PHARE_BASS);
    strip.addEffect(255, &mode_phare_clignomusic, _data_FX_MODE_PHARE_CLIGNOMUSIC);
    strip.addEffect(255, &mode_phare_roll_bass, _data_FX_MODE_PHARE_ROLL_BASS);
    strip.addEffect(255, &mode_phare_flamme, _data_FX_MODE_PHARE_FLAMME);
    strip.addEffect(255, &mode_phare_explosion, _data_FX_MODE_PHARE_EXPLOSION);
    strip.addEffect(255, &mode_phare_artifice, _data_FX_MODE_PHARE_ARTIFICE);
    strip.addEffect(255, &mode_phare_vumetre, _data_FX_MODE_PHARE_VUMETRE);
    strip.addEffect(255, &mode_phare_echo, _data_FX_MODE_PHARE_ECHO);
    strip.addEffect(255, &mode_phare_drop, _data_FX_MODE_PHARE_DROP);
    strip.addEffect(255, &mode_phare_respimusic, _data_FX_MODE_PHARE_RESPIMUSIC);
    strip.addEffect(255, &mode_phare_dj, _data_FX_MODE_PHARE_DJ);
    strip.addEffect(255, &mode_phare_roll, _data_FX_MODE_PHARE_ROLL);
    strip.addEffect(255, &mode_phare_rebond, _data_FX_MODE_PHARE_REBOND);
    strip.addEffect(255, &mode_phare_remplir, _data_FX_MODE_PHARE_REMPLIR);
    strip.addEffect(255, &mode_phare_balle, _data_FX_MODE_PHARE_BALLE);
    strip.addEffect(255, &mode_phare_goutte, _data_FX_MODE_PHARE_GOUTTE);
    strip.addEffect(255, &mode_phare_alternance, _data_FX_MODE_PHARE_ALTERNANCE);
    strip.addEffect(255, &mode_phare_clignotant, _data_FX_MODE_PHARE_CLIGNOTANT);
    strip.addEffect(255, &mode_phare_respiration, _data_FX_MODE_PHARE_RESPIRATION);
    strip.addEffect(255, &mode_phare_bougies, _data_FX_MODE_PHARE_BOUGIES);
    strip.addEffect(255, &mode_phare_coeur, _data_FX_MODE_PHARE_COEUR);
    strip.addEffect(255, &mode_phare_detresse, _data_FX_MODE_PHARE_DETRESSE);
    strip.addEffect(255, &mode_phare_lava, _data_FX_MODE_PHARE_LAVA);
    strip.addEffect(255, &mode_phare_ascenseur, _data_FX_MODE_PHARE_ASCENSEUR);
    strip.addEffect(255, &mode_phare_orage, _data_FX_MODE_PHARE_ORAGE);
    strip.addEffect(255, &mode_phare_neon, _data_FX_MODE_PHARE_NEON);
    strip.addEffect(255, &mode_phare_marin, _data_FX_MODE_PHARE_MARIN);
    strip.addEffect(255, &mode_phare_pluie, _data_FX_MODE_PHARE_PLUIE);
    strip.addEffect(255, &mode_phare_sos, _data_FX_MODE_PHARE_SOS);
    strip.addEffect(255, &mode_phare_test, _data_FX_MODE_PHARE_TEST);
    strip.addEffect(255, &mode_phare_auto_anim, _data_FX_MODE_PHARE_AUTO_ANIM);
    static const char* const nos[] = {_data_FX_MODE_PHARE_BASS, _data_FX_MODE_PHARE_CLIGNOMUSIC, _data_FX_MODE_PHARE_ROLL_BASS, _data_FX_MODE_PHARE_FLAMME, _data_FX_MODE_PHARE_EXPLOSION, _data_FX_MODE_PHARE_ARTIFICE, _data_FX_MODE_PHARE_VUMETRE, _data_FX_MODE_PHARE_ECHO, _data_FX_MODE_PHARE_DROP, _data_FX_MODE_PHARE_RESPIMUSIC, _data_FX_MODE_PHARE_DJ, _data_FX_MODE_PHARE_ROLL, _data_FX_MODE_PHARE_REBOND, _data_FX_MODE_PHARE_REMPLIR, _data_FX_MODE_PHARE_BALLE, _data_FX_MODE_PHARE_GOUTTE, _data_FX_MODE_PHARE_ALTERNANCE, _data_FX_MODE_PHARE_CLIGNOTANT, _data_FX_MODE_PHARE_RESPIRATION, _data_FX_MODE_PHARE_BOUGIES, _data_FX_MODE_PHARE_COEUR, _data_FX_MODE_PHARE_DETRESSE, _data_FX_MODE_PHARE_LAVA, _data_FX_MODE_PHARE_ASCENSEUR, _data_FX_MODE_PHARE_ORAGE, _data_FX_MODE_PHARE_NEON, _data_FX_MODE_PHARE_MARIN, _data_FX_MODE_PHARE_PLUIE, _data_FX_MODE_PHARE_SOS, _data_FX_MODE_PHARE_TEST, _data_FX_MODE_PHARE_AUTO_ANIM};
    phareMasquerNatifs(nos, sizeof(nos) / sizeof(nos[0]));

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

  void loop() override { phareLoop(); } // mesure du tempo
  uint16_t getId() override { return USERMOD_ID_USER_FX; }
};

static UserFxUsermod user_fx;
REGISTER_USERMOD(user_fx);
