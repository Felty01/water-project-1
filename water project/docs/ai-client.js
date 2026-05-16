/** ИИ в браузере (без Python) — те же правила, что на сервере */
function buildAiAnalysis(snap) {
  const wc = snap.wave_class || "calm";
  const temp = snap.temp_c;
  const water = snap.water || "dry";
  const mpu = !!snap.mpu_ok;
  const gps = snap.gps_status || "offline";
  const tilt = snap.tilt_max_deg || 0;
  const gyro = snap.gyro_dps || 0;
  const waveCm = snap.wave_cm || 0;
  const speed = snap.wave_speed_ms || 0;
  const motion = !!snap.motion;

  let risk = "low";
  const bullets = [];
  let summary;
  let recommendation;

  if (!mpu) {
    risk = "high";
    summary = "Датчик движения (MPU) не отвечает — станция не может оценивать волны.";
    recommendation = "Проверьте проводку I2C: SDA→D3, SCL→D4, питание 3.3 V.";
    bullets.push("MPU: ошибка связи");
  } else if (wc === "high") {
    risk = "high";
    summary = `Сильный наклон (до ${tilt.toFixed(0)}°). Класс: высокие. Гиро ${gyro.toFixed(0)}°/с.`;
    recommendation = "Снизьте качку или закрепите макет.";
    bullets.push(`Наклон макс.: ${tilt.toFixed(1)}°`);
  } else if (wc === "medium") {
    risk = "medium";
    summary = `Умеренное волнение, наклон до ${tilt.toFixed(0)}°, ~${waveCm.toFixed(0)} см.`;
    recommendation = "Продолжайте мониторинг.";
  } else if (wc === "low") {
    risk = "low";
    summary = `Слабое волнение, наклон до ${tilt.toFixed(0)}°.`;
    recommendation = "Условия спокойные.";
  } else {
    summary = "Штиль: значимых колебаний не обнаружено.";
    recommendation = "Покачайте плату для демонстрации.";
  }

  if (motion && wc === "calm") {
    bullets.push("Кратковременное движение без смены класса.");
  }
  if (water === "liquid") {
    bullets.push("Обнаружена жидкость.");
    if (risk === "low") risk = "medium";
  } else {
    bullets.push("Уровень: сухо.");
  }
  if (temp != null) {
    bullets.push(`Температура: ${Number(temp).toFixed(1)} °C.`);
  }
  if (gps === "offline") bullets.push("GPS: нет связи.");
  else if (gps === "waiting") bullets.push(`GPS: ожидание спутников (${snap.gps_sats || 0}).`);
  else if (gps === "fix") bullets.push("GPS: координаты есть.");
  if (speed > 0.1 && wc !== "calm") {
    bullets.push(`Скорость волны ~${speed.toFixed(2)} m/s.`);
  }

  const riskLabels = {
    low: "Низкий риск",
    medium: "Средний риск",
    high: "Высокий риск",
  };

  return {
    engine: "local_rules",
    engine_label: "ИИ в браузере (онлайн MQTT)",
    risk,
    risk_label: riskLabels[risk] || risk,
    summary,
    recommendation,
    bullets: bullets.slice(0, 5),
  };
}
