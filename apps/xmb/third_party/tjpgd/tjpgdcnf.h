/* TJpgDec: своя конфигурация — с масштабированием при декодировании (обложки 1000+ px → миниатюры) */
#define JD_SZBUF        1024
#define JD_FORMAT       0   /* RGB888 */
#define JD_USE_SCALE    1
#define JD_TBLCLIP      1
#define JD_FASTDECODE   1
