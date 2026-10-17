---
id: XMB-0019
title: Скругления углов экрана не совсем круглые
type: bug
status: open
priority: P2
area: display
source: user
verify-in: review
created: 2026-10-17
updated: 2026-10-17
links: XMB-0004
---

## Описание

Скругления углов экрана (`corners_create`, маска A8, `CORNER_R` 12) выглядят не совсем круглыми.

## Репро

Посмотреть на любой угол экрана на светлом фоне.

## Критерий закрытия

Углы выглядят как ровная дуга окружности.

## История

- 2026-10-17 (agent): создан (bug, open).
- 2026-10-17 (agent): links XMB-0004
