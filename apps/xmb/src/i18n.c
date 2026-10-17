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
	{ "Отдать карту", "Give the card", "Ceder la tarjeta" },
	{ "Прокрутите вниз", "Scroll down", "Gira hacia abajo" },
	{ "Прокрутите вверх", "Scroll up", "Gira hacia arriba" },
	{ "Карта у компьютера", "Card is with the computer", "La tarjeta está en el ordenador" },
	{ "Чтобы вернуть её плееру, прокрутите колесо вверх", "To give it back to the player, scroll the wheel up",
	  "Para devolverla al reproductor, gira la rueda hacia arriba" },
	{ "вернуть карту", "return the card", "devolver la tarjeta" }, { "удалить", "delete", "eliminar" },
	{ "перезагрузить", "restart", "reiniciar" },
	{ "Удалить с карты?", "Delete from the card?", "¿Eliminar de la tarjeta?" },
	{ "«%s» удалится с карты насовсем", "\"%s\" will be deleted from the card for good", "«%s» se eliminará de la tarjeta para siempre" },
	{ "Удалить плейлист?", "Delete the playlist?", "¿Eliminar la lista?" },
	{ "«%s» — треки останутся на карте", "\"%s\" — the tracks stay on the card", "«%s»: las pistas se quedan en la tarjeta" },
	{ "Перезагрузить в сток?", "Restart into stock?", "¿Reiniciar en el sistema original?" },
	{ "Один раз загрузится стоковый плеер HiBy, следующая загрузка — снова XMB",
	  "The stock HiBy player will start once, the next boot is XMB again",
	  "Se iniciará una vez el reproductor original de HiBy; el siguiente arranque, XMB otra vez" },
	{ "Подключите кабель к компьютеру", "Connect the cable to a computer", "Conecta el cable a un ordenador" },
	{ "карта у компьютера — извлеките её там\nили удерживайте колесо, чтобы вернуть",
	  "the card is on the computer — eject it there\nor hold the wheel to return it",
	  "la tarjeta está en el ordenador — expúlsala allí\no mantén la rueda para recuperarla" },
	{ "Перезагрузить в сток", "Reboot to stock", "Reiniciar al original" }, { "Отмена", "Cancel", "Cancelar" },
	{ "О системе", "About", "Acerca de" },
	{ "Библиотека", "Library", "Biblioteca" }, { "Карта памяти", "Memory card", "Tarjeta de memoria" },
	{ "Батарея", "Battery", "Batería" }, { "Устройство", "Device", "Dispositivo" }, { "Лицензия", "License", "Licencia" },
	{ "сборка %s", "build %s", "compilación %s" },
	{ "%d треков · %d альбомов · %d исполнителей", "%d tracks · %d albums · %d artists", "%d pistas · %d álbumes · %d artistas" },
	{ "свободно %.1f ГБ из %.1f ГБ", "%.1f GB free of %.1f GB", "%.1f GB libres de %.1f GB" }, { "нет карты", "no card", "sin tarjeta" },
	{ "%d%% · заряжается", "%d%% · charging", "%d%% · cargando" },
	{ "Создан «%s»", "Created “%s”", "Creada «%s»" },
	{ "Звук", "Sound", "Sonido" }, { "эквалайзер, усиление, ReplayGain", "equalizer, gain, ReplayGain", "ecualizador, ganancia, ReplayGain" },
	{ "тема, фон, яркость, размер", "theme, background, brightness, size", "tema, fondo, brillo, tamaño" },
	{ "Управление", "Controls", "Controles" }, { "кнопки на корпусе, сенсор", "hardware buttons, touch", "botones, táctil" },
	{ "Питание", "Power", "Energía" }, { "таймер сна, сон устройства", "sleep timer, device sleep", "temporizador, reposo" },
	{ "Система", "System", "Sistema" }, { "стоковый плеер, о системе", "stock player, about", "reproductor original, acerca de" },
	{ "Конец очереди", "End of queue", "Fin de la cola" },
	{ "Сон через %d мин", "Sleep in %d min", "Reposo en %d min" }, { "Один", "One", "Una" }, { "Все", "All", "Todas" },
	{ "Сенсор", "Touch", "Táctil" },
	{ "Подкасты", "Podcasts", "Podcasts" }, { "Обновить", "Update", "Actualizar" }, { "прослушан", "played", "escuchado" },
	{ "%sосталось %d мин", "%s%d min left", "%squedan %d min" }, { "%s%d мин", "%s%d min", "%s%d min" },
	{ "подкастов: %d", "podcasts: %d", "podcasts: %d" }, { "выпусков пока нет", "no episodes yet", "aún no hay episodios" },
	{ "подписки — в Podcasts/подписки.txt", "subscriptions: Podcasts/подписки.txt", "suscripciones: Podcasts/подписки.txt" },
	{ "ссылки на RSS — в Podcasts/подписки.txt", "RSS links go in Podcasts/подписки.txt", "enlaces RSS en Podcasts/подписки.txt" },
	{ "проверить новые выпуски", "check for new episodes", "buscar episodios nuevos" }, { "нужен Wi-Fi", "Wi-Fi needed", "se necesita Wi-Fi" },
	{ "Включите Wi-Fi: Настройки → Wi-Fi", "Turn on Wi-Fi: Settings → Wi-Fi", "Activa el Wi-Fi: Ajustes → Wi-Fi" },
	{ "Положите ссылки на RSS в Podcasts/подписки.txt", "Put RSS links in Podcasts/подписки.txt", "Pon enlaces RSS en Podcasts/подписки.txt" },
	{ "проверка подписок: %d из %d", "checking feeds: %d of %d", "revisando: %d de %d" },
	{ "скачивается выпуск %d (подписка %d)", "downloading episode %d (feed %d)", "descargando episodio %d (fuente %d)" },
	{ "Подкасты: новых выпусков — %d", "Podcasts: %d new episodes", "Podcasts: %d episodios nuevos" },
	{ "Подкасты: подписки не открылись", "Podcasts: feeds could not be opened", "Podcasts: no se abrieron las fuentes" },
	{ "Фон плеера", "Player background", "Fondo del reproductor" }, { "Затемнение", "Dimmed", "Oscurecido" },
	{ "Цвет обложки", "Cover color", "Color de la portada" },
	{ "Текст", "Lyrics", "Letra" },
	{ "Текста нет. С Wi-Fi найду его в LRCLIB", "No lyrics. With Wi-Fi I'll look in LRCLIB", "Sin letra. Con Wi-Fi la buscaré en LRCLIB" },
	{ "Ищу текст…", "Looking for lyrics…", "Buscando la letra…" }, { "Текст не найден", "Lyrics not found", "Letra no encontrada" },
	{ "LRCLIB не отвечает", "LRCLIB is not responding", "LRCLIB no responde" },
	{ "Сети", "Networks", "Redes" }, { "Wi-Fi выключен", "Wi-Fi is off", "Wi-Fi apagado" },
	{ "Экран сбоит — перезагрузите плеер", "Display failure — restart the player", "Fallo de pantalla: reinicie el reproductor" }, { "Искать сети", "Search for networks", "Buscar redes" },
	{ "выключен", "off", "apagado" }, { "сохранена", "saved", "guardada" }, { "с паролем", "secured", "con contraseña" },
	{ "открытая", "open", "abierta" }, { "сильный сигнал", "strong signal", "señal fuerte" }, { "средний сигнал", "medium signal", "señal media" },
	{ "слабый сигнал", "weak signal", "señal débil" }, { "Пароль сети", "Network password", "Contraseña de la red" },
	{ "Уже подключено — удержание колеса: забыть сеть", "Connected — hold the wheel to forget", "Conectado — mantén la rueda para olvidar" },
	{ "Забыть сеть", "Forget network", "Olvidar red" }, { "Сеть забыта", "Network forgotten", "Red olvidada" },
	{ "Не подключилось — неверный пароль?", "Could not connect — wrong password?", "No se conectó — ¿contraseña incorrecta?" },
	{ "поиск сетей…", "searching…", "buscando…" }, { "получение адреса…", "getting an address…", "obteniendo dirección…" }, { "Для этого трека нет текста", "No lyrics for this track", "No hay letra para esta pista" },
	{ "Батарея %d%% — подключите зарядку", "Battery %d%% — connect the charger", "Batería %d%% — conecta el cargador" },
	{ "Зарядка — выключение отменено", "Charging — power-off cancelled", "Cargando — apagado cancelado" },
	{ "Батарея села — выключение через %d с", "Battery empty — powering off in %d s", "Batería agotada — apagado en %d s" },
	{ "подключено — клик отключит", "connected — click to disconnect", "conectado — clic para desconectar" },
	{ "сопряжено", "paired", "emparejado" }, { "Bluetooth выключен", "Bluetooth is off", "Bluetooth apagado" },
	{ "включите его выше", "turn it on above", "actívalo arriba" }, { "Искать устройства", "Search for devices", "Buscar dispositivos" },
	{ "наушники — в режим сопряжения", "put headphones in pairing mode", "auriculares en modo emparejamiento" },
	{ "поиск устройств…", "searching…", "buscando…" }, { "подключение…", "connecting…", "conectando…" },
	{ "Подключено", "Connected", "Conectado" }, { "Не удалось подключить", "Could not connect", "No se pudo conectar" },
	{ "Отключено", "Disconnected", "Desconectado" }, { "Подключение к «%s»…", "Connecting to “%s”…", "Conectando a «%s»…" },
	{ "не подключено", "not connected", "sin conectar" }, { "нет устройств", "no devices", "sin dispositivos" },
	{ "Язык · Language", "Language", "Idioma" },
	/* категории и музыка */
	{ "Настройки", "Settings", "Ajustes" }, { "Музыка", "Music", "Música" }, { "Папки", "Folders", "Carpetas" },
	{ "Плейлисты", "Playlists", "Listas" },
	{ "Сканирование библиотеки…", "Scanning library…", "Analizando la biblioteca…" },
	{ "просмотрено файлов: %d", "files scanned: %d", "archivos analizados: %d" },
	{ "Музыка не найдена", "No music found", "No se encontró música" },
	{ "на карте нет FLAC, MP3 или WAV", "no FLAC, MP3 or WAV on the card", "no hay FLAC, MP3 ni WAV en la tarjeta" },
	{ "укажите папку в XMB_MUSIC", "set the folder in XMB_MUSIC", "indica la carpeta en XMB_MUSIC" },
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
	{ "подключено", "connected", "conectado" }, 
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
