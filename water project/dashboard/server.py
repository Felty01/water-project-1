"""
Сервер дашборда Water Project.

USB:    python server.py --port COM5
WiFi:   python server.py --wifi http://192.168.0.217   (самая локальная сеть)
MQTT:   python server.py --mqtt                        (из любой сети / города)

Браузер: http://127.0.0.1:5000
"""

from __future__ import annotations

import argparse
import json
import os
import threading
import time
from pathlib import Path

import serial
from flask import Flask, Response, jsonify, request, send_from_directory

app = Flask(__name__, static_folder="static", static_url_path="")

_latest: dict = {"connected": False, "updated_at": None}
_lock = threading.Lock()
_serial: serial.Serial | None = None
_feed_started = False
_feed_lock = threading.Lock()


def _set_latest(payload: dict) -> None:
    with _lock:
        _latest.clear()
        _latest.update(payload)
        _latest["connected"] = True
        _latest["updated_at"] = time.time()


def _serial_loop(port: str, baud: int) -> None:
    global _serial
    while True:
        try:
            _serial = serial.Serial(port, baud, timeout=1)
            print(f"[OK] Serial {port} @ {baud}")
            break
        except serial.SerialException as exc:
            print(f"[...] Ожидание порта {port}: {exc}")
            time.sleep(2)

    buf = ""
    while True:
        try:
            chunk = _serial.read(_serial.in_waiting or 1)
            if not chunk:
                continue
            buf += chunk.decode("utf-8", errors="ignore")
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                line = line.strip()
                if line.startswith("@DATA "):
                    try:
                        data = json.loads(line[6:])
                        _set_latest(data)
                    except json.JSONDecodeError as exc:
                        print("[warn] JSON:", exc, line[:80])
        except Exception as exc:
            print("[err] serial:", exc)
            time.sleep(1)


def _wifi_loop(base_url: str) -> None:
    import urllib.error
    import urllib.request

    url = base_url.rstrip("/") + "/data"
    print(f"[OK] WiFi режим: {url}")

    while True:
        try:
            req = urllib.request.Request(
                url,
                headers={"Accept": "application/json"},
            )
            with urllib.request.urlopen(req, timeout=4) as resp:
                data = json.loads(resp.read().decode("utf-8"))
            data["connected"] = True
            data["link"] = "wifi"
            _set_latest(data)
        except Exception as exc:
            with _lock:
                _latest.clear()
                _latest["connected"] = False
                _latest["link"] = "wifi"
                _latest["updated_at"] = time.time()
            print("[wifi] ожидание ESP:", exc)
            time.sleep(2)
            time.sleep(1.2)


def _mqtt_loop(broker: str, port: int, topic: str) -> None:
    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        print("Установи: pip install paho-mqtt")
        return

    print(f"[MQTT] broker={broker}:{port} topic={topic}")

    def on_connect(client, userdata, flags, rc, properties=None):
        if rc == 0:
            print(f"[MQTT] подключено, подписка: {topic}")
            client.subscribe(topic, qos=0)
        else:
            print(f"[MQTT] ошибка подключения rc={rc}")

    def on_message(client, userdata, msg):
        try:
            data = json.loads(msg.payload.decode("utf-8"))
            data["connected"] = True
            data["link"] = "mqtt"
            _set_latest(data)
        except json.JSONDecodeError as exc:
            print("[MQTT] JSON:", exc)

    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except (AttributeError, TypeError):
        client = mqtt.Client()

    client.on_connect = on_connect
    client.on_message = on_message

    while True:
        try:
            client.connect(broker, port, keepalive=60)
            client.loop_forever()
        except Exception as exc:
            print("[MQTT] переподключение:", exc)
            time.sleep(3)


@app.route("/")
def index() -> Response:
    return send_from_directory(app.static_folder, "index.html")


@app.route("/health")
def health() -> Response:
    return "ok", 200


@app.route("/api/live")
def api_live() -> Response:
    with _lock:
        return jsonify(_payload_with_ai(dict(_latest)))


def _payload_with_ai(payload: dict) -> dict:
    out = dict(payload)
    if out.get("connected"):
        out["ai"] = build_ai_analysis(out)
    else:
        out["ai"] = {
            "engine": "local_rules",
            "engine_label": "Локальный ИИ-модуль",
            "risk": "low",
            "risk_label": "Ожидание",
            "summary": "Нет данных. Запусти: python server.py --mqtt (плата должна быть в Wi‑Fi AlexL).",
            "recommendation": "Lisa: включи плату дома. Ты: server.py --mqtt с любой сети.",
            "bullets": [],
        }
    return out


@app.route("/api/stream")
def api_stream() -> Response:
    def generate():
        last_ts = None
        while True:
            with _lock:
                payload = _payload_with_ai(dict(_latest))
            ts = payload.get("updated_at")
            if ts != last_ts:
                last_ts = ts
                yield f"data: {json.dumps(payload, ensure_ascii=False)}\n\n"
            time.sleep(0.4)

    return Response(generate(), mimetype="text/event-stream")


@app.route("/api/ai/analyze")
@app.route("/api/ai/hint", methods=["GET", "POST"])
def api_ai_analyze() -> Response:
    """ИИ-блок для сайта: анализ текущих показаний датчиков."""
    with _lock:
        snap = dict(_latest)
    analysis = build_ai_analysis(snap) if snap.get("connected") else _payload_with_ai(snap)["ai"]
    return jsonify(analysis)


def build_ai_analysis(snap: dict) -> dict:
    api_key = os.environ.get("OPENAI_API_KEY", "").strip()
    base = _rule_based_analysis(snap)
    if not api_key:
        return base

    try:
        import urllib.request

        body = {
            "model": "gpt-4o-mini",
            "messages": [
                {
                    "role": "system",
                    "content": (
                        "Ты ИИ-модуль мониторинга водной станции. Ответь строго JSON: "
                        '{"summary":"2 предложения","recommendation":"1 предложение",'
                        '"risk":"low|medium|high","bullets":["пункт1","пункт2"]}. '
                        "По-русски."
                    ),
                },
                {
                    "role": "user",
                    "content": json.dumps(snap, ensure_ascii=False),
                },
            ],
            "max_tokens": 280,
        }
        req = urllib.request.Request(
            "https://api.openai.com/v1/chat/completions",
            data=json.dumps(body).encode("utf-8"),
            headers={
                "Authorization": f"Bearer {api_key}",
                "Content-Type": "application/json",
            },
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=30) as resp:
            result = json.loads(resp.read().decode())
        raw = result["choices"][0]["message"]["content"].strip()
        if raw.startswith("```"):
            raw = raw.split("\n", 1)[-1].rsplit("```", 1)[0]
        parsed = json.loads(raw)
        risk = parsed.get("risk", base["risk"])
        return {
            "engine": "openai",
            "engine_label": "ИИ OpenAI (GPT)",
            "risk": risk,
            "risk_label": _risk_label(risk),
            "summary": parsed.get("summary", base["summary"]),
            "recommendation": parsed.get("recommendation", base["recommendation"]),
            "bullets": parsed.get("bullets", base["bullets"])[:4],
        }
    except Exception:
        base["engine_label"] = "Локальный ИИ (OpenAI недоступен)"
        return base


def _risk_label(risk: str) -> str:
    return {"low": "Низкий риск", "medium": "Средний риск", "high": "Высокий риск"}.get(
        risk, "Неизвестно"
    )


def _rule_based_analysis(snap: dict) -> dict:
    wc = snap.get("wave_class", "calm")
    temp = snap.get("temp_c")
    water = snap.get("water", "dry")
    mpu = snap.get("mpu_ok", False)
    gps = snap.get("gps_status", "offline")
    tilt = snap.get("tilt_max_deg") or 0
    gyro = snap.get("gyro_dps") or 0
    wave_cm = snap.get("wave_cm") or 0
    speed = snap.get("wave_speed_ms") or 0
    motion = snap.get("motion", False)

    risk = "low"
    bullets: list[str] = []

    if not mpu:
        risk = "high"
        summary = "Датчик движения (MPU) не отвечает — станция не может оценивать волны."
        recommendation = "Проверьте проводку I2C: SDA→D3, SCL→D4, питание 3.3 V."
        bullets.append("MPU: ошибка связи")
    elif wc == "high":
        risk = "high"
        summary = (
            f"Зафиксирован сильный наклон платформы (до {tilt:.0f}°). "
            f"Класс волн: высокие. Гироскоп {gyro:.0f}°/с."
        )
        recommendation = "Снизьте качку или закрепите макет; при реальном плавании — осторожность."
        bullets.append(f"Наклон макс.: {tilt:.1f}°")
    elif wc == "medium":
        risk = "medium"
        summary = (
            f"Умеренное волнение: класс «средние», наклон до {tilt:.0f}°, "
            f"оценочная высота ~{wave_cm:.0f} см."
        )
        recommendation = "Продолжайте мониторинг; параметры в рабочем диапазоне."
    elif wc == "low":
        risk = "low"
        summary = f"Слабое волнение (низкие волны), наклон до {tilt:.0f}°."
        recommendation = "Условия спокойные; подходит для калибровки датчиков."
    else:
        summary = "Штиль: значимых колебаний не обнаружено, платформа стабильна."
        recommendation = "Для демонстрации слегка покачайте плату — ИИ обновит класс волн."

    if motion and wc == "calm":
        bullets.append("Есть движение, но класс ещё «штиль» — кратковременная вибрация.")

    if water == "liquid":
        bullets.append("Оптический датчик: обнаружена жидкость.")
        if risk == "low":
            risk = "medium"
    else:
        bullets.append("Уровень воды: сухо.")

    if temp is not None:
        bullets.append(f"Температура воды/воздуха: {temp:.1f} °C.")
        if temp < 5:
            risk = "medium"
            bullets.append("Низкая температура — возможное обледенение датчиков.")
        elif temp > 35:
            risk = "medium"
            bullets.append("Высокая температура — проверьте размещение DS18B20.")

    if gps == "offline":
        bullets.append("GPS: нет связи (проверьте TX→D7).")
    elif gps == "waiting":
        bullets.append(f"GPS: ожидание спутников ({snap.get('gps_sats', 0)} шт.).")
    elif gps == "fix":
        bullets.append("GPS: координаты получены.")

    if speed > 0.1 and wc != "calm":
        bullets.append(f"Оценочная скорость волны: {speed:.2f} m/s.")

    if not snap.get("connected"):
        summary = "Нет связи с платой."
        recommendation = "Подключите USB (--port COM) или WiFi (--wifi http://IP/data)."
        risk = "low"
        bullets = []

    return {
        "engine": "local_rules",
        "engine_label": "Локальный ИИ-модуль (экспертные правила)",
        "risk": risk,
        "risk_label": _risk_label(risk),
        "summary": summary,
        "recommendation": recommendation,
        "bullets": bullets[:5],
    }


def start_mqtt_feed(
    broker: str = "broker.hivemq.com",
    port: int = 1883,
    topic: str = "waterproject/alexl/station/data",
) -> None:
    global _feed_started
    with _feed_lock:
        if _feed_started:
            return
        _feed_started = True
    threading.Thread(target=_mqtt_loop, args=(broker, port, topic), daemon=True).start()


def start_wifi_feed(base_url: str) -> None:
    global _feed_started
    with _feed_lock:
        if _feed_started:
            return
        _feed_started = True
    threading.Thread(target=_wifi_loop, args=(base_url,), daemon=True).start()


def start_serial_feed(port: str, baud: int = 115200) -> None:
    global _feed_started
    with _feed_lock:
        if _feed_started:
            return
        _feed_started = True
    threading.Thread(target=_serial_loop, args=(port, baud), daemon=True).start()


def init_cloud_deploy() -> None:
    """Автозапуск MQTT при деплое (Render / Railway и т.д.)."""
    if os.environ.get("CLOUD_DEPLOY", "1").lower() in ("0", "false", "no"):
        return
    start_mqtt_feed(
        os.environ.get("MQTT_BROKER", "broker.hivemq.com"),
        int(os.environ.get("MQTT_PORT", "1883")),
        os.environ.get("MQTT_TOPIC", "waterproject/alexl/station/data"),
    )


# Облако: если задан PORT (Render), подключаемся к MQTT при старте gunicorn
if os.environ.get("PORT"):
    init_cloud_deploy()


def main() -> None:
    parser = argparse.ArgumentParser(description="Water project dashboard")
    parser.add_argument("--port", "-p", default=None, help="COM-порт (USB)")
    parser.add_argument("--wifi", "-w", default=None, help="http://192.168.x.x (локальная сеть)")
    parser.add_argument(
        "--mqtt",
        "-m",
        action="store_true",
        help="Облако MQTT — из любой сети (рекомендуется удалённо)",
    )
    parser.add_argument("--mqtt-broker", default="broker.hivemq.com")
    parser.add_argument("--mqtt-port", type=int, default=1883)
    parser.add_argument(
        "--mqtt-topic",
        default="waterproject/alexl/station/data",
        help="Тот же topic, что в .ino",
    )
    parser.add_argument("--baud", "-b", type=int, default=115200)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--web-port", type=int, default=5000)
    args = parser.parse_args()

    if args.mqtt:
        start_mqtt_feed(args.mqtt_broker, args.mqtt_port, args.mqtt_topic)
    elif args.wifi:
        start_wifi_feed(args.wifi)
    elif args.port:
        start_serial_feed(args.port, args.baud)
    else:
        print("Режимы:")
        print("  python server.py --mqtt          # из любой сети")
        print("  python server.py --wifi http://192.168.0.217")
        print("  python server.py --port COM5")
        return

    host = args.host
    port = args.web_port
    if os.environ.get("PORT"):
        host = "0.0.0.0"
        port = int(os.environ["PORT"])
    print(f"Открой в браузере: http://{host}:{port}")
    app.run(host=host, port=port, threaded=True, debug=False)


if __name__ == "__main__":
    main()
