# Сайт в интернете (домен)

Плата шлёт данные в **MQTT** → облачный сервер → сайт с ИИ открывается по ссылке **из любой точки мира**.

## 1. Бесплатный адрес (Render.com)

1. Зарегистрируйся: https://render.com  
2. **New → Web Service**  
3. Подключи GitHub-репозиторий **или** загрузи папку `dashboard`  
4. Настройки:
   - **Root Directory:** `dashboard` (если репо целиком `water project`)
   - **Build:** `pip install -r requirements.txt`
   - **Start:** `gunicorn server:app --workers 1 --threads 4 --timeout 120 --bind 0.0.0.0:$PORT`
   - **Plan:** Free

5. Переменные окружения (Environment):

| Key | Value |
|-----|--------|
| `CLOUD_DEPLOY` | `1` |
| `MQTT_BROKER` | `broker.hivemq.com` |
| `MQTT_PORT` | `1883` |
| `MQTT_TOPIC` | `waterproject/alexl/station/data` |

6. **Deploy** → получишь ссылку вида:

`https://water-project-dashboard.onrender.com`

Открой в браузере — это уже **ваш сайт в интернете**.

> Free-план засыпает без посещений ~15 мин. Первый заход — подожди 30–60 сек.

---

## 2. Свой домен (например `water.ваш-сайт.bg`)

1. Купи домен (Reg.ru, Namecheap, Cloudflare и т.д.)  
2. Render → ваш сервис → **Settings → Custom Domains**  
3. Добавь домен, Render покажет **CNAME** (например `water-project.onrender.com`)  
4. У регистратора DNS:

| Тип | Имя | Значение |
|-----|-----|----------|
| CNAME | `www` или `@` | то, что дал Render |

5. Подожди 5–60 минут → сайт откроется по вашему домену.

---

## 3. Что должна делать Lisa

- Плата **включена**, Wi‑Fi **AlexL**, прошивка с **MQTT publish OK**  
- **Не** нужно запускать `server.py` на её ПК  

---

## 4. Локально (как раньше)

```bash
python server.py --mqtt
```

---

## 5. OpenAI на облаке (по желанию)

Render → Environment → `OPENAI_API_KEY` = ваш ключ.
