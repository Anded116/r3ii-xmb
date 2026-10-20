---
id: XMB-0071
title: Изучить unrau/sonix-player 1.3.0.1 под R3II 2025: сон, звук после сна, Wi-Fi, HBC3000, частота панели
type: tech
status: fixed
priority: P1
area: firmware
source: user
verify-in: review
created: 2026-10-20
updated: 2026-10-20
commits: dc82915
---


## Описание

<суть задачи>

## Критерий закрытия

<что должно быть правдой, чтобы тикет закрыть>

## История

- 2026-10-20 (agent): создан (tech, open).
- 2026-10-20 (agent): status open -> in-progress
- 2026-10-20 (agent): commits dc82915 - Изучено: unrau/sonix-player pr/r3ii-2025 (4 коммита поверх Sonix). Маршруты как у R3 Pro II: 2 — наушники 3.5, 3 — 4.4, 1 — линейный; XMB писал 1 → теперь 2/3 (dc82915). Частоту панели не меняют (стоковый lcd_st77903_8bit.sh). Wi-Fi — стоковый, как у нас. Не грузят soc_gpio и sa_hgl_dma (модули стокового интерфейса). Сон — общий код Sonix (стоп трека, маршрут на другой разъём, тишина ЦАП, маршрут после сна в фоне). Новых решений по падениям после сна нет.
- 2026-10-20 (agent): status in-progress -> fixed
