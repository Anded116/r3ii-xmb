---
id: XMB-0027
title: Crystal Castles: (II) и (III) слиты в один альбом, (III) играет одним 40-минутным треком
type: bug
status: fixed
priority: P2
area: library
source: agent
verify-in: review
created: 2026-10-17
updated: 2026-10-17
commits: 3007137
---


## Описание

<наблюдаемое поведение и ожидаемое>

## Репро

<шаги; сценарий XMB_SCRIPT или замер, если есть>

## Критерий закрытия

<что должно быть правдой, чтобы тикет закрыть>

## История

- 2026-10-17 (agent): Причина — разметка рипа: в CUE (II) TITLE «Crystal Castles (III)», в CUE (III) FILE «Crystal Castles - (III).flac» вместо «Crystal Castles - Crystal Castles (III).flac». Теперь: CUE без совпадения по имени привязывается к единственному звуковому файлу папки; одноимённые альбомы из разных папок с разным годом не сливаются. Сверено со старой версией (libtest LIBALBUMS): кроме Crystal Castles изменился только Idealism — трек из папки Idealistic (Promo) с годом 2008 больше не подмешивается в Idealism 2007. Название (II) в плеере остаётся «(III)», пока не исправить TITLE в его CUE — это файл пользователя.
