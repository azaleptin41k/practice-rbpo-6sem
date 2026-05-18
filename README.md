# РБПО — Windows-клиент (трей и служба)

Учебный репозиторий с **ветками по заданиям**: от базового трей-приложения до связки со службой, локальным RPC (ALPC), HTTPS к REST API и антивирусным движком.

Это **клиентская** часть стека. Сервер REST/JWT и лицензий — Java‑проект **`rbpo_backend`** (Spring Boot).

```text
~/Documents/rbpo_prac      ← этот репозиторий (служба + GUI)
~/Documents/rbpo_backend   ← бэкенд API
```

---

## Ветки и содержание

| Ветка | Что внутри |
| ----- | ----------- |
| **main** | Актуальный код после слияния zad-1 … zad-4. |
| **zad-1** | `rbpo-app.exe`: трей, иконка, меню, single-instance, CMake, GitHub Actions. |
| **zad-2** | `rbpo-app.exe` + `rbpo-service.exe`: запуск GUI в пользовательских сессиях, RPC остановки по `ncalrpc` (ALPC). |
| **zad-3** | Добавлены RPC для auth/license, JWT в памяти службы, HTTPS к `rbpo_backend`, GUI входа и активации продукта. |
| **zad-4** | Антивирусный движок, сканирование файлов / директорий, все необязательные требования (см. ниже). |

---

## Ключевые файлы

| Файл | Назначение |
| ---- | ---------- |
| `src/service/service_main.cpp` | Точка входа службы, все RPC-реализации |
| `src/service/state.cpp` | Auth/license workers, JWT-обновление |
| `src/service/av_engine.h/cpp` | Антивирусный движок (zad-4) |
| `src/rpc/rbpo_rpc.idl` | IDL-интерфейс (MIDL → `rpc_gen/`) |
| `src/main.cpp` | GUI (трей-приложение) |
| `src/rbpo_rpc_constants.h` | Имена службы, endpoint, коды ошибок |

Имя службы: **`RBPOService`**. RPC transport: `ncalrpc`, endpoint `RBPOServiceEndpoint`.

---

## Антивирусный движок (zad-4)

### Структура AV-базы в оперативной памяти

```text
std::map<uint64_t, vector<AvRecord>>
  ключ   — ObjectSignaturePrefix (первые 8 байт сигнатуры, little-endian uint64)
  значение — массив записей AvRecord:
    prefix        (8 байт)  — первые 8 байт сигнатуры
    sigLen        (4 байта) — полная длина сигнатуры
    sigHash              — SHA-256 всех байт сигнатуры (BCrypt)
    offsetBegin   (8 байт) — начало допустимого диапазона позиции (-1 = любая)
    offsetEnd     (8 байт) — конец допустимого диапазона позиции (-1 = любая)
    type          (1 байт) — ObjectType: PE=0, Script=1
    recordSig            — SHA-256 всех вышеперечисленных полей (ЭЦП)
```

`std::map` реализован как красно-чёрное дерево → поиск по префиксу O(log K).

### Алгоритм сканирования (обязательный, п.3)

1. Позиция чтения = 0.
2. Считать 8 байт → поиск по ключу в `std::map` (O(log K)).
3. Для каждой найденной записи (от дешёвой проверки к дорогой):
   - 3.3.1 Тип объекта совпадает с `ObjectType`?
   - 3.3.2 Позиция попадает в `[OffsetBegin, OffsetEnd]`?
   - 3.3.3 Считать ещё `sigLen − 8` байт.
   - 3.3.4 Вычислить SHA-256(prefix_bytes || extra_bytes).
   - 3.3.5 Сравнить хэш с `ObjectSignature`.
4. Несовпавшие записи исключаются; если список пуст — сдвиг на 1 байт, goto 2.
5. Если осталась хоть одна запись — объект вредоносен.

### Алгоритм Ахо-Корасика (необязательный, доп. баллы)

При загрузке базы (`AvLoad`) дополнительно строится автомат из реальных байтов всех сигнатур. `ScanStream` использует AC для одного прохода по файлу O(N + M) вместо O(N log K), проверяя type и offset при совпадении.

### Определение типа файла

| Условие | Тип |
| ------- | --- |
| Расширение `.py`, `.ps1`, `.js`, `.vbs` | Script |
| Первые байты `MZ` | PE |
| Иначе | Script |

### Тестовые сигнатуры

| Сигнатура (16 байт) | Тип | Детект |
| ------------------- | --- | ------ |
| `RBPOTESTVRS1.000` | PE | Файл содержит эту последовательность + MZ-заголовок |
| `#RBPOTESTVRS2.00` | Script | Файл содержит эту последовательность + расширение .py/.ps1 |

### RPC-методы (зарегистрированы в `RBPOServiceRpc`)

**Обязательные:**

| Метод | Описание |
| ----- | -------- |
| `RBPO_GetAvDbInfo` | Дата выпуска базы + кол-во записей |
| `RBPO_ScanFile` | Сканирование одного файла |
| `RBPO_ScanDirectory` | Рекурсивное сканирование директории |

**Необязательные:**

| Метод | Описание |
| ----- | -------- |
| `RBPO_ScanAllDrives` | Сканирование всех несъёмных дисков (`DRIVE_FIXED`) |
| `RBPO_SetScanSchedule` | Установить расписание (путь + интервал в секундах) |
| `RBPO_ClearScanSchedule` | Сбросить расписание |
| `RBPO_GetScheduleResults` | Результаты последнего планового сканирования + timestamp |
| `RBPO_AddMonitorDirectory` | Начать мониторинг директории (`ReadDirectoryChangesW`) |
| `RBPO_RemoveMonitorDirectory` | Остановить мониторинг |
| `RBPO_GetMonitorResults` | Результаты мониторинга (файлы, обнаруженные при создании/изменении) |

Сканирующие методы защищены `LicenseGate()` — требуют активной лицензии.

### GUI (лицензионная панель)

- Метка с датой базы и количеством записей.
- Кнопки: «Скан файл», «Скан папку», «Скан все диски».
- Секция расписания: поле пути, поле интервала (сек), «Установить» / «Сбросить» / «Результаты».
- Секция мониторинга: поле пути, «Добавить» / «Удалить» / «Результаты».

---

## Связь с бэкендом

Порт по умолчанию **8081** (HTTP). Переопределение: env-переменные `RBPO_BACKEND_HOST`, `RBPO_BACKEND_PORT`, `RBPO_BACKEND_USE_TLS`.

| Endpoint | Назначение |
| -------- | ---------- |
| `POST /api/auth/login` | Логин (тело: `username` / `password`) |
| `POST /api/auth/refresh` | Обновление JWT |
| `GET /api/auth/me` | Профиль пользователя |
| `POST /api/licenses/activate` | Активация ключа |
| `POST /api/licenses/check` | Проверка лицензии |

---

## Сборка (Windows)

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Артефакты: `build/Release/rbpo-app.exe`, `build/Release/rbpo-service.exe`. Оба exe должны лежать в **одном каталоге**.

### Установка службы

```bat
sc create RBPOService binPath= "C:\path\to\rbpo-service.exe" start= demand DisplayName= "RBPO Service"
sc start RBPOService
```

### Удаление службы

```bat
sc stop RBPOService
sc delete RBPOService
```

---

## CI

Workflow `.github/workflows/build.yml` собирает оба exe на `windows-latest` и публикует артефакты.
