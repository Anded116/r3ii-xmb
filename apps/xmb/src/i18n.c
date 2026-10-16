/* Переводы интерфейса: ключ — русская строка (как она написана в коде), tx() отдаёт строку выбранного языка.
 * Нет перевода — строка как есть (названия треков, исполнителей и прочие данные проходят через tx() без изменений). */
#include "i18n.h"

#include <string.h>

static const char *const table[][LANG_COUNT] = {
	/* размеры, фон, время */
	{ "Очень мелкий", "Extra small", "Muy pequeño" }, { "Мелкий", "Small", "Pequeño" },
	{ "Обычный", "Normal", "Normal" }, { "Крупный", "Large", "Grande" },
	{ "Январь", "January", "Enero" }, { "Февраль", "February", "Febrero" }, { "Март", "March", "Marzo" },
	{ "Апрель", "April", "Abril" }, { "Май", "May", "Mayo" }, { "Июнь", "June", "Junio" },
	{ "Июль", "July", "Julio" }, { "Август", "August", "Agosto" }, { "Сентябрь", "September", "Septiembre" },
	{ "Октябрь", "October", "Octubre" }, { "Ноябрь", "November", "Noviembre" }, { "Декабрь", "December", "Diciembre" },
	{ "Волна PS3", "PS3 wave", "Onda PS3" }, { "Лента", "Ribbon", "Cinta" }, { "Частицы", "Particles", "Partículas" },
	{ "Спектр", "Spectrum", "Espectro" }, { "Шёлк", "Silk", "Seda" }, { "Сияние", "Aurora", "Aurora" },
	{ "Звёздная пыль", "Stardust", "Polvo de estrellas" },
	{ "Волна PSP", "PSP wave", "Onda PSP" }, { "Пульс", "Pulse", "Pulso" }, { "Круги", "Ripples", "Ondas" },
	{ "Выключен", "Off", "Desactivado" }, { "Включён", "On", "Activado" }, { "Включено", "On", "Activado" },
	{ "Выключено", "Off", "Desactivado" },
	{ "1 минута", "1 minute", "1 minuto" }, { "5 минут", "5 minutes", "5 minutos" }, { "10 минут", "10 minutes", "10 minutos" },
	{ "30 минут", "30 minutes", "30 minutos" }, { "15 секунд", "15 seconds", "15 segundos" },
	{ "30 секунд", "30 seconds", "30 segundos" }, { "2 минуты", "2 minutes", "2 minutos" }, { "Никогда", "Never", "Nunca" },
	{ "Через 1 минуту", "After 1 minute", "Tras 1 minuto" }, { "Через 5 минут", "After 5 minutes", "Tras 5 minutos" },
	{ "Сетка", "Grid", "Cuadrícula" }, { "Список", "List", "Lista" },
	{ "Список с обложками", "List with covers", "Lista con portadas" },
	{ "Высокое", "High", "Alta" }, { "Среднее", "Medium", "Media" }, { "Низкое", "Low", "Baja" },
	{ "По трекам", "Track", "Por pista" }, { "По альбомам", "Album", "Por álbum" },
	{ "2 с", "2 s", "2 s" }, { "4 с", "4 s", "4 s" }, { "6 с", "6 s", "6 s" },
	/* кнопки */
	{ "Ничего", "Nothing", "Nada" }, { "Пауза / воспроизведение", "Play / pause", "Reproducir / pausa" },
	{ "Следующий трек", "Next track", "Pista siguiente" }, { "Предыдущий трек", "Previous track", "Pista anterior" },
	{ "Следующий альбом", "Next album", "Álbum siguiente" }, { "Предыдущий альбом", "Previous album", "Álbum anterior" },
	{ "Вперёд на 10 с", "Forward 10 s", "Avanzar 10 s" }, { "Назад на 10 с", "Back 10 s", "Retroceder 10 s" },
	{ "Случайный порядок", "Shuffle", "Aleatorio" }, { "Повтор", "Repeat", "Repetir" },
	{ "Избранное", "Favorites", "Favoritos" }, { "Экран", "Screen", "Pantalla" },
	{ "«След. трек»: нажатие", "\"Next\": press", "«Siguiente»: pulsar" },
	{ "«След. трек»: удержание", "\"Next\": hold", "«Siguiente»: mantener" },
	{ "«След. трек»: два нажатия", "\"Next\": double press", "«Siguiente»: doble pulsación" },
	{ "«Пауза»: нажатие", "\"Pause\": press", "«Pausa»: pulsar" },
	{ "«Пауза»: удержание", "\"Pause\": hold", "«Pausa»: mantener" },
	{ "«Пауза»: два нажатия", "\"Pause\": double press", "«Pausa»: doble pulsación" },
	{ "«Пред. трек»: нажатие", "\"Previous\": press", "«Anterior»: pulsar" },
	{ "«Пред. трек»: два нажатия", "\"Previous\": double press", "«Anterior»: doble pulsación" },
	{ "Как было", "Reset", "Restablecer" },
	{ "вернуть назначения по умолчанию", "restore default actions", "restaurar las acciones por defecto" },
	{ "Кнопки — как по умолчанию", "Buttons reset to default", "Botones restablecidos" },
	/* эквалайзер */
	{ "Ровный", "Flat", "Plano" }, { "Басы", "Bass", "Graves" }, { "Рок", "Rock", "Rock" }, { "Поп", "Pop", "Pop" },
	{ "Электроника", "Electronic", "Electrónica" }, { "Джаз", "Jazz", "Jazz" }, { "Классика", "Classical", "Clásica" },
	{ "Вокал", "Vocal", "Voz" }, { "Высокие", "Treble", "Agudos" }, { "Свой", "Custom", "Personalizado" },
	{ "Авто", "Auto", "Auto" }, { "Эквалайзер", "Equalizer", "Ecualizador" }, { "Пресет", "Preset", "Preajuste" },
	{ "Настроить полосы", "Adjust bands", "Ajustar bandas" },
	{ "свой пресет: колесо — полоса, клик — изменить", "custom: wheel — band, click — edit",
	  "personal: rueda — banda, clic — editar" },
	{ "колесо — уровень, клик — готово", "wheel — level, click — done", "rueda — nivel, clic — listo" },
	{ "колесо — полоса, клик — изменить", "wheel — band, click — edit", "rueda — banda, clic — editar" },
	{ "Пресет эквалайзера", "Equalizer preset", "Preajuste del ecualizador" },
	{ "Эквалайзер: %s", "Equalizer: %s", "Ecualizador: %s" },
	{ "Эквалайзер выключен%s", "Equalizer off%s", "Ecualizador desactivado%s" },
	/* настройки */
	{ "Устройства", "Devices", "Dispositivos" }, { "Кодек", "Codec", "Códec" },
	{ "Быстрые настройки", "Quick settings", "Ajustes rápidos" },
	{ "шторка: яркость, беспроводные, режимы", "brightness, wireless, modes", "brillo, conexiones, modos" },
	{ "Тема оформления", "Theme", "Tema" }, { "Фон", "Background", "Fondo" },
	{ "Размер интерфейса", "Interface size", "Tamaño de la interfaz" }, { "Вид альбомов", "Album view", "Vista de álbumes" },
	{ "Сенсорное управление", "Touch control", "Control táctil" }, { "Яркость", "Brightness", "Brillo" },
	{ "Усиление", "Gain", "Ganancia" }, { "Кроссфейд", "Crossfade", "Fundido cruzado" }, { "Кнопки", "Buttons", "Botones" },
	{ "что делают кнопки на корпусе", "what the hardware buttons do", "qué hacen los botones físicos" },
	{ "Таймер сна", "Sleep timer", "Temporizador" }, { "Гашение экрана", "Screen off", "Apagar pantalla" },
	{ "Сон устройства", "Device sleep", "Suspensión" }, { "Стоковый плеер", "Stock player", "Reproductor original" },
	{ "обновление прошивки", "firmware update", "actualizar firmware" },
	{ "USB-накопитель", "USB storage", "Almacenamiento USB" },
	{ "карта — компьютеру по кабелю", "the card as a drive for a computer", "la tarjeta como unidad del ordenador" },
	{ "Только зарядка", "Charge only", "Solo cargar" },
	{ "Отдать карту компьютеру", "Give the card to the computer", "Ceder la tarjeta al ordenador" },
	{ "Музыка встанет на паузу", "Music will pause", "La música se pausará" },
	{ "Прокрутите вниз", "Scroll down", "Gira hacia abajo" },
	{ "Прокрутите вверх", "Scroll up", "Gira hacia arriba" },
	{ "Карта у компьютера", "Card is with the computer", "La tarjeta está en el ordenador" },
	{ "Можно извлечь её на компьютере", "You can eject it on the computer", "Puedes expulsarla en el ordenador" },
	{ "Подключите кабель к компьютеру", "Connect the cable to a computer", "Conecta el cable a un ordenador" },
	{ "карта у компьютера — извлеките её там\nили удерживайте колесо, чтобы вернуть",
	  "the card is on the computer — eject it there\nor hold the wheel to return it",
	  "la tarjeta está en el ordenador — expúlsala allí\no mantén la rueda para recuperarla" },
	{ "Перезагрузить в сток", "Reboot to stock", "Reiniciar al original" }, { "Отмена", "Cancel", "Cancelar" },
	{ "О системе", "About", "Acerca de" },
	{ "HiBy R3II 2025 · XMB-прототип", "HiBy R3II 2025 · XMB prototype", "HiBy R3II 2025 · prototipo XMB" },
	{ "Язык · Language", "Language", "Idioma" },
	/* категории и музыка */
	{ "Настройки", "Settings", "Ajustes" }, { "Музыка", "Music", "Música" }, { "Папки", "Folders", "Carpetas" },
	{ "Плейлисты", "Playlists", "Listas" },
	{ "Сканирование библиотеки…", "Scanning library…", "Analizando la biblioteca…" },
	{ "просмотрено файлов: %d", "files scanned: %d", "archivos analizados: %d" },
	{ "Музыка не найдена", "No music found", "No se encontró música" },
	{ "на карте нет FLAC, MP3 или WAV", "no FLAC, MP3 or WAV on the card", "no hay FLAC, MP3 ni WAV en la tarjeta" },
	{ "укажите папку в XMB_MUSIC", "set the folder in XMB_MUSIC", "indica la carpeta en XMB_MUSIC" },
	{ "Сейчас играет", "Now playing", "Reproduciendo" }, { "ничего не играет", "nothing playing", "nada en reproducción" },
	{ "Очередь", "Queue", "Cola" }, { "пусто", "empty", "vacío" }, { "Альбомы", "Albums", "Álbumes" },
	{ "Исполнители", "Artists", "Artistas" }, { "Все треки", "All tracks", "Todas las pistas" },
	{ "Поиск", "Search", "Buscar" }, { "исполнители, альбомы, треки", "artists, albums, tracks", "artistas, álbumes, pistas" },
	{ "Неизвестный исполнитель", "Unknown artist", "Artista desconocido" },
	{ "Разные исполнители", "Various artists", "Varios artistas" }, { "SD-карта", "SD card", "Tarjeta SD" },
	/* шторка и плеер */
	{ "Повтор: все", "Repeat: all", "Repetir: todo" }, { "Повтор: один", "Repeat: one", "Repetir: una" },
	{ "USB-ЦАП", "USB DAC", "DAC USB" }, { "Тач", "Touch", "Táctil" },
	{ "Нет трека", "No track", "Sin pista" }, { "Сон: %d:%02d", "Sleep: %d:%02d", "Apagado: %d:%02d" },
	{ "Случайно", "Shuffle", "Aleatorio" }, { "Ещё", "More", "Más" }, { "По порядку", "In order", "En orden" },
	/* плейлисты */
	{ "пусто — сердечко в плеере", "empty — tap the heart in the player", "vacío — toca el corazón en el reproductor" },
	{ "Добавлено в «%s»", "Added to \"%s\"", "Añadido a «%s»" }, { "Убрано из «%s»", "Removed from \"%s\"", "Quitado de «%s»" },
	{ "Все уже в «%s»", "All already in \"%s\"", "Todo ya está en «%s»" }, { "Уже в «%s»", "Already in \"%s\"", "Ya está en «%s»" },
	{ "%d треков — в «%s»", "%d tracks added to \"%s\"", "%d pistas añadidas a «%s»" },
	{ "Новый плейлист", "New playlist", "Nueva lista" }, { "В плейлист", "Add to playlist", "Añadir a lista" },
	{ "Пустое имя", "Empty name", "Nombre vacío" }, { "Имя «%s» уже занято", "Name \"%s\" is taken", "El nombre «%s» ya existe" },
	{ "Теперь «%s»", "Renamed to \"%s\"", "Ahora «%s»" }, { "Название плейлиста", "Playlist name", "Nombre de la lista" },
	{ "Удалить «%s»", "Delete \"%s\"", "Eliminar «%s»" }, { "Удалён «%s»", "Deleted \"%s\"", "Eliminada «%s»" },
	{ "«Избранное» не переименовывается и не удаляется", "Favorites can't be renamed or deleted",
	  "Favoritos no se puede renombrar ni eliminar" },
	/* поиск */
	{ "ничего не найдено", "nothing found", "sin resultados" }, { "Ничего не найдено", "Nothing found", "Sin resultados" },
	{ "исполнителей %d · альбомов %d · треков %d", "artists %d · albums %d · tracks %d", "artistas %d · álbumes %d · pistas %d" },
	{ "Введите, что искать", "Type something to search", "Escribe qué buscar" },
	/* действия */
	{ "Действия", "Actions", "Acciones" }, { "Переименовать", "Rename", "Renombrar" },
	{ "Удалить плейлист", "Delete playlist", "Eliminar lista" }, { "Убрать из очереди", "Remove from queue", "Quitar de la cola" },
	{ "Убрать из избранного", "Remove from favorites", "Quitar de favoritos" },
	{ "В избранное", "Add to favorites", "Añadir a favoritos" }, { "В плейлист…", "Add to playlist…", "Añadir a lista…" },
	{ "Играть следующим", "Play next", "Reproducir después" }, { "В конец очереди", "Add to queue", "Añadir a la cola" },
	{ "Убрать из плейлиста", "Remove from playlist", "Quitar de la lista" }, { "Выше", "Move up", "Subir" },
	{ "Ниже", "Move down", "Bajar" }, { "Все треки — в плейлист…", "All tracks to playlist…", "Todas las pistas a lista…" },
	{ "Следующим: %s", "Next: %s", "Después: %s" }, { "В конце очереди: %s", "Queued: %s", "En cola: %s" },
	{ "Следующими — %d треков", "Next: %d tracks", "Después: %d pistas" },
	{ "В конец очереди — %d треков", "Queued: %d tracks", "En cola: %d pistas" },
	/* меню «Ещё» */
	{ "A отмечена — теперь отметьте B", "A set — now set B", "A marcado — ahora marca B" },
	{ "Повтор A-B", "A-B repeat", "Repetir A-B" }, { "Повтор A-B выключен", "A-B repeat off", "Repetir A-B desactivado" },
	{ "Показать очередь", "Show queue", "Ver cola" }, { "Перейти к альбому", "Go to album", "Ir al álbum" },
	{ "Перейти к папке", "Go to folder", "Ir a la carpeta" },
	{ "Выключить эквалайзер", "Turn equalizer off", "Desactivar ecualizador" },
	{ "Включить эквалайзер", "Turn equalizer on", "Activar ecualizador" },
	{ "Пресет: %s", "Preset: %s", "Preajuste: %s" }, { "Таймер сна: %s", "Sleep timer: %s", "Temporizador: %s" },
	{ "Повтор A-B: отметить A", "A-B repeat: set A", "Repetir A-B: marcar A" },
	{ "Повтор A-B: отметить B", "A-B repeat: set B", "Repetir A-B: marcar B" },
	{ "Выключить повтор A-B", "Turn A-B repeat off", "Desactivar repetir A-B" }, { "Усиление: %s", "Gain: %s", "Ganancia: %s" },
	{ "подключено", "connected", "conectado" }, { "Поиск устройств…", "Search for devices…", "Buscar dispositivos…" },
	{ "Удалить с карты…", "Delete from card…", "Eliminar de la tarjeta…" },
	{ "Удалить «%s» с карты", "Delete \"%s\" from card", "Eliminar «%s» de la tarjeta" },
	{ "Трек внутри образа — удалите альбом целиком", "Track inside an image — delete the whole album",
	  "Pista dentro de una imagen — elimina el álbum entero" },
	/* сон */
	{ "Сон через %d", "Sleep in %d", "Suspensión en %d" },
	{ "отпустите колесо — отмена", "release the wheel to cancel", "suelta la rueda para cancelar" },
	{ "нажмите любую кнопку — отмена", "press any button to cancel", "pulsa cualquier botón para cancelar" },
};

#define HSIZE 512   /* степень двойки, больше удвоенного числа строк */
static short hash[HSIZE];   /* номер строки таблицы + 1, 0 — пусто */
static int lang, built;

static unsigned fnv(const char *s)
{
	unsigned h = 2166136261u;
	while (*s)
		h = (h ^ (unsigned char)*s++) * 16777619u;
	return h;
}

static void build(void)
{
	for (int i = 0; i < (int)(sizeof(table) / sizeof(table[0])); i++) {
		unsigned k = fnv(table[i][LANG_RU]) & (HSIZE - 1);
		while (hash[k])
			k = (k + 1) & (HSIZE - 1);
		hash[k] = (short)(i + 1);
	}
}

void i18n_set(int l)
{
	if (!built++)
		build();
	lang = l < 0 || l >= LANG_COUNT ? LANG_RU : l;
}

int i18n_lang(void)
{
	return lang;
}

const char *tx(const char *s)
{
	if (!s || lang == LANG_RU || !*s)
		return s;
	for (unsigned k = fnv(s) & (HSIZE - 1); hash[k]; k = (k + 1) & (HSIZE - 1)) {
		const char *const *row = table[hash[k] - 1];
		if (!strcmp(row[LANG_RU], s))
			return row[lang] ? row[lang] : s;
	}
	return s;
}
