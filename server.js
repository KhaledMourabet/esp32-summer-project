// ============================================================
// TRIVIA GAME — Node.js backend server (ESP32-C3 edition)
// ------------------------------------------------------------
// What this file does:
//
//   1. Runs a WebSocket server on port 8080 for the BROWSER.
//      The browser (page.js) connects here to receive game
//      events and send answer selections.
//
//   2. Runs a WebSocket server on port 8081 for the ESP32s.
//      All 5 ESP32-C3 units (1 admin + 4 players) connect here.
//      They identify themselves on connect, then exchange
//      game messages in both directions.
//
//   3. Serves static files on port 3000 (HTTP).
//      - GET /          → page.html  (main game screen)
//      - GET /join?player=P1  → join.html  (player name entry)
//      - POST /join     → receives player name submission
//
//   4. Manages game state machine:
//      LOBBY → IDLE → QUESTION → BUZZABLE → ANSWERED → IDLE
//
//      LOBBY    — players scan QR codes and enter names.
//                 Game cannot start until all 4 names are set.
//      IDLE     — waiting for admin button press.
//      QUESTION — question shown, players can buzz in.
//      BUZZABLE — one player has buzzed, admin picks answer.
//                 (SPINNING state removed — no more spin phase)
//      ANSWERED — result shown, admin presses to continue.
//
// HOW TO RUN:
//   1. npm install
//   2. node server.js
//   3. Open http://localhost:3000 in your browser
//   4. Players scan QR codes shown on screen and enter names
//   5. Admin presses button (or click "Start" for testing)
// ============================================================

'use strict';

const path = require('path');
const fs   = require('fs');
const http = require('http');
const XLSX = require('xlsx');
const QRCode = require('qrcode');
const { WebSocketServer, WebSocket } = require('ws');
const os = require('os');


// ── CONFIGURATION ─────────────────────────────────────────────

const STATIC_DIR    = __dirname;
const QUESTIONS_FILE = path.join(__dirname, 'trivia_game.xlsx');
const HTTP_PORT     = 3000;
const WS_BROWSER_PORT = 8080;  // browser connects here
const WS_ESP32_PORT   = 8081;  // ESP32s connect here

// Get the local IP so QR codes point to the right address.
// We pick the first non-loopback IPv4 address on the machine.
// This is the IP players need to reach the join page on their phones.
function getLocalIP() {
  const interfaces = os.networkInterfaces();
  // Prefer interfaces whose name suggests WiFi or Ethernet.
  // This filters out WSL virtual adapters (vEthernet, WSL)
  // which would give the wrong IP for QR codes and ESP32 connections.
  const preferred = ['wi-fi', 'wifi', 'wlan', 'ethernet', 'en0', 'en1', 'eth'];
  for (const name of Object.keys(interfaces)) {
    const nameLower = name.toLowerCase();
    const isPreferred = preferred.some(p => nameLower.includes(p));
    if (!isPreferred) continue;
    for (const iface of interfaces[name]) {
      if (iface.family === 'IPv4' && !iface.internal) {
        return iface.address;
      }
    }
  }
  // Fallback: return the first non-internal IPv4 that isn't 172.x (WSL range)
  for (const name of Object.keys(interfaces)) {
    for (const iface of interfaces[name]) {
      if (iface.family === 'IPv4' && !iface.internal && !iface.address.startsWith('172.')) {
        return iface.address;
      }
    }
  }
  return 'localhost';
}
const LOCAL_IP = '172.20.10.2'; //change to current IP
console.log(`Local IP detected: ${LOCAL_IP}`);


// ── LOAD QUESTIONS ────────────────────────────────────────────

function loadQuestions() {
  const workbook = XLSX.readFile(QUESTIONS_FILE);
  const sheet = workbook.Sheets[workbook.SheetNames[0]];
  const rows = XLSX.utils.sheet_to_json(sheet);
  console.log(`Loaded ${rows.length} questions from ${QUESTIONS_FILE}`);
  return rows;
}

const ALL_QUESTIONS = loadQuestions();
const usedQuestionIndices = new Set();

function getRandomQuestion() {
  if (usedQuestionIndices.size >= ALL_QUESTIONS.length) {
    usedQuestionIndices.clear();
    console.log('All questions used — reshuffling.');
  }
  let index;
  do { index = Math.floor(Math.random() * ALL_QUESTIONS.length); }
  while (usedQuestionIndices.has(index));
  usedQuestionIndices.add(index);
  return ALL_QUESTIONS[index];
}


// ── GAME STATE ────────────────────────────────────────────────

const STATE = {
  LOBBY:    'LOBBY',     // waiting for all players to enter names
  IDLE:     'IDLE',      // waiting for admin to start a round
  QUESTION: 'QUESTION',  // question shown, players can buzz in
  BUZZABLE: 'BUZZABLE',  // a player buzzed in, admin picks answer
  ANSWERED: 'ANSWERED',  // result shown, admin presses to continue
};

let gameState = STATE.LOBBY;
let currentQuestion = null;
let buzzer = null; // 'P1'|'P2'|'P3'|'P4' or null

// Player data. Names start empty — filled via QR code join page.
// readyToStart becomes true once all 4 names are set.
const players = {
  P1: { name: '', score: 0, joined: false, inactive: false },
  P2: { name: '', score: 0, joined: false, inactive: false },
  P3: { name: '', score: 0, joined: false, inactive: false },
  P4: { name: '', score: 0, joined: false, inactive: false },
};

function allPlayersJoined() {
  return Object.values(players).every(p => p.joined);
}

// Pre-generate QR code data URLs for each player join link.
// We generate them once at startup and cache them.
const qrCodes = {};
async function generateQRCodes() {
  for (const id of ['P1', 'P2', 'P3', 'P4']) {
    const url = `http://${LOCAL_IP}:${HTTP_PORT}/join?player=${id}`;
    qrCodes[id] = await QRCode.toDataURL(url, { width: 200, margin: 1 });
    console.log(`QR code for ${id}: ${url}`);
  }
}


// ── ESP32 CONNECTION REGISTRY ─────────────────────────────────

// We keep a reference to each ESP32's WebSocket connection so
// we can send messages directly to specific units.
//
// esp32Clients = {
//   ADMIN: <WebSocket>,
//   P1:    <WebSocket>,
//   P2:    <WebSocket>,
//   P3:    <WebSocket>,
//   P4:    <WebSocket>,
// }
const esp32Clients = {};

// Send a JSON message to a specific ESP32 by its role/player ID.
// Safe to call even if that ESP32 is not currently connected —
// it just logs a warning instead of crashing.
function sendToESP32(id, obj) {
  const ws = esp32Clients[id];
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    console.warn(`sendToESP32("${id}") skipped — not connected.`);
    return;
  }
  ws.send(JSON.stringify(obj));
}

// Send a message to ALL connected ESP32s.
function broadcastToESP32s(obj) {
  for (const id of Object.keys(esp32Clients)) {
    sendToESP32(id, obj);
  }
}


// ── BROWSER WEBSOCKET SERVER (port 8080) ──────────────────────

const wssBrowser = new WebSocketServer({ port: WS_BROWSER_PORT });

wssBrowser.on('listening', () => {
  console.log(`Browser WebSocket running on ws://localhost:${WS_BROWSER_PORT}`);
});

wssBrowser.on('connection', (ws) => {
  console.log('Browser connected.');

  // Send full current state immediately so the page syncs on refresh.
  sendStateToBrowser(ws);
  // Also send QR codes so the lobby screen can display them.
  sendQRCodesToBrowser(ws);

  ws.on('message', (data) => {
    try {
      const msg = JSON.parse(data);
      console.log('Browser → Server:', msg);

      if (msg.type === 'ANSWER') {
        handleAnswer(msg.answer);
      }
      // SET_NAME is no longer used from the browser —
      // names come from the QR join page now.
      // We keep it as a fallback for testing without ESP32s.
      if (msg.type === 'SET_NAME' && players[msg.player] !== undefined) {
        setPlayerName(msg.player, msg.name);
      }
      // Allow admin to trigger ADMIN press from browser (for testing
      // without the physical admin ESP32).
      if (msg.type === 'ADMIN') {
        handleAdminPress();
      }
      // Skip a player slot — marks them inactive so the game
      // can start without all 4 players joining.
      if (msg.type === 'SKIP_PLAYER' && players[msg.player] !== undefined) {
        skipPlayer(msg.player);
      }
    } catch (e) {
      console.error('Bad message from browser:', data.toString());
    }
  });

  ws.on('close', () => console.log('Browser disconnected.'));
});

// Send a message to all connected browser clients.
function broadcastToBrowser(obj) {
  wssBrowser.clients.forEach(client => {
    if (client.readyState === WebSocket.OPEN) {
      client.send(JSON.stringify(obj));
    }
  });
}

// Send one specific browser client the full game state.
function sendStateToBrowser(ws) {
  const msg = {
    type: 'STATE',
    state: gameState,
    question: currentQuestion ? formatQuestion(currentQuestion) : null,
    buzzer,
    players,
  };
  if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(msg));
}

// Send QR codes to one browser client (used on lobby screen).
function sendQRCodesToBrowser(ws) {
  const msg = { type: 'QR_CODES', codes: qrCodes, localIP: LOCAL_IP };
  if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(msg));
}

// Broadcast full state to all browser clients.
function broadcastStateToBrowser() {
  broadcastToBrowser({
    type: 'STATE',
    state: gameState,
    question: currentQuestion ? formatQuestion(currentQuestion) : null,
    buzzer,
    players,
  });
}


// ── ESP32 WEBSOCKET SERVER (port 8081) ────────────────────────

const wssESP32 = new WebSocketServer({ port: WS_ESP32_PORT });

wssESP32.on('listening', () => {
  console.log(`ESP32 WebSocket running on ws://${LOCAL_IP}:${WS_ESP32_PORT}`);
  console.log(`(ESP32 sketches should connect to that address)`);
});

wssESP32.on('connection', (ws) => {
  console.log('ESP32 connected (not yet identified).');

  // Each ESP32 must send a HELLO message as its very first message.
  // Until it does, we don't know which unit it is.
  // Format: { "type": "HELLO", "role": "ADMIN" }
  //      or { "type": "HELLO", "role": "PLAYER", "player": "P1" }
  let clientId = null; // will be 'ADMIN', 'P1', 'P2', 'P3', or 'P4'

  ws.on('message', (data) => {
    let msg;
    try { msg = JSON.parse(data.toString()); }
    catch (e) { console.error('Bad JSON from ESP32:', data.toString()); return; }

    // ── Handle HELLO (identification) ──
    if (msg.type === 'HELLO') {
      if (msg.role === 'ADMIN') {
        clientId = 'ADMIN';
        esp32Clients['ADMIN'] = ws;
        console.log('Admin ESP32 identified and registered.');
        // Send current state so admin OLED can sync.
        sendToESP32('ADMIN', { type: 'STATE', state: gameState });
      } else if (msg.role === 'PLAYER' && ['P1','P2','P3','P4'].includes(msg.player)) {
        clientId = msg.player;
        esp32Clients[clientId] = ws;
        console.log(`Player ESP32 ${clientId} identified and registered.`);
        // Send current state so OLED syncs immediately.
        sendToESP32(clientId, {
          type: 'STATE',
          state: gameState,
          name: players[clientId].name,
          question: currentQuestion ? formatQuestion(currentQuestion) : null,
        });
      } else {
        console.warn('ESP32 sent HELLO with unknown role:', msg);
      }
      return;
    }

    // ── Ignore messages from unidentified clients ──
    if (!clientId) {
      console.warn('Message from unidentified ESP32 — ignoring. Send HELLO first.');
      return;
    }

    console.log(`ESP32 [${clientId}] → Server:`, msg);

    // ── Handle messages from identified clients ──
    if (msg.type === 'ADMIN') {
      // Admin button pressed.
      handleAdminPress();

    } else if (msg.type === 'PRESS' && ['P1','P2','P3','P4'].includes(clientId)) {
      // Player button pressed.
      handlePlayerPress(clientId);
    }
  });

  ws.on('close', () => {
    if (clientId) {
      console.warn(`ESP32 [${clientId}] disconnected.`);
      delete esp32Clients[clientId];
      // Tell the browser an ESP32 went offline.
      broadcastToBrowser({ type: 'ESP32_OFFLINE', id: clientId });
    }
  });

  ws.on('error', (err) => {
    console.error(`ESP32 [${clientId || 'unknown'}] error:`, err.message);
  });
});


// ── HTTP SERVER (port 3000) ───────────────────────────────────

const httpServer = http.createServer((req, res) => {
  const urlObj = new URL(req.url, `http://${req.headers.host}`);
  const pathname = urlObj.pathname;

  // ── POST /join — player submits their name from their phone ──
  if (req.method === 'POST' && pathname === '/join') {
    let body = '';
    req.on('data', chunk => { body += chunk.toString(); });
    req.on('end', () => {
      try {
        const { player, name } = JSON.parse(body);
        if (['P1','P2','P3','P4'].includes(player) && name && name.trim()) {
          setPlayerName(player, name.trim());
          res.writeHead(200, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({ ok: true }));
        } else {
          res.writeHead(400, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({ ok: false, error: 'Invalid player or name' }));
        }
      } catch (e) {
        res.writeHead(400, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ ok: false, error: 'Bad JSON' }));
      }
    });
    return;
  }

  // ── GET /join?player=P1 — serve the mobile name-entry page ──
  if (req.method === 'GET' && pathname === '/join') {
    const filePath = path.join(STATIC_DIR, 'join.html');
    fs.readFile(filePath, (err, data) => {
      if (err) {
        res.writeHead(404, { 'Content-Type': 'text/plain' });
        res.end('join.html not found');
        return;
      }
      res.writeHead(200, { 'Content-Type': 'text/html' });
      res.end(data);
    });
    return;
  }

  // ── GET / and other static files ──
  let filePath = pathname === '/' ? '/page.html' : pathname;
  const fullPath = path.join(STATIC_DIR, filePath);

  const contentTypes = {
    '.html': 'text/html',
    '.js':   'text/javascript',
    '.css':  'text/css',
    '.xlsx': 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet',
  };
  const ext = path.extname(fullPath).toLowerCase();
  const contentType = contentTypes[ext] || 'application/octet-stream';

  fs.readFile(fullPath, (err, data) => {
    if (err) {
      res.writeHead(404, { 'Content-Type': 'text/plain' });
      res.end(`Not found: ${filePath}`);
      return;
    }
    res.writeHead(200, { 'Content-Type': contentType });
    res.end(data);
  });
});

httpServer.listen(HTTP_PORT, () => {
  console.log(`HTTP server running on http://localhost:${HTTP_PORT}`);
  console.log('Open that URL in your browser to start the game.');
});


// ── GAME LOGIC ────────────────────────────────────────────────

// skipPlayer() marks a player slot as inactive.
// The lobby card shows "Inactive" and that slot is ignored
// for buzzing and scoring. The game can start without them.
function skipPlayer(player) {
  if (players[player].joined) return; // can't skip someone who already joined
  players[player].inactive = true;
  players[player].joined   = true;   // counts as "ready" for lobby purposes
  players[player].name     = 'Inactive';
  console.log(`${player} marked as inactive.`);

  broadcastToBrowser({ type: 'PLAYER_JOINED', player, name: 'Inactive', players });

  // If all slots are now either joined or inactive, start the game.
  if (gameState === STATE.LOBBY && allPlayersJoined()) {
    gameState = STATE.IDLE;
    console.log('All slots ready. Moving to IDLE.');
    broadcastStateToBrowser();
    broadcastToESP32s({ type: 'STATE', state: STATE.IDLE });
  }
}

// setPlayerName() is called when a player submits their name
// via the QR join page (HTTP POST /join).
function setPlayerName(player, name) {
  players[player].name = name;
  players[player].joined = true;
  console.log(`${player} set name to "${name}".`);

  // Tell the matching player ESP32 its name so the OLED can show it.
  sendToESP32(player, { type: 'SET_NAME', name });

  // Tell the browser to update the lobby screen.
  broadcastToBrowser({ type: 'PLAYER_JOINED', player, name, players });

  // If all players have joined, move from LOBBY to IDLE.
  if (gameState === STATE.LOBBY && allPlayersJoined()) {
    gameState = STATE.IDLE;
    console.log('All players joined. Moving to IDLE — ready to start.');
    broadcastStateToBrowser();
    broadcastToESP32s({ type: 'STATE', state: STATE.IDLE });
  }
}

// formatQuestion() converts a raw Excel row into the format
// used by both the browser and the ESP32 OLEDs.
// The correct answer is NEVER included — server checks it privately.
function formatQuestion(q) {
  return {
    category: q['Category'],
    question: q['Question'],
    optionA:  q['Option A'],
    optionB:  q['Option B'],
    optionC:  q['Option C'],
    optionD:  q['Option D'],
  };
}

// handleAdminPress() drives the state machine forward.
// Called when the admin ESP32 sends { type: "ADMIN" },
// or when the browser sends { type: "ADMIN" } (testing mode).
function handleAdminPress() {
  console.log(`Admin pressed. State: ${gameState}`);

  if (gameState === STATE.LOBBY) {
    // Allow admin to force-skip lobby if testing without phones.
    // In real use, players must join via QR first.
    console.log('Admin pressed in LOBBY — skipping to IDLE (testing mode).');
    gameState = STATE.IDLE;
    broadcastStateToBrowser();
    broadcastToESP32s({ type: 'STATE', state: STATE.IDLE });
    return;
  }

  if (gameState === STATE.IDLE) {
    // Start a new round: pick question, go to QUESTION state.
    // In the new flow there is no SPINNING state —
    // players can buzz in immediately after the question appears.
    currentQuestion = getRandomQuestion();
    buzzer = null;
    gameState = STATE.QUESTION;

    const formatted = formatQuestion(currentQuestion);

    // Tell the browser to show the question.
    broadcastToBrowser({
      type: 'STATE',
      state: gameState,
      question: formatted,
      buzzer: null,
      players,
    });

    // Tell ALL player ESP32s the category of the new question.
    // This is the core "ESP32 connected to backend" demonstration.
    for (const id of ['P1','P2','P3','P4']) {
      sendToESP32(id, {
        type: 'QUESTION',
        category: currentQuestion['Category'],
      });
    }
    // Tell admin ESP32 the state changed.
    sendToESP32('ADMIN', { type: 'STATE', state: gameState });

    console.log(`New question: ${currentQuestion['Question']}`);
    return;
  }

  if (gameState === STATE.QUESTION) {
    // Second admin press: open the buzzer window.
    // Players can now press their buttons.
    gameState = STATE.BUZZABLE;

    broadcastToBrowser({
      type: 'STATE',
      state: gameState,
      question: formatQuestion(currentQuestion),
      buzzer: null,
      players,
    });

    // Tell all player ESP32s they can now buzz in.
    broadcastToESP32s({ type: 'BUZZABLE' });
    sendToESP32('ADMIN', { type: 'STATE', state: gameState });

    console.log('Buzzer window open.');
    return;
  }

  if (gameState === STATE.ANSWERED) {
    // Admin acknowledges result, resets for next round.
    gameState = STATE.IDLE;
    currentQuestion = null;
    buzzer = null;

    // Turn off all LEDs on the admin ESP32.
    sendToESP32('ADMIN', { type: 'LED', command: 'OFF' });

    // Reset all player ESP32 OLEDs to idle screen.
    broadcastToESP32s({ type: 'STATE', state: STATE.IDLE });

    broadcastToBrowser({
      type: 'STATE',
      state: gameState,
      question: null,
      buzzer: null,
      players,
    });

    console.log('Round over. Back to IDLE.');
  }
}

// handlePlayerPress() is called when a player ESP32 sends
// { type: "PRESS" } over the ESP32 WebSocket.
function handlePlayerPress(player) {
  // Only accept buzzes during QUESTION or BUZZABLE states.
  // In the new flow, players can buzz as soon as the question appears.
  if (gameState !== STATE.QUESTION && gameState !== STATE.BUZZABLE) return;

  // Ignore inactive players.
  if (players[player].inactive) return;

  // Only the first press counts.
  if (buzzer !== null) return;

  buzzer = player;
  gameState = STATE.BUZZABLE;

  const playerName = players[player].name || player;
  console.log(`${playerName} (${player}) buzzed in!`);

  // Tell the browser who buzzed.
  broadcastToBrowser({
    type: 'BUZZED',
    player,
    name: playerName,
  });

  // Tell the buzzing player's ESP32 it won the buzzer
  // (lights up button LED, shows "YOUR TURN" on OLED).
  sendToESP32(player, { type: 'BUZZED_IN' });

  // Tell all OTHER player ESP32s they are locked out
  // (dims their button LED, shows "LOCKED" on OLED).
  for (const id of ['P1','P2','P3','P4']) {
    if (id !== player) {
      sendToESP32(id, { type: 'LOCKED' });
    }
  }

  // Tell the admin ESP32 who buzzed (so it can show the name on its OLED).
  sendToESP32('ADMIN', { type: 'BUZZED', player, name: playerName });
}

// handleAnswer() is called when the browser sends
// { type: "ANSWER", answer: "A" } (admin clicks an answer button).
function handleAnswer(answer) {
  if (gameState !== STATE.BUZZABLE || buzzer === null) {
    console.log('Answer received but no active buzzer — ignoring.');
    return;
  }

  const correct = currentQuestion['Correct Answer'];
  const isCorrect = (answer === correct);
  const playerName = players[buzzer].name || buzzer;

  if (isCorrect) {
    players[buzzer].score += 1;
    // Corner LEDs light up on admin ESP32.
    sendToESP32('ADMIN', { type: 'LED', command: 'WIN' });
    // Show ✓ animation on the buzzing player's OLED.
    sendToESP32(buzzer, { type: 'RESULT', correct: true });
    console.log(`${playerName} correct! Score: ${players[buzzer].score}`);
  } else {
    players[buzzer].score -= 1;
    // All 5 LEDs light up + buzzer beep on admin ESP32.
    sendToESP32('ADMIN', { type: 'LED', command: 'LOSE' });
    // Show ✗ animation on the buzzing player's OLED.
    sendToESP32(buzzer, { type: 'RESULT', correct: false });
    console.log(`${playerName} wrong. Score: ${players[buzzer].score}`);
  }

  // Show neutral result screen on all other player ESP32s.
  for (const id of ['P1','P2','P3','P4']) {
    if (id !== buzzer) {
      sendToESP32(id, { type: 'ROUND_OVER' });
    }
  }

  gameState = STATE.ANSWERED;

  // Tell browser the result with updated scores.
  broadcastToBrowser({
    type: 'RESULT',
    correct: isCorrect,
    selectedAnswer: answer,
    correctAnswer: correct,
    player: buzzer,
    name: playerName,
    players,
  });

  console.log(`Correct answer: ${correct}. Admin presses to continue.`);
}


// ── STARTUP ───────────────────────────────────────────────────

// Generate QR codes last (async) so everything else is ready first.
generateQRCodes().then(() => {
  console.log('QR codes ready.');
}).catch(err => {
  console.error('QR code generation failed:', err.message);
  console.error('Run: npm install qrcode');
});