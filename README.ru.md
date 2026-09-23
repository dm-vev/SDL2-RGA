[English](README.md) | [Русский](README.ru.md)

# SDL2-RGA

Форк **SDL 2.28.5** с дополнительным аппаратно-ускоренным renderer на базе
Rockchip RGA. Проект ориентирован на **PicoCalc / Lyra B** и другие Linux-
устройства с Rockchip RGA, `librga` и DMA heap.

SDL — кроссплатформенная библиотека для работы с графикой, аудио, клавиатурой,
мышью, контроллерами и другими устройствами ввода. Изменения этого форка
добавляют renderer `rga` к API `SDL_Renderer`; базовые API и программный
renderer SDL2 остаются доступны.

## RGA renderer

Renderer использует DMA-буферы для совместного доступа CPU и RGA. На Lyra B
окно и вывод на экран обслуживает DirectFB, а графические RGA-операции
выполняются самим SDL renderer.

Аппаратно ускоряются подходящие операции очистки, заливки прямоугольников,
копирования текстур, линейного масштабирования, поворота на 90°/180°/270° при
поддерживаемых размере и центре, а также отражения. Поддерживаются, в частности,
текстуры RGB565 и ARGB8888. Операции, которые не подходят для RGA, продолжают
выполняться программным renderer SDL.

RGA — 2D blitter, а не GPU для произвольной геометрии: точки, линии,
треугольники, произвольные углы поворота и неподдерживаемые режимы смешивания
обрабатываются программным путём. Максимальный размер RGA-буфера — 1280 × 1280.

## Сборка

Для включения renderer нужны Linux, CMake, заголовки Rockchip RGA и библиотека
`librga`. Для вывода через DirectFB также нужны его заголовки и библиотека.
Пример сборки для Linux-устройства с DirectFB:

```sh
cmake -S . -B build \
  -DSDL_RGA=ON \
  -DSDL_DIRECTFB=ON \
  -DSDL_DIRECTFB_SHARED=OFF
cmake --build build --parallel
```

Укажите renderer до его создания:

```c
SDL_SetHint(SDL_HINT_RENDER_DRIVER, "rga");
SDL_Renderer *renderer = SDL_CreateRenderer(
    window, -1, SDL_RENDERER_ACCELERATED);
```

Для Lyra B с DirectFB можно запускать приложение так:

```sh
SDL_VIDEODRIVER=directfb ./your_app
```

Для диагностики доступны подсказки `SDL_RGA_STATS=1` (счётчики аппаратных
операций), `SDL_RGA_FORCE=1` (форсирование допустимого аппаратного пути) и
`SDL_RGA_CACHE=0` (отключение кэша масштабирования). Задавайте их до создания
renderer.

## Документация

- [Подробности реализации, сборка и проверки на PicoCalc / Lyra B](README-Lyra-RGA.md)
- [Документация SDL2 и платформ](docs/README.md)
- [Лицензия](LICENSE.txt)

Изменения SDL2 основаны на upstream-теге [`release-2.28.5`](https://github.com/libsdl-org/SDL/releases/tag/release-2.28.5).
