# РБПО — Windows-клиент (трей и служба)

Учебный репозиторий с **ветками по заданиям**: от базового трей-приложения до связки со службой, локальным RPC (ALPC) и HTTPS к вашему API.

Это **клиентская** часть стека. Сервер REST/JWT и лицензий — Java‑проект **`rbpo_backend`** (Spring Boot). Имеет смысл клонировать его рядом, например:

```text
~/Documents/rbpo_prac      ← этот репозиторий (клиент / «фронт» окон и службы)
~/Documents/rbpo_backend   ← бэкенд API
```

Дальше в README слово «бэкенд» относится именно к **`rbpo_backend`**.

---

## Связь клиента с бэкендом

Бэкенд описан в своём `README.md`; кратко про контракт:

| Назначение | Бэкенд (типично) | Примечание для `TraySvc` (ветка zad-3) |
| ---------- | ---------------- | -------------------------------------- |
| Логин | `POST /api/auth/login`, тело `username` / `password` | Ответ JSON: **`accessToken`**, **`refreshToken`**. В коде службы поддержаны и camelCase, и `access_token` / `refresh_token`. |
| Обновление JWT | `POST /api/auth/refresh`, тело **`refreshToken`** | Тело запроса приведено к формату Spring `RefreshRequest`. |
| Лицензия | `POST /api/licenses/activate`, `check`, `renew` и т.д. | Ответ — **`TicketResponse`** (`ticket` + `signature`). Упрощённый парсер в службе и заглушка GET «status» не эквивалентны продакшен‑контракту; для полной стыковки нужна доработка разбора JSON под `Ticket`/`TicketResponse`. |

Порт по умолчанию у бэкенда — **8081**. Константы URL задаются в `include/tray_zad3_api.h` (файл есть на ветке **zad-3**).

HTTPS: клиент WinHTTP ожидает схему **https**. Включите TLS у Spring Boot (`SSL_ENABLED=true` и keystore в `application.properties`) или используйте reverse‑proxy с TLS.

Режим **`RBPO_SIMULATE_HTTP=1`** в сборке `TraySvc` (по умолчанию в CMake на **zad-3**) позволяет проверять задание без живого сервера.

---

## Ветки и содержание

Репозиторий разбит на этапы по веткам: сначала автономный клиент с треем, затем служба с RPC, затем HTTPS/JWT и интеграция с REST API **`rbpo_backend`**. Состав каталогов и целей CMake на каждой ветке свой — ориентируйтесь на таблицу.

| Ветка | Что внутри |
| ----- | ----------- |
| **main** | Точка входа по документации; исходники клиента смотрите на рабочих ветках ниже. |
| **zad-1** | Один **`rbpo-app.exe`**: трей, ресурсы (`.rc`, иконка), меню «Файл → Выход», single-instance, CMake, GitHub Actions. Без службы и без HTTP к бэкенду. |
| **zad-2** | Дополнительно **`TraySvc.exe`**: запуск GUI в сессиях пользователей, RPC остановки по **ncalrpc** (ALPC), перезапуск иконки при `TaskbarCreated`. Имена службы — в `include/tray_config.h` (`RbpoTrayZad2Svc` …). |
| **zad-3** | Развитие zad-2: второй интерфейс RPC (**TrayZad3**), JWT/лицензия в памяти службы, HTTPS или симуляция, GUI входа и активации. Имена **`RbpoTrayZad3Svc`**, см. `idl/TrayZad3.idl`. |

Для проверки конкретного этапа переключите ветку и пересоберите.

---

## Сборка (Windows)

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

На **zad-1**: `build/Release/rbpo-app.exe`. На **zad-3**: `build/Release/TrayApp.exe`, `build/Release/TraySvc.exe` (имена целей см. `CMakeLists.txt` на ветке).

На не‑Windows CMake цели клиента не добавляет (только сообщение в конфигурации).

### Установка службы (zad-2 / zad-3)

Имя службы возьмите из `include/tray_config.h` на вашей ветке. Пример для zad-3:

```bat
sc create RbpoTrayZad3Svc binPath= "C:\полный\путь\TraySvc.exe" start= demand DisplayName= "RBPO Tray Zad3"
```

Оба exe должны лежать в **одном каталоге**.

---

## CI

На ветках **`zad-1`**, **`zad-2`**, **`zad-3`** в репозитории есть workflow GitHub Actions (файл `.github/workflows/build.yml`): сборка на `windows-latest`, артефакты с собранными exe. На **`main`** может находиться только этот README.
