"use strict";

const DISPLAY_SIZE = 466;
const CENTER = DISPLAY_SIZE / 2;
const SCREEN_RADIUS = CENTER - 3;
const BASE_GRAVITY_STRENGTH = 1500;
const FLAT_CENTERING_STRENGTH = 500;
const FLAT_GRAVITY_THRESHOLD = 0.25;
const ACCEL_SMOOTHING = 0.18;
const VELOCITY_DAMPING = 0.996;
const WALL_TANGENT_DAMPING = 0.65;
const MAX_SPEED = 900;
const COLOR_CYCLE_MS = 15000;
const DIAL_CENTER_Y = 220;
const DIAL_RADIUS = 139;
const COLLISION_CELL_SIZE = 8;
const GRID_SIZE = Math.ceil(DISPLAY_SIZE / COLLISION_CELL_SIZE);
const BITMAP_FONT = {
  " ": [0x00, 0x00, 0x00, 0x00, 0x00],
  "%": [0x23, 0x13, 0x08, 0x64, 0x62],
  "0": [0x3e, 0x51, 0x49, 0x45, 0x3e],
  "1": [0x00, 0x42, 0x7f, 0x40, 0x00],
  "2": [0x72, 0x49, 0x49, 0x49, 0x46],
  "3": [0x21, 0x41, 0x49, 0x4d, 0x33],
  "4": [0x18, 0x14, 0x12, 0x7f, 0x10],
  "5": [0x27, 0x45, 0x45, 0x45, 0x39],
  "6": [0x3c, 0x4a, 0x49, 0x49, 0x31],
  "7": [0x41, 0x21, 0x11, 0x09, 0x07],
  "8": [0x36, 0x49, 0x49, 0x49, 0x36],
  "9": [0x46, 0x49, 0x49, 0x29, 0x1e],
  A: [0x7c, 0x12, 0x11, 0x12, 0x7c],
  B: [0x7f, 0x49, 0x49, 0x49, 0x36],
  C: [0x3e, 0x41, 0x41, 0x41, 0x22],
  D: [0x7f, 0x41, 0x41, 0x41, 0x3e],
  E: [0x7f, 0x49, 0x49, 0x49, 0x41],
  F: [0x7f, 0x09, 0x09, 0x09, 0x01],
  G: [0x3e, 0x41, 0x41, 0x51, 0x73],
  H: [0x7f, 0x08, 0x08, 0x08, 0x7f],
  I: [0x00, 0x41, 0x7f, 0x41, 0x00],
  J: [0x20, 0x40, 0x41, 0x3f, 0x01],
  K: [0x7f, 0x08, 0x14, 0x22, 0x41],
  L: [0x7f, 0x40, 0x40, 0x40, 0x40],
  M: [0x7f, 0x02, 0x1c, 0x02, 0x7f],
  N: [0x7f, 0x04, 0x08, 0x10, 0x7f],
  O: [0x3e, 0x41, 0x41, 0x41, 0x3e],
  P: [0x7f, 0x09, 0x09, 0x09, 0x06],
  Q: [0x3e, 0x41, 0x51, 0x21, 0x5e],
  R: [0x7f, 0x09, 0x19, 0x29, 0x46],
  S: [0x26, 0x49, 0x49, 0x49, 0x32],
  T: [0x03, 0x01, 0x7f, 0x01, 0x03],
  U: [0x3f, 0x40, 0x40, 0x40, 0x3f],
  V: [0x1f, 0x20, 0x40, 0x20, 0x1f],
  W: [0x3f, 0x40, 0x38, 0x40, 0x3f],
  X: [0x63, 0x14, 0x08, 0x14, 0x63],
  Y: [0x03, 0x04, 0x78, 0x04, 0x03],
  Z: [0x61, 0x59, 0x49, 0x4d, 0x43],
};

const canvas = document.querySelector("#display");
const context = canvas.getContext("2d", { alpha: false });
const status = document.querySelector("#status");
const tiltPad = document.querySelector("#tilt-pad");
const tiltKnob = document.querySelector("#tilt-knob");
const gravityReadout = document.querySelector("#gravity-readout");

const state = {
  mode: "wizard",
  wizardPage: 0,
  seedHue: 384,
  variabilityPercent: 100,
  particleCount: 500,
  sensitivityLevel: 5,
  gravityX: 0,
  gravityY: 0,
  smoothedGravityX: 0,
  smoothedGravityY: 0,
  particles: [],
  simulationStartMs: 0,
  lastFrameMs: 0,
  countdown: 0,
};

function quantize565(red, green, blue) {
  return [
    Math.floor(red * 31 / 255) * 255 / 31,
    Math.floor(green * 63 / 255) * 255 / 63,
    Math.floor(blue * 31 / 255) * 255 / 31,
  ];
}

function rgb565(red, green, blue) {
  const [r, g, b] = quantize565(red, green, blue);
  return `rgb(${r}, ${g}, ${b})`;
}

function hueColor(hue) {
  hue = ((hue % 768) + 768) % 768;
  if (hue < 256) return rgb565(255 - hue, hue, 0);
  if (hue < 512) return rgb565(0, 511 - hue, hue - 256);
  return rgb565(hue - 512, 0, 767 - hue);
}

function particleColor(index, now) {
  let hue = state.seedHue;
  if (state.variabilityPercent > 0) {
    const spread = state.variabilityPercent * 768 / 100;
    const particlePhase = (index * 431) % state.particleCount;
    const timePhase = ((now - state.simulationStartMs) % COLOR_CYCLE_MS) * state.particleCount / COLOR_CYCLE_MS;
    const phase = (particlePhase + timePhase) % state.particleCount;
    hue += phase * spread / state.particleCount - spread / 2;
  }
  return hueColor(hue);
}

function parseRgb(color) {
  const values = color.match(/[\d.]+/g).map(Number);
  return values;
}

function scaleColor(color, amount) {
  const [red, green, blue] = parseRgb(color);
  return rgb565(red * amount, green * amount, blue * amount);
}

function brightenColor(color) {
  const [red, green, blue] = parseRgb(color);
  return rgb565(red + (255 - red) / 2, green + (255 - green) / 2, blue + (255 - blue) / 2);
}

function clearDisplay() {
  context.fillStyle = "#000";
  context.fillRect(0, 0, DISPLAY_SIZE, DISPLAY_SIZE);
}

function drawBitmapText(text, centerX, y, size, color) {
  text = String(text);
  let cursorX = Math.round(centerX - text.length * 6 * size / 2);
  context.fillStyle = color;
  for (const character of text) {
    const columns = BITMAP_FONT[character] || BITMAP_FONT[" "];
    for (let column = 0; column < 5; column++) {
      for (let row = 0; row < 8; row++) {
        if (columns[column] & (1 << row)) {
          context.fillRect(cursorX + column * size, y + row * size, size, size);
        }
      }
    }
    cursorX += 6 * size;
  }
}

function drawText(text, y, size, color) {
  drawBitmapText(text, CENTER, y, size, color);
}

function fillCircle(x, y, radius, color) {
  context.beginPath();
  context.arc(x, y, radius, 0, Math.PI * 2);
  context.fillStyle = color;
  context.fill();
}

function strokeCircle(x, y, radius, color, width = 1) {
  context.beginPath();
  context.arc(x, y, radius, 0, Math.PI * 2);
  context.strokeStyle = color;
  context.lineWidth = width;
  context.stroke();
}

function fillArc(x, y, outerRadius, innerRadius, startDegrees, endDegrees, color) {
  const start = startDegrees * Math.PI / 180;
  const end = endDegrees * Math.PI / 180;
  context.beginPath();
  context.arc(x, y, outerRadius, start, end);
  context.arc(x, y, innerRadius, end, start, true);
  context.closePath();
  context.fillStyle = color;
  context.fill();
}

function roundRect(x, y, width, height, radius, color) {
  context.beginPath();
  context.roundRect(x, y, width, height, radius);
  context.fillStyle = color;
  context.fill();
}

function drawWizardFooter() {
  const accent = hueColor(state.seedHue);
  for (let page = 0; page < 4; page++) {
    fillCircle(203 + page * 20, 369, page === state.wizardPage ? 5 : 3,
      page === state.wizardPage ? "#fff" : rgb565(82, 81, 82));
  }
  roundRect(105, 389, 116, 40, 8, state.wizardPage === 0 ? rgb565(33, 32, 33) : rgb565(66, 65, 66));
  drawButtonText("PREV", 163, state.wizardPage === 0 ? rgb565(99, 97, 99) : "#fff");
  roundRect(245, 389, 116, 40, 8, accent);
  drawButtonText(state.wizardPage === 3 ? "START" : "NEXT", 303, "#000");
}

function drawButtonText(text, x, color) {
  drawBitmapText(text, x, 401, 2, color);
}

function drawColorWheel() {
  clearDisplay();
  for (let segment = 0; segment < 128; segment++) {
    fillArc(CENTER, 220, 150, 88, segment * 360 / 128, (segment + 1) * 360 / 128 + 0.5,
      hueColor(segment * 768 / 128));
  }
  drawText("CHOOSE A COLOR", 44, 3, "#fff");
  fillCircle(CENTER, 220, 62, hueColor(state.seedHue));
  strokeCircle(CENTER, 220, 63, "#fff");
  const angle = state.seedHue * Math.PI * 2 / 768;
  const markerX = CENTER + Math.round(Math.cos(angle) * 119);
  const markerY = 220 + Math.round(Math.sin(angle) * 119);
  fillCircle(markerX, markerY, 10, "#000");
  strokeCircle(markerX, markerY, 9, "#fff");
  strokeCircle(markerX, markerY, 8, "#fff");
  drawWizardFooter();
}

function drawDialScreen(title, value, caption, fraction) {
  const accent = hueColor(state.seedHue);
  const angle = 135 + fraction * 270;
  const radians = angle * Math.PI / 180;
  const knobX = CENTER + Math.round(Math.cos(radians) * DIAL_RADIUS);
  const knobY = DIAL_CENTER_Y + Math.round(Math.sin(radians) * DIAL_RADIUS);

  clearDisplay();
  drawText(title, 48, 3, "#fff");
  fillArc(CENTER, DIAL_CENTER_Y, 151, 128, 135, 405, rgb565(33, 32, 33));
  if (fraction > 0) fillArc(CENTER, DIAL_CENTER_Y, 151, 128, 135, angle, accent);
  context.strokeStyle = rgb565(123, 125, 123);
  context.lineWidth = 1;
  for (let tick = 0; tick <= 10; tick++) {
    const tickRadians = (135 + tick * 27) * Math.PI / 180;
    context.beginPath();
    context.moveTo(CENTER + Math.round(Math.cos(tickRadians) * 157), DIAL_CENTER_Y + Math.round(Math.sin(tickRadians) * 157));
    context.lineTo(CENTER + Math.round(Math.cos(tickRadians) * 166), DIAL_CENTER_Y + Math.round(Math.sin(tickRadians) * 166));
    context.stroke();
  }
  fillCircle(knobX, knobY, 13, "#fff");
  fillCircle(knobX, knobY, 8, accent);
  drawText(String(value), 180, 7, "#fff");
  drawText(caption, 252, 2, rgb565(156, 158, 156));
  drawWizardFooter();
}

function drawWizard() {
  state.mode = "wizard";
  status.textContent = `Setup wizard · ${state.wizardPage + 1} of 4`;
  if (state.wizardPage === 0) {
    drawColorWheel();
  } else if (state.wizardPage === 1) {
    const caption = state.variabilityPercent === 0 ? "SOLID" : state.variabilityPercent === 100 ? "RAINBOW" : "AROUND SEED";
    drawDialScreen("COLOR VARIATION", `${state.variabilityPercent}%`, caption, state.variabilityPercent / 100);
  } else if (state.wizardPage === 2) {
    drawDialScreen("PARTICLES", state.particleCount, "COUNT", (state.particleCount - 1) / 499);
  } else {
    drawDialScreen("SENSITIVITY", state.sensitivityLevel, "GRAVITY RESPONSE", (state.sensitivityLevel - 1) / 9);
  }
}

function dialFraction(x, y) {
  let angle = Math.atan2(y - DIAL_CENTER_Y, x - CENTER) * 180 / Math.PI;
  if (angle < 0) angle += 360;
  if (angle < 135) angle += 360;
  return Math.max(0, Math.min(1, (angle - 135) / 270));
}

function handleWizardTouch(x, y) {
  if (y >= 378 && y <= 445) {
    if (x >= 88 && x <= 230 && state.wizardPage > 0) state.wizardPage--;
    if (x >= 236 && x <= 378) {
      if (state.wizardPage < 3) state.wizardPage++;
      else startCountdown();
    }
    if (state.mode === "wizard") drawWizard();
    return;
  }

  const dx = x - CENTER;
  const dy = y - DIAL_CENTER_Y;
  const distance = Math.hypot(dx, dy);
  if (state.wizardPage === 0) {
    if (distance >= 70 && distance <= 175) {
      let hue = Math.round(Math.atan2(dy, dx) * 768 / (Math.PI * 2));
      if (hue < 0) hue += 768;
      state.seedHue = hue;
      drawWizard();
    }
    return;
  }
  if (distance < 82 || distance > 180) return;
  const fraction = dialFraction(x, y);
  if (state.wizardPage === 1) state.variabilityPercent = Math.round(fraction * 100);
  if (state.wizardPage === 2) state.particleCount = 1 + Math.round(fraction * 499);
  if (state.wizardPage === 3) state.sensitivityLevel = 1 + Math.round(fraction * 9);
  drawWizard();
}

function startCountdown() {
  state.mode = "countdown";
  state.countdown = 3;
  status.textContent = "Calibrating · keep still";
  drawCountdown();
  const timer = setInterval(() => {
    state.countdown--;
    if (state.countdown === 0) {
      clearInterval(timer);
      startSimulation();
    } else {
      drawCountdown();
    }
  }, 1000);
}

function drawCountdown() {
  clearDisplay();
  drawText("PUT ME DOWN", 145, 3, "#fff");
  drawText(state.countdown, 205, 8, hueColor(state.seedHue));
}

function initializeParticles() {
  const spacing = 8;
  const columns = Math.ceil(Math.sqrt(state.particleCount));
  const rows = Math.ceil(state.particleCount / columns);
  const startX = CENTER - (columns - 1) * spacing * 0.5;
  const startY = CENTER - (rows - 1) * spacing * 0.5;
  state.particles = Array.from({ length: state.particleCount }, (_, index) => ({
    x: startX + (index % columns) * spacing + (Math.floor(Math.random() * 21) - 10) * 0.1,
    y: startY + Math.floor(index / columns) * spacing + (Math.floor(Math.random() * 21) - 10) * 0.1,
    vx: 0,
    vy: 0,
    radius: 2 + Math.floor(Math.random() * 3),
  }));
}

function constrainToScreen(particle) {
  const dx = particle.x - CENTER;
  const dy = particle.y - CENTER;
  const maximumDistance = SCREEN_RADIUS - particle.radius;
  const distanceSquared = dx * dx + dy * dy;
  if (distanceSquared <= maximumDistance * maximumDistance) return;
  const distance = Math.sqrt(distanceSquared);
  const normalX = dx / distance;
  const normalY = dy / distance;
  particle.x = CENTER + normalX * maximumDistance;
  particle.y = CENTER + normalY * maximumDistance;
  const outwardSpeed = particle.vx * normalX + particle.vy * normalY;
  if (outwardSpeed > 0) {
    particle.vx -= outwardSpeed * normalX;
    particle.vy -= outwardSpeed * normalY;
  }
  particle.vx *= WALL_TANGENT_DAMPING;
  particle.vy *= WALL_TANGENT_DAMPING;
}

function resolveParticlePair(first, second) {
  const dx = second.x - first.x;
  const dy = second.y - first.y;
  const minimumDistance = first.radius + second.radius;
  const distanceSquared = dx * dx + dy * dy;
  if (distanceSquared >= minimumDistance * minimumDistance || distanceSquared < 0.01) return;
  const distance = Math.sqrt(distanceSquared);
  const normalX = dx / distance;
  const normalY = dy / distance;
  const correction = (minimumDistance - distance) * 0.5;
  first.x -= normalX * correction;
  first.y -= normalY * correction;
  second.x += normalX * correction;
  second.y += normalY * correction;
  const relativeNormalSpeed = (second.vx - first.vx) * normalX + (second.vy - first.vy) * normalY;
  if (relativeNormalSpeed < 0) {
    const impulse = relativeNormalSpeed * 0.5;
    first.vx += impulse * normalX;
    first.vy += impulse * normalY;
    second.vx -= impulse * normalX;
    second.vy -= impulse * normalY;
  }
}

function separateParticles() {
  const cells = Array.from({ length: GRID_SIZE * GRID_SIZE }, () => []);
  state.particles.forEach((particle, index) => {
    const cellX = Math.max(0, Math.min(GRID_SIZE - 1, Math.floor(particle.x / COLLISION_CELL_SIZE)));
    const cellY = Math.max(0, Math.min(GRID_SIZE - 1, Math.floor(particle.y / COLLISION_CELL_SIZE)));
    cells[cellY * GRID_SIZE + cellX].push(index);
  });
  state.particles.forEach((particle, first) => {
    const cellX = Math.max(0, Math.min(GRID_SIZE - 1, Math.floor(particle.x / COLLISION_CELL_SIZE)));
    const cellY = Math.max(0, Math.min(GRID_SIZE - 1, Math.floor(particle.y / COLLISION_CELL_SIZE)));
    for (let offsetY = -1; offsetY <= 1; offsetY++) {
      for (let offsetX = -1; offsetX <= 1; offsetX++) {
        const neighborX = cellX + offsetX;
        const neighborY = cellY + offsetY;
        if (neighborX < 0 || neighborX >= GRID_SIZE || neighborY < 0 || neighborY >= GRID_SIZE) continue;
        for (const second of cells[neighborY * GRID_SIZE + neighborX]) {
          if (second > first) resolveParticlePair(particle, state.particles[second]);
        }
      }
    }
  });
}

function updateSimulation(deltaSeconds) {
  const step = deltaSeconds / 2;
  const planarGravity = Math.hypot(state.smoothedGravityX, state.smoothedGravityY);
  const centerPull = Math.max(0, 1 - planarGravity / FLAT_GRAVITY_THRESHOLD);
  for (let substep = 0; substep < 2; substep++) {
    for (const particle of state.particles) {
      const sensitivityMultiplier = 0.5 + (state.sensitivityLevel - 1) * (4 / 9);
      const gravityStrength = BASE_GRAVITY_STRENGTH * sensitivityMultiplier;
      particle.vx += state.smoothedGravityX * gravityStrength * step;
      particle.vy += state.smoothedGravityY * gravityStrength * step;
      particle.vx -= (particle.x - CENTER) / SCREEN_RADIUS * FLAT_CENTERING_STRENGTH * centerPull * step;
      particle.vy -= (particle.y - CENTER) / SCREEN_RADIUS * FLAT_CENTERING_STRENGTH * centerPull * step;
      particle.vx *= VELOCITY_DAMPING;
      particle.vy *= VELOCITY_DAMPING;
      const speed = Math.hypot(particle.vx, particle.vy);
      if (speed > MAX_SPEED) {
        particle.vx *= MAX_SPEED / speed;
        particle.vy *= MAX_SPEED / speed;
      }
      particle.x += particle.vx * step;
      particle.y += particle.vy * step;
      constrainToScreen(particle);
    }
    separateParticles();
    state.particles.forEach(constrainToScreen);
  }
}

function renderParticles(now) {
  clearDisplay();
  for (let index = 0; index < state.particles.length; index++) {
    const particle = state.particles[index];
    const color = particleColor(index, now);
    fillCircle(Math.round(particle.x), Math.round(particle.y), particle.radius + 2, scaleColor(color, 0.2));
    fillCircle(Math.round(particle.x), Math.round(particle.y), particle.radius, color);
    fillCircle(Math.round(particle.x), Math.round(particle.y), Math.sqrt(2), brightenColor(color));
  }
}

function startSimulation() {
  state.mode = "simulation";
  state.smoothedGravityX = 0;
  state.smoothedGravityY = 0;
  state.simulationStartMs = performance.now();
  state.lastFrameMs = state.simulationStartMs;
  initializeParticles();
  status.textContent = `${state.particleCount} particles · sensitivity ${state.sensitivityLevel}`;
}

function frame(now) {
  if (state.mode === "simulation") {
    const deltaSeconds = Math.min(0.033, (now - state.lastFrameMs) / 1000);
    state.lastFrameMs = now;
    state.smoothedGravityX += (state.gravityX - state.smoothedGravityX) * ACCEL_SMOOTHING;
    state.smoothedGravityY += (state.gravityY - state.smoothedGravityY) * ACCEL_SMOOTHING;
    updateSimulation(deltaSeconds);
    renderParticles(now);
  }
  requestAnimationFrame(frame);
}

function canvasPoint(event) {
  const bounds = canvas.getBoundingClientRect();
  return {
    x: (event.clientX - bounds.left) * DISPLAY_SIZE / bounds.width,
    y: (event.clientY - bounds.top) * DISPLAY_SIZE / bounds.height,
  };
}

canvas.addEventListener("pointerdown", (event) => {
  const point = canvasPoint(event);
  if (state.mode === "simulation") {
    state.wizardPage = 0;
    drawWizard();
  } else if (state.mode === "wizard") {
    handleWizardTouch(point.x, point.y);
  }
});

function setGravity(x, y) {
  const magnitude = Math.hypot(x, y);
  if (magnitude > 1) {
    x /= magnitude;
    y /= magnitude;
  }
  state.gravityX = x;
  state.gravityY = y;
  tiltKnob.style.left = `${50 + x * 39}%`;
  tiltKnob.style.top = `${50 + y * 39}%`;
  gravityReadout.textContent = `X ${x.toFixed(2)}  Y ${y.toFixed(2)}`;
  tiltPad.setAttribute("aria-valuenow", Math.hypot(x, y).toFixed(2));
}

function updateGravityFromPointer(event) {
  const bounds = tiltPad.getBoundingClientRect();
  setGravity((event.clientX - bounds.left - bounds.width / 2) / (bounds.width * 0.39),
    (event.clientY - bounds.top - bounds.height / 2) / (bounds.height * 0.39));
}

tiltPad.addEventListener("pointerdown", (event) => {
  tiltPad.setPointerCapture(event.pointerId);
  updateGravityFromPointer(event);
});
tiltPad.addEventListener("pointermove", (event) => {
  if (tiltPad.hasPointerCapture(event.pointerId)) updateGravityFromPointer(event);
});
tiltPad.addEventListener("dblclick", () => setGravity(0, 0));
tiltPad.addEventListener("keydown", (event) => {
  const step = event.shiftKey ? 0.1 : 0.03;
  if (event.key === "ArrowLeft") setGravity(state.gravityX - step, state.gravityY);
  else if (event.key === "ArrowRight") setGravity(state.gravityX + step, state.gravityY);
  else if (event.key === "ArrowUp") setGravity(state.gravityX, state.gravityY - step);
  else if (event.key === "ArrowDown") setGravity(state.gravityX, state.gravityY + step);
  else if (event.key === "Home" || event.key === "0") setGravity(0, 0);
  else return;
  event.preventDefault();
});

document.querySelector("#level-button").addEventListener("click", () => setGravity(0, 0));
document.querySelector("#restart-button").addEventListener("click", () => {
  state.wizardPage = 0;
  setGravity(0, 0);
  drawWizard();
});

drawWizard();
requestAnimationFrame(frame);