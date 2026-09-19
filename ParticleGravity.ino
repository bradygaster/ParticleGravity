#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <SensorQMI8658.hpp>
#include <TouchDrvCSTXXX.hpp>
#include <Wire.h>
#include "pin_config.h"

// IMU-to-screen mapping. Axis values are 0 = X, 1 = Y, 2 = Z.
constexpr uint8_t SCREEN_X_AXIS = 1;
constexpr uint8_t SCREEN_Y_AXIS = 0;
constexpr float SCREEN_X_SIGN = -1.0f;
constexpr float SCREEN_Y_SIGN = 1.0f;

constexpr uint16_t MAX_PARTICLE_COUNT = 500;
constexpr uint16_t MIN_PARTICLE_COUNT = 1;
constexpr float SCREEN_CENTER_X = LCD_WIDTH * 0.5f;
constexpr float SCREEN_CENTER_Y = LCD_HEIGHT * 0.5f;
constexpr float SCREEN_RADIUS = (LCD_WIDTH * 0.5f) - 3.0f;
constexpr float BASE_GRAVITY_STRENGTH = 1500.0f;
constexpr float FLAT_CENTERING_STRENGTH = 500.0f;
constexpr float FLAT_GRAVITY_THRESHOLD = 0.25f;
constexpr float ACCEL_SMOOTHING = 0.18f;
constexpr float VELOCITY_DAMPING = 0.996f;
constexpr float WALL_TANGENT_DAMPING = 0.65f;
constexpr float MAX_SPEED = 900.0f;
constexpr float METERS_PER_SECOND_SQUARED_PER_G = 9.80665f;
constexpr float CALIBRATION_DEADZONE = 0.05f;
constexpr uint16_t CALIBRATION_SAMPLES = 200;
constexpr uint8_t RENDER_BAND_HEIGHT = 64;
constexpr uint8_t COLLISION_CELL_SIZE = 8;
constexpr uint8_t COLLISION_GRID_WIDTH = (LCD_WIDTH + COLLISION_CELL_SIZE - 1) / COLLISION_CELL_SIZE;
constexpr uint8_t COLLISION_GRID_HEIGHT = (LCD_HEIGHT + COLLISION_CELL_SIZE - 1) / COLLISION_CELL_SIZE;
constexpr uint32_t COLOR_CYCLE_MS = 15000;
constexpr uint32_t FRAME_INTERVAL_US = 16667;
constexpr int16_t DIAL_CENTER_X = LCD_WIDTH / 2;
constexpr int16_t DIAL_CENTER_Y = 220;
constexpr int16_t DIAL_RADIUS = 139;

enum class AppState : uint8_t {
  WIZARD,
  SIMULATION
};

struct Particle {
  float x;
  float y;
  float vx;
  float vy;
  uint8_t radius;
};

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_CO5300 *gfx = new Arduino_CO5300(
  bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);
Arduino_Canvas_Indexed *ui = new Arduino_Canvas_Indexed(LCD_WIDTH, LCD_HEIGHT, gfx);

SensorQMI8658 qmi;
TouchDrvCST92xx touch;
IMUdata acceleration;
Particle particles[MAX_PARTICLE_COUNT];
uint16_t renderBand[LCD_WIDTH * RENDER_BAND_HEIGHT];
uint16_t particleFrameColors[MAX_PARTICLE_COUNT];
uint16_t particleHaloColors[MAX_PARTICLE_COUNT];
uint16_t particleHighlightColors[MAX_PARTICLE_COUNT];
int16_t collisionCellHeads[COLLISION_GRID_WIDTH * COLLISION_GRID_HEIGHT];
int16_t collisionNext[MAX_PARTICLE_COUNT];

volatile bool touchPending = false;
int16_t touchX[2];
int16_t touchY[2];
AppState appState = AppState::WIZARD;
uint8_t wizardPage = 0;
uint16_t activeParticleCount = MAX_PARTICLE_COUNT;
uint8_t variabilityPercent = 100;
uint8_t sensitivityLevel = 5;
uint16_t seedHue = 384;
uint32_t simulationStartMs = 0;

float smoothedGravityX = 0.0f;
float smoothedGravityY = 0.0f;
float accelerationBiasX = 0.0f;
float accelerationBiasY = 0.0f;
uint32_t lastFrameUs = 0;
uint32_t lastReportMs = 0;
uint32_t frameCount = 0;

void IRAM_ATTR onTouchInterrupt() {
  touchPending = true;
}

bool readTouch(int16_t &x, int16_t &y) {
  noInterrupts();
  const bool pending = touchPending;
  touchPending = false;
  interrupts();
  if (!pending) return false;

  const uint8_t points = touch.getPoint(touchX, touchY, 2);
  if (points == 0) return false;
  x = touchX[0];
  y = touchY[0];
  Serial.printf("Touch x=%d y=%d\n", x, y);
  return true;
}

uint16_t hueColor(int32_t hue) {
  hue %= 768;
  if (hue < 0) hue += 768;
  uint8_t red;
  uint8_t green;
  uint8_t blue;

  if (hue < 256) {
    red = 255 - hue;
    green = hue;
    blue = 0;
  } else if (hue < 512) {
    red = 0;
    green = 511 - hue;
    blue = hue - 256;
  } else {
    red = hue - 512;
    green = 0;
    blue = 767 - hue;
  }
  return gfx->color565(red, green, blue);
}

uint16_t particleColor(uint16_t index) {
  int32_t hue = seedHue;
  if (variabilityPercent > 0) {
    const uint32_t spread = variabilityPercent * 768UL / 100;
    const uint32_t particlePhase = index * 431UL % activeParticleCount;
    const uint32_t timePhase =
      ((millis() - simulationStartMs) % COLOR_CYCLE_MS) * activeParticleCount / COLOR_CYCLE_MS;
    const uint32_t phase = (particlePhase + timePhase) % activeParticleCount;
    hue += static_cast<int32_t>(phase * spread / activeParticleCount) - spread / 2;
  }
  return hueColor(hue);
}

uint16_t scaleColor(uint16_t color, uint8_t numerator, uint8_t denominator) {
  const uint8_t red = ((color >> 11) & 0x1F) * numerator / denominator;
  const uint8_t green = ((color >> 5) & 0x3F) * numerator / denominator;
  const uint8_t blue = (color & 0x1F) * numerator / denominator;
  return (red << 11) | (green << 5) | blue;
}

uint16_t brightenColor(uint16_t color) {
  const uint8_t red = (color >> 11) & 0x1F;
  const uint8_t green = (color >> 5) & 0x3F;
  const uint8_t blue = color & 0x1F;
  return ((red + (31 - red) / 2) << 11) |
    ((green + (63 - green) / 2) << 5) |
    (blue + (31 - blue) / 2);
}

void drawTextCenteredAtX(const char *text, int16_t centerX, int16_t y, uint8_t size, uint16_t color) {
  ui->setTextWrap(false);
  ui->setTextColor(color);
  ui->setTextSize(size);

  int16_t boundsX;
  int16_t boundsY;
  uint16_t boundsWidth;
  uint16_t boundsHeight;
  ui->getTextBounds(text, 0, 0, &boundsX, &boundsY, &boundsWidth, &boundsHeight);
  ui->setCursor(centerX - boundsWidth / 2, y);
  ui->print(text);
}

void drawTextCenteredAt(const char *text, int16_t y, uint8_t size, uint16_t color) {
  drawTextCenteredAtX(text, LCD_WIDTH / 2, y, size, color);
}

void drawTextCenteredInRect(const char *text, int16_t x, int16_t y,
  int16_t width, int16_t height, uint8_t size, uint16_t color) {
  int16_t boundsX;
  int16_t boundsY;
  uint16_t boundsWidth;
  uint16_t boundsHeight;
  ui->setTextWrap(false);
  ui->setTextColor(color);
  ui->setTextSize(size);
  ui->getTextBounds(text, 0, 0, &boundsX, &boundsY, &boundsWidth, &boundsHeight);
  ui->setCursor(
    x + (width - boundsWidth) / 2 - boundsX,
    y + (height - boundsHeight) / 2 - boundsY);
  ui->print(text);
}

void drawCenteredText(const char *text, uint8_t size, uint16_t color) {
  int16_t boundsX;
  int16_t boundsY;
  uint16_t boundsWidth;
  uint16_t boundsHeight;
  ui->setTextSize(size);
  ui->getTextBounds(text, 0, 0, &boundsX, &boundsY, &boundsWidth, &boundsHeight);
  drawTextCenteredAt(text, (LCD_HEIGHT - boundsHeight) / 2, size, color);
}

void showImuError() {
  ui->fillScreen(RGB565_BLACK);
  drawCenteredText("IMU ERROR", 4, RGB565_RED);
  ui->flush();
}

bool calibrateAccelerometer() {
  ui->fillScreen(RGB565_BLACK);
  drawCenteredText("KEEP STILL", 3, RGB565_WHITE);
  ui->flush();
  Serial.println("Keep the device still: calibrating QMI8658...");

  float sumX = 0.0f;
  float sumY = 0.0f;
  uint16_t validSamples = 0;
  for (uint16_t sample = 0; sample < CALIBRATION_SAMPLES; ++sample) {
    if (qmi.getAccelerometer(acceleration.x, acceleration.y, acceleration.z)) {
      sumX += acceleration.x;
      sumY += acceleration.y;
      ++validSamples;
    }
    delay(10);
  }

  if (validSamples == 0) return false;
  accelerationBiasX = sumX / validSamples;
  accelerationBiasY = sumY / validSamples;
  Serial.printf("Calibration complete: bias x=%.4f y=%.4f\n", accelerationBiasX, accelerationBiasY);
  ui->fillScreen(RGB565_BLACK);
  ui->flush();
  return true;
}

void drawWizardFooter() {
  const uint16_t accentColor = hueColor(seedHue);
  const char *nextLabel = wizardPage == 3 ? "START" : "NEXT";
  for (uint8_t page = 0; page < 4; ++page) {
    ui->fillCircle(203 + page * 20, 369, page == wizardPage ? 5 : 3,
      page == wizardPage ? RGB565_WHITE : 0x528A);
  }
  ui->fillRoundRect(105, 389, 116, 40, 8, wizardPage == 0 ? 0x2104 : 0x4208);
  drawTextCenteredInRect("PREV", 105, 389, 116, 40, 2,
    wizardPage == 0 ? 0x630C : RGB565_WHITE);
  ui->fillRoundRect(245, 389, 116, 40, 8, accentColor);
  drawTextCenteredInRect(nextLabel, 245, 389, 116, 40, 2, RGB565_BLACK);
}

void drawColorWheel() {
  constexpr int16_t wheelY = 220;
  constexpr int16_t outerRadius = 150;
  constexpr int16_t innerRadius = 88;

  ui->fillScreen(RGB565_BLACK);
  for (uint16_t segment = 0; segment < 128; ++segment) {
    const float startAngle = segment * 360.0f / 128.0f;
    const float endAngle = (segment + 1) * 360.0f / 128.0f + 0.5f;
    ui->fillArc(DIAL_CENTER_X, wheelY, outerRadius, innerRadius,
      startAngle, endAngle, hueColor(segment * 768 / 128));
  }

  drawTextCenteredAt("CHOOSE A COLOR", 44, 3, RGB565_WHITE);
  ui->fillCircle(DIAL_CENTER_X, wheelY, 62, hueColor(seedHue));
  ui->drawCircle(DIAL_CENTER_X, wheelY, 63, RGB565_WHITE);
  const float angle = seedHue * TWO_PI / 768.0f;
  const int16_t markerX = DIAL_CENTER_X + lroundf(cosf(angle) * 119);
  const int16_t markerY = wheelY + lroundf(sinf(angle) * 119);
  ui->fillCircle(markerX, markerY, 10, RGB565_BLACK);
  ui->drawCircle(markerX, markerY, 9, RGB565_WHITE);
  ui->drawCircle(markerX, markerY, 8, RGB565_WHITE);
  drawWizardFooter();
  ui->flush();
}

void drawDialScreen(const char *title, const char *value, const char *caption, float fraction) {
  const uint16_t accentColor = hueColor(seedHue);
  const float angle = 135.0f + fraction * 270.0f;
  const float radians = angle * DEG_TO_RAD;
  const int16_t knobX = DIAL_CENTER_X + lroundf(cosf(radians) * DIAL_RADIUS);
  const int16_t knobY = DIAL_CENTER_Y + lroundf(sinf(radians) * DIAL_RADIUS);

  ui->fillScreen(RGB565_BLACK);
  drawTextCenteredAt(title, 48, 3, RGB565_WHITE);
  ui->fillArc(DIAL_CENTER_X, DIAL_CENTER_Y, 151, 128, 135, 405, 0x2104);
  if (fraction > 0.0f) {
    ui->fillArc(DIAL_CENTER_X, DIAL_CENTER_Y, 151, 128, 135, angle, accentColor);
  }
  for (uint8_t tick = 0; tick <= 10; ++tick) {
    const float tickRadians = (135.0f + tick * 27.0f) * DEG_TO_RAD;
    ui->drawLine(
      DIAL_CENTER_X + lroundf(cosf(tickRadians) * 157),
      DIAL_CENTER_Y + lroundf(sinf(tickRadians) * 157),
      DIAL_CENTER_X + lroundf(cosf(tickRadians) * 166),
      DIAL_CENTER_Y + lroundf(sinf(tickRadians) * 166),
      0x7BEF);
  }
  ui->fillCircle(knobX, knobY, 13, RGB565_WHITE);
  ui->fillCircle(knobX, knobY, 8, accentColor);
  drawTextCenteredAt(value, 180, 7, RGB565_WHITE);
  drawTextCenteredAt(caption, 252, 2, 0x9CF3);
  drawWizardFooter();
  ui->flush();
}

void drawWizardScreen() {
  char value[16];
  if (wizardPage == 0) {
    drawColorWheel();
  } else if (wizardPage == 1) {
    snprintf(value, sizeof(value), "%u%%", variabilityPercent);
    drawDialScreen("COLOR VARIATION", value, variabilityPercent == 0 ? "SOLID" :
      (variabilityPercent == 100 ? "RAINBOW" : "AROUND SEED"), variabilityPercent / 100.0f);
  } else if (wizardPage == 2) {
    snprintf(value, sizeof(value), "%u", activeParticleCount);
    drawDialScreen("PARTICLES", value, "COUNT", (activeParticleCount - 1) / 499.0f);
  } else {
    snprintf(value, sizeof(value), "%u", sensitivityLevel);
    drawDialScreen("SENSITIVITY", value, "GRAVITY RESPONSE", (sensitivityLevel - 1) / 9.0f);
  }
}

void showCountdown() {
  for (int8_t count = 3; count >= 1; --count) {
    char countText[2] = { static_cast<char>('0' + count), '\0' };
    ui->fillScreen(RGB565_BLACK);
    drawTextCenteredAt("PUT ME DOWN", 145, 3, RGB565_WHITE);
    drawTextCenteredAt(countText, 205, 8, hueColor(seedHue));
    ui->flush();
    delay(1000);
  }
}

void initializeParticles() {
  constexpr float spacing = 8.0f;
  const uint8_t columns = ceilf(sqrtf(activeParticleCount));
  const uint8_t rows = (activeParticleCount + columns - 1) / columns;
  const float startX = SCREEN_CENTER_X - ((columns - 1) * spacing * 0.5f);
  const float startY = SCREEN_CENTER_Y - ((rows - 1) * spacing * 0.5f);

  for (uint16_t index = 0; index < activeParticleCount; ++index) {
    const uint8_t column = index % columns;
    const uint8_t row = index / columns;
    Particle &particle = particles[index];

    particle.x = startX + column * spacing + random(-10, 11) * 0.1f;
    particle.y = startY + row * spacing + random(-10, 11) * 0.1f;
    particle.vx = 0.0f;
    particle.vy = 0.0f;
    particle.radius = 2 + random(0, 3);
  }
}

void constrainToScreen(Particle &particle) {
  const float dx = particle.x - SCREEN_CENTER_X;
  const float dy = particle.y - SCREEN_CENTER_Y;
  const float maximumDistance = SCREEN_RADIUS - particle.radius;
  const float distanceSquared = dx * dx + dy * dy;

  if (distanceSquared <= maximumDistance * maximumDistance) return;

  const float distance = sqrtf(distanceSquared);
  const float normalX = dx / distance;
  const float normalY = dy / distance;
  particle.x = SCREEN_CENTER_X + normalX * maximumDistance;
  particle.y = SCREEN_CENTER_Y + normalY * maximumDistance;

  const float outwardSpeed = particle.vx * normalX + particle.vy * normalY;
  if (outwardSpeed > 0.0f) {
    particle.vx -= outwardSpeed * normalX;
    particle.vy -= outwardSpeed * normalY;
  }
  particle.vx *= WALL_TANGENT_DAMPING;
  particle.vy *= WALL_TANGENT_DAMPING;
}

void resolveParticlePair(Particle &a, Particle &b) {
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  const float minimumDistance = a.radius + b.radius;
  const float distanceSquared = dx * dx + dy * dy;

  if (distanceSquared >= minimumDistance * minimumDistance || distanceSquared < 0.01f) return;

  const float distance = sqrtf(distanceSquared);
  const float normalX = dx / distance;
  const float normalY = dy / distance;
  const float correction = (minimumDistance - distance) * 0.5f;
  a.x -= normalX * correction;
  a.y -= normalY * correction;
  b.x += normalX * correction;
  b.y += normalY * correction;

  const float relativeNormalSpeed = (b.vx - a.vx) * normalX + (b.vy - a.vy) * normalY;
  if (relativeNormalSpeed < 0.0f) {
    const float impulse = relativeNormalSpeed * 0.5f;
    a.vx += impulse * normalX;
    a.vy += impulse * normalY;
    b.vx -= impulse * normalX;
    b.vy -= impulse * normalY;
  }
}

void separateParticles() {
  memset(collisionCellHeads, 0xFF, sizeof(collisionCellHeads));

  for (uint16_t index = 0; index < activeParticleCount; ++index) {
    const uint8_t cellX = constrain(static_cast<int>(particles[index].x) / COLLISION_CELL_SIZE, 0, COLLISION_GRID_WIDTH - 1);
    const uint8_t cellY = constrain(static_cast<int>(particles[index].y) / COLLISION_CELL_SIZE, 0, COLLISION_GRID_HEIGHT - 1);
    const uint16_t cell = cellY * COLLISION_GRID_WIDTH + cellX;
    collisionNext[index] = collisionCellHeads[cell];
    collisionCellHeads[cell] = index;
  }

  for (uint16_t first = 0; first < activeParticleCount; ++first) {
    Particle &a = particles[first];
    const int8_t cellX = constrain(static_cast<int>(a.x) / COLLISION_CELL_SIZE, 0, COLLISION_GRID_WIDTH - 1);
    const int8_t cellY = constrain(static_cast<int>(a.y) / COLLISION_CELL_SIZE, 0, COLLISION_GRID_HEIGHT - 1);

    for (int8_t offsetY = -1; offsetY <= 1; ++offsetY) {
      const int8_t neighborY = cellY + offsetY;
      if (neighborY < 0 || neighborY >= COLLISION_GRID_HEIGHT) continue;
      for (int8_t offsetX = -1; offsetX <= 1; ++offsetX) {
        const int8_t neighborX = cellX + offsetX;
        if (neighborX < 0 || neighborX >= COLLISION_GRID_WIDTH) continue;

        int16_t second = collisionCellHeads[neighborY * COLLISION_GRID_WIDTH + neighborX];
        while (second >= 0) {
          if (second > first) resolveParticlePair(a, particles[second]);
          second = collisionNext[second];
        }
      }
    }
  }
}

void updateSimulation(float deltaSeconds) {
  constexpr uint8_t substeps = 2;
  const float step = deltaSeconds / substeps;
  const float planarGravity = sqrtf(
    smoothedGravityX * smoothedGravityX + smoothedGravityY * smoothedGravityY);
  const float centerPull = max(0.0f, 1.0f - planarGravity / FLAT_GRAVITY_THRESHOLD);

  for (uint8_t substep = 0; substep < substeps; ++substep) {
    for (uint16_t index = 0; index < activeParticleCount; ++index) {
      Particle &particle = particles[index];
      const float sensitivityMultiplier = 0.5f + (sensitivityLevel - 1) * (4.0f / 9.0f);
      const float gravityStrength = BASE_GRAVITY_STRENGTH * sensitivityMultiplier;
      particle.vx += smoothedGravityX * gravityStrength * step;
      particle.vy += smoothedGravityY * gravityStrength * step;
      particle.vx -= (particle.x - SCREEN_CENTER_X) / SCREEN_RADIUS *
        FLAT_CENTERING_STRENGTH * centerPull * step;
      particle.vy -= (particle.y - SCREEN_CENTER_Y) / SCREEN_RADIUS *
        FLAT_CENTERING_STRENGTH * centerPull * step;
      particle.vx *= VELOCITY_DAMPING;
      particle.vy *= VELOCITY_DAMPING;

      const float speedSquared = particle.vx * particle.vx + particle.vy * particle.vy;
      if (speedSquared > MAX_SPEED * MAX_SPEED) {
        const float speedScale = MAX_SPEED / sqrtf(speedSquared);
        particle.vx *= speedScale;
        particle.vy *= speedScale;
      }

      particle.x += particle.vx * step;
      particle.y += particle.vy * step;
      constrainToScreen(particle);
    }

    separateParticles();
    for (uint16_t index = 0; index < activeParticleCount; ++index) {
      constrainToScreen(particles[index]);
    }
  }
}

void renderParticles() {
  for (uint16_t index = 0; index < activeParticleCount; ++index) {
    particleFrameColors[index] = particleColor(index);
    particleHaloColors[index] = scaleColor(particleFrameColors[index], 1, 5);
    particleHighlightColors[index] = brightenColor(particleFrameColors[index]);
  }

  for (int16_t bandY = 0; bandY < LCD_HEIGHT; bandY += RENDER_BAND_HEIGHT) {
    const int16_t bandHeight = min<int16_t>(RENDER_BAND_HEIGHT, LCD_HEIGHT - bandY);
    memset(renderBand, 0, LCD_WIDTH * bandHeight * sizeof(uint16_t));

    for (uint16_t index = 0; index < activeParticleCount; ++index) {
      const Particle &particle = particles[index];
      const int16_t centerX = lroundf(particle.x);
      const int16_t centerY = lroundf(particle.y);
      const int16_t outerRadius = particle.radius + 2;
      const int16_t top = max<int16_t>(bandY, centerY - outerRadius);
      const int16_t bottom = min<int16_t>(bandY + bandHeight - 1, centerY + outerRadius);

      for (int16_t pixelY = top; pixelY <= bottom; ++pixelY) {
        const int16_t dy = pixelY - centerY;
        const int16_t halfWidth = floorf(sqrtf(outerRadius * outerRadius - dy * dy));
        const int16_t left = max<int16_t>(0, centerX - halfWidth);
        const int16_t right = min<int16_t>(LCD_WIDTH - 1, centerX + halfWidth);
        uint16_t *row = renderBand + (pixelY - bandY) * LCD_WIDTH;
        for (int16_t pixelX = left; pixelX <= right; ++pixelX) {
          const int16_t dx = pixelX - centerX;
          const int16_t distanceSquared = dx * dx + dy * dy;
          if (distanceSquared <= 2) {
            row[pixelX] = particleHighlightColors[index];
          } else if (distanceSquared <= particle.radius * particle.radius) {
            row[pixelX] = particleFrameColors[index];
          } else {
            row[pixelX] = particleHaloColors[index];
          }
        }
      }
    }

    gfx->draw16bitRGBBitmap(0, bandY, renderBand, LCD_WIDTH, bandHeight);
  }
}

void startSimulation() {
  showCountdown();
  if (!calibrateAccelerometer()) {
    Serial.println("Failed to read QMI8658 during calibration.");
    showImuError();
    while (true) delay(1000);
  }

  smoothedGravityX = 0.0f;
  smoothedGravityY = 0.0f;
  randomSeed(micros());
  initializeParticles();
  simulationStartMs = millis();
  renderParticles();
  lastFrameUs = micros();
  lastReportMs = millis();
  frameCount = 0;
  noInterrupts();
  touchPending = false;
  interrupts();
  appState = AppState::SIMULATION;
  Serial.println("ParticleGravity started.");
}

float dialFraction(int16_t x, int16_t y) {
  float angle = atan2f(y - DIAL_CENTER_Y, x - DIAL_CENTER_X) / DEG_TO_RAD;
  if (angle < 0.0f) angle += 360.0f;
  if (angle < 135.0f) angle += 360.0f;
  return constrain((angle - 135.0f) / 270.0f, 0.0f, 1.0f);
}

void advanceWizard() {
  if (wizardPage < 3) {
    ++wizardPage;
    drawWizardScreen();
    delay(180);
    noInterrupts();
    touchPending = false;
    interrupts();
  } else {
    startSimulation();
  }
}

void retreatWizard() {
  if (wizardPage == 0) return;
  --wizardPage;
  drawWizardScreen();
  delay(180);
  noInterrupts();
  touchPending = false;
  interrupts();
}

void handleWizardTouch(int16_t x, int16_t y) {
  if (y >= 378 && y <= 445) {
    if (x >= 88 && x <= 230) {
      retreatWizard();
    } else if (x >= 236 && x <= 378) {
      advanceWizard();
    }
    return;
  }

  const float dx = x - DIAL_CENTER_X;
  const float dy = y - DIAL_CENTER_Y;
  const float distance = sqrtf(dx * dx + dy * dy);

  if (wizardPage == 0) {
    if (distance >= 70.0f && distance <= 175.0f) {
      int32_t hue = lroundf(atan2f(dy, dx) * 768.0f / TWO_PI);
      if (hue < 0) hue += 768;
      seedHue = hue;
      drawWizardScreen();
    }
    return;
  }

  if (distance < 82.0f || distance > 180.0f) return;
  const float fraction = dialFraction(x, y);
  if (wizardPage == 1) {
    variabilityPercent = lroundf(fraction * 100.0f);
  } else if (wizardPage == 2) {
    activeParticleCount = MIN_PARTICLE_COUNT + lroundf(fraction * (MAX_PARTICLE_COUNT - MIN_PARTICLE_COUNT));
  } else {
    sensitivityLevel = 1 + lroundf(fraction * 9.0f);
  }
  drawWizardScreen();
}

void setup() {
  Serial.begin(115200);
  delay(300);

  if (!gfx->begin()) {
    Serial.println("Display initialization failed.");
    while (true) delay(1000);
  }
  gfx->setBrightness(200);
  gfx->fillScreen(RGB565_BLACK);
  if (!ui->begin(GFX_SKIP_OUTPUT_BEGIN)) {
    Serial.println("UI framebuffer allocation failed.");
    gfx->setTextColor(RGB565_RED);
    gfx->setTextSize(3);
    gfx->setCursor(45, LCD_HEIGHT / 2);
    gfx->print("DISPLAY ERROR");
    while (true) delay(1000);
  }

  Wire.begin(IIC_SDA, IIC_SCL);
  touch.setPins(TP_RESET, TP_INT);
  if (!touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    Serial.println("CST9217 initialization failed.");
    drawCenteredText("TOUCH ERROR", 3, RGB565_RED);
    ui->flush();
    while (true) delay(1000);
  }
  touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
  touch.setMirrorXY(true, true);
  pinMode(TP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TP_INT), onTouchInterrupt, FALLING);

  if (!qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    Serial.println("Failed to find QMI8658 - check your wiring!");
    showImuError();
    while (true) delay(1000);
  }

  qmi.configAccelerometer(
    SensorQMI8658::ACC_RANGE_4G,
    SensorQMI8658::ACC_ODR_1000Hz,
    SensorQMI8658::LPF_MODE_0);
  qmi.enableAccelerometer();
  drawWizardScreen();
  Serial.println("ParticleGravity wizard ready.");
}

void loop() {
  int16_t pressedX;
  int16_t pressedY;
  if (readTouch(pressedX, pressedY)) {
    if (appState == AppState::SIMULATION) {
      appState = AppState::WIZARD;
      wizardPage = 0;
      drawWizardScreen();
      delay(150);
      return;
    }
    handleWizardTouch(pressedX, pressedY);
  }

  if (appState == AppState::WIZARD) {
    delay(5);
    return;
  }

  const uint32_t nowUs = micros();
  const uint32_t elapsedUs = nowUs - lastFrameUs;
  if (elapsedUs < FRAME_INTERVAL_US) {
    delayMicroseconds(FRAME_INTERVAL_US - elapsedUs);
  }

  const uint32_t frameUs = micros();
  float deltaSeconds = (frameUs - lastFrameUs) * 0.000001f;
  lastFrameUs = frameUs;
  if (deltaSeconds > 0.033f) deltaSeconds = 0.033f;

  if (qmi.getDataReady() && qmi.getAccelerometer(acceleration.x, acceleration.y, acceleration.z)) {
    float calibratedX = acceleration.x - accelerationBiasX;
    float calibratedY = acceleration.y - accelerationBiasY;
    if (fabsf(calibratedX) < CALIBRATION_DEADZONE) calibratedX = 0.0f;
    if (fabsf(calibratedY) < CALIBRATION_DEADZONE) calibratedY = 0.0f;

    const float calibratedAxes[] = { calibratedX, calibratedY, acceleration.z };
    const float gravityX = calibratedAxes[SCREEN_X_AXIS] * SCREEN_X_SIGN / METERS_PER_SECOND_SQUARED_PER_G;
    const float gravityY = calibratedAxes[SCREEN_Y_AXIS] * SCREEN_Y_SIGN / METERS_PER_SECOND_SQUARED_PER_G;
    smoothedGravityX += (gravityX - smoothedGravityX) * ACCEL_SMOOTHING;
    smoothedGravityY += (gravityY - smoothedGravityY) * ACCEL_SMOOTHING;
  }

  updateSimulation(deltaSeconds);
  renderParticles();
  ++frameCount;

  const uint32_t nowMs = millis();
  const uint32_t reportElapsedMs = nowMs - lastReportMs;
  if (reportElapsedMs >= 1000) {
    const float fps = frameCount * 1000.0f / reportElapsedMs;
    Serial.printf(
      "ACC x=%7.3f y=%7.3f z=%7.3f | gravity x=%7.3f y=%7.3f | FPS=%5.1f\n",
      acceleration.x, acceleration.y, acceleration.z,
      smoothedGravityX, smoothedGravityY, fps);
    frameCount = 0;
    lastReportMs = nowMs;
  }
}