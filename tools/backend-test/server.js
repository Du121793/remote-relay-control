const express = require("express");
const fs = require("fs");
const path = require("path");
const crypto = require("crypto");

const HTTP_PORT = Number(process.env.HTTP_PORT || 4300);
const HTTP_HOST = String(process.env.HTTP_HOST || "127.0.0.1");
const MQTT_CONTROL_PORT = Number(process.env.MQTT_CONTROL_PORT || 4301);
const CLOUD_BROKER_HOST = String(process.env.CLOUD_BROKER_HOST || "mqtt.autocontrol.du1217.club");
const CLOUD_BROKER_PORT = Number(process.env.CLOUD_BROKER_PORT || 8883);
const CLOUD_TRANSPORT = 1; /* MQTT only */
const KNOWN_NODE_SCAN_MS = Number(process.env.KNOWN_NODE_SCAN_MS || 2000);
const CLIENT_STALE_MS = Number(process.env.CLIENT_STALE_MS || 12000);
const ALLOW_PRIVATE_KEY_EXPORT = process.env.ALLOW_PRIVATE_KEY_EXPORT === "1";
const REMOTE_CLUSTER_PASSWORD = process.env.REMOTE_CLUSTER_PASSWORD || "autoControl-cluster";
const REMOTE_SESSION_CACHE_TTL_MS = 14 * 60 * 1000;
const DATA_DIR = path.join(__dirname, "data");
const KEYS_PATH = path.join(DATA_DIR, "keys.json");
const CLOUD_DEVICES_PATH = path.join(DATA_DIR, "cloud-devices.json");
const CLOUD_CLUSTERS_PATH = path.join(DATA_DIR, "cloud-clusters.json");
const LEGACY_BOOTSTRAP_KEY = process.env.BACKEND_KEY || "dev-key";
const DASHBOARD_USER = process.env.DASHBOARD_USER || "admin123";
const DASHBOARD_PASSWORD = process.env.DASHBOARD_PASSWORD || "admin123";
const DASHBOARD_SESSION_TTL_MS = 12 * 60 * 60 * 1000;
const REMOTE_DIRECT_HTTP_ENABLED = process.env.REMOTE_DIRECT_HTTP_ENABLED === "1";
const REMOTE_DIRECT_HTTP_TIMEOUT_MS = Number(process.env.REMOTE_DIRECT_HTTP_TIMEOUT_MS || 1500);
const REMOTE_QUERY_CACHE_TTL_MS = Number(process.env.REMOTE_QUERY_CACHE_TTL_MS || 3000);
const REMOTE_QUERY_CACHE_STALE_MS = Number(process.env.REMOTE_QUERY_CACHE_STALE_MS || 15000);
const REMOTE_QUERY_SCHEDULER_STATUS_TTL_MS = Number(process.env.REMOTE_QUERY_SCHEDULER_STATUS_TTL_MS || 1000);
const REMOTE_QUERY_SCHEDULER_STATUS_STALE_MS = Number(process.env.REMOTE_QUERY_SCHEDULER_STATUS_STALE_MS || 5000);
const LIVE_STATE_FRESH_MS = Number(process.env.LIVE_STATE_FRESH_MS || 5000);
const LIVE_STATE_STALE_MS = Number(process.env.LIVE_STATE_STALE_MS || 15000);

const DEVICE_STATE_TEXT = {
  0: "線上",
  1: "開啟",
  2: "故障",
};

const app = express();
app.use(express.json());
app.use(express.urlencoded({ extended: false }));
app.use((error, _req, res, next) => {
  if (error instanceof SyntaxError && error.status === 400 && "body" in error) {
    pushEvent("http-bad-json", { error: error.message });
    return res.status(400).json({ ok: false, error: "bad_json" });
  }
  return next(error);
});

const events = [];
const knownNodes = new Map();
const cloudDevices = new Map();
const cloudClusters = new Map();
const sseClients = new Set();
const remoteSessionCache = new Map();
const relayConfirmTimers = new Map();
const relayConfirmInflight = new Set();
const relayConfirmDirty = new Set();
const autoProvisionTimers = new Map();
const helloProvisionTimers = new Map();
const dashboardSessions = new Map();
const remoteQueryInflight = new Map();
const remoteQueryCache = new Map();
let keyStore = [];
let stateBroadcastScheduled = false;
let cloudRegistrySaveTimer = 0;

/* ?? MQTT command publishing via broker control API (:4301) ??
   The mqtt-psk-broker.js exposes POST /api/publish so the backend
   does not need its own MQTT client ??just HTTP POST to the broker. */
const BROKER_CONTROL_URL = `http://127.0.0.1:${MQTT_CONTROL_PORT}`;

async function mqttPublishCommand(clusterName, topicSuffix, payload) {
  try {
    const topic = `cluster/${clusterName}/command/${topicSuffix}`;
    const payloadJson = JSON.stringify(payload);
    const response = await fetch(`${BROKER_CONTROL_URL}/api/publish`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ topic, payload: payloadJson }),
    });
    const data = await response.json();
    if (data.ok === true) return true;
    console.error(`[mqtt-publish] control API returned: ${JSON.stringify(data)}`);
  } catch (err) {
    console.error(`[mqtt-publish] control API failed: ${err.message}`);
    console.error(`[mqtt-publish] Ensure Mosquitto HTTP listener is on port ${MQTT_CONTROL_PORT}`);
    console.error(`[mqtt-publish] Or set MQTT_CONTROL_PORT env var to the correct port`);
  }
  return false;
}

async function mqttPublishCommandWithAck(clusterName, topicSuffix, payload, timeoutMs = 8000) {
  const requestId = String(payload?.request_id || makeRequestId());
  const topic = `cluster/${clusterName}/command/${topicSuffix}`;
  const responseTopic = `cluster/${clusterName}/ack/${requestId}`;
  const requestPayload = {
    ...payload,
    request_id: requestId,
  };

  try {
    const response = await fetch(`${BROKER_CONTROL_URL}/api/request-response`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        requestTopic: topic,
        responseTopic,
        payload: requestPayload,
        timeoutMs,
      }),
    });
    const data = await response.json();
    if (!response.ok || data.ok === false) {
      return {
        ok: false,
        requestId,
        delivered: Number(data.delivered || 0),
        error: data.error || `mqtt_ack_http_${response.status}`,
      };
    }
    let ack = null;
    try {
      ack = data.payload ? JSON.parse(data.payload) : null;
    } catch (_error) {
      ack = null;
    }
    return {
      ok: true,
      requestId,
      delivered: Number(data.delivered || 0),
      ack,
    };
  } catch (err) {
    return {
      ok: false,
      requestId,
      error: err.message || "mqtt_ack_failed",
    };
  }
}

function ensureDataDir() {
  if (!fs.existsSync(DATA_DIR)) {
    fs.mkdirSync(DATA_DIR, { recursive: true });
  }
}

function parseCookieHeader(header) {
  const out = {};
  for (const part of String(header || "").split(";")) {
    const idx = part.indexOf("=");
    if (idx <= 0) continue;
    const key = part.slice(0, idx).trim();
    const value = part.slice(idx + 1).trim();
    if (key) out[key] = decodeURIComponent(value);
  }
  return out;
}

function createDashboardSession() {
  const token = crypto.randomBytes(24).toString("base64url");
  dashboardSessions.set(token, {
    user: DASHBOARD_USER,
    expiresAt: Date.now() + DASHBOARD_SESSION_TTL_MS,
  });
  return token;
}

function getDashboardSession(req) {
  const cookies = parseCookieHeader(req.headers.cookie || "");
  const token = cookies.ac_backend_session || String(req.headers["x-backend-token"] || "");
  const session = dashboardSessions.get(token);
  if (!session || session.expiresAt < Date.now()) {
    if (token) dashboardSessions.delete(token);
    return null;
  }
  session.expiresAt = Date.now() + DASHBOARD_SESSION_TTL_MS;
  return { token, ...session };
}

function isPublicBackendPath(req) {
  if (req.path === "/api/auth/login" ||
      req.path === "/api/auth/logout" ||
      req.path === "/api/auth/session" ||
      req.path === "/favicon.ico") {
    return true;
  }
  if (req.path === "/api/provision") {
    return true;
  }
  return false;
}

function makeRequestId() {
  return crypto.randomBytes(8).toString("hex");
}

function nodeClusterName(nodeInfo) {
  return String(nodeInfo?.cluster || nodeInfo?.clusterName || nodeInfo?.cloudClusterId || "default").trim() || "default";
}

function publishNodeCommand(nodeId, topicSuffix, body = {}) {
  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) {
    return { ok: false, status: 404, error: "node_not_found" };
  }
  const targetNodeId = Number(body.targetNodeId || body.node_id || nodeId);
  const payload = {
    ...body,
    node_id: targetNodeId,
    request_id: String(body.request_id || makeRequestId()),
  };
  delete payload.targetNodeId;
  return {
    ok: true,
    cluster: nodeClusterName(nodeInfo),
    nodeInfo,
    targetNodeId,
    requestId: payload.request_id,
    payload,
    publish: async (options = {}) => {
      const waitForAck = Boolean(options.waitForAck);
      if (!waitForAck) {
        const ok = await mqttPublishCommand(nodeClusterName(nodeInfo), topicSuffix, payload);
        if (ok) invalidateRemoteQueryCache(targetNodeId);
        return { ok, queued: ok, completed: false, requestId: payload.request_id, ack: null };
      }
      const result = await mqttPublishCommandWithAck(nodeClusterName(nodeInfo), topicSuffix, payload, Number(options.timeoutMs || 8000));
      if (result.ok) invalidateRemoteQueryCache(targetNodeId);
      return result;
    },
  };
}

function relayConfigSkeleton() {
  return Array.from({ length: 16 }, (_, index) => ({
    channel: index + 1,
    name: `CH${index + 1}`,
    enabled: 1,
    remember_state: 0,
    boot_default_on: 0,
    adc_channel: index,
    protection_enabled: 0,
    device_strategy: 0,
    overcurrent_milliamps: 4500,
    trip_delay_ms: 1000,
    reset_hysteresis_milliamps: 500,
    source: "backend-fallback",
  }));
}

function protectionSkeleton() {
  return {
    ok: true,
    fallback: true,
    channels: Array.from({ length: 16 }, (_, index) => ({
      ch: index + 1,
      channel: index + 1,
      fault_level: 0,
      fault_reason: 0,
      protection_enabled: 0,
      ct_valid: 0,
      current_ma: 0,
      device_strategy: 0,
      learning_complete: 0,
    })),
  };
}

function protectionFromLiveState(node) {
  const enabledMask = Number(node?.protectionEnabledMask || 0) >>> 0;
  const trippedMask = Number(node?.protectionTrippedMask || 0) >>> 0;
  const faultMask = Number(node?.protectionFaultLockMask || 0) >>> 0;
  const ctMask = Number(node?.protectionCtValidMask || 0) >>> 0;
  const learningMask = Number(node?.protectionLearningMask || 0) >>> 0;
  const strategyLsbMask = Number(node?.protectionStrategyLsbMask || 0) >>> 0;
  const strategyMsbMask = Number(node?.protectionStrategyMsbMask || 0) >>> 0;
  const meta = liveStateMeta(node);
  return {
    ok: true,
    fallback: true,
    state_cache: true,
    stale: meta.stateFresh ? false : true,
    cache_age_ms: meta.cacheAgeMs,
    fault_lock_mask: faultMask,
    channels: Array.from({ length: 16 }, (_, index) => {
      const bit = 1 << index;
      const enabled = (enabledMask & bit) ? 1 : 0;
      const tripped = (trippedMask & bit) ? 1 : 0;
      const locked = ((faultMask >>> index) & 1) ? 1 : 0;
      const ctValid = (ctMask & bit) ? 1 : 0;
      const learningComplete = (learningMask & bit) ? 1 : 0;
      const strategy = ((strategyLsbMask & bit) ? 1 : 0) | ((strategyMsbMask & bit) ? 2 : 0);
      return {
        ch: index + 1,
        channel: index + 1,
        fault_level: locked ? 3 : (tripped ? 2 : 0),
        fault_reason: locked ? 8 : (tripped ? 1 : 0),
        protection_enabled: enabled,
        tripped,
        ct_valid: ctValid,
        current_ma: 0,
        device_strategy: strategy,
        learning_complete: learningComplete,
        fault_locked: locked,
      };
    }),
  };
}

function schedulerStatusFromLiveState(node, channel) {
  const ch = Number(channel);
  const bit = ch >= 1 && ch <= 16 ? (1 << (ch - 1)) : 0;
  const meta = liveStateMeta(node);
  return {
    ok: true,
    fallback: true,
    state_cache: true,
    stale: meta.stateFresh ? false : true,
    cache_age_ms: meta.cacheAgeMs,
    channel: ch,
    schedule_enabled: (Number(node?.schedulerEnabledMask || 0) & bit) ? 1 : 0,
    pause_active: (Number(node?.schedulerPauseMask || 0) & bit) ? 1 : 0,
    schedule_pause_until: 0,
    pause_blocks_auto_trigger: (Number(node?.schedulerPauseBlocksMask || 0) & bit) ? 1 : 0,
    auto_hold_active: 0,
    time_valid: 0,
  };
}

function protectionConfigDefaults() {
  return {
    ok: true,
    fallback: true,
    cooldown_seconds: 3,
    retry_delay_seconds: 30,
    max_retry_count: 1,
    boot_delay_seconds: 30,
    learning_samples: 5,
    aging_update_hours: 24,
    aging_cap_percent: 50,
    relay_on_spacing_ms: 500,
    same_relay_on_spacing_ms: 3000,
    stall_multiplier_x10: 20,
  };
}

function requireDashboardSession(req, res, next) {
  if (isPublicBackendPath(req)) return next();
  if (req.path === "/" || req.path.startsWith("/api/") || req.path.startsWith("/send/")) {
    const session = getDashboardSession(req);
    if (!session) {
      if (req.path === "/") return next();
      return res.status(401).json({ ok: false, error: "unauthorized" });
    }
    req.dashboardUser = session.user;
  }
  return next();
}

function saveKeyStore() {
  ensureDataDir();
  fs.writeFileSync(KEYS_PATH, JSON.stringify({ keys: keyStore }, null, 2));
}

function saveCloudRegistry() {
  ensureDataDir();
  fs.writeFileSync(CLOUD_DEVICES_PATH, JSON.stringify({
    devices: Array.from(cloudDevices.values()),
  }, null, 2));
  fs.writeFileSync(CLOUD_CLUSTERS_PATH, JSON.stringify({
    clusters: Array.from(cloudClusters.values()),
  }, null, 2));
}

function scheduleCloudRegistrySave() {
  if (cloudRegistrySaveTimer) {
    return;
  }
  cloudRegistrySaveTimer = setTimeout(() => {
    cloudRegistrySaveTimer = 0;
    saveCloudRegistry();
  }, 200);
}

function createKeyRecord(label, keyText) {
  return {
    id: crypto.randomUUID(),
    label: (label || "未命名金鑰").trim() || "未命名金鑰",
    key: keyText,
    enabled: true,
    createdAt: new Date().toISOString(),
    lastSeenAt: null,
    lastNodeId: 0,
    lastCluster: "",
    lastDevice: "",
  };
}

function loadKeyStore() {
  ensureDataDir();
  if (fs.existsSync(KEYS_PATH)) {
    try {
      const parsed = JSON.parse(fs.readFileSync(KEYS_PATH, "utf8"));
      keyStore = Array.isArray(parsed.keys) ? parsed.keys : [];
    } catch (_error) {
      keyStore = [];
    }
  }

  if (keyStore.length === 0) {
    keyStore.push(createKeyRecord("bootstrap", LEGACY_BOOTSTRAP_KEY));
    saveKeyStore();
    return;
  }

  if (!keyStore.some((item) => String(item.key || "").trim() === LEGACY_BOOTSTRAP_KEY)) {
    keyStore.unshift(createKeyRecord("bootstrap", LEGACY_BOOTSTRAP_KEY));
    saveKeyStore();
  }
}

function generateKeyText() {
  return crypto.randomBytes(24).toString("base64url");
}

function generateClusterIdentityId() {
  return `cluster-${crypto.randomBytes(6).toString("hex")}`;
}

function generateDeviceIdentityId() {
  return `device-${crypto.randomBytes(6).toString("hex")}`;
}

function generateCloudKeyId() {
  return `key-${crypto.randomBytes(4).toString("hex")}`;
}

function generatePemKeyPair() {
  const { publicKey, privateKey } = crypto.generateKeyPairSync("ed25519");
  return {
    publicPem: publicKey.export({ type: "spki", format: "pem" }).toString(),
    privatePem: privateKey.export({ type: "pkcs8", format: "pem" }).toString(),
  };
}

function fingerprintPem(pem) {
  return crypto.createHash("sha256").update(String(pem || ""), "utf8").digest("hex");
}

function signJson(privatePem, payload) {
  const body = Buffer.from(JSON.stringify(payload), "utf8");
  return crypto.sign(null, body, privatePem).toString("base64");
}

function buildClusterCaBundle(clusterId, keyId) {
  const pair = generatePemKeyPair();
  return {
    caPublicPem: pair.publicPem,
    caPrivatePem: pair.privatePem,
    caFingerprint: fingerprintPem(pair.publicPem),
    bindingSecret: crypto.randomBytes(32).toString("base64url"),
    issuedForClusterId: clusterId,
    issuedByKeyId: keyId,
    issuedAt: nowIso(),
  };
}

function buildDeviceCredentialBundle(clusterRecord, deviceId, deviceName, keyId) {
  const pair = generatePemKeyPair();
  const certificatePayload = {
    version: 1,
    clusterId: clusterRecord.clusterId,
    clusterCaFingerprint: clusterRecord.caFingerprint || "",
    deviceId,
    deviceName,
    keyId,
    publicKeyPem: pair.publicPem,
    issuedAt: nowIso(),
    issuerKeyId: clusterRecord.keyId,
  };
  return {
    devicePublicPem: pair.publicPem,
    devicePrivatePem: pair.privatePem,
    deviceFingerprint: fingerprintPem(pair.publicPem),
    certificatePayload,
    certificateSignature: signJson(clusterRecord.caPrivatePem, certificatePayload),
  };
}

function rotateDeviceCredential(clusterRecord, record) {
  if (!clusterRecord || !record) {
    return null;
  }
  const bundle = buildDeviceCredentialBundle(
    clusterRecord,
    record.deviceId,
    record.device || record.deviceId,
    record.keyId || clusterRecord.keyId || generateCloudKeyId()
  );
  record.devicePublicPem = bundle.devicePublicPem;
  record.devicePrivatePem = bundle.devicePrivatePem;
  record.deviceFingerprint = bundle.deviceFingerprint;
  record.certificatePayload = bundle.certificatePayload;
  record.certificateSignature = bundle.certificateSignature;
  record.lastUpdatedAt = nowIso();
  return record;
}

function rotateClusterCa(clusterRecord) {
  if (!clusterRecord) {
    return null;
  }
  const bundle = buildClusterCaBundle(clusterRecord.clusterId, clusterRecord.keyId || generateCloudKeyId());
  clusterRecord.caPublicPem = bundle.caPublicPem;
  clusterRecord.caPrivatePem = bundle.caPrivatePem;
  clusterRecord.caFingerprint = bundle.caFingerprint;
  clusterRecord.bindingSecret = bundle.bindingSecret;
  clusterRecord.lastUpdatedAt = nowIso();
  for (const deviceRecord of cloudDevices.values()) {
    if (deviceRecord.clusterId === clusterRecord.clusterId) {
      rotateDeviceCredential(clusterRecord, deviceRecord);
      cloudDevices.set(deviceRecord.registryKey, deviceRecord);
    }
  }
  return clusterRecord;
}

function ensureClusterIdentityMaterial(record) {
  if (!record) {
    return null;
  }
  if (record.caPublicPem && record.caPrivatePem && record.caFingerprint && record.bindingSecret) {
    return record;
  }
  const bundle = buildClusterCaBundle(record.clusterId, record.keyId || generateCloudKeyId());
  record.caPublicPem = bundle.caPublicPem;
  record.caPrivatePem = bundle.caPrivatePem;
  record.caFingerprint = bundle.caFingerprint;
  record.bindingSecret = bundle.bindingSecret;
  record.issuedAt = record.issuedAt || bundle.issuedAt;
  saveCloudRegistry();
  return record;
}

function ensureDeviceIdentityMaterial(clusterRecord, record) {
  if (!clusterRecord || !record) {
    return null;
  }
  if (record.devicePublicPem && record.devicePrivatePem && record.deviceFingerprint && record.certificatePayload && record.certificateSignature) {
    return record;
  }
  const bundle = buildDeviceCredentialBundle(
    clusterRecord,
    record.deviceId,
    record.device || record.deviceId,
    record.keyId || clusterRecord.keyId || generateCloudKeyId()
  );
  record.devicePublicPem = bundle.devicePublicPem;
  record.devicePrivatePem = bundle.devicePrivatePem;
  record.deviceFingerprint = bundle.deviceFingerprint;
  record.certificatePayload = bundle.certificatePayload;
  record.certificateSignature = bundle.certificateSignature;
  return record;
}

function sanitizeIdentitySegment(text, fallback) {
  const normalized = String(text || "")
    .trim()
    .replace(/[^A-Za-z0-9._-]+/g, "-")
    .replace(/-+/g, "-")
    .replace(/^-|-$/g, "");
  return normalized || fallback;
}

function findKeyRecordBySecret(secret) {
  return keyStore.find((item) => item.key === secret) || null;
}

function findKeyRecordById(id) {
  return keyStore.find((item) => item.id === id) || null;
}

function sanitizeKeyRecord(record) {
  return {
    id: record.id,
    label: record.label,
    key: record.key,
    enabled: record.enabled,
    createdAt: record.createdAt,
    lastSeenAt: record.lastSeenAt,
    lastNodeId: record.lastNodeId,
    lastCluster: record.lastCluster,
    lastDevice: record.lastDevice,
  };
}

function pushEvent(type, payload) {
  const deviceMessage = payload && payload.message && typeof payload.message === "object"
    ? payload.message
    : payload;
  const entry = {
    ts: new Date().toISOString(),
    type,
    payload,
    uptime_ms: Number(deviceMessage?.uptime_ms || 0),
    unix_time: Number(deviceMessage?.unix_time || 0),
  };
  events.push(entry);
  while (events.length > 300) {
    events.shift();
  }
  scheduleStateBroadcast();
  return entry;
}

function deviceStateText(value) {
  return DEVICE_STATE_TEXT[value] || "未知";
}

function normalizeIp(ipText) {
  const text = String(ipText || "").trim();
  if (text.startsWith("::ffff:")) {
    return text.slice(7);
  }
  return text;
}

function cloudClusterKeyOf(record) {
  return String(record?.cloudClusterId || "").trim() || String(record?.cluster || "").trim();
}

function cloudDeviceKeyOf(record) {
  const clusterId = cloudClusterKeyOf(record);
  const deviceId = String(record?.cloudDeviceId || "").trim();
  if (!clusterId || !deviceId) {
    return "";
  }
  return `${clusterId}|${deviceId}`;
}

function findCloudDeviceRecord(clusterId, deviceId) {
  return cloudDevices.get(`${String(clusterId || "").trim()}|${String(deviceId || "").trim()}`) || null;
}

function findCloudClusterRecord(clusterId) {
  return cloudClusters.get(String(clusterId || "").trim()) || null;
}

function buildClusterProvisioning(record) {
  return {
    cloud_enabled: 1,
    cloud_transport: Number(record.transport || CLOUD_TRANSPORT),
    cloud_broker_host: String(record.brokerHost || CLOUD_BROKER_HOST),
    cloud_broker_port: Number(record.brokerPort || CLOUD_BROKER_PORT),
    cloud_cluster_id: record.clusterId,
    cloud_key_id: record.keyId,
    cloud_cluster_ca_fingerprint: record.caFingerprint || "",
    cloud_cluster_binding_secret: record.bindingSecret || "",
  };
}

function buildProvisioningFormFields(provisioning) {
  return Object.entries(provisioning)
    .map(([key, value]) => `${key}=${value}`)
    .join("\n");
}

function buildDeviceProvisioning(clusterRecord, deviceRecord) {
  return {
    ...buildClusterProvisioning(clusterRecord),
    cloud_device_id: deviceRecord.deviceId,
    cloud_key_id: deviceRecord.keyId,
    suggested_device_name: deviceRecord.device || deviceRecord.deviceId,
    cluster_name_hint: clusterRecord.clusterName || clusterRecord.clusterId,
    cloud_device_fingerprint: deviceRecord.deviceFingerprint || "",
  };
}

function buildDeviceProvisioningBundle(clusterRecord, deviceRecord) {
  return {
    cluster: cloudClusterSummary(clusterRecord),
    device: cloudDeviceSummary(deviceRecord),
    provisioning: buildDeviceProvisioning(clusterRecord, deviceRecord),
    trustBundle: {
      clusterCaPublicPem: clusterRecord.caPublicPem || "",
      clusterCaFingerprint: clusterRecord.caFingerprint || "",
      clusterBindingSecret: clusterRecord.bindingSecret || "",
    },
    deviceBundle: {
      devicePublicPem: deviceRecord.devicePublicPem || "",
      devicePrivatePem: ALLOW_PRIVATE_KEY_EXPORT ? (deviceRecord.devicePrivatePem || "") : "",
      deviceFingerprint: deviceRecord.deviceFingerprint || "",
      certificatePayload: deviceRecord.certificatePayload || null,
      certificateSignature: deviceRecord.certificateSignature || "",
    },
    privateKeyExportEnabled: ALLOW_PRIVATE_KEY_EXPORT,
  };
}

function buildProvisioningApplyPayload(clusterRecord, deviceRecord) {
  const provisioning = buildDeviceProvisioning(clusterRecord, deviceRecord);
  const fields = [
    ["cloud_enabled", provisioning.cloud_enabled],
    ["cloud_transport", provisioning.cloud_transport],
    ["cloud_broker_host", provisioning.cloud_broker_host],
    ["cloud_broker_port", provisioning.cloud_broker_port],
    ["cloud_cluster_id", provisioning.cloud_cluster_id],
    ["cloud_device_id", provisioning.cloud_device_id],
    ["cloud_key_id", provisioning.cloud_key_id],
    ["cloud_cluster_ca_fingerprint", provisioning.cloud_cluster_ca_fingerprint || ""],
    ["cloud_cluster_binding_secret", provisioning.cloud_cluster_binding_secret || ""],
  ];
  if (provisioning.suggested_device_name) {
    fields.push(["device_name", provisioning.suggested_device_name]);
  }
  return fields
    .map(([key, value]) => `${encodeURIComponent(String(key))}=${encodeURIComponent(String(value))}`)
    .join("&");
}

function buildProvisioningApplyMessage(clusterRecord, deviceRecord) {
  const provisioning = buildDeviceProvisioning(clusterRecord, deviceRecord);
  return {
    type: "config_cloud",
    cloud_enabled: Number(provisioning.cloud_enabled || 0),
    cloud_transport: Number(provisioning.cloud_transport || 0),
    cloud_broker_host: String(provisioning.cloud_broker_host || ""),
    cloud_broker_port: Number(provisioning.cloud_broker_port || 0),
    cloud_cluster_id: String(provisioning.cloud_cluster_id || ""),
    cloud_device_id: String(provisioning.cloud_device_id || ""),
    cloud_key_id: String(provisioning.cloud_key_id || ""),
    cloud_cluster_ca_fingerprint: String(provisioning.cloud_cluster_ca_fingerprint || ""),
    cloud_cluster_binding_secret: String(provisioning.cloud_cluster_binding_secret || ""),
    device_name: String(provisioning.suggested_device_name || ""),
  };
}

function needsProvisioningForClient(client, clusterRecord, deviceRecord) {
  if (!client || !clusterRecord || !deviceRecord) {
    return false;
  }
  if (String(client.cloudClusterId || "").trim() !== String(clusterRecord.clusterId || "").trim()) {
    return true;
  }
  if (String(client.cloudDeviceId || "").trim() !== String(deviceRecord.deviceId || "").trim()) {
    return true;
  }
  if (String(client.cloudKeyId || "").trim() !== String(deviceRecord.keyId || "").trim()) {
    return true;
  }
  if (!String(client.cloudClusterBindingSecret || "").trim()) {
    return true;
  }
  if (!String(client.cloudClusterCaFingerprint || "").trim()) {
    return true;
  }
  if (!isFingerprintCompatible(clusterRecord.caFingerprint, client.cloudClusterCaFingerprint)) {
    return true;
  }
  return false;
}

function resolveProvisioningRecords(client) {
  const clusterId = cloudClusterKeyOf(client);
  const clusterRecord = findCloudClusterRecord(clusterId);
  const deviceId = String(client?.cloudDeviceId || "").trim() ||
    deriveFallbackDeviceId(client);
  let deviceRecord;

  if (!clusterRecord || !deviceId) {
    return { clusterRecord: null, deviceRecord: null, deviceId: "" };
  }

  deviceRecord = findCloudDeviceRecord(clusterId, deviceId);
  if (!deviceRecord) {
    syncCloudRegistryFromKnownNode({
      nodeId: Number(client?.nodeId || 0),
      device: client?.device || "",
      cluster: client?.cluster || "",
      ip: client?.ip || "",
      role: Number(client?.role || 0),
      online: 1,
      deviceState: Number(client?.deviceState || 0),
      relayMask: Number(client?.relayMask || 0) >>> 0,
      sourceMasterNodeId: Number(client?.nodeId || 0),
      cloudClusterId: clusterId,
      cloudDeviceId: deviceId,
      cloudKeyId: String(client?.cloudKeyId || "").trim(),
      cloudTransport: Number(client?.cloudTransport || CLOUD_TRANSPORT),
      backendKey: client?.backendKey || "",
      lastSeenAt: nowIso(),
    });
    deviceRecord = findCloudDeviceRecord(clusterId, deviceId);
  }

  return { clusterRecord, deviceRecord, deviceId };
}

function scheduleProvisioningForClient(client, delayMs = 6000) {
  void client;
  void delayMs;
}

function isFingerprintCompatible(expected, reported) {
  const expectedText = String(expected || "").trim();
  const reportedText = String(reported || "").trim();
  if (!expectedText || !reportedText) {
    return true;
  }
  if (expectedText === reportedText) {
    return true;
  }
  if (reportedText.length >= 16 && expectedText.startsWith(reportedText)) {
    return true;
  }
  return false;
}

async function applyProvisioningToNode(node, clusterRecord, deviceRecord) {
  const nodeId = Number(node.nodeId || 0);
  let data;

  data = await remoteNodeRequest(
    nodeId,
    "/api/config/cluster",
    "POST",
    new URLSearchParams(buildProvisioningApplyPayload(clusterRecord, deviceRecord))
  );
  if (!data || data.ok === false) {
    throw new Error(data?.error || "node_http_apply_failed");
  }
  return { ok: true, via: "http" };
}

function nowIso() {
  return new Date().toISOString();
}

function deriveFallbackDeviceId(source) {
  const nodeId = Number(source?.nodeId || 0);
  if (nodeId > 0) {
    return `node-${nodeId}`;
  }
  return sanitizeIdentitySegment(source?.device || "", "node-unknown");
}

function localClusterKey(backendKey, clusterName) {
  return `${String(backendKey || "").trim()}|${String(clusterName || "").trim()}`;
}

function remoteQueryCachePolicy(pathText) {
  const pathValue = String(pathText || "");
  if (pathValue.includes("/scheduler/status")) {
    return {
      ttlMs: REMOTE_QUERY_SCHEDULER_STATUS_TTL_MS,
      staleMs: REMOTE_QUERY_SCHEDULER_STATUS_STALE_MS,
    };
  }
  return {
    ttlMs: REMOTE_QUERY_CACHE_TTL_MS,
    staleMs: REMOTE_QUERY_CACHE_STALE_MS,
  };
}

function remoteQueryCacheKey(nodeId, pathText, method) {
  return `${Number(nodeId) || 0}|${String(method || "GET").toUpperCase()}|${String(pathText || "")}`;
}

function cloneJson(value) {
  if (value === undefined) return undefined;
  return JSON.parse(JSON.stringify(value));
}

function annotateRemoteQueryCache(data, cacheEntry, stale) {
  const out = cloneJson(data) || {};
  out.cached = true;
  out.stale = stale ? true : false;
  out.cache_age_ms = Math.max(0, Date.now() - Number(cacheEntry?.storedAt || Date.now()));
  out.cached_at = cacheEntry?.storedAtIso || "";
  return out;
}

function invalidateRemoteQueryCache(nodeId) {
  const prefix = `${Number(nodeId) || 0}|`;
  for (const key of Array.from(remoteQueryCache.keys())) {
    if (key.startsWith(prefix)) remoteQueryCache.delete(key);
  }
}

function cacheRemoteQueryResult(cacheKey, data) {
  if (!data || typeof data !== "object" || data.error) return;
  remoteQueryCache.set(cacheKey, {
    storedAt: Date.now(),
    storedAtIso: nowIso(),
    data: cloneJson(data),
  });
}

async function mqttQueryNode(nodeId, pathText, options = {}) {
  const nodeInfo = resolveKnownNode(nodeId);
  const clusterName = nodeClusterName(nodeInfo);
  const requestId = String(options.requestId || makeRequestId());
  const requestTopic = `cluster/${clusterName}/query/request`;
  const responseTopic = `cluster/${clusterName}/query/response/${requestId}`;
  const method = String(options.method || "GET").toUpperCase();
  const cacheable = method === "GET" && options.noCache !== true;
  const cacheKey = remoteQueryCacheKey(nodeId, pathText, method);
  const cachePolicy = remoteQueryCachePolicy(pathText);
  const cached = remoteQueryCache.get(cacheKey);
  const cachedAgeMs = cached ? Date.now() - Number(cached.storedAt || 0) : Number.POSITIVE_INFINITY;
  let response;
  let data;

  if (!nodeInfo || !nodeInfo.cluster) {
    throw new Error("node_not_found");
  }

  if (cacheable && cached && cachedAgeMs <= cachePolicy.ttlMs) {
    return annotateRemoteQueryCache(cached.data, cached, false);
  }

  if (cacheable && remoteQueryInflight.has(cacheKey)) {
    try {
      return cloneJson(await remoteQueryInflight.get(cacheKey));
    } catch (error) {
      if (cached && cachedAgeMs <= cachePolicy.staleMs) {
        return annotateRemoteQueryCache(cached.data, cached, true);
      }
      throw error;
    }
  }

  const doQuery = async () => {
    let parsed;

    response = await fetch(`${BROKER_CONTROL_URL}/api/request-response`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        requestTopic,
        responseTopic,
        timeoutMs: Number(options.timeoutMs || 4000),
        payload: {
          request_id: requestId,
          node_id: Number(nodeId),
          path: String(pathText || ""),
          method,
        },
      }),
    });
    data = await response.json();
    if (!response.ok || !data.ok) {
      throw new Error(data.error || "broker_query_failed");
    }
    try {
      parsed = expandCompactQueryResponse(JSON.parse(String(data.payload || "")));
    } catch (_error) {
      throw new Error("bad_broker_query_response");
    }
    if (parsed && parsed.error) {
      throw new Error(String(parsed.error));
    }
    if (cacheable) cacheRemoteQueryResult(cacheKey, parsed);
    return parsed;
  };

  if (!cacheable) return doQuery();

  const queryPromise = doQuery();
  remoteQueryInflight.set(cacheKey, queryPromise);
  try {
    return cloneJson(await queryPromise);
  } catch (error) {
    if (cached && cachedAgeMs <= cachePolicy.staleMs) {
      return annotateRemoteQueryCache(cached.data, cached, true);
    }
    throw error;
  } finally {
    if (remoteQueryInflight.get(cacheKey) === queryPromise) {
      remoteQueryInflight.delete(cacheKey);
    }
  }
}

function expandCompactQueryResponse(data) {
  if (!data || typeof data !== "object") return data;
  if (data.compact === "relays-v1" && Array.isArray(data.relays)) {
    return {
      ...data,
      relays: data.relays.map((row) => ({
        channel: Number(row[0] || 0),
        name: String(row[1] || ""),
        enabled: Number(row[2] || 0),
        remember_state: Number(row[3] || 0),
        boot_default_on: Number(row[4] || 0),
        adc_channel: Number(row[5] || 0),
        protection_enabled: Number(row[6] || 0),
        device_strategy: Number(row[7] || 0),
        overcurrent_milliamps: Number(row[8] || 0),
        trip_delay_ms: Number(row[9] || 0),
        reset_hysteresis_milliamps: Number(row[10] || 0),
      })),
    };
  }
  if (data.compact === "protection-v1" && Array.isArray(data.channels)) {
    return {
      ...data,
      channels: data.channels.map((row) => ({
        ch: Number(row[0] || 0),
        channel: Number(row[0] || 0),
        phase: Number(row[1] || 0),
        fault_level: Number(row[2] || 0),
        fault_reason: Number(row[3] || 0),
        learning_complete: Number(row[4] || 0),
        protection_enabled: Number(row[5] || 0),
        tripped: Number(row[6] || 0),
        fault_locked: Number(row[7] || 0),
        current_ma: Number(row[8] || 0),
        current_milliamps: Number(row[8] || 0),
        reference_ma: Number(row[9] || 0),
        baseline_ma: Number(row[10] || 0),
        aging_offset_ma: Number(row[11] || 0),
        normal_low_ma: Number(row[12] || 0),
        normal_high_ma: Number(row[13] || 0),
        startup_peak_ma: Number(row[14] || 0),
        startup_duration_ms: Number(row[15] || 0),
        trip_count: Number(row[16] || 0),
        cooldown_until_ms: Number(row[17] || 0),
        learning_samples: Number(row[18] || 0),
        ct_valid: Number(row[19] || 0),
        ct_degraded: Number(row[20] || 0),
        adc_saturated: Number(row[21] || 0),
        device_strategy: Number(row[22] || 0),
        off_since_active: Number(row[23] || 0),
        stuck_pending: Number(row[24] || 0),
      })),
    };
  }
  return data;
}

function resolveKnownNode(nodeId) {
  const numId = Number(nodeId);
  /* Direct nodeId match */
  let found = Array.from(knownNodes.values()).find((item) => Number(item.nodeId) === numId);
  if (found && numId > 0) return found;
  /* Fallback: match by deviceId substring (e.g., node-XXX-54087705) */
  if (!found) {
    found = Array.from(knownNodes.values()).find((item) =>
      String(item.cloudDeviceId || "").includes(String(nodeId))
    );
  }
  /* Fallback: match by cloudDeviceId exact */
  if (!found) {
    found = Array.from(knownNodes.values()).find((item) =>
      String(item.cloudDeviceId) === String(nodeId)
    );
  }
  return found || null;
}

function effectiveMasterNodeIdForGroup(backendKey, clusterName) {
  const clusterKey = localClusterKey(backendKey, clusterName);
  const knownMasters = Array.from(knownNodes.values())
    .filter((item) =>
      localClusterKey(item.backendKey, item.cluster) === clusterKey &&
      Number(item.online ? 1 : 0) === 1 &&
      Number(item.role || 0) === 2)
    .sort((a, b) => {
      const termDiff = Number(b.term || 0) - Number(a.term || 0);
      if (termDiff !== 0) {
        return termDiff;
      }
      return Number(a.nodeId || 0) - Number(b.nodeId || 0);
    });
  if (knownMasters.length > 0) {
    return Number(knownMasters[0].nodeId || 0);
  }

  return 0;
}

async function remoteNodeRequest(nodeId, pathText, method, form) {
  const node = resolveKnownNode(nodeId);
  let token;
  let response;
  let data;
  let controller;
  let timeout;
  /* Backward compat: old calls pass (nodeId, path, form) with 3 args */
  if (typeof method !== 'string' || (method !== 'GET' && method !== 'POST' && method !== 'PUT' && method !== 'PATCH' && method !== 'DELETE')) {
    form = method;
    method = 'POST';
  }
  if (!form) form = "";

  if (!REMOTE_DIRECT_HTTP_ENABLED) {
    throw new Error("direct_http_disabled");
  }

  if (!node || !node.ip) {
    throw new Error("node_offline");
  }

  token = await remoteNodeLogin(node);
  controller = new AbortController();
  timeout = setTimeout(() => controller.abort(), REMOTE_DIRECT_HTTP_TIMEOUT_MS);
  try {
    response = await fetch(`http://${node.ip}${pathText}`, {
      method: method,
      headers: {
        "Content-Type": "application/x-www-form-urlencoded",
        "X-Auth-Token": String(token),
      },
      body: form.toString(),
      signal: controller.signal,
    });
  } finally {
    clearTimeout(timeout);
  }

  data = await readRemoteJson(response);
  if (response.ok && data && data.ok !== false) {
    return data;
  }
  if (data && data.error === "unauthorized") {
    remoteSessionCache.delete(String(Number(node.nodeId || 0)));
    token = await remoteNodeLogin(node);
    controller = new AbortController();
    timeout = setTimeout(() => controller.abort(), REMOTE_DIRECT_HTTP_TIMEOUT_MS);
    try {
      response = await fetch(`http://${node.ip}${pathText}`, {
        method: method,
        headers: {
          "Content-Type": "application/x-www-form-urlencoded",
          "X-Auth-Token": String(token),
        },
        body: form.toString(),
        signal: controller.signal,
      });
    } finally {
      clearTimeout(timeout);
    }
    return readRemoteJson(response);
  }
  return data;
}

function loadCloudRegistry() {
  ensureDataDir();
  cloudDevices.clear();
  cloudClusters.clear();
  let dirty = false;

  if (fs.existsSync(CLOUD_CLUSTERS_PATH)) {
    try {
      const parsed = JSON.parse(fs.readFileSync(CLOUD_CLUSTERS_PATH, "utf8"));
      for (const record of Array.isArray(parsed.clusters) ? parsed.clusters : []) {
        if (record && record.clusterId) {
          ensureClusterIdentityMaterial(record);
          cloudClusters.set(record.clusterId, record);
          dirty = true;
        }
      }
    } catch (_error) {
    }
  }

  if (fs.existsSync(CLOUD_DEVICES_PATH)) {
    try {
      const parsed = JSON.parse(fs.readFileSync(CLOUD_DEVICES_PATH, "utf8"));
      for (const record of Array.isArray(parsed.devices) ? parsed.devices : []) {
        if (record && record.registryKey) {
          const clusterRecord = findCloudClusterRecord(record.clusterId);
          if (clusterRecord) {
            ensureDeviceIdentityMaterial(clusterRecord, record);
            dirty = true;
          }
          cloudDevices.set(record.registryKey, record);
        }
      }
    } catch (_error) {
    }
  }

  if (dirty) {
    saveCloudRegistry();
  }

  for (const [registryKey, record] of Array.from(cloudDevices.entries())) {
    if (!record) {
      continue;
    }
    if (String(record.clusterId || "").trim() === "default" &&
        String(record.deviceId || "").trim() === "autoControl" &&
        String(record.device || "").trim() === "autoControl" &&
        !String(record.cloudDeviceId || "").trim()) {
      cloudDevices.delete(registryKey);
    }
  }
}

async function readRemoteJson(response) {
  const text = await response.text();

  if (!text.trim()) {
    throw new Error(`remote_empty_response_${response.status}`);
  }
  try {
    return JSON.parse(text);
  } catch (_error) {
    throw new Error(`remote_invalid_json_${response.status}`);
  }
}

function knownNodeKey(key, cluster, nodeId) {
  return `${key}|${cluster}|${nodeId}`;
}

function upsertKnownNode(record) {
  const key = knownNodeKey(record.backendKey, record.cluster, record.nodeId);
  const existing = knownNodes.get(key) || {};
  const existingSeenMs = Date.parse(existing.lastSeenAt || "");
  const recordSeenMs = Date.parse(record.lastSeenAt || "");

  if (Number.isFinite(existingSeenMs) &&
      Number.isFinite(recordSeenMs) &&
      recordSeenMs < existingSeenMs) {
    return existing;
  }

  const merged = {
      ...existing,
      ...record,
      lastUpdatedAt: nowIso(),
    };
  knownNodes.set(key, merged);
  syncCloudRegistryFromKnownNode(merged);
  return merged;
}

function setKnownNodeRelayMask(nodeId, relayMask) {
  const node = resolveKnownNode(nodeId);
  if (!node) {
    return;
  }

  upsertKnownNode({
    ...node,
    relayMask: Number(relayMask) >>> 0,
    online: 1,
    deviceState: Number(node.deviceState || 0),
  });
  scheduleStateBroadcast();
}

function liveStateMeta(node) {
  const lastSeenMs = Date.parse(node?.lastSeenAt || node?.lastUpdatedAt || 0);
  const ageMs = Number.isFinite(lastSeenMs) ? Math.max(0, Date.now() - lastSeenMs) : Number.POSITIVE_INFINITY;
  return {
    cacheAgeMs: Number.isFinite(ageMs) ? ageMs : 0,
    stateFresh: Number.isFinite(ageMs) && ageMs <= LIVE_STATE_FRESH_MS ? 1 : 0,
    stateStale: Number.isFinite(ageMs) && ageMs > LIVE_STATE_FRESH_MS && ageMs <= LIVE_STATE_STALE_MS ? 1 : 0,
    stateExpired: !Number.isFinite(ageMs) || ageMs > LIVE_STATE_STALE_MS ? 1 : 0,
  };
}

function brokerClientLiveState(client) {
  return {
    relayMask: Number(client.relayMask || 0) >>> 0,
    schedulerEnabledMask: Number(client.schedulerEnabledMask || 0) >>> 0,
    schedulerPauseMask: Number(client.schedulerPauseMask || 0) >>> 0,
    schedulerPauseBlocksMask: Number(client.schedulerPauseBlocksMask || 0) >>> 0,
    protectionEnabledMask: Number(client.protectionEnabledMask || 0) >>> 0,
    protectionTrippedMask: Number(client.protectionTrippedMask || 0) >>> 0,
    protectionFaultLockMask: Number(client.protectionFaultLockMask || 0) >>> 0,
    protectionCtValidMask: Number(client.protectionCtValidMask || 0) >>> 0,
    protectionLearningMask: Number(client.protectionLearningMask || 0) >>> 0,
    protectionStrategyLsbMask: Number(client.protectionStrategyLsbMask || 0) >>> 0,
    protectionStrategyMsbMask: Number(client.protectionStrategyMsbMask || 0) >>> 0,
    adcValid: Number(client.adcValid || 0),
    adcCtValid: Number(client.adcCtValid || 0),
    adcSignalPresent: Number(client.adcSignalPresent || 0),
    adcRaw: Number(client.adcRaw || 0),
    adcMillivolts: Number(client.adcMillivolts || 0),
    adcMin: Number(client.adcMin || 0),
    adcMax: Number(client.adcMax || 0),
    adcAverage: Number(client.adcAverage || 0),
    adcRawRms: Number(client.adcRawRms || 0),
    adcRmsMillivolts: Number(client.adcRmsMillivolts || 0),
    adcCurrentMilliamps: Number(client.adcCurrentMilliamps || 0),
    adcSampleRateHz: Number(client.adcSampleRateHz || 0),
    adcWindowSamples: Number(client.adcWindowSamples || 0),
    adcSampleCount: Number(client.adcSampleCount || 0),
    adcDmaOverruns: Number(client.adcDmaOverruns || 0),
    adcSaturationCount: Number(client.adcSaturationCount || 0),
    liveStateSource: "mqtt-status",
  };
}

function syncCloudRegistryFromKnownNode(node) {
  const clusterId = cloudClusterKeyOf(node);
  const reportedDeviceId = String(node?.cloudDeviceId || "").trim();
  const deviceId = reportedDeviceId || deriveFallbackDeviceId(node);
  const reportedKeyId = String(node?.cloudKeyId || "").trim();
  const registryKey = clusterId && deviceId ? `${clusterId}|${deviceId}` : "";
  const seenAt = node.lastSeenAt || nowIso();
  const clusterRecord = findCloudClusterRecord(clusterId);
  let existingDevice;
  let deviceRecord;
  let effectiveKeyId;

  if (!clusterId || !deviceId) {
    return;
  }

  existingDevice = cloudDevices.get(registryKey) || {};
  effectiveKeyId = String(reportedKeyId || existingDevice.keyId || "").trim();
  if (!effectiveKeyId && clusterRecord) {
    effectiveKeyId = generateCloudKeyId();
  }
  if (existingDevice.nodeId && Number(existingDevice.nodeId) !== Number(node.nodeId || 0)) {
    pushEvent("cloud-device-rebind", {
      clusterId,
      deviceId,
      previousNodeId: Number(existingDevice.nodeId || 0),
      nextNodeId: Number(node.nodeId || 0),
      previousDeviceName: existingDevice.device || "",
      nextDeviceName: node.device || "",
    });
  }
  deviceRecord = {
    ...existingDevice,
    registryKey,
    clusterId,
    deviceId,
    keyId: effectiveKeyId,
    enabled: existingDevice.enabled !== false,
    transport: Number(node.cloudTransport || CLOUD_TRANSPORT),
    nodeId: Number(node.nodeId || 0),
    device: node.device || existingDevice.device || "",
    clusterName: node.cluster || existingDevice.clusterName || "",
    ip: normalizeIp(node.ip),
    role: Number(node.role || 0),
    online: Number(node.online ? 1 : 0),
    deviceState: Number(node.online ? (node.deviceState || 0) : 0),
    relayMask: Number(node.relayMask || 0) >>> 0,
    sourceMasterNodeId: Number(node.sourceMasterNodeId || 0),
    backendKey: node.backendKey || existingDevice.backendKey || "",
    firstSeenAt: existingDevice.firstSeenAt || seenAt,
    lastSeenAt: seenAt,
  };

  if (clusterRecord) {
    ensureClusterIdentityMaterial(clusterRecord);
    ensureDeviceIdentityMaterial(clusterRecord, deviceRecord);
    cloudClusters.set(clusterRecord.clusterId, clusterRecord);
    if (!existingDevice.registryKey) {
      pushEvent("cloud-device-auto-enrolled", {
        clusterId,
        deviceId,
        keyId: effectiveKeyId,
        nodeId: Number(node.nodeId || 0),
        device: deviceRecord.device || deviceId,
      });
    }
  }

  cloudDevices.set(registryKey, deviceRecord);

  const clusterDevices = Array.from(cloudDevices.values()).filter((item) => item.clusterId === clusterId);
  const onlineDevices = clusterDevices.filter((item) => Number(item.online) === 1).length;
  const masterDevice = clusterDevices.find((item) => Number(item.role || 0) === 2 && Number(item.online || 0) === 1)
    || clusterDevices.find((item) => Number(item.role || 0) === 2)
    || null;
  const existingCluster = cloudClusters.get(clusterId) || {};

  cloudClusters.set(clusterId, {
    ...existingCluster,
    clusterId,
    keyId: String(existingCluster.keyId || "").trim() || effectiveKeyId,
    enabled: existingCluster.enabled !== false,
    transport: Number(node.cloudTransport || CLOUD_TRANSPORT),
    brokerHost: existingCluster.brokerHost || CLOUD_BROKER_HOST,
    brokerPort: Number(existingCluster.brokerPort || CLOUD_BROKER_PORT),
    backendKey: node.backendKey || existingCluster.backendKey || "",
    clusterName: node.cluster || existingCluster.clusterName || "",
    lastMasterNodeId: masterDevice ? Number(masterDevice.nodeId || 0) : Number(existingCluster.lastMasterNodeId || 0),
    lastMasterDeviceId: masterDevice ? String(masterDevice.deviceId || "") : String(existingCluster.lastMasterDeviceId || ""),
    onlineDevices,
    knownDevices: clusterDevices.length,
    firstSeenAt: existingCluster.firstSeenAt || seenAt,
    lastSeenAt: seenAt,
  });

  scheduleCloudRegistrySave();

  void clusterRecord;
  void deviceRecord;
}

async function remoteNodeLogin(node) {
  const cacheKey = String(Number(node.nodeId || 0));
  const cached = remoteSessionCache.get(cacheKey);
  let loginResponse;
  let loginData;
  const requesterNodeId = Number(effectiveMasterNodeIdForGroup(node.backendKey, node.cluster) || node.sourceMasterNodeId || node.nodeId || 1);
  const unixTime = Math.floor(Date.now() / 1000);
  const clusterName = String(node.cluster || "default");
  const passwordHash = crypto.createHash("sha256").update(String(REMOTE_CLUSTER_PASSWORD), "utf8").digest();
  const authInput = `${unixTime}|${requesterNodeId}|${clusterName}`;
  const authTag = crypto.createHmac("sha256", passwordHash).update(authInput, "utf8").digest().readUInt32LE(0);

  if (cached && cached.token && cached.expiresAt > Date.now()) {
    return cached.token;
  }

  loginResponse = await fetch(`http://${node.ip}/api/login`, {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: `scope=service`
      + `&node_id=${encodeURIComponent(String(requesterNodeId))}`
      + `&unix_time=${encodeURIComponent(String(unixTime))}`
      + `&auth_tag=${encodeURIComponent(String(authTag))}`,
  });
  loginData = await loginResponse.json();
  if (!loginResponse.ok || !loginData.ok || !loginData.token) {
    throw new Error("remote_login_failed");
  }

  remoteSessionCache.set(cacheKey, {
    token: loginData.token,
    expiresAt: Date.now() + REMOTE_SESSION_CACHE_TTL_MS,
  });
  return loginData.token;
}

function predictRelayMask(currentMask, channel, value) {
  const bit = 1 << (Number(channel) - 1);
  if (Number(value) === 0) {
    return (Number(currentMask) >>> 0) & (~bit >>> 0);
  }
  return (Number(currentMask) >>> 0) | bit;
}

function queueRelayStateConfirm(nodeId) {
  const key = String(Number(nodeId));
  const existingTimer = relayConfirmTimers.get(key);

  relayConfirmDirty.add(key);
  if (existingTimer) {
    clearTimeout(existingTimer);
  }
  relayConfirmTimers.set(key, setTimeout(() => {
    relayConfirmTimers.delete(key);
    void runRelayStateConfirm(Number(nodeId));
  }, 250));
}

async function runRelayStateConfirm(nodeId) {
  const key = String(Number(nodeId));

  if (relayConfirmInflight.has(key)) return;
  relayConfirmDirty.delete(key);
  relayConfirmInflight.delete(key);
}

function knownNodeSummary(node) {
  const online = Number(node.online ? 1 : 0);
  const deviceState = online ? Number(node.deviceState || 0) : 0;
  const effectiveMasterNodeId = effectiveMasterNodeIdForGroup(node.backendKey, node.cluster);
  const meta = liveStateMeta(node);
  return {
    backendKey: node.backendKey,
    cluster: node.cluster,
    nodeId: node.nodeId,
    device: node.device,
    cloudTransport: Number(node.cloudTransport || CLOUD_TRANSPORT),
    cloudClusterId: node.cloudClusterId || "",
    cloudDeviceId: node.cloudDeviceId || "",
    cloudKeyId: node.cloudKeyId || "",
    cloudClusterCaFingerprint: node.cloudClusterCaFingerprint || "",
    cloudClusterBindingSecret: node.cloudClusterBindingSecret || "",
    role: node.role,
    term: Number(node.term || 0),
    online,
    deviceState,
    deviceStateText: deviceStateText(deviceState),
    relayMask: node.relayMask >>> 0,
    schedulerEnabledMask: Number(node.schedulerEnabledMask || 0) >>> 0,
    schedulerPauseMask: Number(node.schedulerPauseMask || 0) >>> 0,
    schedulerPauseBlocksMask: Number(node.schedulerPauseBlocksMask || 0) >>> 0,
    protectionEnabledMask: Number(node.protectionEnabledMask || 0) >>> 0,
    protectionTrippedMask: Number(node.protectionTrippedMask || 0) >>> 0,
    protectionFaultLockMask: Number(node.protectionFaultLockMask || 0) >>> 0,
    protectionCtValidMask: Number(node.protectionCtValidMask || 0) >>> 0,
    protectionLearningMask: Number(node.protectionLearningMask || 0) >>> 0,
    protectionStrategyLsbMask: Number(node.protectionStrategyLsbMask || 0) >>> 0,
    protectionStrategyMsbMask: Number(node.protectionStrategyMsbMask || 0) >>> 0,
    adcValid: Number(node.adcValid || 0),
    adcCtValid: Number(node.adcCtValid || 0),
    adcSignalPresent: Number(node.adcSignalPresent || 0),
    adcRaw: Number(node.adcRaw || 0),
    adcMillivolts: Number(node.adcMillivolts || 0),
    adcMin: Number(node.adcMin || 0),
    adcMax: Number(node.adcMax || 0),
    adcAverage: Number(node.adcAverage || 0),
    adcRawRms: Number(node.adcRawRms || 0),
    adcRmsMillivolts: Number(node.adcRmsMillivolts || 0),
    adcCurrentMilliamps: Number(node.adcCurrentMilliamps || 0),
    adcSampleRateHz: Number(node.adcSampleRateHz || 0),
    adcWindowSamples: Number(node.adcWindowSamples || 0),
    adcSampleCount: Number(node.adcSampleCount || 0),
    adcDmaOverruns: Number(node.adcDmaOverruns || 0),
    adcSaturationCount: Number(node.adcSaturationCount || 0),
    ip: normalizeIp(node.ip),
    lastSeenAt: node.lastSeenAt,
    cacheAgeMs: meta.cacheAgeMs,
    stateFresh: meta.stateFresh,
    stateStale: meta.stateStale,
    stateExpired: meta.stateExpired,
    liveStateSource: node.liveStateSource || "",
    sourceMasterNodeId: node.sourceMasterNodeId,
    effectiveMasterNodeId,
  };
}

function cloudDeviceSummary(record) {
  return {
    registryKey: record.registryKey,
    clusterId: record.clusterId,
    deviceId: record.deviceId,
    keyId: record.keyId,
    enabled: record.enabled !== false,
    transport: Number(record.transport || CLOUD_TRANSPORT),
    nodeId: Number(record.nodeId || 0),
    device: record.device || "",
    clusterName: record.clusterName || "",
    ip: normalizeIp(record.ip),
    role: Number(record.role || 0),
    online: Number(record.online ? 1 : 0),
    deviceState: Number(record.deviceState || 0),
    deviceStateText: deviceStateText(Number(record.deviceState || 0)),
    relayMask: Number(record.relayMask || 0) >>> 0,
    deviceFingerprint: record.deviceFingerprint || "",
    mcuDeviceFingerprint: record.mcuDeviceFingerprint || "",
    deviceFingerprintSource: record.deviceFingerprintSource || "",
    fingerprintBoundAt: record.fingerprintBoundAt || "",
    lastSeenAt: record.lastSeenAt || "",
    firstSeenAt: record.firstSeenAt || "",
    sourceMasterNodeId: Number(record.sourceMasterNodeId || 0),
  };
}

function cloudClusterSummary(record) {
  return {
    clusterId: record.clusterId,
    keyId: record.keyId,
    enabled: record.enabled !== false,
    transport: Number(record.transport || CLOUD_TRANSPORT),
    brokerHost: record.brokerHost || "",
    brokerPort: Number(record.brokerPort || 0),
    backendKey: record.backendKey || "",
    clusterName: record.clusterName || "",
    caFingerprint: record.caFingerprint || "",
    lastMasterNodeId: Number(record.lastMasterNodeId || 0),
    lastMasterDeviceId: record.lastMasterDeviceId || "",
    lastSeenAt: record.lastSeenAt || "",
    firstSeenAt: record.firstSeenAt || "",
    onlineDevices: Number(record.onlineDevices || 0),
    knownDevices: Number(record.knownDevices || 0),
  };
}

function buildSummarySnapshot() {
  return {
    ok: true,
    httpHost: HTTP_HOST,
    httpPort: HTTP_PORT,
    mqttPort: CLOUD_BROKER_PORT,
    mqttControlPort: MQTT_CONTROL_PORT,
    keyCount: keyStore.length,
    knownNodes: knownNodes.size,
    cloudDevices: cloudDevices.size,
    cloudClusters: cloudClusters.size,
  };
}

function buildClientsSnapshot() {
  return lastBrokerClientsSnapshot;
}

function buildKnownNodesSnapshot() {
  return {
    ok: true,
    nodes: Array.from(knownNodes.values())
      .sort((a, b) => {
        if (a.cluster !== b.cluster) {
          return a.cluster.localeCompare(b.cluster);
        }
        return a.nodeId - b.nodeId;
      })
      .map(knownNodeSummary),
  };
}

function buildEventsSnapshot() {
  return {
    ok: true,
    events: events.slice(-40),
  };
}

function buildCloudDevicesSnapshot() {
  return {
    ok: true,
    devices: Array.from(cloudDevices.values())
      .filter((item) => item && typeof item === "object")
      .sort((a, b) => {
        const aClusterId = String(a.clusterId || "");
        const bClusterId = String(b.clusterId || "");
        if (aClusterId !== bClusterId) {
          return aClusterId.localeCompare(bClusterId);
        }
        return String(a.deviceId || "").localeCompare(String(b.deviceId || ""));
      })
      .map(cloudDeviceSummary),
  };
}

function buildCloudClustersSnapshot() {
  return {
    ok: true,
    clusters: Array.from(cloudClusters.values())
      .filter((item) => item && typeof item === "object")
      .sort((a, b) => a.clusterId.localeCompare(b.clusterId))
      .map(cloudClusterSummary),
  };
}

function buildStatePayload() {
  return {
    summary: buildSummarySnapshot(),
    clients: buildClientsSnapshot(),
    knownNodes: buildKnownNodesSnapshot(),
    cloudDevices: buildCloudDevicesSnapshot(),
    cloudClusters: buildCloudClustersSnapshot(),
    events: buildEventsSnapshot(),
  };
}

function broadcastState() {
  const payload = `data: ${JSON.stringify(buildStatePayload())}\n\n`;
  for (const res of Array.from(sseClients.values())) {
    try {
      res.write(payload);
    } catch (_error) {
      sseClients.delete(res);
    }
  }
}

/* ?? Sync knownNodes from MQTT broker clients ?? */
let brokerNodesSynced = false;
let lastBrokerClientsSnapshot = { ok: true, clients: [], source: "broker" };
async function syncKnownNodesFromBroker() {
  try {
    const response = await fetch(`${BROKER_CONTROL_URL}/api/clients`);
    const data = await response.json();
    if (!data.ok || !Array.isArray(data.clients)) return;
    lastBrokerClientsSnapshot = { ...data, source: "broker" };
    for (const client of data.clients) {
      if (!client.connected || !client.deviceId) continue;
      const registryKey = `${String(client.clusterId || "default")}|${String(client.deviceId)}`;
      const deviceRecord = cloudDevices.get(registryKey);
      const clusterRecord = cloudClusters.get(String(client.clusterId || ""));
      /* Extract nodeId from deviceId if possible (format: node-NNNNNNNNN-NNNNNNNNN) */
      let extractedNodeId = Number(client.nodeId || deviceRecord?.nodeId || 0);
      if (!extractedNodeId && client.deviceId) {
        const m = String(client.deviceId).match(/node-(\d+)-(\d+)/);
        if (m) extractedNodeId = Number(m[2]);
      }
      const knownKey = knownNodeKey(
        clusterRecord?.backendKey || "",
        clusterRecord?.clusterName || String(client.clusterId || "default"),
        extractedNodeId || `dev-${client.deviceId}`
      );
      let node = knownNodes.get(knownKey);
      if (!node) {
        /* Find by deviceId across knownNodes */
        for (const [, n] of knownNodes) {
          if (n.cloudDeviceId === client.deviceId) { node = n; break; }
        }
      }
      if (!node) {
        /* Create new knownNode from broker client info */
        node = {
          backendKey: clusterRecord?.backendKey || "",
          cluster: clusterRecord?.clusterName || String(client.clusterId || "default"),
          nodeId: extractedNodeId,
          cloudClusterId: String(client.clusterId || ""),
          cloudDeviceId: String(client.deviceId),
          cloudKeyId: deviceRecord?.keyId || "",
          online: true,
          device: deviceRecord?.deviceName || client.device || client.clientId || "",
          role: Number(client.role || 0),
          deviceState: Number(client.deviceState || 1),
          deviceStateText: deviceStateText(1),
          ...brokerClientLiveState(client),
          lastSeen: new Date().toISOString(),
          lastSeenAt: new Date().toISOString(),
          lastSeenMs: Date.now(),
          via: "mqtt-broker",
        };
        const key = node.nodeId > 0
          ? knownNodeKey(node.backendKey, node.cluster, node.nodeId)
          : knownNodeKey(node.backendKey, node.cluster, `dev-${client.deviceId}`);
        knownNodes.set(key, node);
        syncCloudRegistryFromKnownNode(node);
      } else {
        node.online = true;
        node.lastSeen = new Date().toISOString();
        node.lastSeenAt = new Date().toISOString();
        node.lastSeenMs = Date.now();
        if (!node.cloudDeviceId) node.cloudDeviceId = String(client.deviceId);
        if (!node.cloudClusterId) node.cloudClusterId = String(client.clusterId || "");
        if (deviceRecord && deviceRecord.nodeId) node.nodeId = deviceRecord.nodeId;
        if (client.device && !node.device) node.device = String(client.device);
        if (client.role) node.role = Number(client.role || 0);
        if (client.term) node.term = Number(client.term || 0);
        Object.assign(node, brokerClientLiveState(client));
        if (client.lastStatusAt) node.lastSeenAt = String(client.lastStatusAt);
        syncCloudRegistryFromKnownNode(node);
      }
      if (Array.isArray(client.nodes)) {
        for (const remote of client.nodes) {
          const remoteNodeId = Number(remote.node_id || remote.nodeId || 0);
          if (!remoteNodeId) continue;
          upsertKnownNode({
            backendKey: clusterRecord?.backendKey || "",
            cluster: clusterRecord?.clusterName || String(client.clusterId || "default"),
            nodeId: remoteNodeId,
            cloudClusterId: String(remote.cloud_cluster_id || client.clusterId || ""),
            cloudDeviceId: String(remote.cloud_device_id || ""),
            cloudKeyId: String(remote.cloud_key_id || ""),
            online: Number(remote.online ? 1 : 0),
            device: String(remote.name || remote.device || `Node ${remoteNodeId}`),
            role: Number(remote.role || 0),
            term: Number(remote.term || client.term || 0),
            deviceState: Number(remote.device_state || 0),
            relayMask: Number(remote.relay_mask || 0) >>> 0,
            schedulerEnabledMask: Number(remote.scheduler_enabled_mask || remote.schedulerEnabledMask || 0) >>> 0,
            schedulerPauseMask: Number(remote.scheduler_pause_mask || remote.schedulerPauseMask || 0) >>> 0,
            schedulerPauseBlocksMask: Number(remote.scheduler_pause_blocks_mask || remote.schedulerPauseBlocksMask || 0) >>> 0,
            protectionEnabledMask: Number(remote.protection_enabled_mask || remote.protectionEnabledMask || 0) >>> 0,
            protectionTrippedMask: Number(remote.protection_tripped_mask || remote.protectionTrippedMask || 0) >>> 0,
            protectionFaultLockMask: Number(remote.protection_fault_lock_mask || remote.protectionFaultLockMask || 0) >>> 0,
            protectionCtValidMask: Number(remote.protection_ct_valid_mask || remote.protectionCtValidMask || 0) >>> 0,
            protectionLearningMask: Number(remote.protection_learning_mask || remote.protectionLearningMask || 0) >>> 0,
            protectionStrategyLsbMask: Number(remote.protection_strategy_lsb_mask || remote.protectionStrategyLsbMask || 0) >>> 0,
            protectionStrategyMsbMask: Number(remote.protection_strategy_msb_mask || remote.protectionStrategyMsbMask || 0) >>> 0,
            ip: normalizeIp(remote.ip_text || remote.ip || ""),
            lastSeenAt: client.lastStatusAt || new Date().toISOString(),
            lastSeenMs: Date.now(),
            sourceMasterNodeId: extractedNodeId,
            via: "mqtt-broker-status",
          });
        }
      }
    }
    brokerNodesSynced = true;
    scheduleStateBroadcast();
  } catch (_) { /* broker not running yet */ }
}

function scheduleStateBroadcast() {
  if (stateBroadcastScheduled) {
    return;
  }
  stateBroadcastScheduled = true;
  setTimeout(() => {
    stateBroadcastScheduled = false;
    broadcastState();
  }, 0);
}

function scanStaleKnownNodes() {
  const nowMs = Date.now();

  for (const node of knownNodes.values()) {
    const lastSeenMs = Date.parse(node.lastSeenAt || node.lastUpdatedAt || 0);
    if (!Number.isFinite(lastSeenMs)) {
      continue;
    }
    if ((nowMs - lastSeenMs) < CLIENT_STALE_MS) {
      continue;
    }
    if (!node.online) {
      continue;
    }
    upsertKnownNode({
      ...node,
      online: 0,
      deviceState: 0,
    });
  }
}


const DASHBOARD_PATH = path.join(__dirname, "dashboard.html");
let _dashboardHtml = null;
let _dashboardMtime = 0;

function renderHtml() {
  try {
    const stat = fs.statSync(DASHBOARD_PATH);
    if (!_dashboardHtml || stat.mtimeMs > _dashboardMtime) {
      _dashboardHtml = fs.readFileSync(DASHBOARD_PATH, "utf8");
      _dashboardMtime = stat.mtimeMs;
    }
    return _dashboardHtml;
  } catch (_error) {
    return "<!doctype html><html lang=zh-Hant><head><meta charset=utf-8><title>autoControl</title></head><body><h1>Dashboard not found</h1><p>Place dashboard.html in tools/backend-test/</p></body></html>";
  }
}

app.post("/api/auth/login", (req, res) => {
  const username = String(req.body.username || "").trim();
  const password = String(req.body.password || "");
  if (username !== DASHBOARD_USER || password !== DASHBOARD_PASSWORD) {
    pushEvent("dashboard-login-failed", { username });
    return res.status(401).json({ ok: false, error: "bad_credentials" });
  }
  const token = createDashboardSession();
  res.setHeader(
    "Set-Cookie",
    `ac_backend_session=${encodeURIComponent(token)}; Path=/; HttpOnly; SameSite=Lax; Max-Age=${Math.floor(DASHBOARD_SESSION_TTL_MS / 1000)}`
  );
  pushEvent("dashboard-login", { username });
  if (String(req.query.redirect || "") === "1") {
    return res.redirect(303, "/");
  }
  return res.json({ ok: true, user: username, expires_in_ms: DASHBOARD_SESSION_TTL_MS });
});

app.post("/api/auth/logout", (req, res) => {
  const session = getDashboardSession(req);
  if (session && session.token) dashboardSessions.delete(session.token);
  res.setHeader("Set-Cookie", "ac_backend_session=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0");
  return res.json({ ok: true });
});

app.get("/api/auth/session", (req, res) => {
  const session = getDashboardSession(req);
  if (!session) return res.status(401).json({ ok: false, error: "unauthorized" });
  return res.json({ ok: true, user: session.user, expires_at: session.expiresAt });
});

app.use(requireDashboardSession);

app.get("/", (_req, res) => {
  res.type("html").send(renderHtml());
});

app.get("/favicon.ico", (_req, res) => {
  res.status(204).end();
});

app.get("/api/stream", (req, res) => {
  res.setHeader("Content-Type", "text/event-stream");
  res.setHeader("Cache-Control", "no-cache, no-transform");
  res.setHeader("Connection", "keep-alive");
  res.flushHeaders?.();
  res.write(`data: ${JSON.stringify(buildStatePayload())}\n\n`);
  sseClients.add(res);

  const keepAlive = setInterval(() => {
    try {
      res.write(": keep-alive\n\n");
    } catch (_error) {
      clearInterval(keepAlive);
      sseClients.delete(res);
    }
  }, 15000);

  req.on("close", () => {
    clearInterval(keepAlive);
    sseClients.delete(res);
  });
});

app.get("/api/summary", (_req, res) => {
  res.json(buildSummarySnapshot());
});

app.get("/api/dashboard/live", async (_req, res) => {
  await syncKnownNodesFromBroker();
  res.json({
    ok: true,
    ...buildStatePayload(),
  });
});

app.get("/api/keys", (_req, res) => {
  res.json({
    ok: true,
    keys: keyStore.map(sanitizeKeyRecord),
  });
});

app.post("/api/keys", (req, res) => {
  const label = typeof req.body.label === "string" ? req.body.label : "";
  const record = createKeyRecord(label, generateKeyText());
  keyStore.push(record);
  saveKeyStore();
  res.json({ ok: true, key: sanitizeKeyRecord(record) });
});

app.patch("/api/keys/:id", (req, res) => {
  const record = findKeyRecordById(req.params.id);
  if (!record) {
    return res.status(404).json({ ok: false, error: "key_not_found" });
  }

  if (typeof req.body.enabled === "boolean") {
    record.enabled = req.body.enabled;
  }
  if (typeof req.body.label === "string" && req.body.label.trim()) {
    record.label = req.body.label.trim();
  }
  saveKeyStore();
  return res.json({ ok: true, key: sanitizeKeyRecord(record) });
});

app.delete("/api/keys/:id", (req, res) => {
  const nextKeys = keyStore.filter((item) => item.id !== req.params.id);
  if (nextKeys.length === keyStore.length) {
    return res.status(404).json({ ok: false, error: "key_not_found" });
  }

  keyStore = nextKeys;
  saveKeyStore();
  return res.json({ ok: true });
});

app.get("/api/known-nodes", async (_req, res) => {
  await syncKnownNodesFromBroker();
  const nodes = Array.from(knownNodes.values())
    .sort((a, b) => {
      if (a.cluster !== b.cluster) {
        return a.cluster.localeCompare(b.cluster);
      }
      return a.nodeId - b.nodeId;
      })
      .map(knownNodeSummary);
  res.json({ ok: true, nodes });
});

app.get("/api/cloud/devices", async (_req, res) => {
  await syncKnownNodesFromBroker();
  res.json(buildCloudDevicesSnapshot());
});

app.get("/api/cloud/clusters", async (_req, res) => {
  await syncKnownNodesFromBroker();
  res.json(buildCloudClustersSnapshot());
});

app.post("/api/cloud/clusters", (req, res) => {
  const clusterId = String(req.body.clusterId || generateClusterIdentityId()).trim();
  const clusterName = String(req.body.clusterName || clusterId).trim();
  const keyId = String(req.body.keyId || generateCloudKeyId()).trim();
  const brokerHost = String(req.body.brokerHost || CLOUD_BROKER_HOST).trim();
  const brokerPort = Number(req.body.brokerPort || CLOUD_BROKER_PORT);
  const caBundle = buildClusterCaBundle(clusterId, keyId);

  if (!clusterId) {
    return res.status(400).json({ ok: false, error: "bad_cluster_id" });
  }
  if (cloudClusters.has(clusterId)) {
    return res.status(409).json({ ok: false, error: "cloud_cluster_exists" });
  }

  cloudClusters.set(clusterId, {
    clusterId,
    keyId,
    enabled: true,
    transport: CLOUD_TRANSPORT,
    brokerHost,
    brokerPort,
    backendKey: "",
    clusterName,
    caPublicPem: caBundle.caPublicPem,
    caPrivatePem: caBundle.caPrivatePem,
    caFingerprint: caBundle.caFingerprint,
    bindingSecret: caBundle.bindingSecret,
    lastMasterNodeId: 0,
    lastMasterDeviceId: "",
    onlineDevices: 0,
    knownDevices: 0,
    firstSeenAt: nowIso(),
    lastSeenAt: nowIso(),
    lastUpdatedAt: nowIso(),
  });
  scheduleCloudRegistrySave();
  scheduleStateBroadcast();
  return res.json({ ok: true, cluster: cloudClusterSummary(cloudClusters.get(clusterId)) });
});

app.get("/api/cloud/clusters/:clusterId/provision", (req, res) => {
  const clusterRecord = findCloudClusterRecord(req.params.clusterId);
  if (!clusterRecord) {
    return res.status(404).json({ ok: false, error: "cloud_cluster_not_found" });
  }
  const provisioning = buildClusterProvisioning(clusterRecord);
  return res.json({
    ok: true,
    cluster: cloudClusterSummary(clusterRecord),
    provisioning,
    formFields: buildProvisioningFormFields(provisioning),
    trustBundle: {
      clusterCaPublicPem: clusterRecord.caPublicPem || "",
      clusterCaFingerprint: clusterRecord.caFingerprint || "",
      clusterBindingSecret: clusterRecord.bindingSecret || "",
    },
  });
});

app.post("/api/cloud/clusters/:clusterId/rotate-ca", (req, res) => {
  const clusterRecord = findCloudClusterRecord(req.params.clusterId);
  if (!clusterRecord) {
    return res.status(404).json({ ok: false, error: "cloud_cluster_not_found" });
  }
  rotateClusterCa(clusterRecord);
  cloudClusters.set(clusterRecord.clusterId, clusterRecord);
  scheduleCloudRegistrySave();
  pushEvent("cloud-cluster-ca-rotated", {
    clusterId: clusterRecord.clusterId,
    keyId: clusterRecord.keyId,
    caFingerprint: clusterRecord.caFingerprint || "",
  });
  return res.json({
    ok: true,
    cluster: cloudClusterSummary(clusterRecord),
    provision: {
      cluster: buildClusterProvisioning(clusterRecord),
      devices: Array.from(cloudDevices.values())
        .filter((item) => item.clusterId === clusterRecord.clusterId)
        .map((item) => buildDeviceProvisioningBundle(clusterRecord, item)),
    },
  });
});

app.post("/api/cloud/clusters/:clusterId/apply-online", async (req, res) => {
  const clusterRecord = findCloudClusterRecord(req.params.clusterId);
  const targetNodes = Array.from(knownNodes.values()).filter((node) =>
    cloudClusterKeyOf(node) === String(req.params.clusterId || "").trim() &&
    node.ip);
  const results = [];

  if (!clusterRecord) {
    return res.status(404).json({ ok: false, error: "cloud_cluster_not_found" });
  }

  for (const node of targetNodes) {
    try {
      const derivedDeviceId = String(node.cloudDeviceId || "").trim() ||
        deriveFallbackDeviceId(node);
      if (!String(node.cloudDeviceId || "").trim()) {
        syncCloudRegistryFromKnownNode({
          ...node,
          cloudClusterId: clusterRecord.clusterId,
          cloudDeviceId: derivedDeviceId,
          cloudKeyId: String(node.cloudKeyId || "").trim(),
        });
      }
      const deviceRecord = findCloudDeviceRecord(clusterRecord.clusterId, derivedDeviceId);
      if (!deviceRecord) {
        throw new Error("cloud_device_materialize_failed");
      }
      await applyProvisioningToNode(node, clusterRecord, deviceRecord);
      results.push({
        nodeId: Number(node.nodeId || 0),
        device: node.device || "",
        deviceId: derivedDeviceId,
        ok: true,
      });
      pushEvent("cloud-cluster-apply-online-ok", {
        clusterId: clusterRecord.clusterId,
        nodeId: Number(node.nodeId || 0),
        deviceId: derivedDeviceId,
      });
    } catch (error) {
      results.push({
        nodeId: Number(node.nodeId || 0),
        device: node.device || "",
        deviceId: String(node.cloudDeviceId || "").trim(),
        ok: false,
        error: String(error?.message || error),
      });
      pushEvent("cloud-cluster-apply-online-failed", {
        clusterId: clusterRecord.clusterId,
        nodeId: Number(node.nodeId || 0),
        error: String(error?.message || error),
      });
    }
  }

  scheduleCloudRegistrySave();
  scheduleStateBroadcast();
  return res.json({
    ok: true,
    clusterId: clusterRecord.clusterId,
    totalOnlineNodes: targetNodes.length,
    results,
  });
});

app.post("/api/cloud/devices", (req, res) => {
  const clusterId = String(req.body.clusterId || "").trim();
  const deviceId = String(req.body.deviceId || generateDeviceIdentityId()).trim();
  const deviceName = String(req.body.deviceName || deviceId).trim();
  const keyId = String(req.body.keyId || generateCloudKeyId()).trim();
  const registryKey = `${clusterId}|${deviceId}`;
  const clusterRecord = findCloudClusterRecord(clusterId);
  let deviceBundle;

  if (!clusterId) {
    return res.status(400).json({ ok: false, error: "bad_cluster_id" });
  }
  if (!clusterRecord) {
    return res.status(404).json({ ok: false, error: "cloud_cluster_not_found" });
  }
  if (cloudDevices.has(registryKey)) {
    /* Device exists ??update keyId and/or nodeId (e.g. MCU UID
       produces a different self_node_id than the deviceId suffix). */
    const existing = cloudDevices.get(registryKey);
    if (keyId) existing.keyId = keyId;
    if (req.body.nodeId) existing.nodeId = Number(req.body.nodeId);
    saveCloudRegistry();
    return res.json({ ok: true, updated: true, keyId: existing.keyId, nodeId: existing.nodeId });
  }

  deviceBundle = buildDeviceCredentialBundle(clusterRecord, deviceId, deviceName, keyId);

  cloudDevices.set(registryKey, {
    registryKey,
    clusterId,
    deviceId,
    keyId,
    enabled: true,
    transport: 1,
    nodeId: 0,
    device: deviceName,
    clusterName: clusterRecord.clusterName || clusterId,
    ip: "",
    role: 0,
    online: 0,
    deviceState: 0,
    relayMask: 0,
    sourceMasterNodeId: 0,
    backendKey: clusterRecord.backendKey || "",
    devicePublicPem: deviceBundle.devicePublicPem,
    devicePrivatePem: deviceBundle.devicePrivatePem,
    deviceFingerprint: deviceBundle.deviceFingerprint,
    certificatePayload: deviceBundle.certificatePayload,
    certificateSignature: deviceBundle.certificateSignature,
    firstSeenAt: nowIso(),
    lastSeenAt: "",
    lastUpdatedAt: nowIso(),
  });

  clusterRecord.knownDevices = Array.from(cloudDevices.values()).filter((item) => item.clusterId === clusterId).length;
  clusterRecord.lastUpdatedAt = nowIso();
  cloudClusters.set(clusterId, clusterRecord);

  scheduleCloudRegistrySave();
  scheduleStateBroadcast();
  return res.json({ ok: true, device: cloudDeviceSummary(cloudDevices.get(registryKey)) });
});

app.post("/api/cloud/claim-node", (req, res) => {
  const nodeId = Number(req.body.nodeId || req.body.node_id || 0);
  const clusterId = String(req.body.clusterId || req.body.cluster_id || "default").trim() || "default";
  const deviceId = String(req.body.deviceId || req.body.device_id || (nodeId ? `node-${nodeId}` : "")).trim();
  const deviceName = String(req.body.deviceName || req.body.device_name || deviceId || `Node ${nodeId}`).trim();
  const owner = String(req.body.owner || DASHBOARD_USER).trim();
  let clusterRecord = findCloudClusterRecord(clusterId);
  let deviceRecord;

  if (!nodeId || !deviceId) {
    return res.status(400).json({ ok: false, error: "missing_node_id_or_device_id" });
  }

  if (!clusterRecord) {
    const keyId = generateCloudKeyId();
    const caBundle = buildClusterCaBundle(clusterId, keyId);
    clusterRecord = {
      clusterId,
      clusterName: clusterId,
      keyId,
      enabled: true,
      transport: CLOUD_TRANSPORT,
      brokerHost: CLOUD_BROKER_HOST,
      brokerPort: CLOUD_BROKER_PORT,
      backendKey: "",
      revision: 1,
      caRevocationTag: 0,
      ...caBundle,
      lastMasterNodeId: 0,
      lastMasterDeviceId: "",
      onlineDevices: 0,
      knownDevices: 0,
      firstSeenAt: nowIso(),
      lastSeenAt: nowIso(),
      lastUpdatedAt: nowIso(),
    };
    cloudClusters.set(clusterId, clusterRecord);
  }

  deviceRecord = findCloudDeviceRecord(clusterId, deviceId);
  if (!deviceRecord) {
    const keyId = generateCloudKeyId();
    const bundle = buildDeviceCredentialBundle(clusterRecord, deviceId, deviceName, keyId);
    deviceRecord = {
      registryKey: `${clusterId}|${deviceId}`,
      clusterId,
      deviceId,
      keyId,
      enabled: true,
      transport: 1,
      nodeId,
      device: deviceName,
      clusterName: clusterRecord.clusterName || clusterId,
      ip: "",
      role: 0,
      online: 0,
      deviceState: 0,
      relayMask: 0,
      sourceMasterNodeId: 0,
      backendKey: "",
      devicePublicPem: bundle.devicePublicPem,
      devicePrivatePem: bundle.devicePrivatePem,
      deviceFingerprint: bundle.deviceFingerprint,
      certificatePayload: bundle.certificatePayload,
      certificateSignature: bundle.certificateSignature,
      firstSeenAt: nowIso(),
      lastSeenAt: "",
      lastUpdatedAt: nowIso(),
    };
  }

  deviceRecord.nodeId = nodeId;
  deviceRecord.device = deviceName;
  deviceRecord.owner = owner;
  deviceRecord.claimedAt = deviceRecord.claimedAt || nowIso();
  deviceRecord.lastUpdatedAt = nowIso();
  cloudDevices.set(deviceRecord.registryKey, deviceRecord);
  clusterRecord.knownDevices = Array.from(cloudDevices.values()).filter((item) => item.clusterId === clusterId).length;
  clusterRecord.lastUpdatedAt = nowIso();
  cloudClusters.set(clusterId, clusterRecord);
  scheduleCloudRegistrySave();
  scheduleStateBroadcast();
  pushEvent("cloud-node-claimed", { clusterId, deviceId, nodeId, owner });
  return res.json({
    ok: true,
    cluster: cloudClusterSummary(clusterRecord),
    device: cloudDeviceSummary(deviceRecord),
    provisioningHint: {
      cluster_id: clusterId,
      device_id: deviceId,
      note: "目前為測試認領；正式版建議改為簽章 challenge/response。",
    },
  });
});

app.get("/api/cloud/devices/:clusterId/:deviceId/provision", (req, res) => {
  const clusterRecord = findCloudClusterRecord(req.params.clusterId);
  const deviceRecord = findCloudDeviceRecord(req.params.clusterId, req.params.deviceId);
  if (!clusterRecord) {
    return res.status(404).json({ ok: false, error: "cloud_cluster_not_found" });
  }
  if (!deviceRecord) {
    return res.status(404).json({ ok: false, error: "cloud_device_not_found" });
  }
  const provisioning = buildDeviceProvisioning(clusterRecord, deviceRecord);
  const bundle = buildDeviceProvisioningBundle(clusterRecord, deviceRecord);
  return res.json({
    ok: true,
    cluster: bundle.cluster,
    device: bundle.device,
    provisioning,
    formFields: buildProvisioningFormFields(provisioning),
    trustBundle: bundle.trustBundle,
    deviceBundle: bundle.deviceBundle,
    privateKeyExportEnabled: bundle.privateKeyExportEnabled,
  });
});

app.post("/api/cloud/devices/:clusterId/:deviceId/rotate-credential", (req, res) => {
  const clusterRecord = findCloudClusterRecord(req.params.clusterId);
  const deviceRecord = findCloudDeviceRecord(req.params.clusterId, req.params.deviceId);
  if (!clusterRecord) {
    return res.status(404).json({ ok: false, error: "cloud_cluster_not_found" });
  }
  if (!deviceRecord) {
    return res.status(404).json({ ok: false, error: "cloud_device_not_found" });
  }
  rotateDeviceCredential(clusterRecord, deviceRecord);
  cloudDevices.set(deviceRecord.registryKey, deviceRecord);
  scheduleCloudRegistrySave();
  pushEvent("cloud-device-credential-rotated", {
    clusterId: deviceRecord.clusterId,
    deviceId: deviceRecord.deviceId,
    keyId: deviceRecord.keyId,
    deviceFingerprint: deviceRecord.deviceFingerprint || "",
  });
  return res.json({
    ok: true,
    bundle: buildDeviceProvisioningBundle(clusterRecord, deviceRecord),
  });
});

app.post("/api/cloud/apply/:nodeId", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const clusterId = String(req.body.clusterId || "").trim();
  const deviceId = String(req.body.deviceId || "").trim();
  const node = resolveKnownNode(nodeId);
  const clusterRecord = findCloudClusterRecord(clusterId);
  const deviceRecord = findCloudDeviceRecord(clusterId, deviceId);

  if (!node || !node.ip || !node.online) {
    return res.status(404).json({ ok: false, error: "node_offline" });
  }
  if (!clusterRecord) {
    return res.status(404).json({ ok: false, error: "cloud_cluster_not_found" });
  }
  if (!deviceRecord) {
    return res.status(404).json({ ok: false, error: "cloud_device_not_found" });
  }

  try {
    await applyProvisioningToNode(node, clusterRecord, deviceRecord);
  } catch (error) {
    return res.status(502).json({ ok: false, error: "remote_apply_failed", detail: String(error.message || error) });
  }

  pushEvent("cloud-provision-applied", {
    nodeId,
    ip: node.ip,
    clusterId,
    deviceId,
  });
  return res.json({
    ok: true,
    nodeId,
    clusterId,
    deviceId,
    appliedFields: buildDeviceProvisioning(clusterRecord, deviceRecord),
  });
});

app.patch("/api/cloud/devices/:clusterId/:deviceId", (req, res) => {
  const record = findCloudDeviceRecord(req.params.clusterId, req.params.deviceId);
  if (!record) {
    return res.status(404).json({ ok: false, error: "cloud_device_not_found" });
  }
  if (typeof req.body.enabled === "boolean") {
    record.enabled = req.body.enabled;
  }
  record.lastUpdatedAt = nowIso();
  cloudDevices.set(record.registryKey, record);
  if (record.enabled === false) {
    pushEvent("cloud-device-disabled", {
      clusterId: record.clusterId,
      deviceId: record.deviceId,
      note: "mqtt-broker will reject this device on the next credential check",
    });
  }
  scheduleCloudRegistrySave();
  scheduleStateBroadcast();
  return res.json({ ok: true, device: cloudDeviceSummary(record) });
});

app.post("/api/cloud/devices/from-node", (req, res) => {
  const nodeId = Number(req.body.nodeId || 0);
  const node = resolveKnownNode(nodeId);
  const requestedClusterId = String(req.body.clusterId || "").trim();
  const clusterId = requestedClusterId || String(node?.cloudClusterId || "").trim() || String(node?.cluster || "").trim();
  const clusterRecord = findCloudClusterRecord(clusterId);
  const requestedDeviceId = String(req.body.deviceId || "").trim();
  const deviceId = requestedDeviceId || deriveFallbackDeviceId(node || { nodeId });
  const registryKey = `${clusterId}|${deviceId}`;
  const existing = cloudDevices.get(registryKey) || {};
  const keyId = String(req.body.keyId || existing.keyId || generateCloudKeyId()).trim();
  const seenAt = nowIso();
  let record;
  let deviceBundle;

  if (!node || !node.nodeId) {
    return res.status(404).json({ ok: false, error: "node_not_found" });
  }
  if (!clusterId) {
    return res.status(400).json({ ok: false, error: "bad_cluster_id" });
  }
  if (!clusterRecord) {
    return res.status(404).json({ ok: false, error: "cloud_cluster_not_found" });
  }
  if (!deviceId) {
    return res.status(400).json({ ok: false, error: "bad_device_id" });
  }

  deviceBundle = existing.devicePrivatePem ? {
    devicePublicPem: existing.devicePublicPem || "",
    devicePrivatePem: existing.devicePrivatePem || "",
    deviceFingerprint: existing.deviceFingerprint || "",
    certificatePayload: existing.certificatePayload || null,
    certificateSignature: existing.certificateSignature || "",
  } : buildDeviceCredentialBundle(clusterRecord, deviceId, node?.device || deviceId, keyId);

  record = {
    ...existing,
    registryKey,
    clusterId,
    deviceId,
    keyId,
    enabled: existing.enabled !== false,
    transport: Number(node.cloudTransport || clusterRecord.transport || CLOUD_TRANSPORT),
    nodeId: Number(node.nodeId || 0),
    device: node.device || existing.device || deviceId,
    clusterName: node.cluster || clusterRecord.clusterName || clusterId,
    ip: normalizeIp(node.ip),
    role: Number(node.role || 0),
    online: Number(node.online ? 1 : 0),
    deviceState: Number(node.deviceState || 0),
    relayMask: Number(node.relayMask || 0) >>> 0,
    sourceMasterNodeId: Number(node.sourceMasterNodeId || 0),
    backendKey: node.backendKey || clusterRecord.backendKey || "",
    devicePublicPem: deviceBundle.devicePublicPem,
    devicePrivatePem: deviceBundle.devicePrivatePem,
    deviceFingerprint: deviceBundle.deviceFingerprint,
    certificatePayload: deviceBundle.certificatePayload,
    certificateSignature: deviceBundle.certificateSignature,
    firstSeenAt: existing.firstSeenAt || seenAt,
    lastSeenAt: node.lastSeenAt || seenAt,
    owner: String(req.body.owner || existing.owner || DASHBOARD_USER).trim(),
    claimedAt: existing.claimedAt || seenAt,
  };

  cloudDevices.set(registryKey, record);
  syncCloudRegistryFromKnownNode({
    ...node,
    cloudClusterId: clusterId,
    cloudDeviceId: deviceId,
    cloudKeyId: keyId,
  });
  scheduleCloudRegistrySave();
  pushEvent("cloud-device-created-from-node", {
    nodeId: Number(node.nodeId || 0),
    clusterId,
    deviceId,
    keyId,
  });
  return res.json({ ok: true, device: cloudDeviceSummary(record) });
});

app.patch("/api/cloud/clusters/:clusterId", (req, res) => {
  const record = findCloudClusterRecord(req.params.clusterId);
  if (!record) {
    return res.status(404).json({ ok: false, error: "cloud_cluster_not_found" });
  }
  if (typeof req.body.enabled === "boolean") {
    record.enabled = req.body.enabled;
  }
  if (typeof req.body.transport !== "undefined") {
    const transport = Number(req.body.transport);
    if (!Number.isInteger(transport) || transport < 1 || transport > 255) {
      return res.status(400).json({ ok: false, error: "bad_cloud_transport" });
    }
    record.transport = transport;
  }
  if (typeof req.body.brokerHost !== "undefined") {
    const brokerHost = String(req.body.brokerHost || "").trim();
    if (!brokerHost || brokerHost.length > 253) {
      return res.status(400).json({ ok: false, error: "bad_broker_host" });
    }
    record.brokerHost = brokerHost;
  }
  if (typeof req.body.brokerPort !== "undefined") {
    const brokerPort = Number(req.body.brokerPort);
    if (!Number.isInteger(brokerPort) || brokerPort < 1 || brokerPort > 65535) {
      return res.status(400).json({ ok: false, error: "bad_broker_port" });
    }
    record.brokerPort = brokerPort;
  }
  record.lastUpdatedAt = nowIso();
  cloudClusters.set(record.clusterId, record);
  if (record.enabled === false) {
    pushEvent("cloud-cluster-disabled", {
      clusterId: record.clusterId,
      note: "mqtt-broker will reject devices in this cluster on the next credential check",
    });
  }
  scheduleCloudRegistrySave();
  scheduleStateBroadcast();
  return res.json({ ok: true, cluster: cloudClusterSummary(record) });
});

app.get("/clients", async (_req, res) => {
  try {
    const response = await fetch(`${BROKER_CONTROL_URL}/api/clients`);
    const data = await response.json();
    res.json(data);
  } catch (_) {
    res.json({ ok: true, clients: [] });
  }
});

app.get("/api/clients", async (_req, res) => {
  try {
    const response = await fetch(`${BROKER_CONTROL_URL}/api/clients`);
    const data = await response.json();
    res.json(data);
  } catch (_) {
    res.json({ ok: true, clients: [] });
  }
});

app.get("/events", (_req, res) => {
  res.json({
    ok: true,
    events,
  });
});

app.get("/api/events", (_req, res) => {
  res.json({
    ok: true,
    events,
  });
});

app.get("/api/config/:nodeId/relays", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const node = resolveKnownNode(nodeId);
  let data;

  if (!node || !node.online) {
    return res.json({ ok: true, relays: relayConfigSkeleton(), fallback: true });
  }

  try {
    data = await mqttQueryNode(nodeId, "/api/config/relays");
    if (!data || !data.ok) {
      return res.status(502).json({ ok: false, error: (data && data.error) || "remote_fetch_failed" });
    }
    return res.json(data);
  } catch (_error) {
    return res.json({
      ok: true,
      relays: relayConfigSkeleton(),
      fallback: true,
      broker_only: true,
      error: _error.message || "broker_query_unavailable",
    });
  }
});

app.post("/api/config/:nodeId/device-name", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const node = resolveKnownNode(nodeId);
  const payload = {};
  const command = publishNodeCommand(nodeId, "config/cluster", payload);

  if (!node || !node.ip || !node.online) {
    return res.status(404).json({ ok: false, error: "node_offline" });
  }
  if (typeof req.body.device_name !== "string" || !req.body.device_name.trim()) {
    return res.status(400).json({ ok: false, error: "bad_device_name" });
  }

  payload.device_name = req.body.device_name.trim();
  if (!command.ok) {
    return res.status(command.status || 400).json({ ok: false, error: command.error || "remote_config_failed" });
  }
  const publishResult = await command.publish({ waitForAck: true, timeoutMs: 8000 });
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  upsertKnownNode({
    ...node,
    device: req.body.device_name.trim(),
  });
  return res.json({
    ok: true,
    queued: true,
    completed: true,
    request_id: command.requestId,
    nodeId,
    device_name: req.body.device_name.trim(),
    ack: publishResult.ack || null,
  });
});

app.post("/api/config/:nodeId/relay", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const channel = Number(req.body.channel);
  const payload = {};
  let command;

  if (!Number.isInteger(channel) || channel < 1 || channel > 16) {
    return res.status(400).json({ ok: false, error: "bad_channel" });
  }

  payload.channel = channel;
  if (typeof req.body.name === "string") {
    payload.name = req.body.name;
  }
  if (typeof req.body.enabled !== "undefined") {
    payload.enabled = Number(req.body.enabled) ? 1 : 0;
  }
  if (typeof req.body.remember_state !== "undefined") {
    payload.remember_state = Number(req.body.remember_state) ? 1 : 0;
  }
  if (typeof req.body.boot_default_on !== "undefined") {
    payload.boot_default_on = Number(req.body.boot_default_on) ? 1 : 0;
  }
  if (typeof req.body.adc_channel !== "undefined") {
    /* Backward compatibility only.  MCU firmware now uses fixed CHn -> ADCn
       mapping, so the backend must not persist or forward user-selected ADC
       bindings from older UI/API clients. */
    void req.body.adc_channel;
  }
  payload.adc_channel = channel - 1;
  if (typeof req.body.protection_enabled !== "undefined") {
    payload.protection_enabled = Number(req.body.protection_enabled) ? 1 : 0;
  }
  if (typeof req.body.device_strategy !== "undefined") {
    const strategy = Number(req.body.device_strategy);
    if (![0, 1, 2].includes(strategy)) return res.status(400).json({ ok: false, error: "bad_strategy" });
    payload.device_strategy = strategy;
  }
  for (const field of [
    "overcurrent_milliamps",
    "trip_delay_ms",
    "reset_hysteresis_milliamps",
  ]) {
    if (typeof req.body[field] !== "undefined") {
      const fieldValue = Number(req.body[field]);
      const minimum = field === "overcurrent_milliamps" ? 100 : 0;
      const maximum = field === "trip_delay_ms" ? 600000 : 100000;
      if (!Number.isInteger(fieldValue) || fieldValue < minimum || fieldValue > maximum) {
        return res.status(400).json({ ok: false, error: `bad_${field}` });
      }
      payload[field] = fieldValue;
    }
  }

  command = publishNodeCommand(nodeId, "config/relay", payload);
  if (!command.ok) return res.status(command.status || 400).json({ ok: false, error: command.error });
  const publishResult = await command.publish({ waitForAck: true, timeoutMs: 8000 });
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  return res.json({ ok: true, queued: true, completed: true, request_id: command.requestId, nodeId, targetNodeId: command.targetNodeId, ack: publishResult.ack || null });
});

function validateRemoteNodeId(nodeId) {
  const nid = Number(nodeId);
  const node = resolveKnownNode(nid);
  if (!Number.isFinite(nid) || nid <= 0) return { error: "bad_node_id" };
  if (!node || !node.online) return { error: "node_offline", status: 404 };
  return { nid, node };
}

function appendProtectionConfigForm(form, body) {
  for (const field of [
    "cooldown_seconds",
    "retry_delay_seconds",
    "max_retry_count",
    "boot_delay_seconds",
    "learning_samples",
    "aging_update_hours",
    "aging_cap_percent",
    "stall_multiplier_x10",
  ]) {
    if (typeof body[field] !== "undefined") {
      const value = Number(body[field]);
      if (!Number.isInteger(value) || value < 0) return `bad_${field}`;
      form.set(field, String(value));
    }
  }
  return "";
}

app.get("/api/cluster/node/:nodeId/config/relays", async (req, res) => {
  const v = validateRemoteNodeId(req.params.nodeId);
  if (v.error) return res.status(v.status || 400).json({ ok: false, error: v.error });
  try {
    const data = await mqttQueryNode(v.nid, "/api/config/relays");
    return res.json(data);
  } catch (error) {
    return res.json({ ok: true, relays: relayConfigSkeleton(), fallback: true, broker_only: true, error: error.message || "broker_query_unavailable" });
  }
});

app.post("/api/cluster/node/:nodeId/config/relay", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const channel = Number(req.body.channel);
  const payload = {};
  let command;

  if (!Number.isInteger(channel) || channel < 1 || channel > 16) {
    return res.status(400).json({ ok: false, error: "bad_channel" });
  }

  payload.channel = channel;
  for (const field of ["name"]) {
    if (typeof req.body[field] === "string") payload[field] = req.body[field];
  }
  for (const field of ["enabled", "remember_state", "boot_default_on", "protection_enabled"]) {
    if (typeof req.body[field] !== "undefined") payload[field] = Number(req.body[field]) ? 1 : 0;
  }
  if (typeof req.body.device_strategy !== "undefined") {
    const strategy = Number(req.body.device_strategy);
    if (![0, 1, 2].includes(strategy)) return res.status(400).json({ ok: false, error: "bad_strategy" });
    payload.device_strategy = strategy;
  }
  if (typeof req.body.adc_channel !== "undefined") {
    /* Backward compatibility only; see /api/config/:nodeId/relay above. */
    void req.body.adc_channel;
  }
  payload.adc_channel = channel - 1;
  for (const field of ["overcurrent_milliamps", "trip_delay_ms", "reset_hysteresis_milliamps"]) {
    if (typeof req.body[field] !== "undefined") {
      const value = Number(req.body[field]);
      const minimum = field === "overcurrent_milliamps" ? 100 : 0;
      const maximum = field === "trip_delay_ms" ? 600000 : 100000;
      if (!Number.isInteger(value) || value < minimum || value > maximum) {
        return res.status(400).json({ ok: false, error: `bad_${field}` });
      }
      payload[field] = value;
    }
  }

  command = publishNodeCommand(nodeId, "config/relay", payload);
  if (!command.ok) return res.status(command.status || 400).json({ ok: false, error: command.error });
  const publishResult = await command.publish({ waitForAck: true, timeoutMs: 8000 });
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  return res.json({ ok: true, queued: true, completed: true, request_id: command.requestId, nodeId, targetNodeId: command.targetNodeId, ack: publishResult.ack || null });
});

app.get("/api/cluster/node/:nodeId/protection", async (req, res) => {
  const v = validateRemoteNodeId(req.params.nodeId);
  const node = resolveKnownNode(v.nid);
  if (v.error) return res.status(v.status || 400).json({ ok: false, error: v.error });
  try {
    return res.json(await mqttQueryNode(v.nid, "/api/protection"));
  } catch (error) {
    return res.json({ ...(node ? protectionFromLiveState(node) : protectionSkeleton()), broker_only: true, error: error.message || "broker_query_unavailable" });
  }
});

app.get("/api/cluster/node/:nodeId/config/protection", async (req, res) => {
  const v = validateRemoteNodeId(req.params.nodeId);
  if (v.error) return res.status(v.status || 400).json({ ok: false, error: v.error });
  try {
    return res.json(await mqttQueryNode(v.nid, "/api/config/protection"));
  } catch (error) {
    return res.json({ ...protectionConfigDefaults(), broker_only: true, error: error.message || "broker_query_unavailable" });
  }
});

app.post("/api/cluster/node/:nodeId/config/protection", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const payload = {};
  let formError;
  let command;
  formError = appendProtectionConfigForm({
    set(key, value) { payload[key] = Number(value); },
  }, req.body || {});
  if (formError) return res.status(400).json({ ok: false, error: formError });
  command = publishNodeCommand(nodeId, "config/protection", payload);
  if (!command.ok) return res.status(command.status || 400).json({ ok: false, error: command.error });
  const publishResult = await command.publish({ waitForAck: true, timeoutMs: 8000 });
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  return res.json({ ok: true, queued: true, completed: true, request_id: command.requestId, nodeId, targetNodeId: command.targetNodeId, ack: publishResult.ack || null });
});

app.post("/api/cluster/node/:nodeId/protection/:action", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const action = String(req.params.action || "");
  const suffix = action === "strategy" ? "protection/strategy" : `protection/${action}`;
  const payload = {};
  const channel = Number(req.body.channel);
  let command;
  if (!["unlock", "force-stop", "clear-learning", "strategy"].includes(action)) {
    return res.status(404).json({ ok: false, error: "bad_action" });
  }
  if (!Number.isInteger(channel) || channel < 1 || channel > 16) {
    return res.status(400).json({ ok: false, error: "bad_channel" });
  }
  payload.channel = channel;
  if (action === "strategy") {
    const strategy = Number(req.body.strategy);
    if (![0, 1, 2].includes(strategy)) return res.status(400).json({ ok: false, error: "bad_strategy" });
    payload.strategy = strategy;
  }
  command = publishNodeCommand(nodeId, suffix, payload);
  if (!command.ok) return res.status(command.status || 400).json({ ok: false, error: command.error });
  const publishResult = await command.publish({ waitForAck: true, timeoutMs: 8000 });
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  return res.json({ ok: true, queued: true, completed: true, request_id: command.requestId, nodeId, targetNodeId: command.targetNodeId, ack: publishResult.ack || null });
});

/* ?? Scheduler proxy APIs (dashboard ??backend ??MCU HTTP) ?? */
function validateSchedulerParams(nodeId, channel) {
  const nid = Number(nodeId);
  const ch = Number(channel);
  if (!Number.isFinite(nid) || nid <= 0) return { error: "bad_node_id" };
  if (!Number.isInteger(ch) || ch < 1 || ch > 16) return { error: "bad_channel" };
  return { nid, ch };
}
function validateWeekday(wd) {
  const w = Number(wd);
  if (!Number.isInteger(w) || w < 0 || w > 6) return null;
  return w;
}

function parseTargetWeekdays(value) {
  const out = [];
  const seen = new Set();
  for (const part of String(value || "").split(/[,\s]+/)) {
    if (!part) continue;
    const wd = validateWeekday(part);
    if (wd === null || seen.has(wd)) continue;
    seen.add(wd);
    out.push(wd);
  }
  return out;
}

function intervalsToSchedulerPayload(channel, weekday, intervals) {
  const safeIntervals = Array.isArray(intervals) ? intervals.slice(0, 8) : [];
  const payload = { channel, weekday, count: safeIntervals.length };
  safeIntervals.forEach((item, index) => {
    payload[`s${index}`] = Number(item.start_minute || item.start || 0);
    payload[`e${index}`] = Number(item.end_minute || item.end || 0);
  });
  return payload;
}

async function publishSchedulerDayCopy(nodeId, channel, sourceWeekday, targetWeekdaysValue) {
  const targetWeekdays = parseTargetWeekdays(targetWeekdaysValue);
  const week = await mqttQueryNode(nodeId, `/api/relay/channels/${channel}/schedule-week`);
  const sourceDay = (week.days || week.weekdays || []).find((item) => Number(item.weekday) === Number(sourceWeekday));
  const intervals = sourceDay && Array.isArray(sourceDay.intervals) ? sourceDay.intervals : [];
  const published = [];

  if (targetWeekdays.length === 0) {
    throw new Error("bad_target_weekdays");
  }
  for (const weekday of targetWeekdays) {
    const command = publishNodeCommand(
      nodeId,
      "scheduler/set",
      intervalsToSchedulerPayload(channel, weekday, intervals),
    );
    if (!command.ok) {
      throw new Error(command.error || "publish_command_failed");
    }
    const publishResult = await command.publish({ waitForAck: true, timeoutMs: 8000 });
    if (!publishResult.ok) {
      throw new Error(publishResult.error || "mqtt_ack_timeout");
    }
    published.push({ weekday, request_id: command.requestId, completed: true });
  }
  return { ok: true, queued: true, completed: true, source_weekday: sourceWeekday, copied: published };
}

app.get("/api/scheduler/:nodeId/:channel/status", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  const node = resolveKnownNode(v.nid);
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  try {
    const data = await mqttQueryNode(v.nid, `/api/relay/channels/${v.ch}/scheduler/status`);
    res.json(data);
  } catch (error) {
    res.json({ ...(node ? schedulerStatusFromLiveState(node, v.ch) : { ok: true, fallback: true, channel: v.ch, schedule_enabled: 0, pause_active: 0 }), broker_only: true, error: error.message || "broker_query_unavailable" });
  }
});

app.get("/api/scheduler/:nodeId/:channel/week", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  try {
    const data = await mqttQueryNode(v.nid, `/api/relay/channels/${v.ch}/schedule-week`);
    res.json(data);
  } catch (error) {
    res.json({ ok: true, fallback: true, broker_only: true, channel: v.ch, weekdays: [], error: error.message || "broker_query_unavailable" });
  }
});

app.post("/api/scheduler/:nodeId/:channel/enabled", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  let command;
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  command = publishNodeCommand(v.nid, "scheduler/enabled", {
    channel: v.ch,
    enabled: Number(req.body.enabled) ? 1 : 0,
  });
  if (!command.ok) return res.status(command.status || 400).json({ ok: false, error: command.error });
  const publishResult = await command.publish({ waitForAck: true, timeoutMs: 8000 });
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  res.json({ ok: true, queued: true, completed: true, request_id: command.requestId, ack: publishResult.ack || null });
});

app.post("/api/scheduler/:nodeId/:channel/pause", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  let command;
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  command = publishNodeCommand(v.nid, "scheduler/pause", {
    channel: v.ch,
    mode: Number(req.body.mode || 0),
    duration_seconds: Number(req.body.duration_seconds || 0),
    until: Number(req.body.until || 0),
    pause_blocks_auto_trigger: req.body.pause_blocks_auto_trigger ? 1 : 0,
  });
  if (!command.ok) return res.status(command.status || 400).json({ ok: false, error: command.error });
  const publishResult = await command.publish({ waitForAck: true, timeoutMs: 8000 });
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  res.json({ ok: true, queued: true, completed: true, request_id: command.requestId, ack: publishResult.ack || null });
});

app.post("/api/scheduler/:nodeId/:channel/resume", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  let command;
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  command = publishNodeCommand(v.nid, "scheduler/resume", { channel: v.ch });
  if (!command.ok) return res.status(command.status || 400).json({ ok: false, error: command.error });
  const publishResult = await command.publish({ waitForAck: true, timeoutMs: 8000 });
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  res.json({ ok: true, queued: true, completed: true, request_id: command.requestId, ack: publishResult.ack || null });
});

app.post("/api/scheduler/:nodeId/:channel/copy", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  const srcWd = validateWeekday(req.body.source_weekday);
  if (srcWd === null) return res.status(400).json({ ok: false, error: "bad_source_weekday" });
  try {
    res.json(await publishSchedulerDayCopy(v.nid, v.ch, srcWd, req.body.target_weekdays));
  } catch (error) {
    res.status(502).json({ ok: false, error: error.message || "broker_copy_failed" });
  }
});

app.put("/api/scheduler/:nodeId/:channel/week/:weekday", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  const wd = validateWeekday(req.params.weekday);
  const payload = { channel: v.ch, weekday: wd, count: Number(req.body.count || 0) };
  let command;
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  if (wd === null) return res.status(400).json({ ok: false, error: "bad_weekday" });
  for (let i = 0; i < 8; i++) {
    if (typeof req.body[`s${i}`] !== "undefined") payload[`s${i}`] = Number(req.body[`s${i}`]);
    if (typeof req.body[`e${i}`] !== "undefined") payload[`e${i}`] = Number(req.body[`e${i}`]);
  }
  command = publishNodeCommand(v.nid, "scheduler/set", payload);
  if (!command.ok) return res.status(command.status || 400).json({ ok: false, error: command.error });
  const publishResult = await command.publish({ waitForAck: true, timeoutMs: 8000 });
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  res.json({ ok: true, queued: true, completed: true, request_id: command.requestId, ack: publishResult.ack || null });
});

app.get("/api/cluster/node/:nodeId/relay/channels/:channel/scheduler/status", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  const node = resolveKnownNode(v.nid);
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  try {
    return res.json(await mqttQueryNode(v.nid, `/api/relay/channels/${v.ch}/scheduler/status`));
  } catch (error) {
    return res.json({ ...(node ? schedulerStatusFromLiveState(node, v.ch) : { ok: true, fallback: true, channel: v.ch, schedule_enabled: 0, pause_active: 0 }), broker_only: true, error: error.message || "broker_query_unavailable" });
  }
});

app.get("/api/cluster/node/:nodeId/relay/channels/:channel/schedule-week", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  try {
    return res.json(await mqttQueryNode(v.nid, `/api/relay/channels/${v.ch}/schedule-week`));
  } catch (error) {
    return res.json({ ok: true, fallback: true, broker_only: true, channel: v.ch, weekdays: [], error: error.message || "broker_query_unavailable" });
  }
});

app.put("/api/cluster/node/:nodeId/relay/channels/:channel/schedule-week/:weekday", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  const wd = validateWeekday(req.params.weekday);
  const payload = { channel: v.ch, weekday: wd, count: Number(req.body.count || 0) };
  let command;
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  if (wd === null) return res.status(400).json({ ok: false, error: "bad_weekday" });
  for (let i = 0; i < 8; i++) {
    if (typeof req.body[`s${i}`] !== "undefined") payload[`s${i}`] = Number(req.body[`s${i}`]);
    if (typeof req.body[`e${i}`] !== "undefined") payload[`e${i}`] = Number(req.body[`e${i}`]);
  }
  command = publishNodeCommand(v.nid, "scheduler/set", payload);
  if (!command.ok) return res.status(command.status || 400).json({ ok: false, error: command.error });
  const publishResult = await command.publish({ waitForAck: true, timeoutMs: 8000 });
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  return res.json({ ok: true, queued: true, completed: true, request_id: command.requestId, ack: publishResult.ack || null });
});

app.post("/api/cluster/node/:nodeId/relay/channels/:channel/schedule-week/:weekday/copy", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  const srcWd = validateWeekday(req.params.weekday);
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  if (srcWd === null) return res.status(400).json({ ok: false, error: "bad_source_weekday" });
  try {
    return res.json(await publishSchedulerDayCopy(v.nid, v.ch, srcWd, req.body.target_weekdays));
  } catch (error) {
    return res.status(502).json({ ok: false, error: error.message || "broker_copy_failed" });
  }
});

app.post("/api/cluster/node/:nodeId/relay/channels/:channel/scheduler/enabled", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  let command;
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  command = publishNodeCommand(v.nid, "scheduler/enabled", {
    channel: v.ch,
    enabled: Number(req.body.enabled) ? 1 : 0,
  });
  if (!command.ok) return res.status(command.status || 400).json({ ok: false, error: command.error });
  const publishResult = await command.publish({ waitForAck: true, timeoutMs: 8000 });
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  return res.json({ ok: true, queued: true, completed: true, request_id: command.requestId, ack: publishResult.ack || null });
});

app.post("/api/cluster/node/:nodeId/relay/channels/:channel/scheduler/pause", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  let command;
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  command = publishNodeCommand(v.nid, "scheduler/pause", {
    channel: v.ch,
    mode: Number(req.body.mode || 0),
    duration_seconds: Number(req.body.duration_seconds || 0),
    until: Number(req.body.until || 0),
    pause_blocks_auto_trigger: req.body.pause_blocks_auto_trigger ? 1 : 0,
  });
  if (!command.ok) return res.status(command.status || 400).json({ ok: false, error: command.error });
  const publishResult = await command.publish({ waitForAck: true, timeoutMs: 8000 });
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  return res.json({ ok: true, queued: true, completed: true, request_id: command.requestId, ack: publishResult.ack || null });
});

app.post("/api/cluster/node/:nodeId/relay/channels/:channel/scheduler/resume", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  let command;
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  command = publishNodeCommand(v.nid, "scheduler/resume", { channel: v.ch });
  if (!command.ok) return res.status(command.status || 400).json({ ok: false, error: command.error });
  const publishResult = await command.publish({ waitForAck: true, timeoutMs: 8000 });
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  return res.json({ ok: true, queued: true, completed: true, request_id: command.requestId, ack: publishResult.ack || null });
});

app.post("/send/:nodeId/relay", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const channel = Number(req.body.channel);
  const value = Number(req.body.value);

  if (!Number.isInteger(channel) || channel < 1 || channel > 16 || ![0, 1].includes(value)) {
    return res.status(400).json({ ok: false, error: "bad_relay_request" });
  }

  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) {
    return res.status(404).json({ ok: false, error: "node_not_found" });
  }
  const payload = {
    channel, value,
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: crypto.randomBytes(8).toString("hex"),
  };
  const publishResult = await mqttPublishCommandWithAck(nodeInfo.cluster, "relay", payload, 6000);
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  invalidateRemoteQueryCache(Number(payload.node_id));
  {
    const bit = 1 << (channel - 1);
    const oldMask = Number(nodeInfo.relayMask || 0) >>> 0;
    const nextMask = value ? (oldMask | bit) : (oldMask & ~bit);
    setKnownNodeRelayMask(Number(payload.node_id), nextMask);
  }
  queueRelayStateConfirm(Number(payload.node_id));
  pushEvent("send-relay", { nodeId, channel, value, targetNodeId: payload.node_id });
  return res.json({ ok: true, queued: true, completed: true, request_id: payload.request_id, ack: publishResult.ack || null });
});

app.post("/send/:nodeId/mask", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const mask = Number(req.body.mask);
  if (!Number.isInteger(mask) || mask < 0 || mask > 0xffff) {
    return res.status(400).json({ ok: false, error: "bad_mask" });
  }
  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) {
    return res.status(404).json({ ok: false, error: "node_not_found" });
  }
  const payload = {
    mask,
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: crypto.randomBytes(8).toString("hex"),
  };
  const publishResult = await mqttPublishCommandWithAck(nodeInfo.cluster, "mask", payload, 6000);
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  invalidateRemoteQueryCache(Number(payload.node_id));
  setKnownNodeRelayMask(Number(payload.node_id), mask);
  queueRelayStateConfirm(Number(payload.node_id));
  pushEvent("send-mask", { nodeId, mask, targetNodeId: payload.node_id });
  return res.json({ ok: true, queued: true, completed: true, request_id: payload.request_id, ack: publishResult.ack || null });
});

app.post("/send/:nodeId/all", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const value = Number(req.body.value);
  if (![0, 1].includes(value)) {
    return res.status(400).json({ ok: false, error: "bad_value" });
  }
  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) {
    return res.status(404).json({ ok: false, error: "node_not_found" });
  }
  const payload = {
    mask: (value === 1) ? 0xffff : 0,
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: crypto.randomBytes(8).toString("hex"),
  };
  const publishResult = await mqttPublishCommandWithAck(nodeInfo.cluster, "mask", payload, 6000);
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  invalidateRemoteQueryCache(Number(payload.node_id));
  setKnownNodeRelayMask(Number(payload.node_id), payload.mask);
  queueRelayStateConfirm(Number(payload.node_id));
  pushEvent("send-all", { nodeId, value, targetNodeId: payload.node_id });
  return res.json({ ok: true, queued: true, completed: true, request_id: payload.request_id, ack: publishResult.ack || null });
});

/* ?? New MQTT-based command routes ?? */
app.post("/send/:nodeId/auto-hold", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) return res.status(404).json({ ok: false, error: "node_not_found" });
  const payload = {
    channel: Number(req.body.channel || 1),
    target: Number(req.body.target || 0),
    duration_seconds: Number(req.body.duration_seconds || 0),
    session: Number(req.body.session || 0),
    command_ts: Number(req.body.command_ts || Math.floor(Date.now() / 1000)),
    reason: String(req.body.reason || "").slice(0, 63),
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: crypto.randomBytes(8).toString("hex"),
  };
  const publishResult = await mqttPublishCommandWithAck(nodeInfo.cluster, "auto-hold", payload, 6000);
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  invalidateRemoteQueryCache(Number(payload.node_id));
  return res.json({ ok: true, queued: true, completed: true, request_id: payload.request_id, ack: publishResult.ack || null });
});

app.post("/send/:nodeId/control", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) return res.status(404).json({ ok: false, error: "node_not_found" });
  const payload = {
    channel: Number(req.body.channel || 1),
    target: Number(req.body.target || 0),
    pause_mode: Number(req.body.pause_mode || 0),
    pause_duration_seconds: Number(req.body.pause_duration_seconds || 0),
    pause_until: Number(req.body.pause_until || 0),
    pause_blocks_auto_trigger: Number(req.body.pause_blocks_auto_trigger || 0),
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: crypto.randomBytes(8).toString("hex"),
  };
  const publishResult = await mqttPublishCommandWithAck(nodeInfo.cluster, "control", payload, 6000);
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  invalidateRemoteQueryCache(Number(payload.node_id));
  return res.json({ ok: true, queued: true, completed: true, request_id: payload.request_id, ack: publishResult.ack || null });
});

app.post("/send/:nodeId/scheduler/set", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) return res.status(404).json({ ok: false, error: "node_not_found" });
  const payload = {
    channel: Number(req.body.channel || 1),
    weekday: Number(req.body.weekday || 0),
    count: Number(req.body.count || 0),
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: crypto.randomBytes(8).toString("hex"),
  };
  for (let i = 0; i < 8; i++) {
    payload[`s${i}`] = Number(req.body[`s${i}`] || 0);
    payload[`e${i}`] = Number(req.body[`e${i}`] || 0);
  }
  const publishResult = await mqttPublishCommandWithAck(nodeInfo.cluster, "scheduler/set", payload, 8000);
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  invalidateRemoteQueryCache(Number(payload.node_id));
  return res.json({ ok: true, queued: true, completed: true, request_id: payload.request_id, ack: publishResult.ack || null });
});

app.post("/send/:nodeId/scheduler/enabled", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) return res.status(404).json({ ok: false, error: "node_not_found" });
  const payload = {
    channel: Number(req.body.channel || 1),
    enabled: Number(req.body.enabled || 0),
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: crypto.randomBytes(8).toString("hex"),
  };
  const publishResult = await mqttPublishCommandWithAck(nodeInfo.cluster, "scheduler/enabled", payload, 8000);
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  invalidateRemoteQueryCache(Number(payload.node_id));
  return res.json({ ok: true, queued: true, completed: true, request_id: payload.request_id, ack: publishResult.ack || null });
});

app.post("/send/:nodeId/scheduler/pause", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) return res.status(404).json({ ok: false, error: "node_not_found" });
  const payload = {
    channel: Number(req.body.channel || 1),
    mode: Number(req.body.mode || 0),
    duration_seconds: Number(req.body.duration_seconds || 0),
    until: Number(req.body.until || 0),
    pause_blocks_auto_trigger: Number(req.body.pause_blocks_auto_trigger || 0),
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: crypto.randomBytes(8).toString("hex"),
  };
  const publishResult = await mqttPublishCommandWithAck(nodeInfo.cluster, "scheduler/pause", payload, 8000);
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  invalidateRemoteQueryCache(Number(payload.node_id));
  return res.json({ ok: true, queued: true, completed: true, request_id: payload.request_id, ack: publishResult.ack || null });
});

app.post("/send/:nodeId/scheduler/resume", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) return res.status(404).json({ ok: false, error: "node_not_found" });
  const payload = {
    channel: Number(req.body.channel || 1),
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: makeRequestId(),
  };
  const publishResult = await mqttPublishCommandWithAck(nodeInfo.cluster, "scheduler/resume", payload, 8000);
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  invalidateRemoteQueryCache(Number(payload.node_id));
  return res.json({ ok: true, queued: true, completed: true, request_id: payload.request_id, ack: publishResult.ack || null });
});

app.post("/send/:nodeId/protection/force-stop", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const channel = Number(req.body.channel || 1);
  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) return res.status(404).json({ ok: false, error: "node_not_found" });
  if (!Number.isInteger(channel) || channel < 1 || channel > 16) {
    return res.status(400).json({ ok: false, error: "bad_channel" });
  }
  const payload = {
    channel,
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: crypto.randomBytes(8).toString("hex"),
  };
  const publishResult = await mqttPublishCommandWithAck(nodeInfo.cluster, "protection/force-stop", payload, 6000);
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  invalidateRemoteQueryCache(Number(payload.node_id));
  setKnownNodeRelayMask(Number(payload.node_id), (Number(nodeInfo.relayMask || 0) >>> 0) & ~(1 << (channel - 1)));
  return res.json({ ok: true, queued: true, completed: true, request_id: payload.request_id, ack: publishResult.ack || null });
});

app.post("/send/:nodeId/protection/unlock", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) return res.status(404).json({ ok: false, error: "node_not_found" });
  const payload = {
    channel: Number(req.body.channel || 1),
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: crypto.randomBytes(8).toString("hex"),
  };
  const publishResult = await mqttPublishCommandWithAck(nodeInfo.cluster, "protection/unlock", payload, 6000);
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  invalidateRemoteQueryCache(Number(payload.node_id));
  return res.json({ ok: true, queued: true, completed: true, request_id: payload.request_id, ack: publishResult.ack || null });
});

app.post("/send/:nodeId/protection/clear-learning", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) return res.status(404).json({ ok: false, error: "node_not_found" });
  const payload = {
    channel: Number(req.body.channel || 1),
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: crypto.randomBytes(8).toString("hex"),
  };
  const publishResult = await mqttPublishCommandWithAck(nodeInfo.cluster, "protection/clear-learning", payload, 6000);
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  invalidateRemoteQueryCache(Number(payload.node_id));
  return res.json({ ok: true, queued: true, completed: true, request_id: payload.request_id, ack: publishResult.ack || null });
});

app.post("/send/:nodeId/protection/strategy", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const channel = Number(req.body.channel || 1);
  const strategy = Number(req.body.strategy);
  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) return res.status(404).json({ ok: false, error: "node_not_found" });
  if (!Number.isInteger(channel) || channel < 1 || channel > 16) return res.status(400).json({ ok: false, error: "bad_channel" });
  if (![0, 1, 2].includes(strategy)) return res.status(400).json({ ok: false, error: "bad_strategy" });
  const payload = {
    channel,
    strategy,
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: makeRequestId(),
  };
  const publishResult = await mqttPublishCommandWithAck(nodeInfo.cluster, "protection/strategy", payload, 6000);
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  invalidateRemoteQueryCache(Number(payload.node_id));
  return res.json({ ok: true, queued: true, completed: true, request_id: payload.request_id, ack: publishResult.ack || null });
});

app.post("/send/:nodeId/config/protection", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const nodeInfo = resolveKnownNode(nodeId);
  const payload = {
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: makeRequestId(),
  };
  let formError;
  if (!nodeInfo || !nodeInfo.cluster) return res.status(404).json({ ok: false, error: "node_not_found" });
  formError = appendProtectionConfigForm({
    set(key, value) { payload[key] = Number(value); },
  }, req.body || {});
  if (formError) return res.status(400).json({ ok: false, error: formError });
  const publishResult = await mqttPublishCommandWithAck(nodeInfo.cluster, "config/protection", payload, 8000);
  if (!publishResult.ok) {
    return res.status(502).json({ ok: false, error: publishResult.error || "mqtt_ack_timeout" });
  }
  invalidateRemoteQueryCache(Number(payload.node_id));
  return res.json({ ok: true, queued: true, completed: true, request_id: payload.request_id, ack: publishResult.ack || null });
});

// ?? MCU Provisioning API ??????????????????????????????????????????
// Path A in the dual-path architecture: the MCU calls this over HTTPS
// (through Cloudflare) to obtain its PSK and broker configuration.
//
// MCU sends:   POST /api/provision
//              { device_id, cluster_id, device_fingerprint }
//
// Server replies with binding_secret, broker host/port, CA fingerprint,
// and CA root cert PEM so the MCU can verify the MQTT broker on connect.

app.post("/api/provision", (req, res) => {
  const clusterId = String(req.body.cluster_id || "").trim();
  const deviceId = String(req.body.device_id || "").trim();
  const deviceFingerprint = String(req.body.device_fingerprint || "").trim();

  if (!clusterId || !deviceId) {
    return res.status(400).json({ ok: false, error: "missing_cluster_or_device_id" });
  }

  const deviceRecord = findCloudDeviceRecord(clusterId, deviceId);
  if (!deviceRecord) {
    return res.status(404).json({ ok: false, error: "device_not_registered" });
  }

  if (!deviceRecord.enabled) {
    return res.status(403).json({ ok: false, error: "device_disabled" });
  }

  /* Devices must be claimed by a user before they can provision.
     An unclaimed device is registered but has no owner ??it cannot
     obtain a PSK and therefore cannot connect to the MQTT broker. */
  if (!deviceRecord.owner || String(deviceRecord.owner).trim() === "") {
    return res.status(403).json({ ok: false, error: "device_not_claimed" });
  }

  // Device fingerprint is trust-on-first-use for MCU provisioning.
  //
  // Older dashboard-created records may contain a backend-generated
  // deviceFingerprint from an unused device-key bundle.  That value is not the
  // MCU fingerprint and must not block the first MCU provision request.
  const boundFingerprint = String(deviceRecord.mcuDeviceFingerprint || "").trim();
  const fingerprintSource = String(deviceRecord.deviceFingerprintSource || "").trim();
  if (deviceFingerprint) {
    if (boundFingerprint) {
      if (deviceFingerprint !== boundFingerprint) {
        return res.status(403).json({ ok: false, error: "fingerprint_mismatch" });
      }
    } else if (fingerprintSource === "mcu" && deviceRecord.deviceFingerprint) {
      if (deviceFingerprint !== String(deviceRecord.deviceFingerprint || "")) {
        return res.status(403).json({ ok: false, error: "fingerprint_mismatch" });
      }
    } else {
      deviceRecord.mcuDeviceFingerprint = deviceFingerprint;
      deviceRecord.deviceFingerprintSource = "mcu";
      deviceRecord.fingerprintBoundAt = nowIso();
      deviceRecord.lastUpdatedAt = nowIso();
      cloudDevices.set(deviceRecord.registryKey, deviceRecord);
      scheduleCloudRegistrySave();
      pushEvent("device-fingerprint-bound", {
        clusterId,
        deviceId,
        fingerprint: deviceFingerprint,
      });
    }
  }

  const clusterRecord = findCloudClusterRecord(clusterId);
  if (!clusterRecord) {
    return res.status(500).json({ ok: false, error: "cluster_not_found" });
  }

  pushEvent("mcu-provision-request", {
    clusterId,
    deviceId,
    ip: req.ip,
    timestamp: new Date().toISOString(),
  });

  return res.json({
    ok: true,
    cluster_id: clusterId,
    device_id: deviceId,
    cloud_key_id: deviceRecord.keyId || "",
    cloud_transport: clusterRecord.transport || CLOUD_TRANSPORT,
    cloud_broker_host: clusterRecord.brokerHost || CLOUD_BROKER_HOST,
    cloud_broker_port: clusterRecord.brokerPort || CLOUD_BROKER_PORT,
    cloud_cluster_ca_fingerprint: clusterRecord.caFingerprint || "",
    cloud_cluster_ca_cert_pem: clusterRecord.caPublicPem || "",
    cloud_cluster_binding_secret: clusterRecord.bindingSecret || "",
    provisioning_revision: Number(clusterRecord.revision || 0),
    ca_revocation_tag: Number(clusterRecord.caRevocationTag || 0),
  });
});

// ?? Device Claim API ??????????????????????????????????????????????
// A user claims ownership of a registered device.  Until claimed,
// the device cannot obtain a PSK through /api/provision and cannot
// connect to the MQTT broker.

app.post("/api/devices/:deviceId/claim", (req, res) => {
  const deviceId = String(req.params.deviceId || "").trim();
  const owner = String(req.body.owner || "").trim();

  if (!deviceId || !owner) {
    return res.status(400).json({ ok: false, error: "missing_device_id_or_owner" });
  }

  let foundRecord = null;
  for (const [key, record] of Array.from(cloudDevices.entries())) {
    if (String(record.deviceId || "").trim() === deviceId) {
      foundRecord = record;
      break;
    }
  }
  if (!foundRecord) {
    return res.status(404).json({ ok: false, error: "device_not_found" });
  }

  const currentOwner = String(foundRecord.owner || "").trim();
  if (currentOwner && currentOwner !== owner) {
    return res.status(403).json({ ok: false, error: "device_already_claimed", currentOwner });
  }

  foundRecord.owner = owner;
  cloudDevices.set(foundRecord.registryKey, foundRecord);
  scheduleCloudRegistrySave();
  pushEvent("device-claimed", { deviceId, owner, timestamp: new Date().toISOString() });
  return res.json({ ok: true, deviceId, owner });
});

app.post("/api/cloud/devices/:clusterId/:deviceId/reset-fingerprint", (req, res) => {
  const clusterId = String(req.params.clusterId || "").trim();
  const deviceId = String(req.params.deviceId || "").trim();
  const record = findCloudDeviceRecord(clusterId, deviceId);

  if (!record) {
    return res.status(404).json({ ok: false, error: "cloud_device_not_found" });
  }

  delete record.mcuDeviceFingerprint;
  delete record.deviceFingerprintSource;
  delete record.fingerprintBoundAt;
  record.lastUpdatedAt = nowIso();
  cloudDevices.set(record.registryKey, record);
  scheduleCloudRegistrySave();
  scheduleStateBroadcast();
  pushEvent("device-fingerprint-reset", { clusterId, deviceId });
  return res.json({ ok: true, device: cloudDeviceSummary(record) });
});

app.post("/api/devices/:deviceId/reset-fingerprint", (req, res) => {
  const deviceId = String(req.params.deviceId || "").trim();
  let foundRecord = null;

  for (const record of cloudDevices.values()) {
    if (String(record.deviceId || "").trim() === deviceId) {
      foundRecord = record;
      break;
    }
  }
  if (!foundRecord) {
    return res.status(404).json({ ok: false, error: "device_not_found" });
  }

  delete foundRecord.mcuDeviceFingerprint;
  delete foundRecord.deviceFingerprintSource;
  delete foundRecord.fingerprintBoundAt;
  foundRecord.lastUpdatedAt = nowIso();
  cloudDevices.set(foundRecord.registryKey, foundRecord);
  scheduleCloudRegistrySave();
  scheduleStateBroadcast();
  pushEvent("device-fingerprint-reset", {
    clusterId: foundRecord.clusterId,
    deviceId: foundRecord.deviceId,
  });
  return res.json({ ok: true, deviceId: foundRecord.deviceId });
});

// ?? PSK Rotation ??????????????????????????????????????????????????
// Increments the cluster revision, generates a new binding secret,
// and stores the old secret for a 5-minute transition window.
app.post("/api/cloud/clusters/:clusterId/rotate-psk", (req, res) => {
  const clusterRecord = findCloudClusterRecord(req.params.clusterId);
  if (!clusterRecord) {
    return res.status(404).json({ ok: false, error: "cluster_not_found" });
  }

  const oldSecret = clusterRecord.bindingSecret || "";
  clusterRecord.bindingSecret = crypto.randomBytes(32).toString("base64url").slice(0, 43);
  clusterRecord.previousSecret = oldSecret;
  clusterRecord.revision = (Number(clusterRecord.revision || 0)) + 1;
  clusterRecord.secretRotatedAt = Date.now();

  cloudClusters.set(clusterRecord.clusterId, clusterRecord);
  scheduleCloudRegistrySave();
  pushEvent("psk-rotated", {
    clusterId: clusterRecord.clusterId,
    revision: clusterRecord.revision,
    timestamp: new Date().toISOString(),
  });
  return res.json({
    ok: true,
    revision: clusterRecord.revision,
    rotatedAt: clusterRecord.secretRotatedAt,
  });
});

loadKeyStore();
loadCloudRegistry();

/* Commands are published to the MQTT broker via its control API on port 4301 */

/* ????????????????????????????????????????????????????????????????
   Sensor Auto Hold API ??Grafana/backend sensor alerts ??MCU
   ????????????????????????????????????????????????????????????????*/

// activeSensors: key = "cluster|device|channel"
const activeSensors = new Map();

function makeSensorKey(clusterId, deviceId, channel) {
  return `${String(clusterId || "default")}|${String(deviceId || "")}|${Number(channel)}`;
}

// POST /api/sensor/alert
// Body: { cluster_id?, device_id?, channel, target, duration_seconds, reason }
// Creates or renews an Auto Hold session.  Only sends MQTT if this is
// a new session OR ?? minutes have passed since the last MQTT renew.
app.post("/api/sensor/alert", async (req, res) => {
  const channel        = Number(req.body.channel);
  const target         = Number(req.body.target ?? 1);
  const durationSec    = Number(req.body.duration_seconds ?? 600);
  const reason         = String(req.body.reason || "sensor alert").slice(0, 63);
  const clusterId      = String(req.body.cluster_id || "default");
  const deviceId       = String(req.body.device_id || "");
  const nowMs          = Date.now();
  let   nowUnix        = Math.floor(nowMs / 1000);

  if (!channel || channel < 1 || channel > 16) {
    return res.status(400).json({ ok: false, error: "invalid_channel" });
  }
  if (target !== 0 && target !== 1) {
    return res.status(400).json({ ok: false, error: "invalid_target" });
  }
  if (durationSec < 1 || durationSec > 1800) {
    return res.status(400).json({ ok: false, error: "invalid_duration", max: 1800 });
  }

  // Resolve which device to target
  if (!cloudClusters.has(clusterId)) {
    return res.status(404).json({ ok: false, error: "cluster_not_found" });
  }

  // Try explicit node_id, then cloudDevices, then knownNodes
  let targetNodeId = Number(req.body.node_id) || 0;
  if (!targetNodeId && deviceId) {
    const registryKey = `${clusterId}|${deviceId}`;
    if (cloudDevices.has(registryKey)) {
      const dev = cloudDevices.get(registryKey);
      if (dev && dev.nodeId) targetNodeId = dev.nodeId;
    }
  }
  if (!targetNodeId) {
    // Pick any device in this cluster with a known nodeId
    for (const [, dev] of cloudDevices) {
      if (String(dev.clusterId || "default") === clusterId && dev.nodeId) {
        targetNodeId = dev.nodeId;
        break;
      }
    }
  }
  // Fallback: try knownNodes
  if (!targetNodeId) {
    for (const [nid, node] of knownNodes) {
      targetNodeId = Number(nid);
      break;
    }
  }
  if (!targetNodeId) {
    return res.status(503).json({ ok: false, error: "no_online_device", hint: "pass node_id explicitly" });
  }

  const key = makeSensorKey(clusterId, deviceId, channel);
  let sensor = activeSensors.get(key);
  const isNewSession = !sensor || sensor.session === 0;

  // Ensure command_ts is always increasing (MCU rejects older ts)
  if (sensor && sensor.lastCommandTs && nowUnix <= sensor.lastCommandTs) {
    nowUnix = sensor.lastCommandTs + 1;
  }
  if (sensor) sensor.lastCommandTs = nowUnix;

  if (isNewSession) {
    // First alert ??create new session
    sensor = {
      session:     crypto.randomBytes(4).readUInt32BE(0) >>> 0,  // positive uint32
      channel,
      target,
      durationSec,
      reason,
      clusterId,
      deviceId,
      targetNodeId,
      lastMqttRenewMs: 0,        // force immediate send
      lastAlertMs: nowMs,
      createdAtMs: nowMs,
    };
    activeSensors.set(key, sensor);
  } else {
    // Existing session ??update tracking, maybe renew
    sensor.lastAlertMs = nowMs;
    sensor.reason = reason;
    activeSensors.set(key, sensor);
  }

  // Decide whether to send MQTT now
  const elapsedSinceRenew = (nowMs - sensor.lastMqttRenewMs);
  const shouldSendMqtt = isNewSession || (elapsedSinceRenew >= 5 * 60 * 1000);

  let mqttResult = null;
  if (shouldSendMqtt) {
    const nodeInfo = resolveKnownNode(targetNodeId);
    const clusterName = nodeInfo?.cluster || clusterId;
    const payload = {
      channel,
      target,
      duration_seconds: durationSec,
      session: sensor.session,
      command_ts: nowUnix,
      reason,
      node_id: targetNodeId,
      request_id: crypto.randomBytes(8).toString("hex"),
    };
    const mqttOk = await mqttPublishCommand(clusterName, "auto-hold", payload);
    mqttResult = mqttOk ? "sent" : "mqtt_unavailable";
    if (mqttOk) {
      invalidateRemoteQueryCache(targetNodeId);
      sensor.lastMqttRenewMs = nowMs;
      sensor.targetNodeId = targetNodeId;
    }
  } else {
    mqttResult = "deferred";  // <5min since last MQTT, skip
  }

  return res.json({
    ok: true,
    session: sensor.session,
    channel,
    mqtt: mqttResult,
    is_new_session: isNewSession,
    next_renew_available_ms: sensor.lastMqttRenewMs + 5 * 60 * 1000,
  });
});

// POST /api/sensor/recover
// Body: { cluster_id?, device_id?, channel }
// Ends the active sensor Auto Hold for this channel.
app.post("/api/sensor/recover", async (req, res) => {
  const channel   = Number(req.body.channel);
  const clusterId = String(req.body.cluster_id || "default");
  const deviceId  = String(req.body.device_id || "");
  const key       = makeSensorKey(clusterId, deviceId, channel);
  const sensor    = activeSensors.get(key);

  if (!sensor || sensor.session === 0) {
    return res.json({ ok: true, cleared: false, reason: "no_active_session" });
  }

  const nowUnix = Math.floor(Date.now() / 1000);
  const nodeInfo = resolveKnownNode(sensor.targetNodeId);
  const clusterName = nodeInfo?.cluster || clusterId;
  const payload = {
    channel,
    target: sensor.target,       // must match creation target
    duration_seconds: 0,         // clear
    session: sensor.session,
    command_ts: nowUnix,
    reason: "sensor_clear",
    node_id: sensor.targetNodeId,
    request_id: crypto.randomBytes(8).toString("hex"),
  };

  const mqttOk = await mqttPublishCommand(clusterName, "auto-hold", payload);
  if (mqttOk) invalidateRemoteQueryCache(sensor.targetNodeId);
  activeSensors.delete(key);

  return res.json({
    ok: true,
    cleared: true,
    session: sensor.session,
    channel,
    mqtt: mqttOk ? "sent" : "mqtt_unavailable",
  });
});

// GET /api/sensor/status ??list all active sensor sessions
app.get("/api/sensor/status", (_req, res) => {
  const list = [];
  for (const [key, s] of activeSensors) {
    list.push({
      key,
      channel: s.channel,
      target: s.target,
      session: s.session,
      duration_sec: s.durationSec,
      reason: s.reason,
      last_alert_ms: s.lastAlertMs,
      last_mqtt_renew_ms: s.lastMqttRenewMs,
      created_ms: s.createdAtMs,
    });
  }
  return res.json({ ok: true, active: list.length, sensors: list });
});

app.listen(HTTP_PORT, HTTP_HOST, () => {
  console.log(`[backend-test] http listening on ${HTTP_HOST}:${HTTP_PORT}`);
});

setInterval(syncKnownNodesFromBroker, 5000);
syncKnownNodesFromBroker();

setInterval(scanStaleKnownNodes, KNOWN_NODE_SCAN_MS);
