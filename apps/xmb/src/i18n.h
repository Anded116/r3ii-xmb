#ifndef I18N_H
#define I18N_H

/* Языки интерфейса. Строки в коде — русские; tx() отдаёт перевод на выбранный язык (нет перевода — как есть) */
enum { LANG_RU, LANG_EN, LANG_ES, LANG_COUNT };

void i18n_set(int lang);
int i18n_lang(void);
const char *tx(const char *s);

#endif
