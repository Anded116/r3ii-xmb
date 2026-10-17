---
id: XMB-0004
title: Рамка экрана: чёрная верхняя панель и скругления углов
type: feature
status: fixed
priority: P2
area: display
source: user
verify-in: review
created: 2026-10-17
updated: 2026-10-17
commits: 06abee1
---

## Описание

Верхняя панель всегда чёрная, углы экрана скруглены маской A8 (`corners_create`, `CORNER_R` 12),
в подменю 8 строк. Вопрос формы скругления — XMB-0019.

## Критерий закрытия

Рамка выглядит цельно на всех экранах.

## История

- 2026-10-17 (agent): создан (feature, fixed).
- 2026-10-17 (agent): commits 06abee1
