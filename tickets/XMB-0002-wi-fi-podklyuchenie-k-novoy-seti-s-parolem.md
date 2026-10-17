---
id: XMB-0002
title: Wi-Fi: подключение к новой сети с паролем
type: feature
status: fixed
priority: P2
area: net
source: user
verify-in: review
created: 2026-10-17
updated: 2026-10-17
commits: e638c6d
---

## Описание

Сохранённая сеть подключается сама (проверено). Не проверено подключение к новой сети с вводом пароля
с экранной клавиатуры (`wifi.c`, `/data/wpa_supplicant.conf`).

## Критерий закрытия

Новая сеть с паролем подключается, после перезагрузки подключается сама.

## История

- 2026-10-17 (agent): создан (feature, fixed).
- 2026-10-17 (agent): commits e638c6d
