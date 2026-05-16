# Water Project — дашборд + облако

## Удалённо из любой сети (MQTT) — основной способ

**Lisa (дома):** только включить плату в Wi‑Fi `AlexL` (прошивка с MQTT уже залита).

**Ты (где угодно):**

```bash
cd dashboard
pip install -r requirements.txt
python server.py --mqtt
```

Браузер: http://127.0.0.1:5000

Топик MQTT (в скетче и сервере): `waterproject/alexl/station/data`  
Брокер: `broker.hivemq.com` (публичный, бесплатный).

### Arduino: библиотека PubSubClient

IDE → Менеджер библиотек → **PubSubClient** (Nick O'Leary).

---

## Локально по Wi‑Fi (одна сеть с платой)

```bash
python server.py --wifi http://192.168.0.217
```

## USB

```bash
python server.py --port COM5
```

---

## Проверка MQTT (Lisa)

Serial Monitor 115200 → каждые 2 с:

`MQTT publish OK`
