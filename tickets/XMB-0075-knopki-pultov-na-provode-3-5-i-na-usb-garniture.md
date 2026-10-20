---
id: XMB-0075
title: Кнопки пультов: на проводе 3.5 и на USB-гарнитуре
type: feature
status: fixed
priority: P2
area: input
source: user
verify-in: review
created: 2026-10-20
updated: 2026-10-20
commits: f2bea5d
---


## Описание

<суть задачи>

## Критерий закрытия

<что должно быть правдой, чтобы тикет закрыть>

## История

- 2026-10-20 (agent): создан (feature, open).
- 2026-10-20 (agent): status open -> fixed; commits f2bea5d
- 2026-10-20 (agent): провод 3.5 молчал: драйвер earpods_adc опрашивает кнопку только после «on» в earpods_adc_sw (по умолчанию выключен, сток включает сам) — XMB теперь держит его включённым, проверка раз в секунду
