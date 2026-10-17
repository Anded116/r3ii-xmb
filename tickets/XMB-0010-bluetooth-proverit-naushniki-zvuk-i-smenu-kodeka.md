---
id: XMB-0010
title: Bluetooth: проверить наушники, звук и смену кодека
type: feature
status: open
priority: P2
area: net
source: user
verify-in: review
created: 2026-10-17
updated: 2026-10-17
labels: later
---

## Описание

`bt.c` над bluetoothctl/bluealsa, вывод через `bluealsa-cli open`, программная громкость, ресемплинг.
Не проверено вживую: подключение наушников, звук, смена кодека. Отложено пользователем.

## Критерий закрытия

Наушники подключаются из меню, звук идёт без щелчков, громкость колесом работает.

## История

- 2026-10-17 (agent): создан (feature, open).
