# SDL2 2.28.5: RGA renderer для PicoCalc / Lyra B

Локальная ветка `lyra-rga` основана на upstream-теге `release-2.28.5`
(`15ead9a`). Включение `-DSDL_RGA=ON` добавляет renderer с именем `rga`
к API `SDL_Renderer`. Для текущего устройства выбирается
`SDL_VIDEODRIVER=directfb`; DirectFB обеспечивает окно и вывод на SPI fb0,
но **RGA-операции выполняет сам SDL renderer, а не DirectFB gfxdriver**.

## Что реализовано

- RGB565-кадр и подходящие текстуры получают собственные DMA-буферы из
  `/dev/dma_heap/linux,cma`. CPU/RGA синхронизируются через
  `DMA_BUF_IOCTL_SYNC`; текстуры обновляются при `SDL_UpdateTexture`,
  `SDL_UnlockTexture` и использовании в качестве render target.
- DMA-буферы импортируются в RGA один раз через `importbuffer_fd` и
  переиспользуют handle. Синхронизация CPU/RGA выполняется при смене
  владельца: последовательные аппаратные команды не делают лишних
  START/END; CPU fallback, чтение пикселей и present получают CPU-доступ
  до обращения к памяти. Аппаратные операции остаются синхронными.
- Повторяемое линейное масштабирование непрозрачных RGB565-текстур
  кэшируется после второго одинакового запроса. Один вариант на текстуру,
  общий бюджет пикселей кэша **512 КиБ обычной RAM**, без дополнительной CMA.
  Ключ учитывает исходный прямоугольник и выходной размер; обновление
  содержимого инвалидирует кэш. Альфа, modulation, несовместимый blend,
  nearest и частичный clip проходят прежний путь. При нехватке бюджета
  операция продолжает выполняться через RGA без кэширования.
- Аппаратный путь: `SDL_RenderClear`, непрозрачные и полупрозрачные
  `SDL_RenderFillRect(s)` (`imfill_t` и premultiplied RGBA + SRC_OVER),
  `SDL_RenderCopy` (RGB565, ARGB8888 с альфой, обрезка и линейный scale),
  `SDL_RenderCopyEx` (повороты 90/180/270° и горизонтальное/вертикальное
  отражение при поддерживаемом центре и размере). Clip/viewport
  соблюдаются; частично отсечённые текстуры обрабатываются CPU.
- Все остальные команды SDL_Renderer сохраняют программный путь SDL2:
  точки, линии, произвольная геометрия и треугольники, поворот на
  произвольный угол, nearest-neighbor scale, маленькие/нестандартные
  текстуры и несовместимые режимы смешивания или модуляции.
- Изменение размера окна создаёт новый DMA-буфер; render target остаётся
  программной SDL-поверхностью и синхронизируется при копировании на экран.
- Present: копирование RGB565-кадра в DirectFB surface и flip. Для
  dummy/offscreen используется window surface.
- `SDL_RENDERER_ACCELERATED` выбирает RGA при доступности DMA-heap.
  Явный выбор: `SDL_HINT_RENDER_DRIVER="rga"`.
- Для приложений, управляющих консолью, `SDL_DIRECTFB_HANDLE_SIGNALS=0`
  отключает аварийный SigHandler DirectFB: SIGINT/SIGTERM обслуживает SDL,
  приложение может штатно восстановить консоль. По умолчанию поведение
  DirectFB прежнее. Launcher включает подсказку при захвате консоли.

**Автоматический выбор по измерениям на Lyra B:** маленькие операции
дешевле выполнять CPU; в текущем коде пороги площади: непрозрачная заливка
250000 px, альфа-заливка 8192 px, линейный scale 4096 px, обычный RGB565
blit 120000 px, alpha blit / поворот 16384 px. Дополнительно учитываются
выравнивание, clipping и поддерживаемый формат. Для диагностики полный
допустимый аппаратный путь включается через `SDL_RGA_FORCE=1` до создания
renderer. `SDL_RGA_STATS=1` включает итоговые счётчики реально выполненных
аппаратных fill/alpha/copy/rotate, попаданий `cache` и успешных `imports`.
`SDL_RGA_CACHE=0` отключает только кэш масштабирования; `SDL_RGA_FORCE=1`
тоже обходит кэш для проверки аппаратуры. Подсказки задаются до создания
renderer. Пороги — эмпирические для этой прошивки.

RGA2 не является GPU треугольников. Альфа-текстуры предварительно
умножаются на альфу при загрузке в DMA-буфер, в исходной software-поверхности
сохраняется straight alpha. Линейная фильтрация и альфа-смешивание на RGA
могут отличаться от SDL software на 1–2 шага канала RGB565.
Дисплей PicoCalc 320 × 320; DirectFB present протестирован для RGB16.
Лимит RGA — 1280 × 1280. Ядро SDL2, DirectFB и `librga` — adopted code;
соответствие MISRA не заявлено.

## Кросс-компиляция на компьютере

Запускать из корня PicoLauncher, где расположен `toolchains/lyra.cmake`.
Нужны host CMake, Ninja, `arm-linux-gnueabihf-gcc` и ADB. Для точного
соответствия glibc 2.38 взять **sysroot именно подключённой прошивки**.
Команда ниже только читает `/usr/include`, `/usr/lib`, `/lib` с платы и
распаковывает их **на компьютере** (около 500 МиБ). Root ADB нужен лишь
из-за прав USB на текущем ПК; в примере используется отдельный сервер.

```sh
mkdir -p /tmp/opencode/lyra-sysroot
sudo -n adb -P 5038 -s 47914e716b161828 exec-out \
  'tar -C / -cf - lib usr/include usr/lib' \
  | tar -xf - -C /tmp/opencode/lyra-sysroot

export LYRA_SYSROOT=/tmp/opencode/lyra-sysroot
export PKG_CONFIG_SYSROOT_DIR="$LYRA_SYSROOT"
export PKG_CONFIG_LIBDIR="$LYRA_SYSROOT/usr/lib/pkgconfig"

cmake -S SDL2-RGA -B /tmp/opencode/sdl2-rga-cross -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/toolchains/lyra.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSDL_C_ONLY=ON -DSDL_RGA=ON \
  -DSDL_DIRECTFB=ON -DSDL_DIRECTFB_SHARED=OFF \
  -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF \
  -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VULKAN=OFF \
  -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_HIDAPI=OFF \
  -DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF \
  -DSDL_JACK=OFF -DSDL_LIBUDEV=OFF -DSDL_DBUS=OFF \
  -DSDL_LIBSAMPLERATE=OFF -DHAVE_LIBSAMPLERATE_H=OFF -DSDL_OSS=OFF
cmake --build /tmp/opencode/sdl2-rga-cross -j 6
```

`SDL_C_ONLY=ON` устраняет ненужную зависимость от C++-кросс-компилятора
на этой Linux-платформе. `HAVE_LIBSAMPLERATE_H=OFF` предотвращает
автообнаружение заголовка без линковки с отключённым libsamplerate.

Библиотека: `/tmp/opencode/sdl2-rga-cross/libSDL2-2.0.so.0.2800.5`.
Это **host-путь**; сборка и исходники на плату не копируются.

## Проверка готового бинарника

Тестовый файл расположен в `tools/rga_renderer_probe.c` корня проекта:

```sh
arm-linux-gnueabihf-gcc --sysroot="$LYRA_SYSROOT" -std=c17 \
  -Wall -Wextra -Werror \
  -I SDL2-RGA/include \
  -I /tmp/opencode/sdl2-rga-cross/include-config-release/SDL2 \
  tools/rga_renderer_probe.c \
  /tmp/opencode/sdl2-rga-cross/libSDL2-2.0.so.0.2800.5 \
  -Wl,-rpath-link="$LYRA_SYSROOT/usr/lib" \
  -o /tmp/opencode/rga_renderer_probe
```

Для аппаратного теста можно временно загрузить только готовые бинарники
в `/tmp` платы (tmpfs), запустить, затем удалить их. Не устанавливать
поверх системного SDL2:

```sh
sudo -n adb -P 5038 -s 47914e716b161828 push \
  /tmp/opencode/sdl2-rga-cross/libSDL2-2.0.so.0.2800.5 /tmp/libSDL2-2.0.so.0
sudo -n adb -P 5038 -s 47914e716b161828 push \
  /tmp/opencode/rga_renderer_probe /tmp/pico_rga_renderer_probe
sudo -n adb -P 5038 -s 47914e716b161828 shell \
  'LD_LIBRARY_PATH=/tmp SDL_VIDEODRIVER=directfb /tmp/pico_rga_renderer_probe'
sudo -n adb -P 5038 -s 47914e716b161828 shell \
  'rm -f /tmp/libSDL2-2.0.so.0 /tmp/pico_rga_renderer_probe'
```

## Результат на подключённой плате

Кросс-сборка успешно прошла на ПК. Тест в Buildroot 2024.02 проверил
clear, clip, alpha-заливку, RGB565/ARGB8888 текстуры, линейный scale,
копирование render target, повороты, отражения, геометрию через CPU,
переходы CPU → RGA и resize в dummy-режиме: `failures=0`. Форсированный
тест на DirectFB подтвердил `fill=11 alpha=1 copy=5 rotate=5` — реальные
операции RGA; `/dev/rga` и DMA heap также обнаружены в трассировке.
После `SDL_RenderPresent` пиксель `(210,210)` в `/dev/fb0` равен `0xffff`.
Временные бинарники и следы трассировки после проверок удалены с платы.

## Замеры производительности на Lyra B

Актуальное A/B-сравнение с предыдущей библиотекой, 300 измерений на строку:

| Операция / сцена | Прежний RGA | Оптимизированный RGA |
| --- | ---: | ---: |
| Повторный linear scale 64→128, растеризация | 297,79 мкс | **25,08 мкс** |
| Scale с меняющимся размером, без попаданий в кэш | 293,71 мкс | **218,17 мкс** |
| Alpha rect 200×200, растеризация | 348,83 мкс | **279,42 мкс** |
| 9 масштабированных иконок + курсор + present | 4,275 мс | **1,738 мс** |
| Alpha rect 210×210 + scale128 + present | 2,198 мс | **1,837 мс** |
| 8 alpha rect + present | 4,774 мс | **4,279 мс** |
| Clear + непрозрачный rect + present | 1,377 мс | 1,319 мс |

Кэш даёт около **12×** для повторного scale и **2,5×** для полного кадра
с девятью копиями иконки. При отключённом кэше новый scale64→128 занимает
206,79 мкс: снижение накладных расходов помогает и без повторного
использования пикселей. Простой кадр по-прежнему медленнее native DirectFB
(1,131 мс), поскольку вывод RGA включает дополнительное копирование.

**Уточнение предыдущего сравнения:** `SDL_DirectFB_render.c` в этой версии
SDL игнорирует `SDL_SetTextureScaleMode`. Поэтому native DirectFB и RGA
в сценах с scale не обеспечивают одинаковую фильтрацию. Чистый CPU linear
scale измеряется отдельным SDL software renderer в `raster`; основное
A/B-сравнение выше — две версии RGA с одинаковой линейной фильтрацией.

Методика, ограничения, проверки, сохранённый CSV и команды:
[`../docs/performance-rga.md`](../docs/performance-rga.md).
