# Всё онлайн — одна ссылка для всех

## Схема

```
Плата Lisa (Wi‑Fi AlexL) → MQTT интернет → Сервер Render → https://ваш-сайт
```

Никому не нужен Python на компьютере. Только браузер.

---

## Часть 1 — Lisa (один раз)

1. Прошить `water_project_sensors.ino` (Wi‑Fi AlexL, MQTT).
2. Библиотека Arduino: **PubSubClient**.
3. Плата в розетку, Wi‑Fi **AlexL**.
4. Serial Monitor 115200 → каждые 2 с: **MQTT publish OK**.

Готово. Плату только держать включённой.

---

## Часть 2 — выложить сайт (ты, 20–30 мин)

### A. GitHub (удобно)

1. https://github.com/new — репозиторий `water-project` (Public).
2. Загрузить папку **`dashboard`** целиком (или весь `water project`, тогда Root Directory = `dashboard`).

### B. Render

1. https://render.com — регистрация (можно через GitHub).
2. **New +** → **Web Service** → выбрать репозиторий.
3. Настройки:

| Поле | Значение |
|------|----------|
| Root Directory | `dashboard` (если залит только dashboard — оставить пустым) |
| Build Command | `pip install -r requirements.txt` |
| Start Command | `gunicorn server:app --workers 1 --threads 4 --timeout 120 --bind 0.0.0.0:$PORT` |
| Instance type | Free |

4. **Environment Variables** → Add:

```
CLOUD_DEPLOY = 1
MQTT_BROKER = broker.hivemq.com
MQTT_PORT = 1883
MQTT_TOPIC = waterproject/alexl/station/data
```

5. **Create Web Service** → ждать Deploy (зелёный Live).

6. Ссылка сверху, например:

`https://water-project-dashboard.onrender.com`

**Это ваш онлайн-сайт.** Отправь Lisa.

---

## Часть 3 — свой домен (по желанию)

Render → ваш сервис → **Settings** → **Custom Domains** → добавить домен → CNAME в DNS у регистратора.

---

## Проверка

1. Плата Lisa: **MQTT publish OK**.
2. Открыть ссылку Render (первый раз подождать до 60 сек — free просыпается).
3. Должно быть: **Облако · MQTT**, датчики, **ИИ-анализ**.

---

## Если «Ожидание данных»

- Плата выключена или не в AlexL.
- Нет **MQTT publish OK** — прошить заново.
- Сервер только проснулся — обновить страницу через минуту.

---

## Локально (не обязательно)

```bash
python server.py --mqtt
```

Только для отладки. Для школы достаточно ссылки Render.
