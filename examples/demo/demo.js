// SPDX-License-Identifier: Apache-2.0
// Echo console for wsrv: connect, send, and watch what the browser sees on the wire.
// No framework, no network access except the WebSocket the user opens.
"use strict";

const PREVIEW_CHARS = 80;
const CLOSE_MEANINGS = new Map([
  [1000, "normal closure"],
  [1001, "going away (server shutting down, or an idle or unanswered peer)"],
  [1002, "protocol error"],
  [1003, "unsupported data"],
  [1006, "no Close frame: handshake refused (check Origin) or connection lost"],
  [1007, "invalid payload data (not UTF-8)"],
  [1008, "policy violation (a limit was exceeded)"],
  [1009, "message too big"],
  [1011, "server error"],
]);

const el = {
  form: document.getElementById("connect"),
  url: document.getElementById("url"),
  toggle: document.getElementById("toggle"),
  state: document.getElementById("state"),
  log: document.getElementById("log"),
  empty: document.getElementById("empty"),
  text: document.getElementById("text"),
  binarySize: document.getElementById("binary-size"),
  closeCode: document.getElementById("close-code"),
  closeReason: document.getElementById("close-reason"),
};

let socket = null;
let openedAt = 0;

function setState(state, label) {
  el.state.dataset.state = state;
  el.state.textContent = label;
  el.toggle.textContent = state === "closed" ? "Connect" : "Disconnect";
  const open = state === "open";
  for (const control of document.querySelectorAll("[data-needs-open]")) {
    control.disabled = !open;
  }
}

function byteLength(text) {
  return new TextEncoder().encode(text).length;
}

function preview(text) {
  const flat = text.replace(/\s+/g, " ");
  return flat.length > PREVIEW_CHARS ? `${flat.slice(0, PREVIEW_CHARS)}…` : flat;
}

/** side: "sent" | "recv" | "note". Text goes in via textContent: payloads are never parsed as HTML. */
function addEntry(side, title, detail, { alert = false } = {}) {
  const item = document.createElement("li");
  item.className = `entry ${side}${alert ? " alert" : ""} fresh`;
  const card = document.createElement("div");
  card.className = "card";
  const heading = document.createElement("strong");
  heading.textContent = title;
  const meta = document.createElement("span");
  meta.className = "meta";
  meta.textContent = openedAt ? `+${Math.round(performance.now() - openedAt)} ms` : "";
  card.append(heading, meta);
  if (detail) {
    const body = document.createElement("span");
    body.className = "payload";
    body.textContent = detail;
    card.append(body);
  }
  item.append(card);
  el.log.append(item);
  el.empty.hidden = true;
  item.scrollIntoView({ block: "nearest" });
}

function connect() {
  let ws;
  try {
    ws = new WebSocket(el.url.value.trim());
  } catch (error) {
    addEntry("note", "Cannot connect", `${el.url.value} is not a valid ws:// or wss:// URL.`, { alert: true });
    return;
  }
  ws.binaryType = "arraybuffer";
  socket = ws;
  openedAt = performance.now();
  setState("connecting", "Connecting");
  addEntry("note", "Handshake request", el.url.value.trim());

  ws.addEventListener("open", () => {
    setState("open", "Connected");
    addEntry("note", "Connected", ws.protocol ? `subprotocol ${ws.protocol}` : "no subprotocol");
  });
  ws.addEventListener("message", (event) => {
    if (typeof event.data === "string") {
      addEntry("recv", `Text, ${byteLength(event.data)} bytes`, preview(event.data));
    } else {
      addEntry("recv", `Binary, ${event.data.byteLength} bytes`, "");
    }
  });
  ws.addEventListener("error", () => {
    // Browsers hide the reason on purpose; the close event that follows has the code.
    addEntry("note", "Connection error", "The browser does not say why. See the close code below.", { alert: true });
  });
  ws.addEventListener("close", (event) => {
    const meaning = CLOSE_MEANINGS.get(event.code) ?? "application-defined or unassigned";
    const reason = event.reason ? ` Reason: "${event.reason}".` : "";
    addEntry("note", `Closed with ${event.code}`, `${meaning}.${reason} Clean handshake: ${event.wasClean ? "yes" : "no"}.`,
      { alert: event.code !== 1000 && event.code !== 1001 });
    if (socket === ws) {
      socket = null;
      openedAt = 0;
      setState("closed", "Disconnected");
    }
  });
}

function sendText(text) {
  socket.send(text);
  addEntry("sent", `Text, ${byteLength(text)} bytes`, preview(text));
}

function sendBinary(size) {
  const bytes = new Uint8Array(size);
  for (let i = 0; i < size; i += 1) {
    bytes[i] = i & 0xff;
  }
  socket.send(bytes);
  addEntry("sent", `Binary, ${size} bytes`, "");
}

el.form.addEventListener("submit", (event) => {
  event.preventDefault();
  if (socket) {
    socket.close(1000, "client disconnect");
  } else {
    connect();
  }
});

document.getElementById("text-form").addEventListener("submit", (event) => {
  event.preventDefault();
  sendText(el.text.value);
});

document.getElementById("binary-form").addEventListener("submit", (event) => {
  event.preventDefault();
  const size = Number.parseInt(el.binarySize.value, 10);
  if (Number.isInteger(size) && size >= 0 && size <= 16 * 1024 * 1024) {
    sendBinary(size);
  } else {
    addEntry("note", "Not sent", "Binary size must be from 0 to 16777216 bytes.", { alert: true });
  }
});

for (const button of document.querySelectorAll("[data-probe]")) {
  button.addEventListener("click", () => sendText("a".repeat(Number(button.dataset.probe))));
}

document.getElementById("close-form").addEventListener("submit", (event) => {
  event.preventDefault();
  const code = Number.parseInt(el.closeCode.value, 10);
  try {
    socket.close(code, el.closeReason.value);
    addEntry("sent", `Close request, code ${code}`, el.closeReason.value);
  } catch (error) {
    addEntry("note", "Not sent", `${error.name}: browsers accept only 1000 and 3000 to 4999.`, { alert: true });
  }
});

document.getElementById("clear").addEventListener("click", () => {
  el.log.replaceChildren();
  el.empty.hidden = false;
});

setState("closed", "Disconnected");
