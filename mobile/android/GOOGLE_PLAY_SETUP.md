# Публікація LanVid Monitor у Google Play Store (5-хвилинний гайд)

Застосунок **`LanVid Monitor`** (`applicationId = "com.manul02.lanvid.monitor"`) повністю підготовлено до публікації в Google Play:
- Підписаний `.aab` із R8/ProGuard та повними нативними символами (`ndk.debugSymbolLevel = "FULL"`), `arm64-v8a` + `armeabi-v7a`, `targetSdk = 35`, `minSdk = 26`.
- Адаптивна іконка, банер `1024×500`, `512×512` іконка та по 4 скриншоти (`1080×2400`) для `en-US` та `uk`.
- Вбудований режим **`Open Demo Studio`** для модераторів Google Play без підключеного ПК.
- Автоматизація релізів через **Fastlane `supply`** та GitHub Actions (`.github/workflows/android_googleplay.yml`).

---

## Крок 1. Створення Upload Keystore (1 раз локально)

```bash
keytool -genkeypair -v \
  -keystore lanvid-upload.jks \
  -keyalg RSA -keysize 2048 -validity 10000 \
  -alias lanvid_upload
```

Закодуйте файл у Base64 (Windows PowerShell):
```powershell
[Convert]::ToBase64String([IO.File]::ReadAllBytes("lanvid-upload.jks")) | Set-Clipboard
```
(macOS/Linux: `base64 -i lanvid-upload.jks | pbcopy`). Збережіть резервну копію `lanvid-upload.jks` у надійному місці!

---

## Крок 2. Створення Google Play Service Account JSON

1. Відкрийте **Google Play Console** → **Setup → API access** (або **Google Cloud Console → IAM & Admin → Service Accounts**).
2. Створіть Service Account (наприклад, `lanvid-fastlane@<project>.iam.gserviceaccount.com`) і згенеруйте **JSON key**.
3. У **Google Play Console → Users and permissions → Invite new users** додайте email цього Service Account із правами **Release to production / testing tracks** та **Manage store presence**.

---

## Крок 3. Додавання 5 секретів у GitHub Secrets

У репозиторії GitHub (**Settings → Secrets and variables → Actions**) додайте:

| Секрет | Значення |
|---|---|
| `ANDROID_KEYSTORE_BASE64` | Base64-рядок файлу `lanvid-upload.jks` |
| `ANDROID_KEYSTORE_PASSWORD` | Пароль keystore |
| `ANDROID_KEY_ALIAS` | `lanvid_upload` |
| `ANDROID_KEY_PASSWORD` | Пароль ключа |
| `GOOGLE_PLAY_SERVICE_ACCOUNT_JSON` | Повний вміст JSON-ключа Service Account |

---

## Крок 4. Готові відповіді для анкет Google Play Console (App content)

| Розділ Google Play Console | Готова відповідь |
|---|---|
| **Privacy Policy URL** | `https://manul02-ndn.github.io/lanvid/privacy-policy.html` |
| **Support URL** | `https://manul02-ndn.github.io/lanvid/support.html` |
| **Data Safety (Безпека даних)** | **Does your app collect or share any of the required user data types?** → **No**. Увесь аудіо- та керуючий трафік передається виключно напряму між смартфоном і ПК у локальній Wi-Fi мережі користувача; дані не збираються й не надсилаються на зовнішні сервери. |
| **Ads (Реклама)** | **No, my app does not contain ads.** |
| **App Access (Доступ для перевірки)** | **All functionality is available without special access.** (Опціонально в примітках для рев'юера: *Tap **"Open Demo Studio"** on the Connect screen to test all 8 channels, live VU meters, and Monitor FX DSP locally without a PC server.*) |
| **Content Rating (Віковий рейтинг)** | **PEGI 3 / Everyone** (Утиліта / музичний інструмент; немає насильства, чатів, UGC, покупок чи реклами). |
| **Target Audience** | `13–15`, `16–17`, `18 and over` (Not designed primarily for children). |
| **Category** | **Music & Audio** |
| **Foreground Service Declaration** | `mediaPlayback` — використовується для безперервного відтворення персонального аудіомонітору в навушниках музиканта при вимкненому екрані смартфона. |

---

## Крок 5. Запуск першого релізу

> **Примітка Google Play:** найперший `.aab` потрібно завантажити **вручну** 1 раз у трек **Internal testing** через веб-інтерфейс Google Play Console (вимога Google Play API для ініціалізації пакета). Після цього всі наступні релізи повністю автоматичні:

1. Відкрийте GitHub → **Actions → Android Google Play Release → Run workflow**.
2. Оберіть `track`:
   - `internal` — миттєве внутрішнє тестування (для команди та 12 тестувальників);
   - `closed` — закрите тестування (`alpha`);
   - `production` — публічний реліз у Google Play Store.
