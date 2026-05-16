const MQTT_TOPIC = "waterproject/alexl/station/data";
const MQTT_WS = "wss://broker.hivemq.com:8884/mqtt";

const WAVE_LABELS = {
  calm: "Штиль / нет волн",
  low: "Низкие",
  medium: "Средние",
  high: "Высокие",
};

const $ = (id) => document.getElementById(id);

function fmt(v, digits = 1) {
  if (v === null || v === undefined || Number.isNaN(v)) return "—";
  return Number(v).toFixed(digits);
}

function applyData(d) {
  const live = d.connected && d.wave_class !== undefined;
  const badge = $("connBadge");
  if (live && d.wifi_ip) {
    badge.textContent = "WiFi · " + d.wifi_ip;
  } else if (live && d.link === "mqtt") {
    badge.textContent = "Онлайн · MQTT";
  } else if (live && d.link === "wifi") {
    badge.textContent = "Подключено · WiFi";
  } else {
    badge.textContent = live ? "Подключено" : "Ожидание данных с платы…";
  }
  badge.className = "hero-badge " + (live ? "live" : "wait");

  const wc = d.wave_class || "calm";
  $("waveCard").className = "card card-hero wave-card " + wc;

  $("waveClass").textContent = d.wave_class_label || WAVE_LABELS[wc] || wc;
  $("waveClassSub").textContent =
    "класс по наклону · макс. " + fmt(d.tilt_max_deg) + "°";

  $("waveHeight").textContent = fmt(d.wave_cm);
  $("waveSpeed").textContent = fmt(d.wave_speed_ms, 2);
  $("tiltMax").textContent = fmt(d.tilt_max_deg);

  const pitch = d.pitch_deg ?? 0;
  const roll = d.roll_deg ?? 0;
  $("tiltText").textContent = `pitch ${fmt(pitch)}° · roll ${fmt(roll)}°`;
  $("gyroVal").textContent = fmt(d.gyro_dps);
  $("motionVal").textContent = d.motion ? "да" : "нет";

  const maxTilt = 45;
  const bx = Math.max(-1, Math.min(1, roll / maxTilt)) * 42;
  const by = Math.max(-1, Math.min(1, pitch / maxTilt)) * 42;
  $("tiltBubble").style.transform =
    `translate(calc(-50% + ${bx}px), calc(-50% + ${by}px))`;

  $("tempVal").textContent =
    d.temp_c != null ? `${fmt(d.temp_c)} °C` : "нет данных";
  $("waterVal").textContent = d.water === "liquid" ? "Вода" : "Сухо";

  const gpsMap = {
    fix: "Фиксация",
    waiting: "Ожидание спутников",
    offline: "Нет связи",
  };
  $("gpsStatus").textContent = gpsMap[d.gps_status] || d.gps_status || "—";
  $("gpsSats").textContent = d.gps_sats ?? "—";
  if (d.gps_lat != null && d.gps_lng != null) {
    $("gpsCoords").textContent = `${d.gps_lat}, ${d.gps_lng}`;
  } else {
    $("gpsCoords").textContent = "—";
  }

  const badges = $("sensorBadges");
  badges.innerHTML = "";
  [
    ["MPU", d.mpu_ok],
    ["GPS", d.gps_ok],
    ["Темп.", d.temp_c != null],
  ].forEach(([name, ok]) => {
    const el = document.createElement("span");
    el.className = "badge " + (ok ? "ok" : "bad");
    el.textContent = `${name}: ${ok ? "OK" : "—"}`;
    badges.appendChild(el);
  });

  if (d.updated_at) {
    $("lastUpdate").textContent =
      "Обновлено: " + new Date(d.updated_at * 1000).toLocaleTimeString("ru-RU");
  }

  const ai = d.ai || (typeof buildAiAnalysis === "function" ? buildAiAnalysis(d) : null);
  if (ai) applyAi(ai);
}

function applyAi(ai) {
  $("aiRisk").textContent = ai.risk_label || "—";
  $("aiRisk").className = "ai-risk " + (ai.risk || "low");
  $("aiEngine").textContent = ai.engine_label || "ИИ-модуль";
  $("aiSummary").textContent = ai.summary || "—";
  $("aiRecommendation").textContent = ai.recommendation || "—";
  const list = $("aiBullets");
  list.innerHTML = "";
  (ai.bullets || []).forEach((text) => {
    const li = document.createElement("li");
    li.textContent = text;
    list.appendChild(li);
  });
  $("aiUpdated").textContent =
    "ИИ обновлён: " + new Date().toLocaleTimeString("ru-RU");
}

async function pollLive() {
  try {
    const res = await fetch("/api/live", { cache: "no-store" });
    applyData(await res.json());
    return true;
  } catch {
    return false;
  }
}

function startServerMode() {
  pollLive();
  setInterval(pollLive, 2000);
  if (typeof EventSource !== "undefined") {
    const es = new EventSource("/api/stream");
    es.onmessage = (ev) => {
      try {
        applyData(JSON.parse(ev.data));
      } catch (e) {
        console.warn(e);
      }
    };
  }
}

function startMqttBrowserMode() {
  if (typeof mqtt === "undefined") {
    $("connBadge").textContent = "Ошибка: нет MQTT библиотеки";
    return;
  }

  $("connBadge").textContent = "Подключение к облаку MQTT…";
  $("connBadge").className = "hero-badge wait";

  const client = mqtt.connect(MQTT_WS, {
    clientId: "web_" + Math.random().toString(16).slice(2, 10),
    clean: true,
    reconnectPeriod: 3000,
  });

  client.on("connect", () => {
    client.subscribe(MQTT_TOPIC, (err) => {
      if (!err) {
        $("connBadge").textContent = "Онлайн · ждём плату Lisa";
        $("connBadge").className = "hero-badge wait";
      }
    });
  });

  client.on("message", (_topic, payload) => {
    try {
      const data = JSON.parse(payload.toString());
      data.connected = true;
      data.link = "mqtt";
      data.updated_at = Date.now() / 1000;
      applyData(data);
    } catch (e) {
      console.warn("JSON", e);
    }
  });

  client.on("error", () => {
    $("connBadge").textContent = "MQTT ошибка";
    $("connBadge").className = "hero-badge wait";
  });
}

async function refreshAi() {
  const btn = $("aiBtn");
  btn.disabled = true;
  try {
    const res = await fetch("/api/ai/analyze");
    applyAi(await res.json());
  } catch {
    const res = await fetch("/api/live", { cache: "no-store" }).catch(() => null);
    if (res && res.ok) {
      const d = await res.json();
      if (typeof buildAiAnalysis === "function") applyAi(buildAiAnalysis(d));
    } else {
      $("aiSummary").textContent =
        "В онлайн-режиме ИИ обновляется автоматически с каждым сообщением MQTT.";
    }
  }
  btn.disabled = false;
}

async function boot() {
  const hasServer = await pollLive();
  if (hasServer) {
    startServerMode();
  } else {
    startMqttBrowserMode();
  }
}

$("aiBtn").addEventListener("click", refreshAi);
boot();
