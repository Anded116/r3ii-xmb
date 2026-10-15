# Из Sonix

Файлы взяты из [Jepl4r/sonix-player](https://github.com/Jepl4r/sonix-player), коммит
`adbb9477c843d9a2bc01ff96f62384c02895bf51` (2026-10-14), `src/system/decode/`, без изменений:

- `mp4.c`, `mp4.h` — чтение контейнера MP4/M4A (таблицы сэмплов, теги iTunes, обложка) — GPL-3.0;
- `alacdec.c`, `alacdec.h` — обёртка декодера ALAC — GPL-3.0;
- `alac/` — декодер ALAC Дэвида Хэмертона, MIT (лицензия — в начале `alac.c`), с двумя правками Sonix (см. `alac/README.md`).

Из-за GPL-3.0 этих файлов XMB, собранный с ними, распространяется на условиях GPL-3.0.
