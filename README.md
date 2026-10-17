# MicroPython для NodeMP

Языковой модуль сервера NodeMP. Кладётся в `modules/mpyhost.dll` и регистрирует ресурсы `type = "micropython"`. Скрипт видит глобальный `node`. Это отдельный репозиторий от хоста CPython: свой интерпретатор, свой список функций.

Оба модуля можно поставить рядом. Если оба резервируют ядро, задайте им разные номера: `NODEMP_PYTHON_CORE` и `NODEMP_MPY_CORE`. По умолчанию оба берут последний процессор.

Полный список имён — в [API.md](API.md). Заголовок SDK лежит в `sdk/node.h` (ABI 2.8). Сервер 1.12.0 говорит на ABI 2.7, поэтому в логе будет `NEWER SDK (2.8 vs 2.7)`. Функции только из 2.8 при вызове дают ошибку. Часть NodeApi в этот хост сознательно не обёрнута: HTTP, структуры машин, `sha256`, сырые байты. Они есть у `type = "python"`.

Сервер по-прежнему вызывает колбэки на одном worker-потоке.

## Отдельное ядро

`node.job(fn, *args, done=cb)` ставит вызов в очередь одного потока. Поток закреплён за одним логическим процессором.

- По умолчанию это последний процессор. `NODEMP_MPY_CORE=3` выбирает другой, `off` оставляет планировщику.
- Пока `NODEMP_MPY_RESERVE` не равен `0`, у остальных потоков процесса этот процессор снимается с маски. Обход повторяется раз в пять секунд. Поток, у которого этот процессор единственный, не трогается.
- На macOS поток получает только метку affinity.
- Очередь одна, до 4096 задач и 256 живых слотов. Поток спит, пока задач нет.
- `done(result, error)` возвращается на worker. `error` — текст исключения или `None`. Из задачи этот callback ждать нельзя.

У интерпретатора один замок. Пока на ядре выполняется скрипт MicroPython, другие колбэки MicroPython ждут, а worker сервера и Lua идут дальше, пока им не понадобится войти в этот интерпретатор.

Куча сборщика мусора по умолчанию 8 МБ. `NODEMP_MPY_HEAP_KB` меняет её (от 256 до 262144).

## Что есть у скрипта

`node.on`, `node.off`, таймеры `set_timeout` / `set_interval` / `cancel`, `node.job`, `node.core_info()`. Плюс прямые функции `NodeApi`, у которых аргументы — числа и строки: `log_info`, `get_player_name`, `storage_get` и остальные такого вида. Списки id: `get_players`, `get_vehicles`, `get_player_vehicles`, `get_vehicles_by_tag`, `get_vehicles_by_tags`. `get_player_vehicle` возвращает `(gid, role)`.

Имена, которые объект имеет сейчас:

```python
names = sorted(name for name in dir(node) if not name.startswith("_"))
node.log_info(" ".join(names))
```

Что делает каждая функция NodeApi, написано в `sdk/node.h` и собрано в [API.md](API.md).

`node.resource_name`, `node.resource_dir` и `node.trusted` читаются как поля. `trusted` повторяет `server.toml`: `[resources] trusted = ["hello-mpy"]`.

В этой сборке нет модулей `os`, `ffi` и сети, и `import` снаружи интерпретатора выключен. Скрипт не открывает файлы и не запускает процессы. `print` попадает в лог сервера с меткой `Mpy`.

Колбэк запроса (`playerConnectRequest` и остальные `*Request`) разрешает действие значением `None` или `True`. `False` или строка запрещают; строка записывается как причина. Исключение тоже запрещает и пишется в лог.

## Сборка

Нужны gcc из MinGW-w64 (`C:\mingw64\bin\gcc.exe`), Git for Windows и Python 3.14+ (`py -3.14`): им пользуются генератор обёрток и скрипты MicroPython. Исходники MicroPython v1.29.0 makefile забирает сам.

```text
mingw32-make
```

Готовый `build/mpyhost.dll` кладётся в `modules/`. Примеры — `examples/hello` и `examples/micropython-gate`. Makefile рассчитан на Windows: MinGW-w64 и Git for Windows по путям из файла.

После изменения `sdk/node.h`:

```text
py -3.14 tools/gen_node.py
py -3.14 tools/gen_api_md.py --host micropython
```
