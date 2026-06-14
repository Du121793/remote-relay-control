const fs = require("fs");
const path = require("path");
const tls = require("tls");

const DATA_DIR = path.join(__dirname, "data");
const clusters = JSON.parse(
  fs.readFileSync(path.join(DATA_DIR, "cloud-clusters.json"), "utf8")
).clusters;
const devices = JSON.parse(
  fs.readFileSync(path.join(DATA_DIR, "cloud-devices.json"), "utf8")
).devices;
const device = devices.find((item) => item.enabled !== false && item.deviceId);
const cluster = clusters.find((item) => item.clusterId === device.clusterId);

function encodeRemainingLength(length) {
  const result = [];
  let value = length;
  do {
    let byte = value % 128;
    value = Math.floor(value / 128);
    if (value > 0) {
      byte |= 0x80;
    }
    result.push(byte);
  } while (value > 0);
  return Buffer.from(result);
}

function encodeString(text) {
  const value = Buffer.from(String(text), "utf8");
  const size = Buffer.alloc(2);
  size.writeUInt16BE(value.length, 0);
  return Buffer.concat([size, value]);
}

function packet(header, body) {
  return Buffer.concat([Buffer.from([header]), encodeRemainingLength(body.length), body]);
}

function connectPacket() {
  const variableHeader = Buffer.concat([
    encodeString("MQTT"),
    Buffer.from([4, 0xc2, 0, 20]),
  ]);
  const payload = Buffer.concat([
    encodeString(`smoke-${Date.now()}`),
    encodeString(device.keyId),
    encodeString(cluster.bindingSecret),
  ]);
  return packet(0x10, Buffer.concat([variableHeader, payload]));
}

function subscribePacket(topic) {
  return packet(0x82, Buffer.concat([
    Buffer.from([0x00, 0x01]),
    encodeString(topic),
    Buffer.from([0x00]),
  ]));
}

function publishPacket(topic, payload, retain = false) {
  return packet(0x30 | (retain ? 1 : 0), Buffer.concat([
    encodeString(topic),
    Buffer.from(payload, "utf8"),
  ]));
}

const topic = `cluster/${cluster.clusterId}/smoke`;
const socket = tls.connect({
  host: "127.0.0.1",
  port: Number(process.env.MQTT_PORT || 8883),
  minVersion: "TLSv1.2",
  maxVersion: "TLSv1.2",
  ciphers: "PSK-AES128-GCM-SHA256",
  rejectUnauthorized: false,
  pskCallback() {
    return {
      identity: device.deviceId,
      psk: Buffer.from(cluster.bindingSecret, "base64url"),
    };
  },
});

let buffer = Buffer.alloc(0);
let connected = false;
let subscribed = false;
let received = false;
let finished = false;
const timeout = setTimeout(() => {
  if (finished) {
    return;
  }
  finished = true;
  console.error("MQTT smoke test timeout");
  process.exitCode = 1;
  socket.destroy();
}, 5000);

socket.on("secureConnect", () => {
  socket.write(connectPacket());
});

socket.on("data", (chunk) => {
  if (finished) {
    return;
  }
  buffer = Buffer.concat([buffer, chunk]);
  if (!connected && buffer.includes(Buffer.from([0x20, 0x02, 0x00, 0x00]))) {
    connected = true;
    buffer = Buffer.alloc(0);
    socket.write(subscribePacket(topic));
    return;
  }
  if (connected && !subscribed && buffer[0] === 0x90) {
    subscribed = true;
    buffer = Buffer.alloc(0);
    socket.write(publishPacket(topic, JSON.stringify({ ok: true }), true));
    return;
  }
  if (subscribed && buffer[0] === 0x31) {
    finished = true;
    received = true;
    clearTimeout(timeout);
    console.log(JSON.stringify({
      ok: true,
      cipher: socket.getCipher().name,
      topic,
      received,
    }));
    socket.end(Buffer.from([0xe0, 0x00]));
  }
});

socket.on("error", (error) => {
  if (finished) {
    return;
  }
  finished = true;
  clearTimeout(timeout);
  console.error(error.stack || error.message);
  process.exitCode = 1;
});
